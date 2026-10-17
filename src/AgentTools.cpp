#ifdef AGENT_BRIDGE

#include "agent/AgentBridge.h"
#include "AgentInternal.h"
#include "AgentUtil.h"

#include <deque>
#include <map>
#include <optional>

namespace Agent
{
    namespace
    {
        // ---- Deferred work (main thread) ----

        struct FrameWait
        {
            int target;
            CallPtr call;
        };
        struct ShotRequest
        {
            CallPtr call;
            ShotOptions options;
            bool annotate = false;
            bool holdsInput = false; // Keeps the agent's pointer in place until the frame is captured.
            Json prefix; // Optional result object sent as a text item before the image.
        };

        void ReleaseInput(const ShotRequest& req)
        {
            if (req.holdsInput)
                Actions::HoldForCapture(-1);
        }

        std::vector<FrameWait> gFrameWaits;
        std::deque<ShotRequest> gShotQueue;
        std::optional<ShotRequest> gArmed;
        std::map<std::string, std::function<std::string()>> gProviders;

        struct Command
        {
            std::string description;
            CommandFn fn;
        };
        std::map<std::string, Command> gCommands;

        // ---- Tools ----

        // imgui reports -FLT_MAX while the cursor is outside the window.
        Json MouseJson(const Snapshot& s)
        {
            if (s.mousePos.x < -1e6f)
                return nullptr;
            return Json::array({R(s.mousePos.x), R(s.mousePos.y)});
        }

        void GetAppInfo(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            const ImGuiIO& io = ImGui::GetIO();
            Json j = Json::object();
            j["imgui"] = ImGui::GetVersion();
            j["frame"] = s.frame;
            j["fps"] = R(io.Framerate);
            j["displaySize"] = Json::array({R(io.DisplaySize.x), R(io.DisplaySize.y)});
            j["pixelsPerPoint"] = io.DisplayFramebufferScale.x;
            j["mouse"] = MouseJson(s);
            j["windows"] = (int)s.windows.size();
            j["items"] = (int)s.items.size();
            j["texts"] = (int)s.texts.size();
            Json names = Json::array();
            for (const auto& [name, provider] : gProviders)
                names.push_back(name);
            j["state"] = std::move(names);
            Json commands = Json::array();
            for (const auto& [name, command] : gCommands)
                commands.push_back(name);
            j["commands"] = std::move(commands);
            c->Ok(j);
        }

        void GetWindows(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            Json windows = Json::array();
            for (const WindowRec& w : s.windows)
                windows.push_back(WindowJson(s, w));
            Json popups = Json::array();
            for (const PopupRec& p : s.popups)
            {
                Json j = Json::object();
                j["id"] = Hex(p.id);
                if (p.windowId)
                    j["window"] = WindowName(s, p.windowId);
                popups.push_back(std::move(j));
            }
            Json out = Json::object();
            out["frame"] = s.frame;
            out["windows"] = std::move(windows);
            out["openPopups"] = std::move(popups);
            c->Ok(out);
        }

        struct Entry
        {
            const ItemRec* rec;
            Json json;
        };

        Json FlatItems(std::vector<Entry>& entries)
        {
            Json items = Json::array();
            for (Entry& e : entries)
                items.push_back(std::move(e.json));
            return items;
        }

