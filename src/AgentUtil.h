#pragma once

#ifdef AGENT_BRIDGE

#include "AgentInternal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Small helpers shared by the tool implementations.
namespace Agent
{
    inline std::string Hex(ImGuiID id)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", id);
        return buf;
    }

    inline bool ParseId(const std::string& text, ImGuiID& out)
    {
        if (text.empty())
            return false;
        char* end = nullptr;
        const unsigned long v = std::strtoul(text.c_str(), &end, 16);
        if (*end != '\0' || v > 0xFFFFFFFFul)
            return false;
        out = (ImGuiID)v;
        return true;
    }

    inline std::string Lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    inline bool ContainsI(const char* haystack, const std::string& needle)
    {
        return needle.empty() || ImStristr(haystack, nullptr, needle.c_str(), nullptr) != nullptr; // Case-insensitive.
    }

    // "a, b, c" from a map's keys, or `none` when it is empty.
    template <class Map>
    std::string JoinKeys(const Map& map, const char* none)
    {
        std::string out;
        for (const auto& entry : map)
            out += (out.empty() ? "" : ", ") + entry.first;
        return out.empty() ? none : out;
    }

    inline std::string ArgStr(const Json& a, const char* key)
    {
        auto it = a.find(key);
        return it != a.end() && it->is_string() ? it->get<std::string>() : std::string();
    }

    inline bool ArgBool(const Json& a, const char* key, bool def)
    {
        auto it = a.find(key);
        return it != a.end() && it->is_boolean() ? it->get<bool>() : def;
    }

    inline bool HasNum(const Json& a, const char* key)
    {
        auto it = a.find(key);
        return it != a.end() && it->is_number();
    }

    inline float ArgNum(const Json& a, const char* key, float def)
    {
        auto it = a.find(key);
        if (it == a.end() || !it->is_number())
            return def;
        const double v = it->get<double>();
        return std::isfinite(v) ? (float)std::clamp(v, -1e9, 1e9) : def;
    }

    inline int ArgInt(const Json& a, const char* key, int def, int lo, int hi)
    {
        return (int)std::lround(std::clamp(ArgNum(a, key, (float)def), (float)lo, (float)hi));
    }

    // Rects can hold huge sentinel values; keep the conversion defined.
    inline int R(float v) { return (int)std::lround(std::clamp(v, -1e9f, 1e9f)); }

    inline Json RectJson(const ImRect& r) { return Json::array({R(r.Min.x), R(r.Min.y), R(r.GetWidth()), R(r.GetHeight())}); }

    inline const WindowRec* FindWindow(const Snapshot& s, ImGuiID id)
    {
        for (const WindowRec& w : s.windows)
            if (w.id == id)
                return &w;
        return nullptr;
    }

    inline const ItemRec* FindItem(const Snapshot& s, ImGuiID id)
    {
        auto it = s.index.find(id);
        return it == s.index.end() ? nullptr : &s.items[it->second];
    }

    inline const char* WindowName(const Snapshot& s, ImGuiID id)
    {
        const WindowRec* w = FindWindow(s, id);
        return w ? s.Str(w->name) : "";
    }

    inline bool Interactive(const ItemRec& r)
    {
        return r.status & (ImGuiItemStatusFlags_Checkable | ImGuiItemStatusFlags_Openable | ImGuiItemStatusFlags_Inputable);
    }

    inline const char* KindOf(const ItemRec& r)
    {
        if (r.isWindow)
            return "window";
        if (r.isTitleBar)
            return "titlebar";
        if (r.status & ImGuiItemStatusFlags_Checkable)
            return "checkable";
        if (r.status & ImGuiItemStatusFlags_Openable)
            return "openable";
        if (r.status & ImGuiItemStatusFlags_Inputable)
            return "input";
        return "item";
    }

    inline Json ItemJson(const Snapshot& s, const ItemRec& r)
    {
        Json j = Json::object();
        j["id"] = Hex(r.id);
        j["kind"] = KindOf(r);
        if (r.label >= 0)
            j["label"] = s.Str(r.label);
        if (r.labelInferred)
            j["labelInferred"] = true;
        j["rect"] = RectJson(r.rect);
        if (r.parentId)
            j["parent"] = Hex(r.parentId);
        if (!r.visible)
            j["visible"] = false;
        if (r.itemFlags & ImGuiItemFlags_Disabled)
            j["disabled"] = true;
        if (r.status & ImGuiItemStatusFlags_Checked)
            j["checked"] = true;
        if (r.status & ImGuiItemStatusFlags_Opened)
            j["open"] = true;
        if (r.status & ImGuiItemStatusFlags_Edited)
            j["edited"] = true;
        if (r.id == s.hoveredId)
            j["hovered"] = true;
        if (r.id == s.activeId)
            j["active"] = true;
        if (r.id == s.navId)
            j["focused"] = true;
        return j;
    }

    inline Json WindowJson(const Snapshot& s, const WindowRec& w)
    {
        Json j = Json::object();
        j["id"] = Hex(w.id);
        j["name"] = s.Str(w.name);
        j["kind"] = w.kind;
        j["rect"] = RectJson(w.rect);
        j["z"] = w.z;
        if (w.parentId)
            j["parent"] = Hex(w.parentId);
        if (w.focused)
            j["focused"] = true;
        if (w.hovered)
            j["hovered"] = true;
        if (w.collapsed)
            j["collapsed"] = true;
        return j;
    }

    // The label without its ##/### id suffix (the whole label if nothing would remain).
    inline std::string DisplayLabel(const char* label)
    {
        const char* hash = std::strstr(label, "##");
        std::string text = hash ? std::string(label, hash) : std::string(label);
        return text.empty() ? std::string(label) : text;
    }

    // "#rrggbb", or "#rrggbbaa" when not fully opaque.
    inline std::string ColorHex(ImU32 color)
    {
        char buf[16];
        const unsigned r = color & 0xFF, g = (color >> 8) & 0xFF, b = (color >> 16) & 0xFF, a = color >> 24;
        if (a == 255)
            std::snprintf(buf, sizeof buf, "#%02x%02x%02x", r, g, b);
        else
            std::snprintf(buf, sizeof buf, "#%02x%02x%02x%02x", r, g, b, a);
        return buf;
    }

    // Finds the strings drawn inside an item's rect. Build once per tool call.
    class TextIndex
    {
    public:
        explicit TextIndex(const Snapshot& s) : snap_(s)
        {
            for (int i = 0; i < (int)s.texts.size(); i++)
                if (s.texts[i].visible) // imgui draws clipped text too once clipping is off; it is not on screen.
                    byWindow_[s.texts[i].windowId].push_back(i);
        }

        // Strings whose center lies inside the item, in draw order, joined by spaces and capped in length.
        std::string In(const ItemRec& item) const
        {
            std::string joined;
            auto it = byWindow_.find(item.windowId);
            if (it == byWindow_.end())
                return joined;
            for (int i : it->second)
            {
                const TextRec& t = snap_.texts[i];
                if (!item.rect.Contains(t.rect.GetCenter()))
                    continue;
                joined += (joined.empty() ? "" : " ") + std::string(snap_.Str(t.str));
                if (joined.size() >= 200)
                    break;
            }
            return joined;
        }

    private:
        const Snapshot& snap_;
        std::unordered_map<ImGuiID, std::vector<int>> byWindow_;
    };

    // Adds the text shown inside the widget (a slider's value, an input's content, ...) unless it is just the label.
    inline void AttachText(Json& itemJson, const Snapshot& s, const ItemRec& r, const TextIndex& texts)
    {
        if (r.isWindow || r.isTitleBar)
            return;
        const std::string shown = texts.In(r);
        if (!shown.empty() && (r.label < 0 || shown != DisplayLabel(s.Str(r.label))))
            itemJson["text"] = shown;
    }

    // Item json plus its window, or just the id when the item is not in the snapshot; null for id 0.
    inline Json DescribeId(const Snapshot& s, ImGuiID id)
    {
        if (id == 0)
            return nullptr;
        const ItemRec* r = FindItem(s, id);
        if (!r)
        {
            Json j = Json::object();
            j["id"] = Hex(id);
            return j;
        }
        Json j = ItemJson(s, *r);
        j["window"] = WindowName(s, r->windowId);
        return j;
    }
} // namespace Agent

#endif // AGENT_BRIDGE
