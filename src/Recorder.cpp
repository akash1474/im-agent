#ifdef AGENT_BRIDGE

#include "AgentInternal.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <algorithm>

namespace
{
    Agent::Snapshot gCur, gLast;

    constexpr size_t kMaxStringBytes = 512;

    // imgui reports labels only for items it doesn't clip, so remember them: an item scrolled out of view keeps its label.
    struct CachedInfo
    {
        std::string label;
        ImGuiItemStatusFlags status = 0;
    };
    std::unordered_map<ImGuiID, CachedInfo> gInfoCache;
    std::unordered_map<ImGuiID, CachedInfo> gPendingInfo; // Reported by widgets that call ITEM_INFO before ItemAdd (tabs).
    constexpr size_t kMaxCachedItems = 50000;

    int Intern(Agent::Snapshot& s, const char* begin, const char* end)
    {
        size_t n = end ? (size_t)(end - begin) : std::strlen(begin);
        if (n > kMaxStringBytes)
        {
            n = kMaxStringBytes;
            while (n > 0 && (begin[n] & 0xC0) == 0x80) // Don't cut a UTF-8 sequence in half.
                n--;
        }
        const int offset = (int)s.strings.size();
        s.strings.append(begin, n);
        s.strings.push_back('\0');
        return offset;
    }

    const char* WindowKind(const ImGuiWindow* w)
    {
        if (w->Flags & ImGuiWindowFlags_Modal)
            return "modal";
        if (w->Flags & ImGuiWindowFlags_Popup)
            return "popup";
        if (w->Flags & ImGuiWindowFlags_Tooltip)
            return "tooltip";
        if (w->Flags & ImGuiWindowFlags_ChildWindow)
            return "child";
        return "window";
    }

    // Some widgets (combos) never report their label to imgui, but draw it right after the frame.
    void ApplyCachedInfo(Agent::Snapshot& s)
    {
        for (Agent::ItemRec& item : s.items)
        {
            if (item.label >= 0 || item.visible)
                continue;
            auto it = gInfoCache.find(item.id);
            if (it == gInfoCache.end())
                continue;
            item.label = Intern(s, it->second.label.data(), it->second.label.data() + it->second.label.size());
            item.status |= it->second.status;
        }
    }

    void InferMissingLabels(Agent::Snapshot& s, float maxGap)
    {
        ApplyCachedInfo(s);
        // Scrollbars, resize grips and title-bar buttons never report a label and are not user widgets: they hang off
        // no item (their id is derived differently) and are thinner than a frame, or sit inside the title bar.
        const float frameHeight = ImGui::GetFrameHeight();
        std::unordered_map<ImGuiID, ImRect> titleBars, windowRects;
        for (const Agent::WindowRec& w : s.windows)
            windowRects[w.id] = w.rect;
        for (const Agent::ItemRec& item : s.items)
            if (item.isTitleBar)
                titleBars[item.windowId] = item.rect;
        for (Agent::ItemRec& item : s.items)
        {
            auto bar = titleBars.find(item.windowId);
            const bool inTitleBar = bar != titleBars.end() && bar->second.Contains(item.rect.GetCenter());
            const bool thin = std::min(item.rect.GetWidth(), item.rect.GetHeight()) < frameHeight;
            const bool detached = item.parentId != item.windowId && !s.index.count(item.parentId);
            bool onEdge = false; // Scrollbars and resize grips hug the window border.
            auto wr = windowRects.find(item.windowId);
            if (wr != windowRects.end())
            {
                const ImRect& r = wr->second;
                onEdge = r.Contains(item.rect.GetCenter()) && (item.rect.Min.x <= r.Min.x + 3 || item.rect.Max.x >= r.Max.x - 3 || item.rect.Min.y <= r.Min.y + 3 || item.rect.Max.y >= r.Max.y - 3);
            }
            item.decoration = !item.isWindow && !item.isTitleBar && item.label < 0 && (inTitleBar || (thin && (detached || onEdge)));
        }

        for (Agent::ItemRec& item : s.items)
        {
            if (item.label >= 0 || item.decoration || item.isWindow || item.isTitleBar || !item.visible)
                continue;
            for (const Agent::TextRec& t : s.texts)
            {
                if (!t.visible)
                    continue;
                const float gap = t.rect.Min.x - item.rect.Max.x;
                const float midY = (t.rect.Min.y + t.rect.Max.y) * 0.5f;
                if (t.windowId == item.windowId && gap >= -1.0f && gap <= maxGap && midY >= item.rect.Min.y && midY <= item.rect.Max.y)
                {
                    item.label = t.str;
                    item.labelInferred = true;
                    break;
                }
            }
        }
    }
} // namespace

