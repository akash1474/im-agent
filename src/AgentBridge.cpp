#ifdef AGENT_BRIDGE

#include "agent/AgentBridge.h"
#include "AgentInternal.h"
#include "AgentUtil.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#include <GLFW/glfw3.h>

#define CPPHTTPLIB_THREAD_POOL_COUNT 8
#include "cpp-httplib/httplib.h"

namespace Agent
{
    namespace
    {
        constexpr int kDefaultPort = 7878;
        constexpr int kMaxBlockedCalls = 4; // Below the pool size so initialize/list/ping always find a worker.

        struct Bridge
        {
            httplib::Server server;
            std::thread thread;
            std::mutex mutex;
            std::deque<CallPtr> queue; // HTTP threads -> main thread.
            std::atomic<bool> stopping{false};
            std::atomic<int> blockedCalls{0}; // tools/call requests currently waiting for the main thread.
            std::string token; // Required as a bearer token when non-empty ($AGENT_TOKEN).
            int port = 0;
        };

        std::unique_ptr<Bridge> gBridge;

        // Host of "host", "host:port" or "[::1]:port".
        std::string HostOf(const std::string& hostPort)
        {
            if (!hostPort.empty() && hostPort[0] == '[')
                return hostPort.substr(0, hostPort.find(']') + 1);
            return hostPort.substr(0, hostPort.find(':'));
        }

        // Port of "host:port" / "[::1]:port", or -1 when absent.
        int PortOf(const std::string& hostPort)
        {
            const size_t colon = hostPort.rfind(':');
            if (colon == std::string::npos || (hostPort[0] == '[' && colon < hostPort.find(']')))
                return -1;
            return std::atoi(hostPort.c_str() + colon + 1);
        }

        bool IsLocalHost(const std::string& host) { return host == "127.0.0.1" || host == "localhost" || host == "[::1]"; }

        // Guards against DNS rebinding and cross-site requests. Browsers always send Origin on cross-origin POSTs;
        // only a page on the bridge's own origin is accepted, so other localhost web apps can't drive the UI.
        bool IsAllowed(const httplib::Request& req, int port)
        {
            if (!IsLocalHost(HostOf(req.get_header_value("Host"))))
                return false;
            const std::string origin = req.get_header_value("Origin");
            if (origin.empty())
                return true;
            const size_t scheme = origin.find("://");
            if (scheme == std::string::npos)
                return false;
            const std::string hostPort = origin.substr(scheme + 3);
            return IsLocalHost(HostOf(hostPort)) && PortOf(hostPort) == port;
        }

        void SendJson(httplib::Response& res, const Json& body) { res.set_content(Dump(body), "application/json"); }

        // Compares in constant time so the token can't be recovered from response timing.
        bool HasValidToken(const Bridge& bridge, const httplib::Request& req)
        {
            if (bridge.token.empty())
                return true;
            const std::string given = req.get_header_value("Authorization");
            const std::string expected = "Bearer " + bridge.token;
            unsigned diff = (unsigned)(given.size() ^ expected.size());
            for (size_t i = 0; i < expected.size(); i++)
                diff |= (unsigned char)(i < given.size() ? given[i] : 0) ^ (unsigned char)expected[i];
            return diff == 0;
        }

        void SendResult(httplib::Response& res, const Json& id, Json result)
        {
            Json j = Json::object();
            j["jsonrpc"] = "2.0";
            j["id"] = id;
            j["result"] = std::move(result);
            SendJson(res, j);
        }

        void SendError(httplib::Response& res, const Json& id, int code, const char* message)
        {
            Json error = Json::object();
            error["code"] = code;
            error["message"] = message;
            Json j = Json::object();
            j["jsonrpc"] = "2.0";
            j["id"] = id;
            j["error"] = std::move(error);
            SendJson(res, j);
        }

        std::string NegotiateVersion(const Json& params)
        {
            static const char* kSupported[] = {"2025-06-18", "2025-03-26", "2024-11-05"};
            const std::string requested = ArgStr(params, "protocolVersion");
            for (const char* v : kSupported)
                if (requested == v)
                    return v;
            return kSupported[0];
        }