        // Puts each item under its nearest emitted ancestor (following parent ids, even through filtered-out items).
        Json NestItems(const Snapshot& s, std::vector<Entry>& entries)
        {
            std::unordered_map<ImGuiID, size_t> position;
            for (size_t k = 0; k < entries.size(); k++)
                position[entries[k].rec->id] = k;

            std::vector<std::vector<size_t>> kids(entries.size());
            std::vector<size_t> roots;
            for (size_t k = 0; k < entries.size(); k++)
            {
                const ItemRec* cur = entries[k].rec;
                ImGuiID parent = cur->parentId;
                bool attached = false;
                for (int depth = 0; parent && parent != cur->id && depth < 32; depth++)
                {
                    auto it = position.find(parent);
                    if (it != position.end() && it->second != k)
                    {
                        kids[it->second].push_back(k);
                        attached = true;
                        break;
                    }
                    const ItemRec* up = FindItem(s, parent);
                    if (!up)
                        break;
                    cur = up;
                    parent = up->parentId;
                }
                if (!attached)
                    roots.push_back(k);
            }

            std::vector<bool> built(entries.size(), false);
            std::function<Json(size_t)> build = [&](size_t k) {
                built[k] = true;
                Json j = std::move(entries[k].json);
                Json children = Json::array();
                for (size_t child : kids[k])
                    if (!built[child])
                        children.push_back(build(child));
                if (!children.empty())
                    j["children"] = std::move(children);
                return j;
            };

            Json items = Json::array();
            for (size_t k : roots)
                items.push_back(build(k));
            for (size_t k = 0; k < entries.size(); k++) // Only reachable through a parent cycle; don't lose them.
                if (!built[k])
                    items.push_back(build(k));
            return items;
        }

        void GetUiTree(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            const std::string windowFilter = ArgStr(c->args, "window");
            const bool visibleOnly = ArgBool(c->args, "visible_only", true);
            const bool labeledOnly = ArgBool(c->args, "labeled_only", true);
            const int limit = ArgInt(c->args, "limit", 300, 1, 2000);
            const bool nested = ArgBool(c->args, "nested", false);

            std::unordered_map<ImGuiID, size_t> windowIndex;
            for (size_t i = 0; i < s.windows.size(); i++)
                windowIndex[s.windows[i].id] = i;

            const TextIndex texts(s);
            std::vector<std::vector<Entry>> perWindow(s.windows.size());
            int emitted = 0;
            bool truncated = false;
            for (const ItemRec& r : s.items)
            {
                if (r.isWindow || r.isTitleBar || (visibleOnly && !r.visible) || (labeledOnly && r.label < 0 && !Interactive(r)))
                    continue;
                auto it = windowIndex.find(r.windowId);
                if (it == windowIndex.end() || !ContainsI(s.Str(s.windows[it->second].name), windowFilter))
                    continue;
                if (emitted >= limit)
                {
                    truncated = true;
                    break;
                }
                Json j = ItemJson(s, r);
                AttachText(j, s, r, texts);
                perWindow[it->second].push_back({&r, std::move(j)});
                emitted++;
            }

            Json windows = Json::array();
            for (size_t i = 0; i < s.windows.size(); i++)
            {
                if (perWindow[i].empty())
                    continue;
                Json w = WindowJson(s, s.windows[i]);
                w["items"] = nested ? NestItems(s, perWindow[i]) : FlatItems(perWindow[i]);
                windows.push_back(std::move(w));
            }
            Json out = Json::object();
            out["frame"] = s.frame;
            out["count"] = emitted;
            if (truncated)
                out["truncated"] = true;
            out["windows"] = std::move(windows);
            c->Ok(out);
        }

        void FindItems(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            const std::string query = ArgStr(c->args, "query");
            const std::string windowFilter = ArgStr(c->args, "window");
            const std::string kind = ArgStr(c->args, "kind");
            const bool visibleOnly = ArgBool(c->args, "visible_only", true);
            const int limit = ArgInt(c->args, "limit", 50, 1, 500);
            ImGuiID wantedId = 0;
            const bool byId = ParseId(ArgStr(c->args, "id"), wantedId);

            const TextIndex texts(s);
            Json items = Json::array();
            bool truncated = false;
            for (const ItemRec& r : s.items)
            {
                if ((byId && r.id != wantedId) || (visibleOnly && !r.visible) || (!kind.empty() && kind != KindOf(r)))
                    continue;
                if (!query.empty() && (r.label < 0 || !ContainsI(s.Str(r.label), query)))
                    continue;
                const char* window = WindowName(s, r.windowId);
                if (!ContainsI(window, windowFilter))
                    continue;
                if ((int)items.size() >= limit)
                {
                    truncated = true;
                    break;
                }
                Json j = ItemJson(s, r);
                AttachText(j, s, r, texts);
                j["window"] = window;
                items.push_back(std::move(j));
            }
            Json out = Json::object();
            out["frame"] = s.frame;
            if (truncated)
                out["truncated"] = true;
            out["items"] = std::move(items);
            c->Ok(out);
        }

