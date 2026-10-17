#pragma once

#ifdef AGENT_BRIDGE

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif
#include "imgui.h"
#include "imgui_internal.h"

// The bridge depends on imgui internals (item hooks, input event tagging, ID path query) and a local patch in imgui_draw.cpp.
static_assert(IMGUI_VERSION_NUM == 19300, "Re-verify the agent library against imgui internals and re-apply patches/imgui-agent-hooks.patch, then update this version.");

namespace Agent
{
    using Json = nlohmann::json;

    // Truncated or lossy UTF-8 from imgui labels must not make dump() throw.
    inline std::string Dump(const Json& j) { return j.dump(-1, ' ', false, Json::error_handler_t::replace); }

    // ---- Per-frame snapshot (built by Recorder.cpp, read by tools). Main thread only. ----

    struct ItemRec
    {
        ImGuiID id = 0, parentId = 0, windowId = 0;
        ImRect rect;
        ImRect clipped; // rect clipped to the window; click point when the item is only partly visible.
        ImGuiItemFlags itemFlags = 0;
        ImGuiItemStatusFlags status = 0;
        int label = -1; // Offset into Snapshot::strings, -1 if the widget reported none.
        bool visible = true, isWindow = false, isTitleBar = false;
        bool labelInferred = false; // label came from the text drawn beside the widget, not from imgui.
        bool decoration = false;    // window furniture (scrollbar, resize grip, title-bar button), not a user widget.
        bool conflict = false;      // another widget was registered with the same id this frame (duplicate ID).
        ImRect conflictRect;        // where the earlier widget with this id was.
    };

    struct TextRec
    {
        ImGuiID windowId = 0;
        ImRect rect;
        int str = -1;
        int font = -1; // Offset into Snapshot::strings.
        float size = 0.0f;
        ImU32 color = 0;
        bool visible = true;
    };

    struct WindowRec
    {
        ImGuiID id = 0, parentId = 0;
        int name = -1;
        ImRect rect;
        int z = 0; // Index in imgui's back-to-front order.
        const char* kind = "window";
        bool focused = false, hovered = false, collapsed = false;
    };

    struct PopupRec
    {
        ImGuiID id = 0, windowId = 0;
    };

    struct Snapshot
    {
        int frame = 0;
        std::vector<ItemRec> items;
        std::unordered_map<ImGuiID, int> index; // item id -> items[]
        std::vector<TextRec> texts;
        std::vector<WindowRec> windows;
        std::vector<PopupRec> popups;
        std::string strings; // NUL-terminated strings referenced by offset.
        std::vector<std::pair<const ImFont*, int>> fontNames; // font -> offset in strings, so each name is stored once per frame.

        ImGuiID hoveredId = 0, activeId = 0, navId = 0, hoveredWindowId = 0, navWindowId = 0;
        ImVec2 mousePos;
        bool wantCaptureMouse = false, wantCaptureKeyboard = false, wantTextInput = false;

        const char* Str(int offset) const { return strings.c_str() + offset; }
        void Clear()
        {
            items.clear();
            index.clear();
            texts.clear();
            windows.clear();
            popups.clear();
            strings.clear();
            fontNames.clear();
        }
    };

    void RecorderBeginFrame();
    void RecorderEndFrame();
    const Snapshot& LastSnapshot();

    // ---- Tool plumbing ----

    // Evaluated on the HTTP thread; returns the MCP tools/call result object.
    using Reply = std::function<Json()>;

    struct Tool;

    struct Call
    {
        const Tool* tool = nullptr;
        Json args = Json::object();
        std::promise<Reply> promise;
        bool done = false;
        std::atomic<bool> cancelled{false}; // Set by the HTTP thread when nobody is waiting for the reply any more.
        // Set for calls made from run_steps: the result goes to the batch instead of an HTTP reply.
        std::function<void(bool isError, Json data)> sink;
        std::shared_ptr<Call> parent; // The run_steps call that started this one.

        bool Cancelled() const { return cancelled || (parent && parent->cancelled); }

        // The reply is serialized on the HTTP thread to keep dumping out of the frame.
        void Ok(Json data)
        {
            if (sink)
                return Deliver(false, std::move(data));
            Lazy([data = std::move(data)] { return Result(TextItem(Dump(data)), false); });
        }
        void Fail(const std::string& message)
        {
            if (sink)
                return Deliver(true, Json(message));
            Lazy([message] { return Result(TextItem(message), true); });
        }
        void Lazy(Reply reply)
        {
            if (done)
                return;
            if (sink)
                return Deliver(true, Json("screenshots are not supported inside run_steps"));
            done = true;
            promise.set_value(std::move(reply));
        }

        static Json TextItem(const std::string& text)
        {
            Json item = Json::object();
            item["type"] = "text";
            item["text"] = text;
            return item;
        }

        // MCP tools/call result with a single content item.
        static Json Result(Json item, bool isError)
        {
            Json result = Json::object();
            result["content"] = Json::array({std::move(item)});
            result["isError"] = isError;
            return result;
        }

    private:
        void Deliver(bool isError, Json data)
        {
            if (done)
                return;
            done = true;
            sink(isError, std::move(data));
        }
    };
    using CallPtr = std::shared_ptr<Call>;

