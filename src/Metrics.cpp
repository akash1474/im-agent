#ifdef AGENT_BRIDGE

#include "AgentInternal.h"
#include "AgentUtil.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Agent
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        struct Sample
        {
            float frameMs, uiMs, renderMs, agentMs;
        };

        constexpr size_t kHistory = 600;
        std::vector<Sample> gSamples(kHistory);
        size_t gNext = 0, gCount = 0;

        // Timestamps of the frame in progress; its sample is stored when the next frame starts.
        Clock::time_point gMark[5];
        unsigned gSeen = 0; // Bit per phase seen this frame; only complete frames become samples.
        bool gHaveFrame = false;

        struct DrawStats
        {
            int vertices = 0, indices = 0, lists = 0, commands = 0;
        } gDraw;

        float Ms(Clock::time_point from, Clock::time_point to) { return std::chrono::duration<float, std::milli>(to - from).count(); }
        double Round2(double v) { return std::round(v * 100.0) / 100.0; }

        Json Stats(std::vector<float> values)
        {
            std::sort(values.begin(), values.end());
            auto percentile = [&](float p) { return values[std::min(values.size() - 1, (size_t)std::max(0.0f, std::ceil(p * (float)values.size()) - 1.0f))]; }; // Nearest rank.
            float sum = 0.0f;
            for (float v : values)
                sum += v;
            Json j = Json::object();
            j["avg"] = Round2(sum / (float)values.size());
            j["p50"] = Round2(percentile(0.50f));
            j["p95"] = Round2(percentile(0.95f));
            j["p99"] = Round2(percentile(0.99f));
            j["max"] = Round2(values.back());
            return j;
        }

        void GetMetrics(const CallPtr& c)
        {
            const size_t want = (size_t)ArgInt(c->args, "frames", 300, 1, (int)kHistory);
            const size_t n = std::min(want, gCount);
            const Snapshot& s = LastSnapshot();
            const ImGuiIO& io = ImGui::GetIO();

            Json out = Json::object();
            out["frame"] = s.frame;
            out["frames"] = (int)n;
            if (n > 0)
            {
                std::vector<float> frame, ui, render, agent;
                for (size_t i = 0; i < n; i++)
                {
                    const Sample& smp = gSamples[(gNext + kHistory - 1 - i) % kHistory];
                    frame.push_back(smp.frameMs);
                    ui.push_back(smp.uiMs);
                    render.push_back(smp.renderMs);
                    agent.push_back(smp.agentMs);
                }
                Json frameStats = Stats(frame);
                const float median = (float)frameStats["p50"].get<double>();
                out["fps"] = Round2(1000.0 / std::max(0.001, frameStats["avg"].get<double>()));
                out["frameMs"] = std::move(frameStats); // Includes vsync waits, so it sits near the refresh interval.
                out["uiMs"] = Stats(ui);                // CPU time to build the UI (NewFrame to just before Render).
                out["renderMs"] = Stats(render);        // Render() plus submitting draw data; the GPU runs on after this.
                out["agentMs"] = Stats(agent);          // Pump and EndFrame work in this library (tool calls, snapshot); per-widget recording is inside uiMs.
                out["slowFrames"] = (int)std::count_if(frame.begin(), frame.end(), [&](float v) { return v > 1.5f * median; });
            }

            Json draw = Json::object();
            draw["vertices"] = gDraw.vertices;
            draw["indices"] = gDraw.indices;
            draw["drawLists"] = gDraw.lists;
            draw["drawCommands"] = gDraw.commands;
            out["draw"] = std::move(draw);
            out["windows"] = io.MetricsActiveWindows;
            out["items"] = (int)s.items.size();
            out["texts"] = (int)s.texts.size();
            if (io.Fonts && io.Fonts->TexData)
                out["fontAtlas"] = Json::array({io.Fonts->TexData->Width, io.Fonts->TexData->Height});

            if (ArgBool(c->args, "reset", false))
                gCount = gNext = 0;
            c->Ok(out);
        }
    } // namespace

    void MetricsMark(MetricsPhase phase)
    {
        const Clock::time_point now = Clock::now();
        if (phase == MetricsPhase::PumpStart && gHaveFrame && gSeen == 0x1F)
        {
            gSamples[gNext] = {Ms(gMark[0], now), Ms(gMark[1], gMark[2]), Ms(gMark[3], gMark[4]), Ms(gMark[0], gMark[1]) + Ms(gMark[2], gMark[3])};
            gNext = (gNext + 1) % kHistory;
            gCount = std::min(gCount + 1, kHistory);
        }
        gMark[(int)phase] = now;
        gSeen = phase == MetricsPhase::PumpStart ? 1u : gSeen | (1u << (int)phase);
        if (phase == MetricsPhase::PumpStart)
            gHaveFrame = false;
        else if (phase == MetricsPhase::AfterDraw)
        {
            gHaveFrame = true;
            if (const ImDrawData* data = ImGui::GetDrawData())
            {
                gDraw = {data->TotalVtxCount, data->TotalIdxCount, data->CmdLists.Size, 0};
                for (const ImDrawList* list : data->CmdLists)
                    gDraw.commands += list->CmdBuffer.Size;
            }
        }
    }

    void AppendMetricsTools(std::vector<Tool>& tools)
    {
        tools.push_back(Tool{
            "get_metrics",
            "Performance numbers over the last N frames: frameMs (includes vsync), uiMs (CPU time of the app's UI code, including this library's per-widget recording in Debug builds), renderMs, "
            "agentMs (this library's per-frame work in Pump and EndFrame: tool calls, snapshot, lint), slowFrames (> 1.5x median), plus draw statistics "
            "(vertices, indices, draw lists and commands), active windows, recorded items and the font atlas size. Times are milliseconds with avg/p50/p95/p99/max. "
            "To measure an action: call with reset=true, perform it, call again.",
            Json::parse(R"({"type":"object","properties":{
                "frames":{"type":"integer","description":"How many recent frames to summarize; default 300, max 600."},
                "reset":{"type":"boolean","description":"Clear the frame history after reading."}}})"),
            GetMetrics});
    }
} // namespace Agent

#endif // AGENT_BRIDGE