        void GetText(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            const std::string query = ArgStr(c->args, "query");
            const std::string windowFilter = ArgStr(c->args, "window");
            const bool visibleOnly = ArgBool(c->args, "visible_only", true);
            const int limit = ArgInt(c->args, "limit", 200, 1, 2000);

            Json texts = Json::array();
            bool truncated = false;
            for (const TextRec& t : s.texts)
            {
                if ((visibleOnly && !t.visible) || !ContainsI(s.Str(t.str), query))
                    continue;
                const char* window = WindowName(s, t.windowId);
                if (!ContainsI(window, windowFilter))
                    continue;
                if ((int)texts.size() >= limit)
                {
                    truncated = true;
                    break;
                }
                Json j = Json::object();
                j["window"] = window;
                j["text"] = s.Str(t.str);
                j["rect"] = RectJson(t.rect);
                j["color"] = ColorHex(t.color);
                j["size"] = std::round(t.size * 10.0f) / 10.0f;
                j["font"] = s.Str(t.font);
                if (!t.visible)
                    j["visible"] = false;
                texts.push_back(std::move(j));
            }
            Json out = Json::object();
            out["frame"] = s.frame;
            if (truncated)
                out["truncated"] = true;
            out["texts"] = std::move(texts);
            c->Ok(out);
        }

        void GetHovered(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            Json out = Json::object();
            out["frame"] = s.frame;
            out["mouse"] = MouseJson(s);
            out["wantCaptureMouse"] = s.wantCaptureMouse;
            out["wantCaptureKeyboard"] = s.wantCaptureKeyboard;
            out["wantTextInput"] = s.wantTextInput;
            out["hoveredWindow"] = s.hoveredWindowId ? Json(WindowName(s, s.hoveredWindowId)) : Json(nullptr);
            out["focusedWindow"] = s.navWindowId ? Json(WindowName(s, s.navWindowId)) : Json(nullptr);
            out["hovered"] = DescribeId(s, s.hoveredId);
            out["active"] = DescribeId(s, s.activeId);
            out["focused"] = DescribeId(s, s.navId);
            c->Ok(out);
        }

        void GetState(const CallPtr& c)
        {
            auto evaluate = [&](const std::string& name, Json& out) {
                Json value = Json::parse(gProviders[name](), nullptr, false);
                if (value.is_discarded())
                {
                    c->Fail("State provider '" + name + "' returned invalid JSON");
                    return false;
                }
                out = std::move(value);
                return true;
            };

            const std::string name = ArgStr(c->args, "name");
            if (!name.empty())
            {
                if (!gProviders.count(name))
                {
                    c->Fail("Unknown state '" + name + "'. Available: " + JoinKeys(gProviders, "(none)"));
                    return;
                }
                Json value;
                if (evaluate(name, value))
                    c->Ok(value);
                return;
            }

            Json state = Json::object();
            for (const auto& [n, provider] : gProviders)
            {
                Json value;
                if (!evaluate(n, value))
                    return;
                state[n] = std::move(value);
            }
            Json out = Json::object();
            out["state"] = std::move(state);
            c->Ok(out);
        }

        struct LogEvent
        {
            const char* name;
            ImGuiDebugLogFlags flag;
        };
        constexpr LogEvent kLogEvents[] = {
            {"error", ImGuiDebugLogFlags_EventError},         {"activeid", ImGuiDebugLogFlags_EventActiveId},
            {"focus", ImGuiDebugLogFlags_EventFocus},         {"popup", ImGuiDebugLogFlags_EventPopup},
            {"nav", ImGuiDebugLogFlags_EventNav},             {"clipper", ImGuiDebugLogFlags_EventClipper},
            {"selection", ImGuiDebugLogFlags_EventSelection}, {"io", ImGuiDebugLogFlags_EventIO},
            {"font", ImGuiDebugLogFlags_EventFont},           {"inputrouting", ImGuiDebugLogFlags_EventInputRouting},
            {"table", ImGuiDebugLogFlags_EventTable},
        };
        constexpr int kMaxLogBytes = 1 << 20;