    struct Tool
    {
        std::string name;
        std::string description;
        Json inputSchema;
        void (*run)(const CallPtr&);
        int timeoutSeconds = 30; // How long the HTTP request waits for the main thread.
    };

    const std::vector<Tool>& Tools();
    const Tool* FindTool(const std::string& name); // nullptr if there is no such tool.
    void AppendActionTools(std::vector<Tool>& tools); // AgentActions.cpp
    void AppendBatchTools(std::vector<Tool>& tools);  // Batch.cpp
    void AppendLintTools(std::vector<Tool>& tools);   // Lint.cpp
    void AppendMetricsTools(std::vector<Tool>& tools); // Metrics.cpp

    // Frame timing marks for get_metrics, called from the public frame hooks in AgentBridge.cpp.
    enum class MetricsPhase { PumpStart, PumpEnd, EndFrameStart, EndFrameEnd, AfterDraw };
    void MetricsMark(MetricsPhase phase);
    void TickPending();                          // Main thread, once per frame: completes frame waits, arms screenshots.
    void TickWaits();                            // wait_for conditions (AgentActions.cpp).
    void TickBatches();                          // run_steps sequencing (Batch.cpp).
    bool HasPendingTools();
    bool HasPendingWaits();
    bool HasPendingBatches();
    void ToolsEndFrame();                        // Draws the screenshot annotation overlay if one is armed.
    void ToolsAfterDraw();                       // Captures the framebuffer if a screenshot is armed.
    void FailPending(const std::string& reason); // Fails every deferred call (shutdown).
    void FailWaits(const std::string& reason);
    void FailBatches(const std::string& reason);

    // ---- Screenshot ----

    struct Capture
    {
        std::shared_ptr<std::vector<uint8_t>> rgba; // Bottom-up rows, as returned by glReadPixels.
        int width = 0, height = 0;
        float pixelsPerPoint = 1.0f;
        ImVec2 displaySize;
    };

    struct ShotOptions
    {
        float scale = 1.0f; // Output pixels per ImGui point.
        bool hasRegion = false;
        ImRect region; // ImGui points.
    };

    bool CaptureFramebuffer(Capture& out);
    Json EncodeScreenshot(const Capture& capture, const ShotOptions& options); // Any thread; returns an MCP result.

    // Sends the next rendered frame as a screenshot; `prefix` (if not null) is added as a text item before the image.
    // Keeps the agent's input (pointer position, hover) taken over until the frame has been captured.
    void QueueScreenshot(const CallPtr& call, const ShotOptions& options, Json prefix);

    // Screen rect in ImGui points of the item (id, or label with window) or window (window alone) that `args` name.
    // Returns an error message, or "" on success; `target` then describes what was matched. AgentActions.cpp.
    std::string ResolveRegion(const Snapshot& snapshot, const Json& args, ImRect& rect, Json& target);

    // ---- Input injection (Input.cpp). Main thread only. ----

    namespace Input
    {
        bool IsActive();
        void Begin();             // Take over input: events from the OS backend are dropped until End().
        void End();               // Release everything held and hand the mouse back to the OS cursor.
        void DropBackendEvents(); // Call every frame while active, after the backend's NewFrame.
        void ReleaseAll();        // Release held buttons and keys.
        ImVec2 MousePos();

        void Move(const ImVec2& pos);
        void Button(int button, bool down);
        void Key(ImGuiKey key, bool down);
        void Text(const std::string& utf8);
        void Wheel(float wheelX, float wheelY); // imgui convention: positive Y scrolls up, positive X scrolls left.
    } // namespace Input

    // ---- Action runner (Input.cpp): scripted input executed one step per frame. ----

    struct Action;

    struct ActionCtx
    {
        const Snapshot& snap;
        Action& action;
        int frame = 0; // Times the current step has already run.
        bool failed = false;
        std::string error;

        void Fail(const std::string& message)
        {
            failed = true;
            error = message;
        }
    };

    using StepFn = std::function<bool(ActionCtx&)>; // Returns true once the step is finished.

    // ---- Change summaries (Changes.cpp): what an action changed on screen. ----

    struct UiSummary;
    std::shared_ptr<UiSummary> CaptureSummary(const Snapshot& snapshot);
    Json DiffSummary(const UiSummary& before, const UiSummary& after); // Null when nothing changed.

    struct Action
    {
        CallPtr call;
        std::deque<StepFn> steps;
        Json result = Json::object();
        Json warnings = Json::array();
        bool screenshot = false;
        bool trackChanges = true;
        std::shared_ptr<UiSummary> before; // Captured when the action starts running.
    };

    namespace Actions
    {
        bool IsActive();
        void Enqueue(Action action);
        void Advance();         // Once per frame from Pump(), after the backend's NewFrame.
        void FailAll(const std::string& reason);
        void Hold(int delta);   // Keeps input taken over between the steps of a run_steps batch.
        void HoldForCapture(int delta); // Keeps the pointer in place, and defers new actions, until a screenshot is captured.
    } // namespace Actions

} // namespace Agent

#endif // AGENT_BRIDGE