        Json InitializeResult(const Json& params)
        {
            Json tools = Json::object();
            tools["listChanged"] = false;
            Json capabilities = Json::object();
            capabilities["tools"] = std::move(tools);
            Json info = Json::object();
            info["name"] = "imagent";
            info["version"] = "0.1.0";

            Json r = Json::object();
            r["protocolVersion"] = NegotiateVersion(params);
            r["capabilities"] = std::move(capabilities);
            r["serverInfo"] = std::move(info);
            r["instructions"] =
                "Debug bridge into a running Dear ImGui app. Data describes the last completed frame. "
                "Rects are [x, y, w, h] in ImGui points; ids are 8-digit hex ImGuiIDs.";
            return r;
        }

        Json ToolList()
        {
            Json list = Json::array();
            for (const Tool& t : Tools())
            {
                Json j = Json::object();
                j["name"] = t.name;
                j["description"] = t.description;
                j["inputSchema"] = t.inputSchema;
                list.push_back(std::move(j));
            }
            Json r = Json::object();
            r["tools"] = std::move(list);
            return r;
        }

        // Queues the call for the main thread and waits for its reply.
        Json RunOnMainThread(Bridge& bridge, const httplib::Request& req, const CallPtr& call)
        {
            auto future = call->promise.get_future();
            {
                std::lock_guard<std::mutex> lock(bridge.mutex);
                bridge.queue.push_back(call);
            }
            glfwPostEmptyEvent();

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(call->tool->timeoutSeconds);
            while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
            {
                const bool stopping = bridge.stopping;
                const bool clientGone = req.is_connection_closed(); // The agent gave up; free the slot for its next call.
                if (stopping || clientGone || std::chrono::steady_clock::now() > deadline)
                {
                    call->cancelled = true; // The main thread drops it instead of finishing work nobody is waiting for.
                    return Call::Result(Call::TextItem(stopping ? "Application is shutting down"
                                                                 : clientGone ? "Client disconnected"
                                                                              : "Application did not render a frame in time (window minimized or blocked?)"),
                                        true);
                }
            }
            return future.get()();
        }

        void HandleMcp(Bridge& bridge, const httplib::Request& req, httplib::Response& res)
        {
            if (!IsAllowed(req, bridge.port))
            {
                res.status = 403;
                return;
            }
            if (!HasValidToken(bridge, req))
            {
                res.status = 401;
                res.set_header("WWW-Authenticate", "Bearer");
                return;
            }

            const Json msg = Json::parse(req.body, nullptr, false);
            if (msg.is_discarded())
                return SendError(res, nullptr, -32700, "Parse error");
            if (!msg.is_object())
                return SendError(res, nullptr, -32600, "Batch requests are not supported");

            // Notifications and client responses need no reply body.
            if (!msg.contains("id") || !msg.contains("method"))
            {
                res.status = 202;
                return;
            }

            const Json& id = msg["id"];
            const std::string method = ArgStr(msg, "method");
            const Json params = msg.contains("params") && msg["params"].is_object() ? msg["params"] : Json::object();

            if (method == "initialize")
                return SendResult(res, id, InitializeResult(params));
            if (method == "ping")
                return SendResult(res, id, Json::object());
            if (method == "tools/list")
                return SendResult(res, id, ToolList());

            if (method == "tools/call")
            {
                const std::string name = ArgStr(params, "name");
                if (const Tool* found = FindTool(name))
                {
                    const Tool& tool = *found;
                    auto call = std::make_shared<Call>();
                    call->tool = &tool;
                    if (params.contains("arguments") && params["arguments"].is_object())
                        call->args = params["arguments"];

                    // Each blocked call pins a worker thread; keep some free for the other methods.
                    if (bridge.blockedCalls.fetch_add(1) >= kMaxBlockedCalls)
                    {
                        bridge.blockedCalls--;
                        return SendResult(res, id, Call::Result(Call::TextItem("Too many concurrent calls; wait for the previous ones to finish"), true));
                    }
                    Json result = RunOnMainThread(bridge, req, call);
                    bridge.blockedCalls--;
                    return SendResult(res, id, std::move(result));
                }
                return SendError(res, id, -32602, "Unknown tool");
            }

            SendError(res, id, -32601, "Method not found");
        }