        // Applies enable/disable lists of event names to imgui's debug-log flags; returns an error message or "".
        std::string SetLogEvents(const Json& args, const char* key, bool enable, ImGuiDebugLogFlags& flags)
        {
            auto list = args.find(key);
            if (list == args.end() || !list->is_array())
                return "";
            for (const Json& entry : *list)
            {
                const std::string name = Lower(entry.is_string() ? entry.get<std::string>() : std::string());
                const LogEvent* event = std::find_if(std::begin(kLogEvents), std::end(kLogEvents), [&](const LogEvent& e) { return name == e.name; });
                if (event == std::end(kLogEvents))
                    return "Unknown log event '" + name + "'. Available: error, activeid, focus, popup, nav, clipper, selection, io, font, inputrouting, table";
                flags = enable ? (flags | event->flag) : (flags & ~event->flag);
            }
            return "";
        }

        void GetLog(const CallPtr& c)
        {
            ImGuiContext& g = *ImGui::GetCurrentContext();
            std::string error = SetLogEvents(c->args, "enable", true, g.DebugLogFlags);
            if (error.empty())
                error = SetLogEvents(c->args, "disable", false, g.DebugLogFlags);
            if (!error.empty())
                return c->Fail(error);

            const int count = ArgInt(c->args, "lines", 50, 1, 500);
            Json lines = Json::array();
            const char* base = g.DebugLogBuf.c_str();
            for (int n = std::max(0, g.DebugLogIndex.size() - count); n < g.DebugLogIndex.size(); n++)
            {
                std::string line(g.DebugLogIndex.get_line_begin(base, n), g.DebugLogIndex.get_line_end(base, n));
                if (!line.empty() && line.back() == '\n')
                    line.pop_back();
                lines.push_back(std::move(line));
            }

            Json enabled = Json::array();
            for (const LogEvent& e : kLogEvents)
                if (g.DebugLogFlags & e.flag)
                    enabled.push_back(e.name);

            Json out = Json::object();
            out["frame"] = g.FrameCount;
            out["enabled"] = std::move(enabled);
            out["lines"] = std::move(lines);

            if (ArgBool(c->args, "clear", false))
            {
                g.DebugLogBuf.clear();
                g.DebugLogIndex.clear();
            }
            c->Ok(std::move(out));
        }

        void ListCommands(const CallPtr& c)
        {
            Json list = Json::array();
            for (const auto& [name, command] : gCommands)
            {
                Json j = Json::object();
                j["name"] = name;
                j["description"] = command.description;
                list.push_back(std::move(j));
            }
            Json out = Json::object();
            out["commands"] = std::move(list);
            c->Ok(out);
        }

        void RunCommand(const CallPtr& c)
        {
            const std::string name = ArgStr(c->args, "name");
            auto it = gCommands.find(name);
            if (it == gCommands.end())
                return c->Fail("Unknown command '" + name + "'. Available: " + JoinKeys(gCommands, "(none registered with AGENT_COMMAND)"));
            auto args = c->args.find("args");
            const std::string result = it->second.fn(args != c->args.end() && args->is_object() ? Dump(*args) : "{}");

            Json parsed = Json::parse(result, nullptr, false);
            Json out = Json::object();
            out["ok"] = true;
            out["command"] = name;
            out["result"] = parsed.is_discarded() ? Json(result) : std::move(parsed);
            c->Ok(out);
        }

        void StepFrames(const CallPtr& c)
        {
            const int frames = ArgInt(c->args, "frames", 1, 1, 600);
            gFrameWaits.push_back({ImGui::GetFrameCount() + frames, c});
        }

