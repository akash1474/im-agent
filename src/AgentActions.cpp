#ifdef AGENT_BRIDGE

#include "AgentInternal.h"
#include "AgentUtil.h"

#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace Agent
{
    namespace
    {
        constexpr float kVisibleSpeed = 1200.0f; // Cursor pixels per second in "visible" mode.
        constexpr float kMaxScrollTicks = 200.0f;  // Each tick takes a frame in visible mode; bounds how long input stays taken over.
        constexpr size_t kMaxTypedChars = 2000;
        constexpr size_t kMaxTypingFrames = 240;

        // ---- Targets ----

        struct Target
        {
            enum class Kind { None, Item, Window, Point };
            Kind kind = Kind::None;
            ImGuiID id = 0;
            ImVec2 point;
        };

        struct Selector
        {
            std::string label, window, kind;
            ImGuiID id = 0;
            bool hasId = false;
        };

        Selector ReadSelector(const Json& a, const std::string& prefix)
        {
            Selector sel;
            sel.label = ArgStr(a, (prefix + "label").c_str());
            sel.window = ArgStr(a, (prefix + "window").c_str());
            if (sel.window.empty())
                sel.window = ArgStr(a, "window");
            sel.kind = ArgStr(a, (prefix + "kind").c_str());
            sel.hasId = ParseId(ArgStr(a, (prefix + "id").c_str()), sel.id);
            return sel;
        }

        // Items matching the label selector: exact (case-sensitive) matches if any, else substring matches.
        std::vector<const ItemRec*> MatchLabel(const Snapshot& s, const Selector& sel)
        {
            std::vector<const ItemRec*> exact, loose;
            for (const ItemRec& r : s.items)
            {
                if (r.isWindow || r.isTitleBar || r.label < 0)
                    continue;
                if (!sel.kind.empty() && sel.kind != KindOf(r))
                    continue;
                if (!ContainsI(WindowName(s, r.windowId), sel.window))
                    continue;
                const char* full = s.Str(r.label);
                const std::string shown = DisplayLabel(full);
                if (shown == sel.label || sel.label == full)
                    exact.push_back(&r);
                else if (ContainsI(shown.c_str(), sel.label))
                    loose.push_back(&r);
            }
            return exact.empty() ? loose : exact;
        }

        // ---- Suggestions for targets that were not found ----

        // Levenshtein distance; returns limit + 1 as soon as the result is certain to exceed the limit.
        int EditDistance(const std::string& a, const std::string& b, int limit)
        {
            if (std::abs((int)a.size() - (int)b.size()) > limit)
                return limit + 1;
            std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
            for (size_t j = 0; j <= b.size(); j++)
                prev[j] = (int)j;
            for (size_t i = 1; i <= a.size(); i++)
            {
                cur[0] = (int)i;
                int rowMin = cur[0];
                for (size_t j = 1; j <= b.size(); j++)
                {
                    cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (a[i - 1] == b[j - 1] ? 0 : 1)});
                    rowMin = std::min(rowMin, cur[j]);
                }
                if (rowMin > limit)
                    return limit + 1;
                std::swap(prev, cur);
            }
            return prev[b.size()];
        }

        // Up to three names close to `wanted`: containing it (or contained in it), else within a small edit distance. Best first.
        std::vector<std::string> ClosestNames(const std::string& wanted, const std::vector<std::string>& names)
        {
            const std::string w = Lower(wanted);
            const int limit = std::max(2, (int)w.size() / 3);
            std::vector<std::pair<int, std::string>> scored;
            std::set<std::string> seen;
            for (const std::string& name : names)
            {
                if (name.empty() || !seen.insert(name).second)
                    continue;
                const std::string n = Lower(name);
                const int lengthGap = std::abs((int)n.size() - (int)w.size());
                if (n.find(w) != std::string::npos || (n.size() >= 3 && w.find(n) != std::string::npos))
                    scored.push_back({lengthGap, name});
                else if (const int d = EditDistance(w, n, limit); d <= limit)
                    scored.push_back({100 + d, name});
            }
            std::stable_sort(scored.begin(), scored.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
            std::vector<std::string> out;
            for (size_t i = 0; i < scored.size() && i < 3; i++)
                out.push_back(scored[i].second);
            return out;
        }

        // " Did you mean: \"A\" in \"Window\", ...?" or "" when nothing is close. Looks in the requested window first, then everywhere.
        std::string SuggestLabels(const Snapshot& s, const Selector& sel)
        {
            for (const bool everywhere : {false, true})
            {
                std::vector<std::string> names;
                std::map<std::string, std::string> windowOf;
                for (const ItemRec& r : s.items)
                {
                    if (r.isWindow || r.isTitleBar || r.label < 0 || r.decoration || (!everywhere && !ContainsI(WindowName(s, r.windowId), sel.window)))
                        continue;
                    const std::string name = DisplayLabel(s.Str(r.label));
                    if (windowOf.emplace(name, WindowName(s, r.windowId)).second)
                        names.push_back(name);
                }
                const std::vector<std::string> close = ClosestNames(sel.label, names);
                if (close.empty())
                    continue;
                std::string msg = " Did you mean: ";
                for (size_t i = 0; i < close.size(); i++)
                    msg += (i ? ", " : "") + std::string("\"") + close[i] + "\" in \"" + windowOf[close[i]] + "\"";
                return msg + "?";
            }
            return "";
        }

        std::string SuggestWindows(const Snapshot& s, const std::string& wanted)
        {
            std::vector<std::string> names;
            for (const WindowRec& w : s.windows)
                names.push_back(s.Str(w.name));
            const std::vector<std::string> close = ClosestNames(wanted, names);
            if (close.empty())
                return "";
            std::string msg = ". Did you mean: ";
            for (size_t i = 0; i < close.size(); i++)
                msg += (i ? ", " : "") + std::string("\"") + close[i] + "\"";
            return msg + "?";
        }

        std::string Ambiguous(const Snapshot& s, const Selector& sel, const std::vector<const ItemRec*>& matches)
        {
            std::string msg = "Label '" + sel.label + "' matches " + std::to_string(matches.size()) + " items; pass id (or window) to pick one:";
            for (size_t i = 0; i < matches.size() && i < 6; i++)
            {
                const ItemRec& r = *matches[i];
                msg += "\n  " + Hex(r.id) + " \"" + DisplayLabel(s.Str(r.label)) + "\" in \"" + WindowName(s, r.windowId) + "\"" + (r.visible ? "" : " (not visible)");
            }
            return msg;
        }

        // Returns an error message, or "" on success. kind stays None when the args carry no target.
        std::string ResolveTarget(const Snapshot& s, const Json& a, const std::string& prefix, Target& out)
        {
            out = Target{};
            const Selector sel = ReadSelector(a, prefix);

            if (sel.hasId)
            {
                if (FindItem(s, sel.id))
                    out.kind = Target::Kind::Item;
                else if (FindWindow(s, sel.id))
                    out.kind = Target::Kind::Window;
                else
                    return "No item or window with id " + Hex(sel.id) + " in the last frame";
                out.id = sel.id;
                return "";
            }

            if (!sel.label.empty())
            {
                std::vector<const ItemRec*> matches = MatchLabel(s, sel);
                if (matches.empty())
                    return "No item with label '" + sel.label + "'" + (sel.window.empty() ? "" : " in window '" + sel.window + "'") + "." + SuggestLabels(s, sel) + " Use find_items to search. Items skipped by a list clipper or inside a collapsed section are not recorded: scroll the area or expand the section first.";
                if (matches.size() > 1)
                {
                    std::vector<const ItemRec*> visible;
                    for (const ItemRec* m : matches)
                        if (m->visible)
                            visible.push_back(m);
                    if (visible.size() != 1)
                        return Ambiguous(s, sel, matches);
                    matches = visible;
                }
                out.kind = Target::Kind::Item;
                out.id = matches[0]->id;
                return "";
            }

            if (HasNum(a, (prefix + "x").c_str()) && HasNum(a, (prefix + "y").c_str()))
            {
                out.kind = Target::Kind::Point;
                out.point = ImVec2(ArgNum(a, (prefix + "x").c_str(), 0.0f), ArgNum(a, (prefix + "y").c_str(), 0.0f));
                return "";
            }

            const std::string windowName = ArgStr(a, (prefix + "window").c_str()); // Only an explicit window means "the window itself".
            if (!windowName.empty())
            {
                std::vector<const WindowRec*> exact, loose;
                for (const WindowRec& w : s.windows)
                {
                    if (windowName == s.Str(w.name))
                        exact.push_back(&w);
                    else if (ContainsI(s.Str(w.name), windowName))
                        loose.push_back(&w);
                }
                const auto& found = exact.empty() ? loose : exact;
                if (found.empty())
                    return "No window named '" + windowName + "'" + SuggestWindows(s, windowName);
                if (found.size() > 1)
                    return "Window name '" + windowName + "' is ambiguous (" + std::to_string(found.size()) + " matches)";
                out.kind = Target::Kind::Window;
                out.id = found[0]->id;
            }
            return "";
        }

        bool TargetPoint(const Snapshot& s, const Target& t, ImVec2& out)
        {
            switch (t.kind)
            {
            case Target::Kind::Point:
                out = t.point;
                return true;
            case Target::Kind::Item:
            {
                const ItemRec* r = FindItem(s, t.id);
                if (!r)
                    return false;
                const ImRect& box = r->visible && r->clipped.GetArea() > 0.0f ? r->clipped : r->rect;
                out = box.GetCenter();
                return true;
            }
            case Target::Kind::Window:
            {
                const WindowRec* w = FindWindow(s, t.id);
                if (!w)
                    return false;
                out = w->rect.GetCenter();
                return true;
            }
            default:
                return false;
            }
        }

        Json DescribeTarget(const Snapshot& s, const Target& t)
        {
            Json j = Json::object();
            switch (t.kind)
            {
            case Target::Kind::Item:
                return DescribeId(s, t.id);
            case Target::Kind::Window:
                j["window"] = WindowName(s, t.id);
                j["id"] = Hex(t.id);
                return j;
            case Target::Kind::Point:
                j["x"] = R(t.point.x);
                j["y"] = R(t.point.y);
                return j;
            default:
                return nullptr;
            }
        }

        // ---- Pace ----

        struct Pace
        {
            bool visible = true;
            int pause = 6;  // Frames to rest on the target before pressing.
            int settle = 12; // Frames to wait after the action.
        };

        std::string ReadPace(const Json& a, int settleVisible, int settleInstant, Pace& pace)
        {
            const std::string speed = ArgStr(a, "speed");
            if (speed == "instant")
            {
                pace.visible = false;
                pace.pause = 1;
            }
            else if (!speed.empty() && speed != "visible")
            {
                return "speed must be 'visible' or 'instant'";
            }
            pace.settle = ArgInt(a, "settle_frames", pace.visible ? settleVisible : settleInstant, 0, 600);
            return "";
        }

        // ---- Steps ----

        void AddWait(Action& a, int frames)
        {
            if (frames > 0)
                a.steps.push_back([frames](ActionCtx& c) { return c.frame + 1 >= frames; });
        }

        void AddStep(Action& a, std::function<void()> fn)
        {
            a.steps.push_back([fn = std::move(fn)](ActionCtx&) {
                fn();
                return true;
            });
        }

        void ScrollIntoView(const ItemRec& r)
        {
            ImGuiWindow* window = ImGui::FindWindowByID(r.windowId);
            if (!window)
                return;
            ImGui::SetScrollFromPosY(window, r.rect.GetCenter().y - window->Pos.y, 0.5f);
            ImGui::SetScrollFromPosX(window, r.rect.GetCenter().x - window->Pos.x, 0.5f);
            for (ImGuiWindow* child = window; (child->Flags & ImGuiWindowFlags_ChildWindow) && child->ParentWindow;)
            {
                ImGuiWindow* parent = child->ParentWindow;
                if (!parent->InnerClipRect.Contains(child->Rect()))
                {
                    ImGui::SetScrollFromPosY(parent, child->Rect().GetCenter().y - parent->Pos.y, 0.5f);
                    ImGui::SetScrollFromPosX(parent, child->Rect().GetCenter().x - parent->Pos.x, 0.5f);
                }
                child = parent;
            }
        }

        // Scrolls the target's window(s) until at least half of it is visible. The snapshot lags two frames, hence the 3-frame cadence.
        void AddReveal(Action& a, Target t)
        {
            if (t.kind != Target::Kind::Item)
                return;
            a.steps.push_back([t](ActionCtx& c) {
                const ItemRec* r = FindItem(c.snap, t.id);
                if (!r)
                {
                    c.Fail("The target disappeared before it could be reached");
                    return true;
                }
                if (r->visible && r->clipped.GetArea() >= r->rect.GetArea() * 0.5f)
                    return true;
                if (c.frame >= 12)
                {
                    if (!r->visible)
                        c.Fail("Could not scroll the target into view (is it inside a scroll area that cannot reach it?)");
                    else
                        c.action.warnings.push_back("Could not scroll the target fully into view");
                    return true;
                }
                if (c.frame % 3 == 0)
                    ScrollIntoView(*r);
                return false;
            });
        }

        // Moves the cursor to the target (re-resolved every frame) taking at least minFrames frames.
        void AddMoveTo(Action& a, Target t, bool visible, int minFrames = 1)
        {
            a.steps.push_back([t, visible, minFrames](ActionCtx& c) {
                ImVec2 dest;
                if (!TargetPoint(c.snap, t, dest))
                {
                    c.Fail("The target disappeared while moving the mouse to it");
                    return true;
                }
                const ImVec2 cur = Input::MousePos();
                const ImVec2 delta = dest - cur;
                const float dist = std::sqrt(delta.x * delta.x + delta.y * delta.y);
                if (dist < 0.5f)
                    return true;

                float step = dist / (float)std::max(1, minFrames - c.frame);
                if (visible)
                    step = std::min(step, kVisibleSpeed * std::clamp(ImGui::GetIO().DeltaTime, 0.001f, 0.05f));
                if (step >= dist)
                {
                    Input::Move(dest);
                    return true;
                }
                Input::Move(cur + delta * (step / dist));
                return false;
            });
        }

        void AddPress(Action& a, int button, Target t)
        {
            a.steps.push_back([button, t](ActionCtx& c) {
                if (t.kind == Target::Kind::Item)
                {
                    const bool hit = c.snap.hoveredId == t.id;
                    c.action.result["hit"] = hit;
                    if (!hit)
                    {
                        const ItemRec* target = FindItem(c.snap, t.id);
                        const bool otherWindow = c.snap.hoveredWindowId && target && c.snap.hoveredWindowId != target->windowId;
                        if (c.snap.hoveredId || otherWindow) // Pressing now would act on the wrong widget.
                        {
                            const std::string cover = c.snap.hoveredId ? Dump(DescribeId(c.snap, c.snap.hoveredId)) : "window '" + std::string(WindowName(c.snap, c.snap.hoveredWindowId)) + "'";
                            c.Fail("Another item is on top of the target: " + cover + ". Use focus_window or close what covers it.");
                            return true;
                        }
                        c.action.warnings.push_back("Nothing was hovered when the button went down; the target may be disabled or the click point missed it");
                    }
                }
                Input::Button(button, true);
                return true;
            });
        }

        void AddClick(Action& a, Target t, const Pace& pace, int button, int count)
        {
            AddReveal(a, t);
            AddMoveTo(a, t, pace.visible);
            AddWait(a, pace.pause);
            for (int i = 0; i < count; i++)
            {
                AddPress(a, button, t);
                AddWait(a, 1);
                AddStep(a, [button] { Input::Button(button, false); });
                if (i + 1 < count)
                    AddWait(a, 1);
            }
        }

        // ---- Keys ----

        struct Chord
        {
            ImGuiKey key = ImGuiKey_None;
            std::vector<ImGuiKey> mods; // ImGuiMod_* values.
        };

        ImGuiKey LeftVariant(ImGuiKey mod)
        {
            switch (mod)
            {
            case ImGuiMod_Ctrl: return ImGuiKey_LeftCtrl;
            case ImGuiMod_Shift: return ImGuiKey_LeftShift;
            case ImGuiMod_Alt: return ImGuiKey_LeftAlt;
            default: return ImGuiKey_LeftSuper;
            }
        }

        bool ParseMod(const std::string& name, ImGuiKey& out)
        {
            const std::string n = Lower(name);
            if (n == "ctrl" || n == "control")
                out = ImGuiMod_Ctrl;
            else if (n == "shift")
                out = ImGuiMod_Shift;
            else if (n == "alt" || n == "option")
                out = ImGuiMod_Alt;
            else if (n == "super" || n == "cmd" || n == "command" || n == "meta" || n == "win")
                out = ImGuiMod_Super;
            else if (n == "shortcut") // The platform's usual shortcut modifier.
                out = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
            else
                return false;
            return true;
        }

        bool ParseKeyName(const std::string& name, ImGuiKey& out)
        {
            static const char* kAliases[][2] = {{"esc", "escape"}, {"return", "enter"}, {"left", "leftarrow"}, {"right", "rightarrow"}, {"up", "uparrow"},
                                                {"down", "downarrow"}, {"del", "delete"}, {"pgup", "pageup"}, {"pgdn", "pagedown"}, {"ins", "insert"}};
            std::string wanted = Lower(name);
            for (const auto& alias : kAliases)
                if (wanted == alias[0])
                    wanted = alias[1];
            for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; k++)
            {
                if (ImGui::IsMouseKey((ImGuiKey)k))
                    continue;
                if (Lower(ImGui::GetKeyName((ImGuiKey)k)) == wanted)
                {
                    out = (ImGuiKey)k;
                    return true;
                }
            }
            return false;
        }

        // Accepts "Enter", "ctrl+s", "shortcut+shift+z".
        std::string ParseChord(const std::string& text, Chord& out)
        {
            std::vector<std::string> parts;
            size_t start = 0;
            for (size_t i = 1; i <= text.size(); i++) // i starts at 1 so a lone "+" is the key itself.
            {
                if (i == text.size() || (text[i] == '+' && i + 1 < text.size()))
                {
                    parts.push_back(text.substr(start, i - start));
                    start = i + 1;
                }
            }
            if (parts.empty() || parts.back().empty())
                return "Empty key";
            if (!ParseKeyName(parts.back(), out.key))
                return "Unknown key '" + parts.back() + "'. Use names like Enter, Escape, Tab, Backspace, Delete, Space, LeftArrow, A, F5.";
            for (size_t i = 0; i + 1 < parts.size(); i++)
            {
                ImGuiKey mod;
                if (!ParseMod(parts[i], mod))
                    return "Unknown modifier '" + parts[i] + "' (ctrl, shift, alt, super, shortcut)";
                out.mods.push_back(mod);
            }
            return "";
        }

        void AddChordDown(Action& a, Chord chord)
        {
            AddStep(a, [chord] {
                for (ImGuiKey mod : chord.mods)
                {
                    Input::Key(mod, true);
                    Input::Key(LeftVariant(mod), true);
                }
                Input::Key(chord.key, true);
            });
        }

        void AddChordUp(Action& a, Chord chord)
        {
            AddStep(a, [chord] {
                Input::Key(chord.key, false);
                for (auto it = chord.mods.rbegin(); it != chord.mods.rend(); ++it)
                {
                    Input::Key(LeftVariant(*it), false);
                    Input::Key(*it, false);
                }
            });
        }

        void AddTap(Action& a, const Chord& chord)
        {
            AddChordDown(a, chord);
            AddWait(a, 1);
            AddChordUp(a, chord);
            AddWait(a, 1);
        }

        std::vector<std::string> SplitUtf8(const std::string& text)
        {
            std::vector<std::string> chars;
            for (size_t i = 0; i < text.size();)
            {
                const unsigned char lead = (unsigned char)text[i];
                size_t n = lead < 0x80 ? 1 : (lead >> 5) == 0x6 ? 2 : (lead >> 4) == 0xE ? 3 : (lead >> 3) == 0x1E ? 4 : 1;
                n = std::min(n, text.size() - i);
                chars.push_back(text.substr(i, n));
                i += n;
            }
            return chars;
        }

        // ---- Tools ----

        // Common setup shared by the action tools.
        void PrepareAction(const CallPtr& c, const char* name, Action& action)
        {
            action.call = c;
            action.screenshot = ArgBool(c->args, "screenshot", false);
            action.result["action"] = name;
        }

        bool NeedTarget(const CallPtr& c, Target& t, const char* what)
        {
            const std::string error = ResolveTarget(LastSnapshot(), c->args, "", t);
            if (!error.empty())
            {
                c->Fail(error);
                return false;
            }
            if (t.kind == Target::Kind::None)
            {
                c->Fail(std::string("Specify what to ") + what + ": id, label (optionally with window), or x and y.");
                return false;
            }
            return true;
        }

        void WarnIfDisabled(const Snapshot& s, const Target& t, Action& a)
        {
            if (t.kind != Target::Kind::Item)
                return;
            const ItemRec* r = FindItem(s, t.id);
            if (r && (r->itemFlags & ImGuiItemFlags_Disabled))
                a.warnings.push_back("The target is disabled; imgui ignores clicks on it");
        }

        void Click(const CallPtr& c)
        {
            Target t;
            if (!NeedTarget(c, t, "click"))
                return;
            Pace pace;
            std::string error = ReadPace(c->args, 12, 3, pace);
            const std::string button = ArgStr(c->args, "button");
            int buttonIndex = 0;
            if (button == "right")
                buttonIndex = 1;
            else if (button == "middle")
                buttonIndex = 2;
            else if (!button.empty() && button != "left")
                error = "button must be left, right or middle";
            if (!error.empty())
                return c->Fail(error);

            const Snapshot& s = LastSnapshot();
            const std::string ensure = ArgStr(c->args, "ensure");
            bool wantOpenState = false, wantOn = false;
            ImGuiID ensureId = 0;
            if (!ensure.empty())
            {
                wantOpenState = ensure == "open" || ensure == "closed";
                wantOn = ensure == "open" || ensure == "checked";
                if (!wantOpenState && ensure != "checked" && ensure != "unchecked")
                    return c->Fail("ensure must be open, closed, checked or unchecked");
                const ItemRec* item = t.kind == Target::Kind::Item ? FindItem(s, t.id) : nullptr;
                if (!item)
                    return c->Fail("ensure needs an item target (id or label)");
                if (wantOpenState && !(item->status & ImGuiItemStatusFlags_Openable))
                    return c->Fail("ensure=open/closed needs an openable item (tree node, header, menu)");
                if (!wantOpenState && !(item->status & ImGuiItemStatusFlags_Checkable))
                    return c->Fail("ensure=checked/unchecked needs a checkbox or checkable menu item (radio buttons and selectables do not report their state; click them normally)");
                const bool isOn = item->status & (wantOpenState ? ImGuiItemStatusFlags_Opened : ImGuiItemStatusFlags_Checked);
                if (isOn == wantOn)
                {
                    Json done = Json::object();
                    done["ok"] = true;
                    done["skipped"] = "already " + ensure;
                    return c->Ok(done);
                }
                ensureId = t.id;
            }

            Action a;
            PrepareAction(c, "click", a);
            a.result["target"] = DescribeTarget(s, t);
            WarnIfDisabled(s, t, a);
            AddClick(a, t, pace, buttonIndex, ArgBool(c->args, "double", false) ? 2 : 1);
            AddWait(a, pace.settle);
            if (ensureId)
            {
                a.steps.push_back([=](ActionCtx& ctx) {
                    const ItemRec* r = FindItem(ctx.snap, ensureId);
                    const bool isOn = r && (r->status & (wantOpenState ? ImGuiItemStatusFlags_Opened : ImGuiItemStatusFlags_Checked));
                    if (r && isOn != wantOn)
                        ctx.action.warnings.push_back("The item is still not " + ensure + " after the click");
                    return true;
                });
            }
            Actions::Enqueue(std::move(a));
        }

        void Hover(const CallPtr& c)
        {
            Target t;
            if (!NeedTarget(c, t, "hover"))
                return;
            Pace pace;
            const std::string error = ReadPace(c->args, 30, 30, pace); // Tooltips need ~0.5 s of hovering.
            if (!error.empty())
                return c->Fail(error);

            Action a;
            PrepareAction(c, "hover", a);
            a.result["target"] = DescribeTarget(LastSnapshot(), t);
            AddReveal(a, t);
            AddMoveTo(a, t, pace.visible);
            AddWait(a, pace.settle);
            Actions::Enqueue(std::move(a));
        }

        void Drag(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            Target from, to;
            if (!NeedTarget(c, from, "drag from"))
                return;
            std::string error = ResolveTarget(s, c->args, "to_", to);
            Pace pace;
            if (error.empty())
                error = ReadPace(c->args, 12, 3, pace);
            if (!error.empty())
                return c->Fail(error);

            if (to.kind == Target::Kind::None)
            {
                ImVec2 start;
                if (!HasNum(c->args, "dx") && !HasNum(c->args, "dy"))
                    return c->Fail("Specify the drop target: to_id, to_label, to_x and to_y, or an offset dx/dy.");
                TargetPoint(s, from, start);
                to.kind = Target::Kind::Point;
                to.point = start + ImVec2(ArgNum(c->args, "dx", 0.0f), ArgNum(c->args, "dy", 0.0f));
            }
            else if (to.kind == Target::Kind::Item)
            {
                const ItemRec* r = FindItem(s, to.id);
                if (r && !r->visible)
                    return c->Fail("The drop target is not visible; scroll it into view first");
            }

            Action a;
            PrepareAction(c, "drag", a);
            a.result["from"] = DescribeTarget(s, from);
            a.result["to"] = DescribeTarget(s, to);
            WarnIfDisabled(s, from, a);
            AddReveal(a, from);
            AddMoveTo(a, from, pace.visible);
            AddWait(a, pace.pause);
            AddPress(a, 0, from);
            AddWait(a, 2);
            AddMoveTo(a, to, pace.visible, pace.visible ? 8 : 4); // imgui needs several moves past the drag threshold.
            AddWait(a, pace.visible ? 4 : 2);
            AddStep(a, [] { Input::Button(0, false); });
            AddWait(a, pace.settle);
            Actions::Enqueue(std::move(a));
        }

        void Scroll(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            Target t;
            std::string error = ResolveTarget(s, c->args, "", t);
            Pace pace;
            if (error.empty())
                error = ReadPace(c->args, 12, 3, pace);
            if (!error.empty())
                return c->Fail(error);

            if (t.kind == Target::Kind::None) // Default to whatever window the mouse or keyboard focus is in.
            {
                const ImGuiID id = s.hoveredWindowId ? s.hoveredWindowId : s.navWindowId;
                if (!FindWindow(s, id))
                    return c->Fail("Specify what to scroll: window, id, label, or x and y.");
                t.kind = Target::Kind::Window;
                t.id = id;
            }
            const float dx = ArgNum(c->args, "dx", 0.0f), dy = ArgNum(c->args, "dy", 0.0f);
            if (t.kind == Target::Kind::Item) // A scrolled-out item names the scroll area; the wheel must go over the area, not over the item.
            {
                const ItemRec* item = FindItem(s, t.id);
                if (item && (!item->visible || item->clipped.GetArea() < item->rect.GetArea() * 0.5f))
                {
                    t.kind = Target::Kind::Window;
                    t.id = item->windowId;
                }
            }
            if (dx == 0.0f && dy == 0.0f)
                return c->Fail("Specify dy (positive scrolls down) and/or dx (positive scrolls right), in wheel ticks.");
            if (std::fabs(dx) > kMaxScrollTicks || std::fabs(dy) > kMaxScrollTicks)
                return c->Fail("dx/dy are wheel ticks; the maximum per call is " + std::to_string((int)kMaxScrollTicks) + ". Call scroll again for more.");

            Action a;
            PrepareAction(c, "scroll", a);
            a.trackChanges = false; // Scrolling changes which strings are drawn, which would only be noise.
            a.result["target"] = DescribeTarget(s, t);
            AddMoveTo(a, t, pace.visible);
            AddWait(a, pace.pause);
            const int ticks = pace.visible ? std::max(1, (int)std::ceil(std::max(std::fabs(dx), std::fabs(dy)))) : 1;
            a.steps.push_back([=](ActionCtx& ctx) {
                Input::Wheel(-dx / ticks, -dy / ticks); // imgui: positive wheel Y scrolls up, positive X scrolls left.
                return ctx.frame + 1 >= ticks;
            });
            AddWait(a, pace.settle);
            Actions::Enqueue(std::move(a));
        }

        void TypeText(const CallPtr& c)
        {
            const std::string text = ArgStr(c->args, "text");
            if (text.empty())
                return c->Fail("Specify text");
            std::vector<std::string> chars = SplitUtf8(text);
            if (chars.size() > kMaxTypedChars)
                return c->Fail("text is limited to " + std::to_string(kMaxTypedChars) + " characters per call; split it into several calls");
            const Snapshot& s = LastSnapshot();
            Target t;
            std::string error = ResolveTarget(s, c->args, "", t);
            Pace pace;
            if (error.empty())
                error = ReadPace(c->args, 6, 3, pace);
            if (!error.empty())
                return c->Fail(error);

            Action a;
            PrepareAction(c, "type_text", a);
            if (t.kind != Target::Kind::None)
            {
                a.result["target"] = DescribeTarget(s, t);
                WarnIfDisabled(s, t, a);
                AddClick(a, t, pace, 0, 1);
                AddWait(a, 2); // Let the text field become active.
            }
            if (ArgBool(c->args, "replace", false))
            {
                Chord selectAll;
                ParseChord("shortcut+a", selectAll);
                AddTap(a, selectAll);
            }

            // Visible mode spreads the text over at most kMaxTypingFrames frames so it can be followed without taking forever.
            const size_t perFrame = pace.visible ? (chars.size() + kMaxTypingFrames - 1) / kMaxTypingFrames : chars.size();
            std::vector<std::string> chunks;
            for (size_t i = 0; i < chars.size(); i += perFrame)
            {
                std::string chunk;
                for (size_t j = i; j < std::min(chars.size(), i + perFrame); j++)
                    chunk += chars[j];
                chunks.push_back(std::move(chunk));
            }
            a.steps.push_back([chunks](ActionCtx& ctx) {
                if (ctx.frame == 0 && !ctx.snap.wantTextInput)
                    ctx.action.warnings.push_back("No text field was active when typing started; the characters may have been ignored. Pass a target or click a field first");
                Input::Text(chunks[ctx.frame]);
                return ctx.frame + 1 >= (int)chunks.size();
            });
            AddWait(a, 1);

            if (ArgBool(c->args, "submit", false))
            {
                Chord enter;
                ParseChord("Enter", enter);
                AddTap(a, enter);
            }
            AddWait(a, pace.settle);
            Actions::Enqueue(std::move(a));
        }

        // Sets a text/number field, slider or drag to an exact value, or a checkbox to on/off.
        // Sliders and drags only accept typing after Ctrl+click, so numbers are entered with the shortcut modifier held.
        void SetValue(const CallPtr& c)
        {
            constexpr size_t kMaxValueChars = 256;
            auto valueIt = c->args.find("value");
            if (valueIt == c->args.end() || !(valueIt->is_number() || valueIt->is_string() || valueIt->is_boolean()))
                return c->Fail("Specify value: a number, string or boolean");
            const Snapshot& s = LastSnapshot();
            Target t;
            if (!NeedTarget(c, t, "set"))
                return;
            const ItemRec* item = t.kind == Target::Kind::Item ? FindItem(s, t.id) : nullptr;
            if (!item)
                return c->Fail("set_value needs a widget: pass id or label");
            Pace pace;
            const std::string error = ReadPace(c->args, 6, 3, pace);
            if (!error.empty())
                return c->Fail(error);

            const ImGuiID id = item->id;
            Action a;
            PrepareAction(c, "set_value", a);
            a.result["target"] = DescribeTarget(s, t);
            a.result["before"] = TextIndex(s).In(*item);
            WarnIfDisabled(s, t, a);

            if (valueIt->is_boolean())
            {
                const bool want = valueIt->get<bool>();
                if (!(item->status & ImGuiItemStatusFlags_Checkable))
                    return c->Fail("A boolean value needs a checkbox or checkable menu item; use click for other widgets");
                if (((item->status & ImGuiItemStatusFlags_Checked) != 0) == want)
                {
                    Json done = Json::object();
                    done["ok"] = true;
                    done["skipped"] = want ? "already checked" : "already unchecked";
                    return c->Ok(done);
                }
                AddClick(a, t, pace, 0, 1);
                AddWait(a, pace.settle);
                a.steps.push_back([id, want](ActionCtx& ctx) {
                    const ItemRec* r = FindItem(ctx.snap, id);
                    if (r && ((r->status & ImGuiItemStatusFlags_Checked) != 0) != want)
                        ctx.action.warnings.push_back(std::string("The item is still not ") + (want ? "checked" : "unchecked") + " after the click");
                    return true;
                });
                return Actions::Enqueue(std::move(a));
            }

            if (!(item->status & ImGuiItemStatusFlags_Inputable))
                return c->Fail(std::string("set_value needs a text or number field, slider, drag or checkbox; this widget is kind '") + KindOf(*item) + "', so use click");
            const bool numeric = valueIt->is_number();
            const std::string text = numeric ? Dump(*valueIt) : valueIt->get<std::string>();
            if (SplitUtf8(text).size() > kMaxValueChars)
                return c->Fail("value is limited to " + std::to_string(kMaxValueChars) + " characters; use type_text for longer text");
            const double wanted = numeric ? valueIt->get<double>() : 0.0;

            ImGuiKey shortcut;
            ParseMod("shortcut", shortcut);
            AddStep(a, [shortcut] {
                Input::Key(shortcut, true);
                Input::Key(LeftVariant(shortcut), true);
            });
            AddClick(a, t, pace, 0, 1);
            AddStep(a, [shortcut] {
                Input::Key(LeftVariant(shortcut), false);
                Input::Key(shortcut, false);
            });
            AddWait(a, 2); // Let the field become active.
            Chord selectAll;
            ParseChord("shortcut+a", selectAll);
            AddTap(a, selectAll);
            if (text.empty()) // Typing nothing would leave the selected text in place.
            {
                Chord erase;
                ParseChord("Backspace", erase);
                AddTap(a, erase);
            }
            a.steps.push_back([text](ActionCtx& ctx) {
                if (!ctx.snap.wantTextInput)
                    ctx.action.warnings.push_back("No text field was active when typing started; the value may not have been entered");
                Input::Text(text);
                return true;
            });
            AddWait(a, 1);
            if (numeric || ArgBool(c->args, "submit", false)) // Sliders and drags apply on Enter; text fields only when asked, as Enter can run app logic.
            {
                Chord enter;
                ParseChord("Enter", enter);
                AddTap(a, enter);
            }
            AddWait(a, pace.settle);
            a.steps.push_back([=](ActionCtx& ctx) {
                const ItemRec* r = FindItem(ctx.snap, id);
                if (!r)
                    return true;
                const std::string shown = TextIndex(ctx.snap).In(*r);
                ctx.action.result["value"] = shown;
                char* end = nullptr;
                const double got = std::strtod(shown.c_str(), &end);
                const bool mismatch = numeric ? end == shown.c_str() || !std::isfinite(got) || std::fabs(got - wanted) > 1e-3 * std::max(1.0, std::fabs(wanted)) : shown != text;
                if (mismatch)
                    ctx.action.warnings.push_back("The widget shows '" + shown + "' after setting '" + text + "'; it may clamp, round, truncate or mask the value");
                return true;
            });
            Actions::Enqueue(std::move(a));
        }

        void KeyTool(const CallPtr& c)
        {
            Chord chord;
            std::string error = ParseChord(ArgStr(c->args, "key"), chord);
            if (error.empty())
            {
                auto mods = c->args.find("mods");
                if (mods != c->args.end() && mods->is_array())
                {
                    for (const Json& m : *mods)
                    {
                        ImGuiKey mod;
                        if (!m.is_string() || !ParseMod(m.get<std::string>(), mod))
                        {
                            error = "Unknown modifier in mods (use ctrl, shift, alt, super, shortcut)";
                            break;
                        }
                        chord.mods.push_back(mod);
                    }
                }
            }
            Pace pace;
            if (error.empty())
                error = ReadPace(c->args, 6, 3, pace);
            if (!error.empty())
                return c->Fail(error);

            Action a;
            PrepareAction(c, "key", a);
            a.result["key"] = ArgStr(c->args, "key");
            const int repeat = ArgInt(c->args, "repeat", 1, 1, 100);
            for (int i = 0; i < repeat; i++)
                AddTap(a, chord);
            AddWait(a, pace.settle);
            Actions::Enqueue(std::move(a));
        }

        // ---- wait_for ----

        struct Waiter
        {
            CallPtr call;
            Selector selector;
            std::string text; // Lowercase; when set, waits on drawn text instead of an item.
            bool gone = false;
            int startFrame = 0;
            int timeoutFrames = 0;
        };
        std::vector<Waiter> gWaiters;

        const ItemRec* FindMatch(const Snapshot& s, const Selector& sel)
        {
            if (sel.hasId)
            {
                const ItemRec* r = FindItem(s, sel.id);
                return r && r->visible ? r : nullptr;
            }
            for (const ItemRec* r : MatchLabel(s, sel))
                if (r->visible)
                    return r;
            return nullptr;
        }

        bool TextPresent(const Snapshot& s, const std::string& needle, const std::string& window)
        {
            for (const TextRec& t : s.texts)
                if (t.visible && ContainsI(s.Str(t.str), needle) && ContainsI(WindowName(s, t.windowId), window))
                    return true;
            return false;
        }

        void WaitFor(const CallPtr& c)
        {
            Waiter w;
            w.call = c;
            w.selector = ReadSelector(c->args, "");
            w.text = ArgStr(c->args, "text");
            w.gone = ArgBool(c->args, "gone", false);
            w.timeoutFrames = ArgInt(c->args, "timeout_frames", 300, 1, 3600);
            w.startFrame = ImGui::GetFrameCount();
            if (w.text.empty() && !w.selector.hasId && w.selector.label.empty())
                return c->Fail("Specify what to wait for: text, id, or label.");
            gWaiters.push_back(std::move(w));
        }

        // ---- describe_item / focus_window ----

        // Labeled ancestors from the snapshot, innermost first (tree nodes, menus, ...).
        Json Ancestors(const Snapshot& s, const ItemRec& item)
        {
            Json chain = Json::array();
            ImGuiID child = item.id;
            ImGuiID parent = item.parentId;
            for (int depth = 0; parent && parent != child && depth < 16; depth++)
            {
                const ItemRec* p = FindItem(s, parent);
                if (!p)
                    break;
                Json j = Json::object();
                j["id"] = Hex(p->id);
                if (p->label >= 0)
                    j["label"] = s.Str(p->label);
                chain.push_back(std::move(j));
                child = parent;
                parent = p->parentId;
            }
            return chain;
        }

        // Readable ID stack of the item imgui's ID-stack query has just resolved (strings, ints, pointers that were hashed).
        void ReadIdPath(const ImGuiDebugItemPathQuery& query, const Snapshot& s, Json& out)
        {
            std::string path;
            Json levels = Json::array();
            for (int n = 0; n < query.Results.Size; n++)
            {
                const ImGuiStackLevelInfo& info = query.Results[n];
                std::string desc;
                if (info.DescOffset == -1 && n == 0)
                {
                    if (const ImGuiWindow* w = ImGui::FindWindowByID(info.ID))
                        desc = w->Name;
                }
                else if (info.QuerySuccess)
                {
                    desc = &query.ResultsDescBuf.Buf[info.DescOffset];
                }
                if (desc.empty())
                {
                    const ItemRec* r = FindItem(s, info.ID);
                    desc = r && r->label >= 0 ? s.Str(r->label) : "???";
                }

                Json level = Json::object();
                level["id"] = Hex(info.ID);
                level["desc"] = desc;
                levels.push_back(std::move(level));

                path += n == 0 ? "//" : "/";
                for (char ch : desc)
                {
                    if (ch == '/')
                        path += '\\';
                    path += ch;
                }
            }
            out["path"] = path;
            out["levels"] = std::move(levels);
        }

        void DescribeItem(const CallPtr& c)
        {
            Target t;
            if (!NeedTarget(c, t, "describe"))
                return;
            const Snapshot& s = LastSnapshot();
            const ItemRec* item = t.kind == Target::Kind::Item ? FindItem(s, t.id) : nullptr;
            if (!item)
                return c->Fail("describe_item needs an item: pass id or label");
            Pace pace;
            const std::string error = ReadPace(c->args, 1, 1, pace);
            if (!error.empty())
                return c->Fail(error);

            Action a;
            PrepareAction(c, "describe_item", a);
            a.result["target"] = DescribeTarget(s, t);
            a.result["ancestors"] = Ancestors(s, *item);
            AddReveal(a, t);
            AddMoveTo(a, t, pace.visible);
            // imgui's ID stack query follows the hovered item over several frames, one stack level per frame.
            a.steps.push_back([t](ActionCtx& ctx) {
                ImGuiContext& g = *ImGui::GetCurrentContext();
                g.DebugIDStackTool.LastActiveFrame = g.FrameCount;
                const ImGuiDebugItemPathQuery& query = g.DebugItemPathQuery;
                if (query.Complete && query.MainID == t.id)
                {
                    ReadIdPath(query, ctx.snap, ctx.action.result);
                    return true;
                }
                if (ctx.frame >= 90)
                {
                    ctx.action.warnings.push_back("Could not resolve the ID path; the item was probably not hovered (covered by another item or window)");
                    return true;
                }
                return false;
            });
            Actions::Enqueue(std::move(a));
        }

        void FocusWindow(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            Target t;
            const std::string error = ResolveTarget(s, c->args, "", t);
            if (!error.empty())
                return c->Fail(error);
            ImGuiID windowId = t.kind == Target::Kind::Window ? t.id : 0;
            if (const ItemRec* item = t.kind == Target::Kind::Item ? FindItem(s, t.id) : nullptr)
                windowId = item->windowId;
            if (!FindWindow(s, windowId))
                return c->Fail("Specify window (name), or an item id/label inside the window");

            Action a;
            PrepareAction(c, "focus_window", a);
            Target windowTarget;
            windowTarget.kind = Target::Kind::Window;
            windowTarget.id = windowId;
            a.result["target"] = DescribeTarget(s, windowTarget);
            AddStep(a, [windowId] {
                if (ImGuiWindow* w = ImGui::FindWindowByID(windowId))
                    ImGui::FocusWindow(w);
            });
            AddWait(a, 2);
            Actions::Enqueue(std::move(a));
        }
    } // namespace

    void TickWaits()
    {
        const Snapshot& s = LastSnapshot();
        const int frame = ImGui::GetFrameCount();
        for (size_t i = 0; i < gWaiters.size();)
        {
            Waiter& w = gWaiters[i];
            if (w.call->Cancelled())
            {
                gWaiters.erase(gWaiters.begin() + i);
                continue;
            }
            const ItemRec* item = w.text.empty() ? FindMatch(s, w.selector) : nullptr;
            const bool present = w.text.empty() ? item != nullptr : TextPresent(s, w.text, w.selector.window);

            if (present != w.gone)
            {
                Json j = Json::object();
                j["ok"] = true;
                j["frame"] = frame;
                j["waited_frames"] = frame - w.startFrame;
                if (item)
                    j["item"] = DescribeId(s, item->id);
                w.call->Ok(j);
            }
            else if (frame - w.startFrame >= w.timeoutFrames)
            {
                const std::string what = w.text.empty() ? (w.selector.hasId ? "item " + Hex(w.selector.id) : "item '" + w.selector.label + "'") : "text '" + w.text + "'";
                w.call->Fail("Timed out after " + std::to_string(w.timeoutFrames) + " frames waiting for " + what + (w.gone ? " to disappear" : " to appear"));
            }
            else
            {
                i++;
                continue;
            }
            gWaiters.erase(gWaiters.begin() + i);
        }
    }

    bool HasPendingWaits() { return !gWaiters.empty(); }

    void FailWaits(const std::string& reason)
    {
        for (Waiter& w : gWaiters)
            w.call->Fail(reason);
        gWaiters.clear();
    }

    std::string ResolveRegion(const Snapshot& s, const Json& args, ImRect& rect, Json& target)
    {
        Target t;
        std::string error = ResolveTarget(s, args, "", t);
        if (!error.empty())
            return error;
        if (t.kind == Target::Kind::Item)
        {
            const ItemRec* r = FindItem(s, t.id);
            if (!r->visible)
                return "The widget is not visible (scrolled out or clipped); scroll it into view first, or screenshot a region";
            rect = r->clipped;
        }
        else if (t.kind == Target::Kind::Window)
        {
            rect = FindWindow(s, t.id)->rect;
        }
        else
        {
            return "Specify id, label (optionally with window), or window";
        }
        target = DescribeTarget(s, t);
        return "";
    }

    void AppendActionTools(std::vector<Tool>& tools)
    {
        const std::string targetProps = R"J(
            "id":{"type":"string","description":"Item id (hex) from get_ui_tree or find_items."},
            "label":{"type":"string","description":"Widget label: exact match preferred, else case-insensitive substring; text after ## is ignored. Fails when ambiguous; narrow with window or id."},
            "window":{"type":"string","description":"Window name filter for label. With no id/label it targets the window itself."},
            "kind":{"type":"string","enum":["item","checkable","openable","input"],"description":"Restrict label matches to a kind."},
            "x":{"type":"number","description":"Absolute x in ImGui points (use with y)."},
            "y":{"type":"number","description":"Absolute y in ImGui points."})J";
        const std::string actionProps = R"J(
            "speed":{"type":"string","enum":["visible","instant"],"description":"visible (default) moves the cursor at a watchable pace; instant teleports it."},
            "settle_frames":{"type":"integer","description":"Frames to wait after the action before replying."},
            "screenshot":{"type":"boolean","description":"Attach a screenshot taken after the action, with the pointer still in place (hover states and tooltips visible)."})J";

        auto schema = [](std::initializer_list<std::string> parts, const char* required = nullptr) {
            std::string s = R"J({"type":"object","properties":{)J";
            bool first = true;
            for (const std::string& p : parts)
            {
                if (p.empty())
                    continue;
                s += (first ? "" : ",") + p;
                first = false;
            }
            s += "}";
            if (required)
                s += std::string(R"J(,"required":[)J") + required + "]";
            return Json::parse(s + "}");
        };
        auto add = [&](const char* name, const char* description, Json inputSchema, void (*run)(const CallPtr&)) {
            tools.push_back(Tool{name, description, std::move(inputSchema), run});
        };

        add("click",
            "Move the mouse to a widget (scrolling it into view if needed) and click it. Target by id, label (+window) or x/y. Replies with whether the target was hovered at press time (hit), warnings, and what changed on screen (changes) or noVisibleChange. Takes control of mouse and keyboard until done.",
            schema({targetProps,
                    R"J("button":{"type":"string","enum":["left","right","middle"],"description":"Default left."})J",
                    R"J("double":{"type":"boolean","description":"Double-click."},"ensure":{"type":"string","enum":["open","closed","checked","unchecked"],"description":"Only click if the item is not already in this state (tree nodes, headers, menus: open/closed; checkboxes and checkable menu items: checked/unchecked). Makes repeated runs safe."})J", actionProps}),
            Click);

        add("hover",
            "Move the mouse onto a widget and rest there (default 30 frames, enough for tooltips).",
            schema({targetProps, actionProps}), Hover);

        add("drag",
            "Press on the source, move to the destination and release (drag and drop, sliders, resizing). Source: id/label/window/x/y. Destination: to_id, to_label (+to_window), to_x and to_y, or an offset dx/dy from the source.",
            schema({targetProps,
                    R"J("to_id":{"type":"string"},"to_label":{"type":"string"},"to_window":{"type":"string"},"to_x":{"type":"number"},"to_y":{"type":"number"})J",
                    R"J("dx":{"type":"number","description":"Drag offset in ImGui points."},"dy":{"type":"number"})J", actionProps}),
            Drag);

        add("scroll",
            "Mouse-wheel scroll over a widget, window or point (default: the hovered or focused window). Positive dy scrolls down, positive dx scrolls right, in wheel ticks.",
            schema({targetProps,
                    R"J("dy":{"type":"number","description":"Vertical ticks; positive scrolls down."},"dx":{"type":"number","description":"Horizontal ticks; positive scrolls right."})J",
                    actionProps}),
            Scroll);

        add("type_text",
            "Type text into the active text field, optionally clicking a target field first. replace=true selects all (Cmd/Ctrl+A) before typing; submit=true presses Enter afterwards.",
            schema({targetProps, R"J("text":{"type":"string"},"replace":{"type":"boolean"},"submit":{"type":"boolean"})J", actionProps}, R"J("text")J"),
            TypeText);

        add("set_value",
            "Set a widget to an exact value: a text field, number field, slider or drag (string or number) or a checkbox (boolean). Replies with the widget's text before and after (value) and warns if it shows something else, e.g. a slider clamped the number. "
            "Numbers are committed with Enter; for strings pass submit=true to press Enter. Other widgets (combos, radios, selectables) are not supported: use click.",
            schema({targetProps,
                    R"J("value":{"type":["number","string","boolean"],"description":"The value to set."},"submit":{"type":"boolean","description":"Strings only: press Enter after typing."})J",
                    actionProps},
                   R"J("value")J"),
            SetValue);

        add("key",
            "Press a key or chord, e.g. \"Enter\", \"Escape\", \"Tab\", \"ctrl+s\", \"shortcut+shift+z\" (shortcut = Cmd on macOS, Ctrl elsewhere). Key names are imgui key names (A, F5, LeftArrow, Backspace...).",
            schema({R"J("key":{"type":"string","description":"Key or chord such as ctrl+s."},"mods":{"type":"array","items":{"type":"string"},"description":"Extra modifiers: ctrl, shift, alt, super, shortcut."},"repeat":{"type":"integer","description":"Times to press, default 1."})J",
                    actionProps},
                   R"J("key")J"),
            KeyTool);

        add("wait_for",
            "Wait until an item (id or label, +window) or drawn text appears, or with gone=true disappears. Fails after timeout_frames (default 300, max 3600).",
            schema({R"J("id":{"type":"string"},"label":{"type":"string"},"window":{"type":"string"},"kind":{"type":"string","enum":["item","checkable","openable","input"]},"text":{"type":"string","description":"Drawn text substring (see get_text)."},"gone":{"type":"boolean","description":"Wait for it to disappear instead."},"timeout_frames":{"type":"integer"})J"}),
            WaitFor);
        tools.back().timeoutSeconds = 120; // Longer than the 3600-frame cap at 60 fps.

        add("describe_item",
            "Resolve an item's readable ID path (e.g. //Dear ImGui Demo/Widgets/Basic/input text) and its labeled ancestors. Hovers the item for a few frames, so it takes control of the mouse briefly.",
            schema({targetProps, actionProps}), DescribeItem);

        add("focus_window",
            "Bring a window to the front and give it focus. Use window (name) or an item id/label inside it. Useful when a window covers the click target (see hit in click replies).",
            schema({R"J("window":{"type":"string","description":"Window name (exact preferred, else substring)."},"id":{"type":"string","description":"Item id inside the window (alternative to window)."},"label":{"type":"string"})J", actionProps}),
            FocusWindow);
    }
} // namespace Agent

#endif // AGENT_BRIDGE