namespace Agent
{
    // GImGuiAgentHookItems is the "recording" flag: imgui only calls the item hooks while it is set.
    void RecorderBeginFrame()
    {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (!g)
            return;
        gCur.Clear();
        gPendingInfo.clear();
        GImGuiAgentHookItems = true;
        g->ItemUnclipByLog = true; // Clipped widgets still run, so imgui reports their labels (an agent must find items it hasn't scrolled to).
    }

    void RecorderEndFrame()
    {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (!g || !GImGuiAgentHookItems)
            return;
        GImGuiAgentHookItems = false;
        g->ItemUnclipByLog = false;

        Snapshot& s = gCur;
        s.frame = g->FrameCount;
        s.hoveredId = g->HoveredId;
        s.activeId = g->ActiveId;
        s.navId = g->NavId;
        s.hoveredWindowId = g->HoveredWindow ? g->HoveredWindow->ID : 0;
        s.navWindowId = g->NavWindow ? g->NavWindow->ID : 0;
        s.mousePos = g->IO.MousePos;
        s.wantCaptureMouse = g->IO.WantCaptureMouse;
        s.wantCaptureKeyboard = g->IO.WantCaptureKeyboard;
        s.wantTextInput = g->IO.WantTextInput;

        for (int i = 0; i < g->Windows.Size; i++)
        {
            ImGuiWindow* w = g->Windows[i];
            if (!w->Active)
                continue;
            WindowRec r;
            r.id = w->ID;
            r.parentId = w->ParentWindow ? w->ParentWindow->ID : 0;
            r.name = Intern(s, w->Name, nullptr);
            r.rect = w->Rect();
            r.z = i;
            r.kind = WindowKind(w);
            r.focused = g->NavWindow && g->NavWindow->RootWindow == w->RootWindow;
            r.hovered = g->HoveredWindow == w;
            r.collapsed = w->Collapsed;
            s.windows.push_back(r);
        }

        for (const ImGuiPopupData& p : g->OpenPopupStack)
        {
            PopupRec r;
            r.id = p.PopupId;
            r.windowId = p.Window ? p.Window->ID : 0;
            s.popups.push_back(r);
        }

        InferMissingLabels(s, g->Style.ItemInnerSpacing.x + 2.0f);
        std::swap(gCur, gLast);
    }

    const Snapshot& LastSnapshot() { return gLast; }
} // namespace Agent

// ---- Hooks declared by imgui_internal.h and imgui_draw.cpp (src/agent/patches/imgui-agent-hooks.patch) ----

bool GImGuiAgentHookItems = false;

void ImGuiAgentHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect& bb, const ImGuiLastItemData* itemData)
{
    if (id == 0)
        return;
    ImGuiWindow* w = ctx->CurrentWindow;

    // An item may be registered twice in a frame (e.g. ButtonBehavior's fallback); the later call wins.
    auto [it, inserted] = gCur.index.try_emplace(id, (int)gCur.items.size());
    if (inserted)
        gCur.items.emplace_back();
    Agent::ItemRec& r = gCur.items[it->second];

    // The same id registered again with a different rect means two widgets share an id (an imgui ID conflict).
    const ImRect previous = r.rect;
    const bool seenBefore = !inserted;

    r.id = id;
    r.parentId = w->IDStack.Size ? w->IDStack.back() : 0;
    r.windowId = w->ID;
    r.rect = bb;
    r.itemFlags = itemData ? itemData->ItemFlags : 0;
    r.status = itemData ? itemData->StatusFlags : 0;
    r.isWindow = id == w->ID;
    r.isTitleBar = id == w->MoveId;
    r.visible = r.isWindow || bb.Overlaps(w->ClipRect);
    r.clipped = bb;
    if (r.visible && !r.isWindow)
        r.clipped.ClipWith(w->ClipRect);

    if (inserted)
    {
        auto pending = gPendingInfo.find(id);
        if (pending != gPendingInfo.end())
        {
            if (!pending->second.label.empty())
                r.label = Intern(gCur, pending->second.label.data(), pending->second.label.data() + pending->second.label.size());
            r.status |= pending->second.status;
            gPendingInfo.erase(pending);
        }
    }
    if (seenBefore && !r.isWindow && !r.isTitleBar && !(r.itemFlags & ImGuiItemFlags_AllowDuplicateId))
    {
        const bool same = std::fabs(previous.Min.x - bb.Min.x) < 1.0f && std::fabs(previous.Min.y - bb.Min.y) < 1.0f &&
                          std::fabs(previous.Max.x - bb.Max.x) < 1.0f && std::fabs(previous.Max.y - bb.Max.y) < 1.0f;
        if (!same)
        {
            r.conflict = true;
            r.conflictRect = previous;
        }
    }
}

void ImGuiAgentHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags flags)
{
    CachedInfo* cached = nullptr;
    if (label && label[0])
    {
        const size_t n = std::min(std::strlen(label), kMaxStringBytes);
        if (gInfoCache.size() > kMaxCachedItems)
            gInfoCache.clear();
        cached = &gInfoCache[id];
        if (cached->label.compare(0, std::string::npos, label, n) != 0)
            cached->label.assign(label, n);
        cached->status = flags;
    }

    // ItemInfo nearly always follows the ItemAdd of the same id, so skip the map lookup.
    int index = (int)gCur.items.size() - 1;
    if (index < 0 || gCur.items[index].id != id)
    {
        auto it = gCur.index.find(id);
        if (it == gCur.index.end())
        {
            CachedInfo& pending = gPendingInfo[id];
            pending.status |= flags;
            if (cached)
                pending.label = cached->label;
            return;
        }
        index = it->second;
    }
    Agent::ItemRec& r = gCur.items[index];
    r.status |= flags;
    if (cached && r.label < 0)
        r.label = Intern(gCur, label, nullptr);
}

void ImGuiAgentHook_Text(ImFont* font, float fontSize, const ImVec2& pos, const char* begin, const char* end, float wrapWidth, const ImVec4& clip, ImU32 color)
{
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g || !GImGuiAgentHookItems)
        return;
    if (!end)
        end = begin + std::strlen(begin);
    if (begin == end)
        return;

    Agent::TextRec t;
    t.windowId = g->CurrentWindow ? g->CurrentWindow->ID : 0;
    t.rect = ImRect(pos, pos + font->CalcTextSizeA(fontSize, FLT_MAX, wrapWidth, begin, end));
    t.visible = t.rect.Overlaps(ImRect(clip));
    if (font->Flags & ImFontFlags_NoLoadGlyphs) // imgui's password mode: it draws '*' but passes the real text.
    {
        std::string masked;
        for (const char* p = begin; p < end;)
        {
            unsigned int codepoint = 0;
            p += std::max(1, ImTextCharFromUtf8(&codepoint, p, end));
            masked += '*';
        }
        t.str = Intern(gCur, masked.data(), masked.data() + masked.size());
    }
    else
        t.str = Intern(gCur, begin, end);
    t.size = fontSize;
    t.color = color;

    auto known = std::find_if(gCur.fontNames.begin(), gCur.fontNames.end(), [&](const auto& entry) { return entry.first == font; });
    if (known == gCur.fontNames.end())
        known = gCur.fontNames.insert(known, {font, Intern(gCur, font->GetDebugName(), nullptr)});
    t.font = known->second;
    gCur.texts.push_back(t);
}

#endif // AGENT_BRIDGE