        void Screenshot(const CallPtr& c)
        {
            ShotRequest req;
            req.call = c;
            req.annotate = ArgBool(c->args, "annotate", false);
            const bool targeted = c->args.contains("id") || c->args.contains("label") || c->args.contains("window");
            // A cropped widget is small, so default to the display's full resolution.
            req.options.scale = ArgNum(c->args, "scale", targeted ? ImGui::GetIO().DisplayFramebufferScale.x : 1.0f);
            auto region = c->args.find("region");
            if (targeted)
            {
                ImRect rect;
                Json target;
                const std::string error = ResolveRegion(LastSnapshot(), c->args, rect, target);
                if (!error.empty())
                    return c->Fail(error);
                const float padding = std::clamp(ArgNum(c->args, "padding", 8.0f), 0.0f, 200.0f);
                rect.Expand(padding);
                req.options.hasRegion = true;
                req.options.region = rect;
                req.prefix = Json::object();
                req.prefix["target"] = std::move(target);
                req.prefix["region"] = RectJson(rect);
            }
            else if (region != c->args.end() && region->is_object())
            {
                const float x = ArgNum(*region, "x", 0.0f), y = ArgNum(*region, "y", 0.0f);
                req.options.hasRegion = true;
                req.options.region = ImRect(x, y, x + ArgNum(*region, "w", 0.0f), y + ArgNum(*region, "h", 0.0f));
            }
            gShotQueue.push_back(std::move(req));
        }

