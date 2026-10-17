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
        const char* const kAllRules[] = {"duplicate-id", "off-display", "tiny-target", "overlap", "low-contrast", "unlabeled", "duplicate-label"};

        struct Issue
        {
            std::string severity, rule, window, message;
            Json items = Json::array();
        };

        // ---- Colors ----

        ImVec4 Over(ImVec4 fg, const ImVec4& bg)
        {
            const float a = fg.w;
            return ImVec4(fg.x * a + bg.x * (1 - a), fg.y * a + bg.y * (1 - a), fg.z * a + bg.z * (1 - a), 1.0f);
        }

        float Luminance(const ImVec4& c)
        {
            auto linear = [](float v) { return v <= 0.03928f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
            return 0.2126f * linear(c.x) + 0.7152f * linear(c.y) + 0.0722f * linear(c.z);
        }

        // WCAG contrast ratio between two opaque colors.
        float Contrast(const ImVec4& a, const ImVec4& b)
        {
            const float la = Luminance(a), lb = Luminance(b);
            return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
        }

        ImVec4 WindowBackground(const ImGuiStyle& style, const WindowRec& w)
        {
            const ImVec4 black(0, 0, 0, 1);
            const ImVec4 window = Over(style.Colors[ImGuiCol_WindowBg], black);
            const std::string kind = w.kind;
            if (kind == "popup" || kind == "tooltip" || kind == "modal")
                return Over(style.Colors[ImGuiCol_PopupBg], window);
            if (kind == "child")
                return Over(style.Colors[ImGuiCol_ChildBg], window);
            return window;
        }

        // ---- Geometry ----

        float Area(const ImRect& r) { return std::max(0.0f, r.GetWidth()) * std::max(0.0f, r.GetHeight()); }

        float OverlapArea(const ImRect& a, const ImRect& b) { return Area(ImRect(ImMax(a.Min, b.Min), ImMin(a.Max, b.Max))); }

        bool IsAncestor(const Snapshot& s, ImGuiID ancestor, const ItemRec& item)
        {
            ImGuiID parent = item.parentId;
            for (int depth = 0; parent && depth < 16; depth++)
            {
                if (parent == ancestor)
                    return true;
                const ItemRec* p = FindItem(s, parent);
                if (!p || p->parentId == parent)
                    return false;
                parent = p->parentId;
            }
            return false;
        }

        Json Brief(const Snapshot& s, const ItemRec& r)
        {
            Json j = Json::object();
            j["id"] = Hex(r.id);
            if (r.label >= 0)
                j["label"] = DisplayLabel(s.Str(r.label));
            j["rect"] = RectJson(r.rect);
            return j;
        }

        void LintWindow(const Snapshot& s, const WindowRec& w, const std::vector<const ItemRec*>& items, const std::set<std::string>& rules,
                        const ImGuiStyle& style, const ImRect& display, std::vector<Issue>& out)
        {
            const std::string windowName = s.Str(w.name);
            auto report = [&](const char* severity, const char* rule, std::string message, Json list) {
                if (!rules.count(rule))
                    return;
                Issue issue;
                issue.severity = severity;
                issue.rule = rule;
                issue.window = windowName;
                issue.message = std::move(message);
                issue.items = std::move(list);
                out.push_back(std::move(issue));
            };
            auto one = [&](const ItemRec& r) {
                Json list = Json::array();
                list.push_back(Brief(s, r));
                return list;
            };

            if (OverlapArea(w.rect, display) <= 0.0f)
                report("error", "off-display", "The whole window lies outside the display, so none of its widgets can be reached.", Json::array());

            std::vector<const ItemRec*> widgets; // Real, visible, user-facing items (not scrollbars, grips or title-bar buttons).
            for (const ItemRec* r : items)
                if (!r->isWindow && !r->isTitleBar && r->visible && !r->decoration)
                    widgets.push_back(r);

            const float minTarget = 0.75f * ImGui::GetFrameHeight();
            for (const ItemRec* r : items)
            {
                if (r->conflict)
                {
                    Json list = one(*r);
                    Json earlier = Json::object();
                    earlier["rect"] = RectJson(r->conflictRect);
                    list.push_back(std::move(earlier));
                    report("error", "duplicate-id",
                           "Two widgets share id " + Hex(r->id) + (r->label >= 0 ? " (label '" + DisplayLabel(s.Str(r->label)) + "')" : "") +
                               ": clicking either acts on the first. Give one a unique ##suffix or wrap them in PushID().",
                           std::move(list));
                }
                if (!r->isWindow && !r->isTitleBar && !r->decoration && OverlapArea(w.rect, r->rect) > 0.0f && !display.Overlaps(r->rect))
                    report("error", "off-display", "The widget lies inside its window but outside the display, so it cannot be reached.", one(*r));
                if (r->label < 0 && !r->labelInferred && !r->isWindow && !r->isTitleBar && r->visible && !r->decoration)
                    report("info", "unlabeled", "The widget reports no label; agents can only target it by id or position.", one(*r));
            }

            for (const ItemRec* r : widgets)
            {
                if (std::strcmp(KindOf(*r), "openable") == 0 || r->rect.GetWidth() <= 0.0f)
                    continue;
                if (std::min(r->rect.GetWidth(), r->rect.GetHeight()) < minTarget)
                    report("warning", "tiny-target",
                           "The hit area is " + std::to_string(R(r->rect.GetWidth())) + "x" + std::to_string(R(r->rect.GetHeight())) +
                               " points; under about " + std::to_string((int)minTarget) + " is hard to click or tap.",
                           one(*r));
            }

            if (widgets.size() <= 400)
            {
                for (size_t i = 0; i < widgets.size(); i++)
                    for (size_t j = i + 1; j < widgets.size(); j++)
                    {
                        const ItemRec &a = *widgets[i], &b = *widgets[j];
                        const float smaller = std::min(Area(a.rect), Area(b.rect));
                        if (smaller <= 0.0f || OverlapArea(a.rect, b.rect) < 0.3f * smaller || IsAncestor(s, a.id, b) || IsAncestor(s, b.id, a))
                            continue;
                        Json list = one(a);
                        list.push_back(Brief(s, b));
                        report("warning", "overlap", "Two widgets overlap, so one may hide or steal clicks from the other.", std::move(list));
                    }
            }

            std::map<std::string, std::vector<const ItemRec*>> byLabel;
            for (const ItemRec* r : widgets)
                if (r->label >= 0 && !r->labelInferred && !DisplayLabel(s.Str(r->label)).empty())
                    byLabel[DisplayLabel(s.Str(r->label))].push_back(r);
            for (const auto& [label, group] : byLabel)
                if (group.size() > 1)
                {
                    Json list = Json::array();
                    for (const ItemRec* r : group)
                        list.push_back(Brief(s, *r));
                    report("info", "duplicate-label",
                           std::to_string(group.size()) + " widgets are labeled '" + label + "'; agents and screen readers can only tell them apart by id.", std::move(list));
                }

            // Text contrast against the background it most likely sits on (stock imgui colors; custom drawing is not seen).
            const ItemRec* titleBar = nullptr;
            for (const ItemRec* r : items)
                if (r->isTitleBar)
                    titleBar = r;
            const ImVec4 windowBg = WindowBackground(style, w);
            std::map<std::pair<ImU32, ImU32>, std::pair<int, std::vector<std::string>>> seen;
            for (const TextRec& t : s.texts)
            {
                if (t.windowId != w.id || !t.visible || (t.color >> 24) < 16)
                    continue;
                const ImVec2 center = t.rect.GetCenter();
                if (titleBar && titleBar->rect.Contains(center))
                    continue;
                const ItemRec* container = nullptr;
                for (const ItemRec* r : widgets)
                    if (r->rect.Contains(center) && (!container || Area(r->rect) < Area(container->rect)))
                        container = r;
                ImVec4 background = windowBg;
                if (container)
                {
                    const std::string kind = KindOf(*container);
                    if (kind == "input" || (kind == "item" && container->labelInferred))
                        background = Over(style.Colors[ImGuiCol_FrameBg], windowBg);
                    else if (kind == "item")
                        background = Over(style.Colors[ImGuiCol_Button], windowBg);
                }
                const ImVec4 text = Over(ImGui::ColorConvertU32ToFloat4(t.color), background);
                const float needed = t.size >= 18.0f ? 3.0f : 4.5f;
                const float ratio = Contrast(text, background);
                if (ratio >= needed)
                    continue;
                auto& entry = seen[{t.color, ImGui::ColorConvertFloat4ToU32(background)}];
                entry.first++;
                if (entry.second.size() < 3)
                    entry.second.push_back(s.Str(t.str));
            }
            for (const auto& [colors, entry] : seen)
            {
                const ImVec4 background = ImGui::ColorConvertU32ToFloat4(colors.second);
                const ImVec4 text = Over(ImGui::ColorConvertU32ToFloat4(colors.first), background);
                char message[200];
                std::snprintf(message, sizeof message, "Text %s on about %s has a contrast ratio of %.1f:1 (WCAG wants 4.5:1; 3:1 for large text). %d string(s).",
                              ColorHex(colors.first).c_str(), ColorHex(ImGui::ColorConvertFloat4ToU32(background)).c_str(), Contrast(text, background), entry.first);
                Json samples = Json::array();
                for (const std::string& sample : entry.second)
                    samples.push_back(Json::object({{"text", sample}}));
                report("warning", "low-contrast", message, std::move(samples));
            }
        }

        void UiLint(const CallPtr& c)
        {
            const Snapshot& s = LastSnapshot();
            const std::string windowFilter = ArgStr(c->args, "window");
            const int maxPerRule = ArgInt(c->args, "max_per_rule", 10, 1, 100);

            std::set<std::string> rules(std::begin(kAllRules), std::end(kAllRules));
            auto requested = c->args.find("rules");
            if (requested != c->args.end() && requested->is_array() && !requested->empty())
            {
                rules.clear();
                for (const Json& r : *requested)
                {
                    const std::string name = r.is_string() ? r.get<std::string>() : std::string();
                    if (std::find(std::begin(kAllRules), std::end(kAllRules), name) == std::end(kAllRules))
                        return c->Fail("Unknown rule '" + name + "'. Rules: duplicate-id, off-display, tiny-target, overlap, low-contrast, unlabeled, duplicate-label");
                    rules.insert(name);
                }
            }

            std::unordered_map<ImGuiID, std::vector<const ItemRec*>> byWindow;
            for (const ItemRec& r : s.items)
                byWindow[r.windowId].push_back(&r);

            const ImGuiIO& io = ImGui::GetIO();
            const ImRect display(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
            std::vector<Issue> issues;
            int windowsChecked = 0;
            for (const WindowRec& w : s.windows)
            {
                if (!ContainsI(s.Str(w.name), windowFilter))
                    continue;
                windowsChecked++;
                LintWindow(s, w, byWindow[w.id], rules, ImGui::GetStyle(), display, issues);
            }

            std::stable_sort(issues.begin(), issues.end(), [](const Issue& a, const Issue& b) {
                auto rank = [](const std::string& sev) { return sev == "error" ? 0 : sev == "warning" ? 1 : 2; };
                return rank(a.severity) < rank(b.severity);
            });

            std::map<std::string, int> perRule, total;
            Json list = Json::array();
            int errors = 0, warnings = 0, infos = 0;
            for (const Issue& issue : issues)
            {
                (issue.severity == "error" ? errors : issue.severity == "warning" ? warnings : infos)++;
                total[issue.rule]++;
                if (perRule[issue.rule]++ >= maxPerRule)
                    continue;
                Json j = Json::object();
                j["severity"] = issue.severity;
                j["rule"] = issue.rule;
                j["window"] = issue.window;
                j["message"] = issue.message;
                if (!issue.items.empty())
                    j["items"] = issue.items;
                list.push_back(std::move(j));
            }

            Json summary = Json::object();
            summary["errors"] = errors;
            summary["warnings"] = warnings;
            summary["info"] = infos;
            summary["windowsChecked"] = windowsChecked;
            Json suppressed = Json::object();
            for (const auto& [rule, count] : total)
                if (count > maxPerRule)
                    suppressed[rule] = count - maxPerRule;
            if (!suppressed.empty())
                summary["notShown"] = std::move(suppressed);

            Json out = Json::object();
            out["frame"] = s.frame;
            out["summary"] = std::move(summary);
            out["issues"] = std::move(list);
            c->Ok(out);
        }
    } // namespace

    void AppendLintTools(std::vector<Tool>& tools)
    {
        tools.push_back(Tool{
            "ui_lint",
            "Check the current UI for common imgui problems: duplicate IDs (two widgets sharing an id), widgets outside the display, tiny hit targets, overlapping widgets, "
            "low text contrast, unlabeled widgets and duplicate labels. Only looks at widgets the app drew this frame, so open collapsed sections first. Contrast uses the stock imgui colors of the "
            "current theme as the background estimate.",
            Json::parse(R"J({"type":"object","properties":{
                "window":{"type":"string","description":"Only windows whose name contains this."},
                "rules":{"type":"array","items":{"type":"string","enum":["duplicate-id","off-display","tiny-target","overlap","low-contrast","unlabeled","duplicate-label"]},"description":"Run only these rules. Default all."},
                "max_per_rule":{"type":"integer","description":"Issues listed per rule, default 10."}}})J"),
            UiLint});
    }
} // namespace Agent

#endif // AGENT_BRIDGE
