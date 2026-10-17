#ifdef AGENT_BRIDGE

#include "AgentInternal.h"
#include "AgentUtil.h"

#include <map>
#include <set>

namespace Agent
{
    struct UiSummary
    {
        struct ItemState
        {
            std::string name;
            std::string text; // What the widget displays besides its label (value, selection, ...).
            bool checked = false, open = false, disabled = false;
        };

        std::set<std::string> windows;
        std::unordered_map<ImGuiID, ItemState> items;
        std::map<std::string, int> texts; // "window\x1ftext" -> how many times it is drawn.
    };

    namespace
    {
        constexpr size_t kMaxListed = 8;

        // Popups and tooltips are named "##Popup_1234"; describe them by what they show instead.
        std::string WindowTitle(const Snapshot& s, const WindowRec& w)
        {
            const std::string name = s.Str(w.name);
            if (name.rfind("##", 0) != 0)
                return DisplayLabel(name.c_str());
            for (const TextRec& t : s.texts)
                if (t.windowId == w.id)
                    return std::string(w.kind) + " \"" + s.Str(t.str) + "\"";
            return w.kind;
        }

        Json Capped(const std::vector<std::string>& list)
        {
            Json out = Json::array();
            for (size_t i = 0; i < list.size() && i < kMaxListed; i++)
                out.push_back(list[i]);
            if (list.size() > kMaxListed)
                out.push_back("+" + std::to_string(list.size() - kMaxListed) + " more");
            return out;
        }

        std::vector<std::string> Missing(const std::set<std::string>& from, const std::set<std::string>& in)
        {
            std::vector<std::string> out;
            for (const std::string& v : from)
                if (!in.count(v))
                    out.push_back(v);
            return out;
        }

        std::string Quote(const std::string& s) { return "'" + s + "'"; }
    } // namespace

    std::shared_ptr<UiSummary> CaptureSummary(const Snapshot& s)
    {
        auto summary = std::make_shared<UiSummary>();
        std::unordered_map<ImGuiID, std::string> titles;
        for (const WindowRec& w : s.windows)
        {
            titles[w.id] = WindowTitle(s, w);
            summary->windows.insert(titles[w.id]);
        }

        const TextIndex texts(s);
        for (const ItemRec& r : s.items)
        {
            if (r.isWindow || r.isTitleBar || (r.label < 0 && !Interactive(r)))
                continue;
            UiSummary::ItemState state;
            state.name = r.label >= 0 ? DisplayLabel(s.Str(r.label)) : Hex(r.id);
            const std::string shown = texts.In(r);
            if (shown != state.name)
                state.text = shown;
            state.checked = r.status & ImGuiItemStatusFlags_Checked;
            state.open = r.status & ImGuiItemStatusFlags_Opened;
            state.disabled = r.itemFlags & ImGuiItemFlags_Disabled;
            summary->items[r.id] = std::move(state);
        }

        for (const TextRec& t : s.texts)
            if (t.visible)
                summary->texts[titles[t.windowId] + "\x1f" + s.Str(t.str)]++;
        return summary;
    }

    Json DiffSummary(const UiSummary& before, const UiSummary& after)
    {
        Json out = Json::object();

        std::vector<std::string> opened = Missing(after.windows, before.windows);
        std::vector<std::string> closed = Missing(before.windows, after.windows);
        if (!opened.empty())
            out["windowsOpened"] = Capped(opened);
        if (!closed.empty())
            out["windowsClosed"] = Capped(closed);

        std::vector<std::string> added, removed, changed;
        std::set<std::string> valueStrings; // Texts already described by a changed widget; not repeated in "text".
        for (const auto& [id, now] : after.items)
        {
            auto it = before.items.find(id);
            if (it == before.items.end())
            {
                added.push_back(now.name + (now.text.empty() ? "" : " = " + Quote(now.text)));
                valueStrings.insert(now.name);
                continue;
            }
            const UiSummary::ItemState& was = it->second;
            if (was.text != now.text)
            {
                changed.push_back(now.name + ": " + Quote(was.text) + " -> " + Quote(now.text));
                valueStrings.insert(was.text);
                valueStrings.insert(now.text);
            }
            if (was.checked != now.checked)
                changed.push_back(now.name + ": " + (now.checked ? "checked" : "unchecked"));
            if (was.open != now.open)
                changed.push_back(now.name + ": " + (now.open ? "opened" : "closed"));
            if (was.disabled != now.disabled)
                changed.push_back(now.name + ": " + (now.disabled ? "disabled" : "enabled"));
        }
        for (const auto& [id, was] : before.items)
            if (!after.items.count(id))
            {
                removed.push_back(was.name);
                valueStrings.insert(was.name);
            }

        Json items = Json::object();
        if (!added.empty())
            items["added"] = Capped(added);
        if (!removed.empty())
            items["removed"] = Capped(removed);
        if (!changed.empty())
            items["changed"] = Capped(changed);
        if (!items.empty())
            out["items"] = std::move(items);

        std::vector<std::string> textAdded, textRemoved;
        auto plain = [](const std::string& key) { return key.substr(key.find('\x1f') + 1); };
        for (const auto& [key, count] : after.texts)
        {
            auto it = before.texts.find(key);
            if (count > (it == before.texts.end() ? 0 : it->second) && !valueStrings.count(plain(key)))
                textAdded.push_back(plain(key));
        }
        for (const auto& [key, count] : before.texts)
        {
            auto it = after.texts.find(key);
            if (count > (it == after.texts.end() ? 0 : it->second) && !valueStrings.count(plain(key)))
                textRemoved.push_back(plain(key));
        }
        Json text = Json::object();
        if (!textAdded.empty())
            text["added"] = Capped(textAdded);
        if (!textRemoved.empty())
            text["removed"] = Capped(textRemoved);
        if (!text.empty())
            out["text"] = std::move(text);

        return out.empty() ? Json(nullptr) : out;
    }
} // namespace Agent

#endif // AGENT_BRIDGE