        void Dispatch(const CallPtr& call)
        {
            if (call->cancelled)
                return;
            try
            {
                call->tool->run(call);
            }
            catch (const std::exception& e) // E.g. a throwing AGENT_EXPOSE provider; report it instead of crashing the app.
            {
                call->Fail(e.what());
            }
        }
    } // namespace

    bool Start(int port)
    {
        if (gBridge)
            return true;
        if (port == 0)
        {
            const char* env = std::getenv("AGENT_PORT");
            port = env ? std::atoi(env) : 0;
            if (port <= 0 || port > 65535)
                port = kDefaultPort;
        }

        auto bridge = std::make_unique<Bridge>();
        Bridge* b = bridge.get();
        if (const char* token = std::getenv("AGENT_TOKEN"))
            b->token = token;
        b->port = port;
        b->server.set_tcp_nodelay(true);
        b->server.set_keep_alive_timeout(2); // Idle keep-alive connections otherwise pin a worker thread.
        b->server.set_read_timeout(5);
        b->server.set_payload_max_length(1024 * 1024);
        b->server.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr) {
            res.status = 500;
            SendError(res, nullptr, -32603, "Internal error");
        });
        b->server.Post("/mcp", [b](const httplib::Request& req, httplib::Response& res) { HandleMcp(*b, req, res); });
        // No server-initiated streams or sessions.
        auto notAllowed = [](const httplib::Request&, httplib::Response& res) {
            res.status = 405;
            res.set_header("Allow", "POST");
        };
        b->server.Get("/mcp", notAllowed);
        b->server.Delete("/mcp", notAllowed);

        if (!b->server.bind_to_port("127.0.0.1", port))
        {
            std::fprintf(stderr, "[agent] Could not bind 127.0.0.1:%d (set AGENT_PORT to use another port); bridge disabled\n", port);
            return false;
        }
        b->thread = std::thread([b] { b->server.listen_after_bind(); });
        gBridge = std::move(bridge);
        std::fprintf(stderr, "[agent] MCP server listening on http://127.0.0.1:%d/mcp (%s)\n", port,
                     gBridge->token.empty() ? "no auth; set AGENT_TOKEN to require a bearer token" : "bearer token required");
        return true;
    }

    void Stop()
    {
        if (!gBridge)
            return;
        gBridge->stopping = true;
        {
            std::lock_guard<std::mutex> lock(gBridge->mutex);
            for (const CallPtr& call : gBridge->queue)
                call->Fail("Application is shutting down");
            gBridge->queue.clear();
        }
        FailPending("Application is shutting down");
        FailBatches("Application is shutting down");
        Actions::FailAll("Application is shutting down");
        gBridge->server.stop();
        gBridge->thread.join();
        gBridge.reset();
    }

    void Pump()
    {
        if (!gBridge || !ImGui::GetCurrentContext())
            return;

        MetricsMark(MetricsPhase::PumpStart);
        std::deque<CallPtr> calls;
        {
            std::lock_guard<std::mutex> lock(gBridge->mutex);
            calls.swap(gBridge->queue);
        }
        for (const CallPtr& call : calls)
            Dispatch(call);

        TickPending();
        Actions::Advance();
        TickBatches();
        RecorderBeginFrame();
        MetricsMark(MetricsPhase::PumpEnd);
        if (IsBusy())
            glfwPostEmptyEvent();
    }

    bool IsBusy()
    {
        if (!gBridge)
            return false;
        {
            std::lock_guard<std::mutex> lock(gBridge->mutex);
            if (!gBridge->queue.empty())
                return true;
        }
        if (gBridge->blockedCalls.load() > 0)
            return true;
        return Input::IsActive() || Actions::IsActive() || HasPendingTools() || HasPendingBatches() || HasPendingWaits();
    }

    void EndFrame()
    {
        if (!gBridge)
            return;
        MetricsMark(MetricsPhase::EndFrameStart);
        RecorderEndFrame();
        ToolsEndFrame();
        MetricsMark(MetricsPhase::EndFrameEnd);
    }

    void AfterDraw()
    {
        if (!gBridge)
            return;
        MetricsMark(MetricsPhase::AfterDraw);
        ToolsAfterDraw();
    }
} // namespace Agent

#endif // AGENT_BRIDGE