        std::vector<Tool> BuildTools()
        {
            std::vector<Tool> t;
            auto add = [&](const char* name, const char* description, const char* schema, void (*run)(const CallPtr&)) {
                t.push_back(Tool{name, description, Json::parse(schema), run});
            };

            add("get_app_info",
                "ImGui version, frame number, FPS, display size, pixelsPerPoint, mouse position, snapshot counts and the names of exposed app state.",
                R"({"type":"object","properties":{}})", GetAppInfo);

            add("get_windows",
                "Active ImGui windows (including child windows, popups, tooltips) with id, name, kind, rect, z order, focus/hover state, plus the open popup stack. Rects are [x, y, w, h] in ImGui points.",
                R"({"type":"object","properties":{}})", GetWindows);

            add("get_ui_tree",
                "Widgets grouped by window from the last completed frame. Each item has id (8-digit hex ImGuiID), kind (item|checkable|openable|input), label when the widget reports one, rect [x,y,w,h], parent id, state flags (checked, open, hovered, active, focused, disabled, edited) and text: what the widget displays inside its rect (a slider's value, an input's content, a combo's selection), omitted when it equals the label. Use nested=true for a parent/child tree. Plain text outside widgets is in get_text.",
                R"({"type":"object","properties":{
                    "window":{"type":"string","description":"Only windows whose name contains this (case-insensitive)."},
                    "visible_only":{"type":"boolean","description":"Skip clipped/scrolled-out items. Default true."},
                    "labeled_only":{"type":"boolean","description":"Skip items without a label (scrollbars, resize grips, collapse buttons). Default true."},
                    "nested":{"type":"boolean","description":"Return each window's items as a tree: children are listed under their parent item. Default false (flat list with parent ids)."},
                    "limit":{"type":"integer","description":"Max items, default 300, max 2000."}}})",
                GetUiTree);

            add("find_items",
                "Search widgets by label substring, id, window, or kind. Returns a flat list with the owning window name.",
                R"({"type":"object","properties":{
                    "query":{"type":"string","description":"Label substring, case-insensitive."},
                    "id":{"type":"string","description":"Exact item id (hex)."},
                    "window":{"type":"string"},
                    "kind":{"type":"string","enum":["item","checkable","openable","input"]},
                    "visible_only":{"type":"boolean","description":"Default true."},
                    "limit":{"type":"integer","description":"Default 50, max 500."}}})",
                FindItems);

            add("get_text",
                "All text drawn in the last frame (labels, Text(), values shown inside sliders, etc.) with window, rect [x,y,w,h], color (#rrggbb, plus alpha when translucent), size (font px) and font name.",
                R"({"type":"object","properties":{
                    "query":{"type":"string","description":"Substring filter, case-insensitive."},
                    "window":{"type":"string"},
                    "visible_only":{"type":"boolean","description":"Default true."},
                    "limit":{"type":"integer","description":"Default 200, max 2000."}}})",
                GetText);

            add("get_hovered",
                "Currently hovered, active and keyboard-focused item, hovered/focused window, mouse position and WantCapture flags.",
                R"({"type":"object","properties":{}})", GetHovered);

            add("get_state",
                "Application state registered with AGENT_EXPOSE. Without a name returns all of it.",
                R"({"type":"object","properties":{"name":{"type":"string"}}})", GetState);

            add("get_log",
                "Tail of imgui's internal debug log (errors, plus any event categories you enable: focus, popup, nav, io, activeid, ...). Use enable/disable to toggle categories and clear to reset. Useful to see why a popup closed or which item took focus.",
                R"J({"type":"object","properties":{
                    "lines":{"type":"integer","description":"Last N lines, default 50, max 500."},
                    "enable":{"type":"array","items":{"type":"string"},"description":"Event categories to start logging."},
                    "disable":{"type":"array","items":{"type":"string"}},
                    "clear":{"type":"boolean","description":"Clear the log after reading."}}})J",
                GetLog);

            add("list_commands",
                "App commands registered with AGENT_COMMAND: functions of the application itself (switch theme, load test data, reset state) that are quicker than clicking through the UI.",
                R"({"type":"object","properties":{}})", ListCommands);

            add("run_command",
                "Run an app command by name with JSON args. See list_commands for names and what args each takes. Runs between frames; the UI reflects the result on the next frame.",
                R"J({"type":"object","properties":{"name":{"type":"string"},"args":{"type":"object","description":"Command-specific arguments."}},"required":["name"]})J",
                RunCommand);

            add("step_frames",
                "Wait for N rendered frames, then return the current frame number. Useful for animations and delayed UI.",
                R"({"type":"object","properties":{"frames":{"type":"integer","description":"1-600, default 1."}}})",
                StepFrames);

            add("screenshot",
                "PNG of the window. By default 1 image pixel = 1 ImGui point, so image coordinates match rects from the other tools. The reply includes the image origin and scale. "
                "Pass id, label (+window) or window to capture just that widget or window, with some padding and at full display resolution.",
                R"({"type":"object","properties":{
                    "id":{"type":"string","description":"Capture this item (hex id)."},
                    "label":{"type":"string","description":"Capture the item with this label (same matching as click). Ambiguous labels are an error."},
                    "window":{"type":"string","description":"With label: the window to look in. Alone: capture the whole window (popups and tooltips are windows too)."},
                    "padding":{"type":"number","description":"Points added around the target, default 8, max 200."},
                    "scale":{"type":"number","description":"Output pixels per ImGui point (default 1, or the display scale for a target; max = display scale)."},
                    "region":{"type":"object","description":"Crop in ImGui points.","properties":{"x":{"type":"number"},"y":{"type":"number"},"w":{"type":"number"},"h":{"type":"number"}}},
                    "annotate":{"type":"boolean","description":"Draw item ids over visible widgets. Default false."}}})",
                Screenshot);

            AppendActionTools(t);
            AppendBatchTools(t);
            AppendLintTools(t);
            AppendMetricsTools(t);
            return t;
        }
    } // namespace

    const std::vector<Tool>& Tools()
    {
        static const std::vector<Tool> tools = BuildTools();
        return tools;
    }

    const Tool* FindTool(const std::string& name)
    {
        for (const Tool& tool : Tools())
            if (tool.name == name)
                return &tool;
        return nullptr;
    }

    void Expose(const char* name, std::function<std::string()> jsonProvider) { gProviders[name] = std::move(jsonProvider); }

    void RegisterCommand(const char* name, const char* description, CommandFn fn) { gCommands[name] = {description, std::move(fn)}; }

    void QueueScreenshot(const CallPtr& call, const ShotOptions& options, Json prefix)
    {
        ShotRequest req;
        req.call = call;
        req.options = options;
        req.prefix = std::move(prefix);
        req.holdsInput = true; // Hover states and tooltips would vanish if the pointer went back to the user first.
        Actions::HoldForCapture(1);
        gShotQueue.push_back(std::move(req));
    }

    bool HasPendingTools() { return !gFrameWaits.empty() || !gShotQueue.empty() || gArmed.has_value(); }

    void TickPending()
    {
        TickWaits();

        // Enabled event categories can be chatty; keep the log bounded.
        ImGuiContext& g = *ImGui::GetCurrentContext();
        if (g.DebugLogBuf.size() > kMaxLogBytes)
        {
            g.DebugLogBuf.clear();
            g.DebugLogIndex.clear();
        }

        const int frame = ImGui::GetFrameCount();
        for (size_t i = 0; i < gFrameWaits.size();)
        {
            if (gFrameWaits[i].call->Cancelled())
            {
                gFrameWaits.erase(gFrameWaits.begin() + i);
                continue;
            }
            if (frame < gFrameWaits[i].target)
            {
                i++;
                continue;
            }
            Json j = Json::object();
            j["frame"] = frame;
            gFrameWaits[i].call->Ok(j);
            gFrameWaits.erase(gFrameWaits.begin() + i);
        }

        if (gArmed && gArmed->call->Cancelled())
        {
            ReleaseInput(*gArmed);
            gArmed.reset();
        }
        while (!gArmed && !gShotQueue.empty())
        {
            ShotRequest next = std::move(gShotQueue.front());
            gShotQueue.pop_front();
            if (!next.call->Cancelled())
                gArmed = std::move(next);
            else
                ReleaseInput(next);
        }
    }

    void ToolsEndFrame()
    {
        if (!gArmed || !gArmed->annotate)
            return;

        const Snapshot& s = LastSnapshot();
        ImDrawList* draw = ImGui::GetForegroundDrawList();
        for (const ItemRec& r : s.items)
        {
            if (!r.visible || r.isWindow || r.isTitleBar || (r.label < 0 && !Interactive(r)))
                continue;
            const std::string text = Hex(r.id);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            draw->AddRect(r.rect.Min, r.rect.Max, IM_COL32(255, 80, 0, 255));
            draw->AddRectFilled(r.rect.Min, r.rect.Min + size, IM_COL32(0, 0, 0, 210));
            draw->AddText(r.rect.Min, IM_COL32(255, 230, 0, 255), text.c_str());
        }
    }

    void ToolsAfterDraw()
    {
        if (!gArmed)
            return;
        ShotRequest req = std::move(*gArmed);
        gArmed.reset();
        ReleaseInput(req);
        if (req.call->Cancelled())
            return;

        Capture capture;
        if (!CaptureFramebuffer(capture))
        {
            req.call->Fail("Framebuffer capture failed (is the window minimized?)");
            return;
        }
        req.call->Lazy([capture, options = req.options, prefix = std::move(req.prefix)] {
            Json result = EncodeScreenshot(capture, options);
            if (!prefix.is_null() && !result["isError"].get<bool>())
            {
                Json& content = result["content"];
                content.insert(content.begin(), Call::TextItem(Dump(prefix)));
            }
            return result;
        });
    }

    void FailPending(const std::string& reason)
    {
        FailWaits(reason);
        for (FrameWait& w : gFrameWaits)
            w.call->Fail(reason);
        gFrameWaits.clear();
        for (ShotRequest& r : gShotQueue)
        {
            r.call->Fail(reason);
            ReleaseInput(r);
        }
        gShotQueue.clear();
        if (gArmed)
        {
            gArmed->call->Fail(reason);
            ReleaseInput(*gArmed);
        }
        gArmed.reset();
    }
} // namespace Agent

#endif // AGENT_BRIDGE
