#include "ui.h"
#include "engine.h"
#include "net.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <map>

// ------------------------------------------------------------------ state ---
static HWND  s_hwnd = nullptr;
static float s_dpi = 1.0f;
static bool  s_styleDirty = false;
static bool  s_topmost = false;
static bool  s_logOpen = true;
static int   s_tab = TAB_DASHBOARD;

static NetState            s_network;
static EgressState         s_egress;
static KitState            s_kit;
static std::vector<AppRow> s_apps;
static std::string         s_appSel;               // selected app by path, stable across rescans
static char                s_appFilter[96] = { 0 };
static bool                s_appsDirty = true;

static double s_lastNet = -1e9, s_lastApps = -1e9, s_lastKit = -1e9, s_lastEgress = -1e9;

static inline float S(float v) { return v * s_dpi; }

// ------------------------------------------------------------------ theme ---
static ImFont* s_fUi = nullptr;     // Segoe UI + icons
static ImFont* s_fSemi = nullptr;   // Segoe UI Semibold + icons
static ImFont* s_fMono = nullptr;   // Cascadia Mono (or Consolas) + icons

static const ImVec4 V_ACCENT(0.55f, 0.36f, 0.96f, 1.00f);
static const ImVec4 V_OK    (0.29f, 0.87f, 0.50f, 1.00f);
static const ImVec4 V_OUT   (0.99f, 0.83f, 0.30f, 1.00f);
static const ImVec4 V_MIX   (0.94f, 0.67f, 0.99f, 1.00f);
static const ImVec4 V_CYAN  (0.40f, 0.91f, 0.98f, 1.00f);
static const ImVec4 V_DIM   (0.61f, 0.56f, 0.73f, 1.00f);
static const ImVec4 V_BAD   (0.98f, 0.44f, 0.52f, 1.00f);
static const ImVec4 V_LILAC (0.77f, 0.71f, 0.99f, 1.00f);

static const ImU32 K_BG     = IM_COL32(0x10, 0x0C, 0x17, 255);
static const ImU32 K_SURF   = IM_COL32(0x18, 0x13, 0x25, 255);
static const ImU32 K_SURF2  = IM_COL32(0x22, 0x1B, 0x35, 255);
static const ImU32 K_SUNK   = IM_COL32(0x0B, 0x08, 0x11, 255);
static const ImU32 K_LINE   = IM_COL32(0x2C, 0x24, 0x42, 255);
static const ImU32 K_LINE2  = IM_COL32(0x4C, 0x3C, 0x78, 255);
static const ImU32 K_TEXT   = IM_COL32(0xEE, 0xEA, 0xF7, 255);
static const ImU32 K_SUB    = IM_COL32(0xA8, 0x9F, 0xC7, 255);
static const ImU32 K_DIM    = IM_COL32(0x74, 0x6B, 0x91, 255);
static const ImU32 K_ACC    = IM_COL32(0x8B, 0x5C, 0xF6, 255);
static const ImU32 K_ACC2   = IM_COL32(0x6D, 0x28, 0xD9, 255);
static const ImU32 K_ACCHI  = IM_COL32(0xA7, 0x86, 0xFA, 255);
static const ImU32 K_ACCTX  = IM_COL32(0xC4, 0xB5, 0xFD, 255);
static const ImU32 K_WHITE  = IM_COL32(0xFF, 0xFF, 0xFF, 255);
static const ImU32 K_DANGER = IM_COL32(0xE1, 0x1D, 0x48, 255);

static ImU32 Col(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
static ImU32 Alpha(ImVec4 c, float a) { c.w = a; return Col(c); }

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) code points
enum : unsigned {
    IC_SHIELD = 0xEA18, IC_PIN = 0xE718, IC_PINNED = 0xE842, IC_MIN = 0xE921, IC_CLOSE = 0xE8BB,
    IC_HOME = 0xE80F, IC_APPS = 0xE71D, IC_FILTER = 0xE71C, IC_GLOBE = 0xE774, IC_REFRESH = 0xE72C,
    IC_COPY = 0xE8C8, IC_WIFI = 0xE701, IC_VPN = 0xE705, IC_NETWORK = 0xE968, IC_FOLDER = 0xE8B7,
    IC_UP = 0xE70E, IC_DOWN = 0xE70D, IC_SEARCH = 0xE721, IC_OUT = 0xE72A, IC_LOCK = 0xE72E,
    IC_DELETE = 0xE74D, IC_INFO = 0xE946, IC_PLAY = 0xE768, IC_STOP = 0xE71A, IC_TEST = 0xE9D9,
    IC_LIST = 0xE8FD,
};

static std::string Ic(unsigned cp) {
    char b[3] = { (char)(0xE0 | (cp >> 12)), (char)(0x80 | ((cp >> 6) & 0x3F)), (char)(0x80 | (cp & 0x3F)) };
    return std::string(b, 3);
}

static const char* ELL = "\xE2\x80\xA6";   // ellipsis
static const char* DOT = "  \xC2\xB7  ";   // middle dot separator

// first font file that exists in %WINDIR%\Fonts, with the icon font merged in
static ImFont* AddUiFont(std::initializer_list<const char*> files, float size) {
    ImGuiIO& io = ImGui::GetIO();
    char win[MAX_PATH] = { 0 };
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string dir = std::string(win) + "\\Fonts\\";
    ImFont* f = nullptr;
    for (const char* file : files) {
        std::string p = dir + file;
        if (GetFileAttributesA(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if ((f = io.Fonts->AddFontFromFileTTF(p.c_str(), size)) != nullptr) break;
    }
    if (!f) {
        ImFontConfig c;
        c.SizePixels = size;
        f = io.Fonts->AddFontDefault(&c);
    }
    std::string icons = dir + "SegoeIcons.ttf";
    if (GetFileAttributesA(icons.c_str()) == INVALID_FILE_ATTRIBUTES) icons = dir + "segmdl2.ttf";
    if (GetFileAttributesA(icons.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ImFontConfig c;
        c.MergeMode = true;
        c.GlyphOffset = ImVec2(0.0f, size * 0.14f);   // the icon font sits higher than Segoe UI
        io.Fonts->AddFontFromFileTTF(icons.c_str(), size, &c);
    }
    return f;
}

static void ApplyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 10.0f;
    s.FrameRounding = 8.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 6.0f;
    s.ScrollbarRounding = 6.0f;
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.WindowPadding = ImVec2(10, 8);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(8, 6);
    s.ItemInnerSpacing = ImVec2(6, 5);
    s.ScrollbarSize = 8.0f;
    s.GrabMinSize = 10.0f;

    auto V = [](ImU32 u) { return ImGui::ColorConvertU32ToFloat4(u); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                 = V(K_TEXT);
    c[ImGuiCol_TextDisabled]         = V(K_DIM);
    c[ImGuiCol_WindowBg]             = V(K_BG);
    c[ImGuiCol_ChildBg]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]              = V(K_SURF2);
    c[ImGuiCol_Border]               = V(K_LINE);
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = V(K_SUNK);
    c[ImGuiCol_FrameBgHovered]       = V(K_SUNK);
    c[ImGuiCol_FrameBgActive]        = V(K_SUNK);
    c[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]        = V(K_LINE);
    c[ImGuiCol_ScrollbarGrabHovered] = V(K_LINE2);
    c[ImGuiCol_ScrollbarGrabActive]  = V(K_ACC);
    c[ImGuiCol_CheckMark]            = V(K_ACCTX);
    c[ImGuiCol_Button]               = V(K_SURF2);
    c[ImGuiCol_ButtonHovered]        = V(K_LINE2);
    c[ImGuiCol_ButtonActive]         = V(K_ACC2);
    c[ImGuiCol_Header]               = V(K_SURF2);
    c[ImGuiCol_HeaderHovered]        = V(K_LINE);
    c[ImGuiCol_HeaderActive]         = V(K_LINE2);
    c[ImGuiCol_Separator]            = V(K_LINE);
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]    = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripActive]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.43f, 0.16f, 0.85f, 0.60f);
    c[ImGuiCol_NavCursor]            = V(K_ACC);

    s.ScaleAllSizes(s_dpi);
    s.FontScaleDpi = s_dpi;
}

// ---------------------------------------------------------------- widgets ---
static ImVec2 TSize(ImFont* f, float sz, const char* t) { return f->CalcTextSizeA(S(sz), FLT_MAX, 0.0f, t); }
static void TDraw(ImDrawList* dl, ImFont* f, float sz, ImVec2 p, ImU32 c, const char* t) { dl->AddText(f, S(sz), p, c, t); }

static void PopUtf8(std::string& s) {
    while (!s.empty() && ((unsigned char)s.back() & 0xC0) == 0x80) s.pop_back();
    if (!s.empty()) s.pop_back();
}

static std::string Elide(ImFont* f, float sz, const std::string& s, float maxW) {
    if (maxW <= S(10)) return std::string();
    if (TSize(f, sz, s.c_str()).x <= maxW) return s;
    std::string out = s;
    while (!out.empty() && TSize(f, sz, (out + ELL).c_str()).x > maxW) PopUtf8(out);
    return out + ELL;
}

// shortens a path in the middle so the file name stays visible
static std::string ElideMiddle(ImFont* f, float sz, const std::string& s, float maxW) {
    static std::map<std::string, std::string> cache;
    std::string key = s + "|" + std::to_string((int)maxW) + "|" + std::to_string((int)(sz * 10));
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 400) cache.clear();
    std::string res = s;
    if (TSize(f, sz, s.c_str()).x > maxW) {
        size_t cut = s.find_last_of("\\/");
        std::string head = (cut == std::string::npos) ? s : s.substr(0, cut);
        std::string tail = (cut == std::string::npos) ? std::string() : s.substr(cut);
        while (head.size() > 3 && TSize(f, sz, (head + ELL + tail).c_str()).x > maxW) PopUtf8(head);
        res = (head.size() > 3) ? head + ELL + tail : Elide(f, sz, s, maxW);
    }
    cache[key] = res;
    return res;
}

static bool IconButton(const char* id, unsigned icon, ImVec2 size, const char* tip,
                       bool framed = false, bool active = false, bool danger = false) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    bool clicked = ImGui::InvisibleButton(id, size);
    bool hov = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 fg = active ? K_ACCTX : K_SUB;
    if (framed) {
        dl->AddRectFilled(p, p + size, hov ? K_SURF2 : K_SURF, S(7));
        dl->AddRect(p, p + size, hov ? K_LINE2 : K_LINE, S(7));
    }
    if (hov || held) {
        if (!framed) dl->AddRectFilled(p, p + size, danger ? K_DANGER : (held ? K_LINE2 : K_SURF2), S(7));
        fg = danger ? K_WHITE : K_TEXT;
    }
    std::string g = Ic(icon);
    ImVec2 ts = TSize(s_fUi, 13.0f, g.c_str());
    TDraw(dl, s_fUi, 13.0f, ImVec2(p.x + (size.x - ts.x) * 0.5f, p.y + (size.y - ts.y) * 0.5f), fg, g.c_str());
    if (tip) ImGui::SetItemTooltip("%s", tip);
    return clicked;
}

enum BtnKind { BTN_NORMAL, BTN_PRIMARY };

static float ActionButtonWidth(unsigned icon, const char* label) {
    return S(11) * 2 + TSize(s_fUi, 12.5f, Ic(icon).c_str()).x + S(6) + TSize(s_fSemi, 12.5f, label).x;
}

static bool ActionButton(const char* id, unsigned icon, const char* label, bool enabled, BtnKind kind = BTN_NORMAL) {
    std::string g = Ic(icon);
    ImVec2 gs = TSize(s_fUi, 12.5f, g.c_str()), ls = TSize(s_fSemi, 12.5f, label);
    ImVec2 size(ActionButtonWidth(icon, label), S(28));
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (!enabled) ImGui::BeginDisabled();
    bool clicked = ImGui::InvisibleButton(id, size);
    if (!enabled) ImGui::EndDisabled();
    bool hov = enabled && ImGui::IsItemHovered(), held = enabled && ImGui::IsItemActive();
    ImU32 bg, brd, fg, gc;
    if (!enabled)                 { bg = K_SURF; brd = K_LINE; fg = K_DIM; gc = K_DIM; }
    else if (kind == BTN_PRIMARY) { bg = held ? K_ACC2 : (hov ? K_ACCHI : K_ACC); brd = bg; fg = K_WHITE; gc = K_WHITE; }
    else                          { bg = held ? K_LINE : (hov ? K_SURF2 : K_SURF); brd = hov ? K_LINE2 : K_LINE; fg = K_TEXT; gc = K_ACCTX; }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, p + size, bg, S(8));
    dl->AddRect(p, p + size, brd, S(8));
    TDraw(dl, s_fUi, 12.5f, ImVec2(p.x + S(11), p.y + (size.y - gs.y) * 0.5f), gc, g.c_str());
    TDraw(dl, s_fSemi, 12.5f, ImVec2(p.x + S(11) + gs.x + S(6), p.y + (size.y - ls.y) * 0.5f), fg, label);
    return clicked && enabled;
}

static ImVec4 ChipTone(const std::string& v) {
    if (v == "IN") return V_OK;
    if (v == "OUT" || v == "OUT proxy" || v == "OUT real") return V_OUT;
    if (v == "MIXED") return V_MIX;
    return V_DIM;
}

// rounded status chip: IN / OUT / OUT proxy / OUT real / MIXED / idle
static void Chip(ImDrawList* dl, ImVec2 p, const std::string& v) {
    ImVec4 tone = ChipTone(v);
    ImVec2 ts = TSize(s_fSemi, 11.0f, v.c_str());
    float h = S(20), w = ts.x + S(24);
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Alpha(tone, 0.12f), h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), Alpha(tone, 0.34f), h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + S(9), p.y + h * 0.5f), S(2.6f), Col(tone));
    TDraw(dl, s_fSemi, 11.0f, ImVec2(p.x + S(15), p.y + (h - ts.y) * 0.5f), Col(tone), v.c_str());
}

// a wrapping row of coloured dots with labels; a transparent colour means no dot
static void Legend(const std::vector<std::pair<ImVec4, std::string>>& items) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 start = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    float lh = S(18), x = start.x, y = start.y;
    for (const auto& it : items) {
        float dotW = (it.first.w > 0.0f) ? S(12) : 0.0f;
        float w = dotW + TSize(s_fUi, 11.5f, it.second.c_str()).x + S(14);
        if (x > start.x && (x - start.x) + w > avail) { x = start.x; y += lh; }
        if (dotW > 0.0f) dl->AddCircleFilled(ImVec2(x + S(4), y + lh * 0.5f), S(3.0f), Col(it.first));
        TDraw(dl, s_fUi, 11.5f, ImVec2(x + dotW, y + (lh - TSize(s_fUi, 11.5f, "A").y) * 0.5f), K_DIM, it.second.c_str());
        x += w;
    }
    ImGui::Dummy(ImVec2(avail, (y - start.y) + lh));
}

static void Wrapped(ImFont* f, float sz, ImU32 col, const char* text) {
    ImGui::PushFont(f, sz);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

static void SectionHead(unsigned icon, const char* title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string g = Ic(icon);
    TDraw(dl, s_fUi, 14.0f, ImVec2(p.x, p.y + S(1)), K_ACC, g.c_str());
    TDraw(dl, s_fSemi, 14.0f, ImVec2(p.x + S(22), p.y), K_TEXT, title);
    ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, S(22)));
}

static void StatusPill(bool on, const std::string& text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec4 tone = on ? V_OK : V_DIM;
    ImVec2 ts = TSize(s_fSemi, 12.0f, text.c_str());
    float h = S(26), w = ts.x + S(30);
    dl->AddRectFilled(p, p + ImVec2(w, h), Alpha(tone, 0.10f), h * 0.5f);
    dl->AddRect(p, p + ImVec2(w, h), Alpha(tone, 0.32f), h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + S(13), p.y + h * 0.5f), S(3.5f), Col(tone));
    TDraw(dl, s_fSemi, 12.0f, ImVec2(p.x + S(22), p.y + (h - ts.y) * 0.5f), on ? K_TEXT : K_SUB, text.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

static void Note(const char* text) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    std::string g = Ic(IC_INFO);
    TDraw(dl, s_fUi, 12.0f, ImVec2(p.x, p.y + S(1)), K_DIM, g.c_str());
    ImGui::Indent(S(20));
    Wrapped(s_fUi, 11.5f, K_DIM, text);
    ImGui::Unindent(S(20));
}

// a command in a sunken box; the copy button copies the full command line
static void CommandBox(const char* id, const char* shown, const std::string& full, const char* tip) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(32);
    dl->AddRectFilled(p, p + ImVec2(w, h), K_SUNK, S(8));
    dl->AddRect(p, p + ImVec2(w, h), K_LINE, S(8));
    ImVec2 ps = TSize(s_fMono, 12.0f, ">");
    TDraw(dl, s_fMono, 12.0f, ImVec2(p.x + S(10), p.y + (h - ps.y) * 0.5f), K_ACC, ">");
    std::string s = Elide(s_fMono, 12.0f, shown, w - S(66));
    TDraw(dl, s_fMono, 12.0f, ImVec2(p.x + S(24), p.y + (h - ps.y) * 0.5f), K_TEXT, s.c_str());
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - S(30), p.y + S(3)));
    ImGui::PushID(id);
    if (IconButton("##copy", IC_COPY, ImVec2(S(26), S(26)), "copy the full command")) {
        ImGui::SetClipboardText(full.c_str());
        Log("ok copied: %s", shown);
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(p);
    ImGui::InvisibleButton(id, ImVec2(w - S(34), h));
    if (tip) ImGui::SetItemTooltip("%s", tip);
    ImGui::Dummy(ImVec2(0, S(1)));
}

// ----------------------------------------------------------------- header ---
static void DrawHeader() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetWindowPos();
    float w = ImGui::GetWindowWidth(), h = S(kHeaderHeight);
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + w, p0.y + h),
                                IM_COL32(0x26, 0x1B, 0x40, 255), IM_COL32(0x15, 0x10, 0x21, 255),
                                IM_COL32(0x15, 0x10, 0x21, 255), IM_COL32(0x26, 0x1B, 0x40, 255));
    dl->AddLine(ImVec2(p0.x, p0.y + h - 1), ImVec2(p0.x + w, p0.y + h - 1), K_LINE);

    float b = S(26);
    ImVec2 bp(p0.x + S(12), p0.y + (h - b) * 0.5f);
    dl->AddRectFilled(bp - ImVec2(S(3), S(3)), bp + ImVec2(b + S(3), b + S(3)), IM_COL32(0x8B, 0x5C, 0xF6, 30), S(11));
    dl->AddRectFilled(bp, bp + ImVec2(b, b), K_ACC2, S(8));
    dl->AddRectFilled(bp, bp + ImVec2(b, b * 0.55f), K_ACC, S(8), ImDrawFlags_RoundCornersTop);
    dl->AddRect(bp, bp + ImVec2(b, b), IM_COL32(0xA7, 0x86, 0xFA, 160), S(8));
    std::string sh = Ic(IC_SHIELD);
    ImVec2 ss = TSize(s_fUi, 13.0f, sh.c_str());
    TDraw(dl, s_fUi, 13.0f, bp + ImVec2((b - ss.x) * 0.5f, (b - ss.y) * 0.5f), K_WHITE, sh.c_str());

    float tx = bp.x + b + S(10);
    ImVec2 ts = TSize(s_fSemi, 15.0f, "xvpn");
    TDraw(dl, s_fSemi, 15.0f, ImVec2(tx, p0.y + (h - ts.y) * 0.5f), K_TEXT, "xvpn");
    std::string sub = "split tunneling";
    if (!s_network.nicAlias.empty()) sub += DOT + s_network.nicAlias;
    float subX = tx + ts.x + S(10);
    sub = Elide(s_fUi, 12.0f, sub, (p0.x + w - S(kWindowButtonsWidth)) - subX - S(8));
    ImVec2 us = TSize(s_fUi, 12.0f, sub.c_str());
    TDraw(dl, s_fUi, 12.0f, ImVec2(subX, p0.y + (h - us.y) * 0.5f + S(1)), K_DIM, sub.c_str());

    ImVec2 bs(S(36), S(30));
    ImGui::SetCursorScreenPos(ImVec2(p0.x + w - S(8) - bs.x * 3 - S(4), p0.y + (h - bs.y) * 0.5f));
    if (IconButton("##pin", s_topmost ? IC_PINNED : IC_PIN, bs, s_topmost ? "stop keeping on top" : "keep on top", false, s_topmost)) {
        s_topmost = !s_topmost;
        SetWindowPos(s_hwnd, s_topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
    }
    ImGui::SameLine(0, S(2));
    if (IconButton("##min", IC_MIN, bs, nullptr)) ShowWindow(s_hwnd, SW_MINIMIZE);
    ImGui::SameLine(0, S(2));
    if (IconButton("##close", IC_CLOSE, bs, nullptr, false, false, true)) PostMessageW(s_hwnd, WM_CLOSE, 0, 0);
}

// ----------------------------------------------------------- status panel ---
static void DrawStatus() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(64);
    const NetState& n = s_network;
    bool ok = n.vpnUp;
    ImVec4 tone = ok ? V_OK : V_BAD;

    dl->AddRectFilled(p, p + ImVec2(w, h), K_SURF, S(12));
    dl->PushClipRect(p + ImVec2(S(1), S(1)), p + ImVec2(w * 0.6f, h - S(1)), true);
    dl->AddRectFilledMultiColor(p, p + ImVec2(w * 0.6f, h), Alpha(tone, 0.10f), Alpha(tone, 0.0f), Alpha(tone, 0.0f), Alpha(tone, 0.10f));
    dl->PopClipRect();
    dl->AddRect(p, p + ImVec2(w, h), K_LINE, S(12));

    // status orb, pulsing while the tunnel is up
    ImVec2 c(p.x + S(32), p.y + h * 0.5f);
    float r = S(18);
    if (ok) {
        float t = (float)fmod(ImGui::GetTime(), 2.4) / 2.4f;
        dl->AddCircle(c, r + S(9) * t, Alpha(tone, 0.45f * (1.0f - t)), 0, S(1.5f));
    }
    dl->AddCircleFilled(c, r, Alpha(tone, 0.14f));
    dl->AddCircle(c, r, Alpha(tone, 0.55f), 0, S(1.2f));
    std::string g = Ic(ok ? IC_SHIELD : IC_WIFI);
    ImVec2 gs = TSize(s_fUi, 16.0f, g.c_str());
    TDraw(dl, s_fUi, 16.0f, c - gs * 0.5f, Col(tone), g.c_str());

    // right: egress address and a button to measure it again
    const EgressState& e = s_egress;
    ImVec2 rb(S(28), S(28));
    ImVec2 rbp(p.x + w - S(12) - rb.x, p.y + (h - rb.y) * 0.5f);
    std::string ev = e.ip.empty() ? (e.busy ? std::string("measuring") + ELL : std::string("unknown")) : e.ip;
    ImVec2 evs = TSize(s_fMono, 16.0f, ev.c_str());
    const char* via = "";
    if (!e.localIp.empty() && e.localIp == n.vpnIp)      via = "via the tunnel";
    else if (!e.localIp.empty() && e.localIp == n.nicIp) via = "via the real network";
    const char* cap = "EGRESS IP";
    ImVec2 cs = TSize(s_fSemi, 10.0f, cap);
    float rightW = std::max(std::max(evs.x, TSize(s_fUi, 11.0f, via).x), cs.x);
    float rx = rbp.x - S(10) - rightW;
    TDraw(dl, s_fSemi, 10.0f, ImVec2(rbp.x - S(10) - cs.x, p.y + S(10)), K_DIM, cap);
    TDraw(dl, s_fMono, 16.0f, ImVec2(rbp.x - S(10) - evs.x, p.y + S(23)), e.ip.empty() ? K_DIM : Col(V_CYAN), ev.c_str());
    if (*via) {
        ImVec2 vs = TSize(s_fUi, 11.0f, via);
        TDraw(dl, s_fUi, 11.0f, ImVec2(rbp.x - S(10) - vs.x, p.y + S(43)), K_DIM, via);
    }
    ImGui::SetCursorScreenPos(rbp);
    if (IconButton("##egress", IC_REFRESH, rb, e.busy ? "measuring" : "measure the egress IP again", true)) {
        StartEgressProbe();
        s_lastEgress = ImGui::GetTime();
    }

    // left: headline and detail, shortened so they never run into the egress block
    float lx = p.x + S(62), room = rx - lx - S(12);
    std::string title = ok ? ("Protected by " + (n.vpnAlias.empty() ? std::string("the VPN") : n.vpnAlias))
                           : std::string("VPN is off");
    std::string detail = ok ? ("tunnel " + n.vpnIp + DOT + std::to_string(n.vpnRoutes) + " routes" + DOT + "if " + std::to_string(n.vpnIf))
                            : ("all traffic uses " + (n.nicAlias.empty() ? std::string("the real network") : n.nicAlias));
    TDraw(dl, s_fSemi, 15.0f, ImVec2(lx, p.y + S(13)), K_TEXT, Elide(s_fSemi, 15.0f, title, room).c_str());
    TDraw(dl, s_fUi, 12.0f, ImVec2(lx, p.y + S(35)), K_SUB, Elide(s_fUi, 12.0f, detail, room).c_str());

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
}

// -------------------------------------------------------------------- nav ---
struct NavItem { unsigned icon; const char* label; std::string badge; };

static void DrawNav(const std::vector<NavItem>& items) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(36);
    dl->AddRectFilled(p, p + ImVec2(w, h), K_SUNK, S(10));
    dl->AddRect(p, p + ImVec2(w, h), K_LINE, S(10));
    int count = (int)items.size();
    float iw = (w - S(8)) / count;
    for (int i = 0; i < count; ++i) {
        const NavItem& it = items[i];
        ImVec2 ip(p.x + S(4) + i * iw, p.y + S(4));
        ImVec2 is(iw - (i < count - 1 ? S(3) : 0.0f), h - S(8));
        ImGui::SetCursorScreenPos(ip);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("nav", is)) s_tab = i;
        bool hov = ImGui::IsItemHovered();
        ImGui::PopID();
        bool on = (s_tab == i);
        if (on) {
            dl->AddRectFilled(ip, ip + is, IM_COL32(0x8B, 0x5C, 0xF6, 44), S(7));
            dl->AddRect(ip, ip + is, IM_COL32(0x8B, 0x5C, 0xF6, 120), S(7));
        } else if (hov) {
            dl->AddRectFilled(ip, ip + is, K_SURF, S(7));
        }
        // icon, label and badge; the badge and then the label are dropped when space runs out
        std::string g = Ic(it.icon);
        ImVec2 gs = TSize(s_fUi, 13.0f, g.c_str());
        ImVec2 ls = TSize(on ? s_fSemi : s_fUi, 12.5f, it.label);
        float bw = it.badge.empty() ? 0.0f : TSize(s_fSemi, 10.0f, it.badge.c_str()).x + S(10);
        bool showBadge = !it.badge.empty(), showLabel = true;
        float cw = gs.x + S(6) + ls.x + (showBadge ? S(5) + bw : 0.0f);
        if (cw > is.x - S(10)) { showBadge = false; cw = gs.x + S(6) + ls.x; }
        if (cw > is.x - S(10)) { showLabel = false; cw = gs.x; }
        float x = ip.x + (is.x - cw) * 0.5f, cy = ip.y + is.y * 0.5f;
        TDraw(dl, s_fUi, 13.0f, ImVec2(x, cy - gs.y * 0.5f), on ? K_ACCTX : (hov ? K_SUB : K_DIM), g.c_str());
        x += gs.x + S(6);
        if (showLabel) {
            TDraw(dl, on ? s_fSemi : s_fUi, 12.5f, ImVec2(x, cy - ls.y * 0.5f), (on || hov) ? K_TEXT : K_SUB, it.label);
            x += ls.x + S(5);
        }
        if (showBadge) {
            ImVec2 bs = TSize(s_fSemi, 10.0f, it.badge.c_str());
            float bh = S(16);
            ImVec2 bp(x, cy - bh * 0.5f);
            dl->AddRectFilled(bp, bp + ImVec2(bw, bh), on ? K_ACC : K_SURF2, bh * 0.5f);
            TDraw(dl, s_fSemi, 10.0f, ImVec2(bp.x + (bw - bs.x) * 0.5f, bp.y + (bh - bs.y) * 0.5f), on ? K_WHITE : K_SUB, it.badge.c_str());
        }
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
}

// -------------------------------------------------------------- dashboard ---
struct Card { unsigned icon; ImVec4 tint; const char* label; std::string value; ImU32 valueCol; std::string detail; };

static void DrawCard(ImVec2 pos, ImVec2 size, const Card& cd) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool hov = ImGui::IsMouseHoveringRect(pos, pos + size) && ImGui::IsWindowHovered();
    dl->AddRectFilled(pos, pos + size, hov ? K_SURF2 : K_SURF, S(10));
    dl->AddRect(pos, pos + size, hov ? K_LINE2 : K_LINE, S(10));

    float t = S(32);
    ImVec2 tp(pos.x + S(11), pos.y + (size.y - t) * 0.5f);
    dl->AddRectFilled(tp, tp + ImVec2(t, t), Alpha(cd.tint, 0.13f), S(8));
    std::string g = Ic(cd.icon);
    ImVec2 gs = TSize(s_fUi, 15.0f, g.c_str());
    TDraw(dl, s_fUi, 15.0f, tp + ImVec2((t - gs.x) * 0.5f, (t - gs.y) * 0.5f), Col(cd.tint), g.c_str());

    float x = tp.x + t + S(11), room = pos.x + size.x - S(10) - x;
    TDraw(dl, s_fSemi, 10.0f, ImVec2(x, pos.y + S(9)), K_DIM, cd.label);
    TDraw(dl, s_fSemi, 14.0f, ImVec2(x, pos.y + S(21)), cd.valueCol, Elide(s_fSemi, 14.0f, cd.value, room).c_str());
    std::string d = Elide(s_fUi, 11.5f, cd.detail, room);
    TDraw(dl, s_fUi, 11.5f, ImVec2(x, pos.y + S(41)), K_DIM, d.c_str());
    if (hov && d != cd.detail) ImGui::SetTooltip("%s", cd.detail.c_str());
}

static void DrawDashboard() {
    const NetState& n = s_network;
    const EgressState& e = s_egress;
    const KitState& k = s_kit;
    std::vector<Card> cards;
    cards.push_back({ IC_VPN, n.vpnUp ? V_OK : V_BAD, "VPN TUNNEL",
                      n.vpnUp ? n.vpnIp : std::string("not connected"), n.vpnUp ? K_TEXT : Col(V_BAD),
                      n.vpnUp ? (n.vpnAlias + DOT + "if " + std::to_string(n.vpnIf) + DOT + std::to_string(n.vpnRoutes) + " routes")
                              : std::string("no VPN tunnel detected") });
    cards.push_back({ IC_WIFI, V_LILAC, "REAL NETWORK",
                      n.nicIp.empty() ? std::string("none") : n.nicIp, K_TEXT,
                      (n.nicAlias.empty() ? std::string("no physical adapter") : n.nicAlias) + (n.nicGw.empty() ? "" : DOT + ("gw " + n.nicGw)) });
    {
        std::string d = "asking 1.0.0.1";
        if (!e.ip.empty() && !e.localIp.empty()) {
            d = "from " + e.localIp;
            if (!n.vpnIp.empty() && e.localIp == n.vpnIp)      d += " (tunnel)";
            else if (!n.nicIp.empty() && e.localIp == n.nicIp) d += " (real network)";
        }
        cards.push_back({ IC_GLOBE, V_CYAN, "EGRESS",
                          e.ip.empty() ? (e.busy ? std::string("measuring") + ELL : std::string("unknown")) : e.ip, Col(V_CYAN), d });
    }
    cards.push_back({ IC_NETWORK, k.proxyUp ? V_OK : V_DIM, "BYPASS PROXY",
                      k.proxyUp ? ("port " + std::to_string(k.proxyPort)) : std::string("not running"), k.proxyUp ? K_TEXT : K_SUB,
                      k.proxyUp ? ("pid " + std::to_string(k.proxyPid) + DOT + "OUT apps use it")
                                : std::string("starts when an app is sent OUT") });
    cards.push_back({ IC_FILTER, k.exceptions ? V_OUT : V_ACCENT, "EXCEPTIONS",
                      std::to_string(k.exceptions) + " installed", K_TEXT,
                      k.exceptions ? std::string("destination routes are active") : std::string("no destination routes") });
    cards.push_back({ IC_FOLDER, V_LILAC, "DATA", "portable", K_TEXT, DataDir() });

    ImVec2 start = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = (avail < S(380)) ? 1 : 2;
    float gap = S(8), cw = (avail - gap * (cols - 1)) / cols, ch = S(60);
    for (int i = 0; i < (int)cards.size(); ++i)
        DrawCard(ImVec2(start.x + (i % cols) * (cw + gap), start.y + (i / cols) * (ch + gap)), ImVec2(cw, ch), cards[i]);
    int rows = ((int)cards.size() + cols - 1) / cols;
    ImGui::Dummy(ImVec2(avail, rows * (ch + gap)));
    Legend({ { V_OK, "IN  inside the VPN" }, { V_OUT, "OUT  real network" }, { V_MIX, "MIXED  both" }, { V_DIM, "idle" } });
}

// ------------------------------------------------------------------- apps ---
static bool ContainsI(const std::string& hay, const char* needle) {
    return !*needle || Lower(hay).find(Lower(needle)) != std::string::npos;
}

static ImU32 AvatarTone(const std::string& name) {
    static const ImU32 tones[] = { IM_COL32(0x8B, 0x5C, 0xF6, 255), IM_COL32(0x22, 0xD3, 0xEE, 255), IM_COL32(0xF4, 0x72, 0xB6, 255),
                                   IM_COL32(0xFB, 0xBF, 0x24, 255), IM_COL32(0x34, 0xD3, 0x99, 255), IM_COL32(0x81, 0x8C, 0xF8, 255) };
    unsigned h = 2166136261u;
    for (char ch : name) h = (h ^ (unsigned char)tolower((unsigned char)ch)) * 16777619u;
    return tones[h % 6];
}

static bool IsBrowser(const std::string& exe) {
    std::string n = Lower(exe);
    return n == "chrome.exe" || n == "msedge.exe" || n == "brave.exe" || n == "vivaldi.exe" || n == "opera.exe" || n == "chromium.exe";
}

static void SendApp(const AppRow& a, bool out) {
    std::string what = "sending " + a.name + (out ? " OUT" : " IN");
    if (IsBrowser(a.name)) what += " - a running browser restarts on the new side";
    RunEngine("app-chooser.ps1", "-Send \"" + a.path + "\" -Mode " + (out ? "Real" : "Vpn"), what);
    s_logOpen = true;
}

static void DrawApps() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float avail = ImGui::GetContentRegionAvail().x;

    float fh = S(30);
    ImGui::PushFont(s_fUi, 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), (fh - ImGui::GetFontSize()) * 0.5f));
    ImGui::SetNextItemWidth(avail - fh - S(6));
    std::string hint = Ic(IC_SEARCH) + "   filter apps";
    ImGui::InputTextWithHint("##filter", hint.c_str(), s_appFilter, sizeof(s_appFilter));
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::SameLine(0, S(6));
    if (IconButton("##rescan", IC_REFRESH, ImVec2(fh, fh), "rescan now", true)) s_appsDirty = true;

    ImVec2 hp = ImGui::GetCursorScreenPos();
    const float liveW = S(96), modeW = S(58);
    float liveX = hp.x + avail - S(8) - liveW, modeX = liveX - modeW;
    TDraw(dl, s_fSemi, 10.0f, ImVec2(hp.x + S(10), hp.y + S(2)), K_DIM, "APP");
    TDraw(dl, s_fSemi, 10.0f, ImVec2(modeX, hp.y + S(2)), K_DIM, "MODE");
    TDraw(dl, s_fSemi, 10.0f, ImVec2(liveX, hp.y + S(2)), K_DIM, "LIVE");
    ImGui::Dummy(ImVec2(avail, S(16)));

    const AppRow* sel = nullptr;
    for (const auto& a : s_apps)
        if (a.path == s_appSel) sel = &a;

    float listH = std::max(ImGui::GetContentRegionAvail().y - S(44), S(60));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(K_SUNK));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(4), S(4)));
    ImGui::BeginChild("applist", ImVec2(avail, listH), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    {
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        int shown = 0;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, S(2)));
        for (const auto& a : s_apps) {
            if (!ContainsI(a.name, s_appFilter) && !ContainsI(a.path, s_appFilter)) continue;
            shown++;
            ImVec2 rp = ImGui::GetCursorScreenPos();
            float rw = ImGui::GetContentRegionAvail().x, rh = S(40);
            ImGui::PushID(a.path.c_str());
            if (ImGui::InvisibleButton("row", ImVec2(rw, rh))) s_appSel = (s_appSel == a.path) ? std::string() : a.path;
            bool hov = ImGui::IsItemHovered();
            ImGui::SetItemTooltip("%s\nconnections: %d in the tunnel, %d on the real network, %d through the proxy",
                                  a.path.c_str(), a.vpn, a.real, a.proxy);
            ImGui::PopID();
            if (a.path == s_appSel) {
                ldl->AddRectFilled(rp, rp + ImVec2(rw, rh), IM_COL32(0x8B, 0x5C, 0xF6, 38), S(7));
                ldl->AddRectFilled(rp + ImVec2(0, S(8)), rp + ImVec2(S(3), rh - S(8)), K_ACC, S(2));
            } else if (hov) {
                ldl->AddRectFilled(rp, rp + ImVec2(rw, rh), K_SURF2, S(7));
            }
            ImVec2 ac(rp.x + S(22), rp.y + rh * 0.5f);
            ImU32 tone = AvatarTone(a.name);
            ldl->AddCircleFilled(ac, S(13), Alpha(ImGui::ColorConvertU32ToFloat4(tone), 0.18f));
            char initial[2] = { (char)toupper((unsigned char)(a.name.empty() ? '?' : a.name[0])), 0 };
            ImVec2 is = TSize(s_fSemi, 13.0f, initial);
            TDraw(ldl, s_fSemi, 13.0f, ac - is * 0.5f, tone, initial);

            float lx = rp.x + S(44);
            float chipX = rp.x + rw - S(4) - liveW - modeW;
            float room = chipX - lx - S(8);
            TDraw(ldl, s_fSemi, 13.0f, ImVec2(lx, rp.y + S(4)), K_TEXT, Elide(s_fSemi, 13.0f, a.name, room).c_str());
            TDraw(ldl, s_fUi, 11.0f, ImVec2(lx, rp.y + S(22)), K_DIM, ElideMiddle(s_fUi, 11.0f, a.path, room).c_str());
            float cy = rp.y + (rh - S(20)) * 0.5f;
            Chip(ldl, ImVec2(chipX, cy), a.mode);
            Chip(ldl, ImVec2(rp.x + rw - S(4) - liveW, cy), a.live);
        }
        ImGui::PopStyleVar();
        if (shown == 0) {
            const char* msg = s_appFilter[0] ? "no app matches the filter" : "no app has a connection right now";
            ImVec2 ms = TSize(s_fUi, 12.5f, msg);
            ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
            TDraw(ldl, s_fUi, 12.5f, ImVec2(wp.x + (ws.x - ms.x) * 0.5f, wp.y + (ws.y - ms.y) * 0.5f), K_DIM, msg);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // footer: actions for the selected app, otherwise the legend
    ImGui::Dummy(ImVec2(0, S(2)));
    if (sel) {
        AppRow a = *sel;
        bool idle = !EngineBusy();
        const char* outLabel = idle ? "Send OUT" : "working";
        float btnW = ActionButtonWidth(IC_COPY, "Copy path") + ActionButtonWidth(IC_OUT, outLabel) +
                     ActionButtonWidth(IC_LOCK, "Keep IN") + S(12);
        ImVec2 fp = ImGui::GetCursorScreenPos();
        std::string name = Elide(s_fSemi, 12.5f, a.name, avail - btnW - S(12));
        ImVec2 ns = TSize(s_fSemi, 12.5f, name.c_str());
        TDraw(dl, s_fSemi, 12.5f, ImVec2(fp.x + S(2), fp.y + (S(28) - ns.y) * 0.5f), K_TEXT, name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(fp.x + avail - btnW, fp.y));
        if (ActionButton("##copy", IC_COPY, "Copy path", true)) {
            ImGui::SetClipboardText(a.path.c_str());
            Log("ok copied %s", a.path.c_str());
        }
        ImGui::SameLine(0, S(6));
        if (ActionButton("##out", IC_OUT, outLabel, idle, BTN_PRIMARY)) SendApp(a, true);
        ImGui::SameLine(0, S(6));
        if (ActionButton("##in", IC_LOCK, "Keep IN", idle)) SendApp(a, false);
    } else {
        ImGui::Dummy(ImVec2(0, S(4)));
        std::string count = std::to_string(s_apps.size()) + " apps" + DOT + "select one to send it OUT or keep it IN";
        Legend({ { V_OK, "IN" }, { V_OUT, "OUT" }, { V_MIX, "MIXED" }, { ImVec4(0, 0, 0, 0), count } });
    }
}

// ------------------------------------------------------------- exceptions ---
static void DrawExceptions() {
    SectionHead(IC_FILTER, "Destination exceptions");
    Wrapped(s_fUi, 12.5f, K_SUB, "Send traffic for chosen IP addresses, networks or hostnames past the VPN, for every app. "
                                 "The routes are temporary: a reboot or Undo removes them.");
    ImGui::Dummy(ImVec2(0, S(4)));
    StatusPill(s_kit.exceptions > 0, s_kit.exceptions ? (std::to_string(s_kit.exceptions) + " installed") : std::string("none installed"));
    ImGui::SameLine(0, S(8));
    if (ActionButton("##exstatus", IC_LIST, "Show status", !EngineBusy()))
        RunEngine("split-tunnel.ps1", "-Status", "reading installed exceptions");
    ImGui::Dummy(ImVec2(0, S(6)));
    CommandBox("ex1", "split-tunnel.ps1 -Exclude 1.1.1.1,example.com -Apply",
               PsCommand("split-tunnel.ps1", "-Exclude 1.1.1.1,example.com -Apply"), "install exceptions - edit the list first");
    CommandBox("ex2", "split-tunnel.ps1 -Exclude 1.1.1.1 -Apply -DryRun",
               PsCommand("split-tunnel.ps1", "-Exclude 1.1.1.1 -Apply -DryRun"), "preview only, changes nothing");
    CommandBox("ex3", "split-tunnel.ps1 -Undo",
               PsCommand("split-tunnel.ps1", "-Undo"), "remove every exception this tool added");
    ImGui::Dummy(ImVec2(0, S(4)));
    Note("Installing and removing routes needs administrator rights. Copy a command and run it in "
         "an elevated PowerShell window (Terminal (Admin)).");
}

// ------------------------------------------------------------------ proxy ---
static void DrawProxy() {
    SectionHead(IC_NETWORK, "Bypass proxy");
    Wrapped(s_fUi, 12.5f, K_SUB, "A local proxy on 127.0.0.1 whose connections are bound to the physical adapter, "
                                 "so an app that uses it leaves on the real network while everything else stays in the VPN.");
    ImGui::Dummy(ImVec2(0, S(4)));
    StatusPill(s_kit.proxyUp, s_kit.proxyUp
        ? ("running on 127.0.0.1:" + std::to_string(s_kit.proxyPort) + DOT + "pid " + std::to_string(s_kit.proxyPid))
        : std::string("not running"));
    ImGui::Dummy(ImVec2(0, S(6)));
    bool idle = !EngineBusy();
    if (ActionButton("##pxstart", IC_PLAY, "Start", idle && !s_kit.proxyUp, BTN_PRIMARY))
        RunEngine("app-chooser.ps1", "-StartProxy", "starting the bypass proxy");
    ImGui::SameLine(0, S(6));
    if (ActionButton("##pxstop", IC_STOP, "Stop", idle && s_kit.proxyUp))
        RunEngine("app-chooser.ps1", "-StopProxy", "stopping the bypass proxy");
    ImGui::SameLine(0, S(6));
    if (ActionButton("##pxtest", IC_TEST, "Test", idle))
        RunEngine("app-chooser.ps1", "-Probe", "testing the bypass (egress with and without the proxy)");
    ImGui::Dummy(ImVec2(0, S(8)));
    Note("Browsers sent OUT from the Apps tab use this proxy automatically. Other apps can use it if they "
         "have a proxy setting: HTTP proxy 127.0.0.1 on the port shown above.");
}

// -------------------------------------------------------------------- log ---
static ImU32 LogTone(const std::string& msg) {
    if (msg.compare(0, 2, "ok") == 0) return Col(V_OK);
    if (msg.compare(0, 2, "!!") == 0) return Col(V_BAD);
    if (msg.compare(0, 2, "..") == 0) return K_DIM;
    return K_SUB;
}

static void DrawLog(float h) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, hh = S(32);
    dl->AddRectFilled(p, p + ImVec2(w, h), K_SURF, S(10));
    dl->AddRect(p, p + ImVec2(w, h), K_LINE, S(10));
    size_t count = LogCount();

    ImGui::SetCursorScreenPos(p);
    if (ImGui::InvisibleButton("##logtoggle", ImVec2(w - S(38), hh))) s_logOpen = !s_logOpen;
    bool hov = ImGui::IsItemHovered();
    std::string chev = Ic(s_logOpen ? IC_DOWN : IC_UP);
    ImVec2 cs = TSize(s_fUi, 10.0f, chev.c_str());
    TDraw(dl, s_fUi, 10.0f, ImVec2(p.x + S(12), p.y + (hh - cs.y) * 0.5f), hov ? K_TEXT : K_DIM, chev.c_str());
    ImVec2 ts = TSize(s_fSemi, 12.5f, "Activity");
    TDraw(dl, s_fSemi, 12.5f, ImVec2(p.x + S(30), p.y + (hh - ts.y) * 0.5f), K_TEXT, "Activity");
    std::string n = std::to_string(count);
    ImVec2 ns = TSize(s_fSemi, 10.0f, n.c_str());
    ImVec2 bp(p.x + S(30) + ts.x + S(7), p.y + (hh - S(16)) * 0.5f);
    dl->AddRectFilled(bp, bp + ImVec2(ns.x + S(10), S(16)), K_SURF2, S(8));
    TDraw(dl, s_fSemi, 10.0f, bp + ImVec2(S(5), (S(16) - ns.y) * 0.5f), K_SUB, n.c_str());
    if (EngineBusy()) {
        const char* busy = "working";
        ImVec2 bs = TSize(s_fUi, 11.0f, busy);
        TDraw(dl, s_fUi, 11.0f, ImVec2(p.x + w - S(40) - bs.x, p.y + (hh - bs.y) * 0.5f), K_ACCTX, busy);
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - S(34), p.y + S(3)));
    if (IconButton("##logclear", IC_DELETE, ImVec2(S(26), S(26)), "clear the log")) ClearLog();

    if (s_logOpen) {
        dl->AddLine(ImVec2(p.x + S(1), p.y + hh), ImVec2(p.x + w - S(1), p.y + hh), K_LINE);
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y + hh + S(5)));
        ImGui::BeginChild("logscroll", ImVec2(w - S(18), h - hh - S(9)));
        static size_t lastCount = 0;
        bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4);
        ImGui::PushFont(s_fMono, 11.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(10), S(3)));
        const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(K_DIM);
        ForEachLogLine([&](const std::string& l) {
            std::string stamp = l.substr(0, std::min<size_t>(8, l.size()));
            std::string msg = l.size() > 10 ? l.substr(10) : std::string();
            ImGui::TextColored(dim, "%s", stamp.c_str());
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, LogTone(msg));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(msg.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        });
        ImGui::PopStyleVar();
        ImGui::PopFont();
        if ((count != lastCount && atEnd) || TakeLogScroll()) ImGui::SetScrollHereY(1.0f);
        lastCount = count;
        ImGui::EndChild();
    }
    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(w, h));
}

// ----------------------------------------------------------------- public ---
void UiInit(HWND hwnd, float dpi, int startTab) {
    s_hwnd = hwnd;
    s_dpi = dpi;
    s_tab = startTab;
    s_fUi = AddUiFont({ "segoeui.ttf" }, 14.0f);
    s_fSemi = AddUiFont({ "seguisb.ttf", "segoeuib.ttf", "segoeui.ttf" }, 14.0f);
    s_fMono = AddUiFont({ "CascadiaMono.ttf", "consola.ttf" }, 13.0f);
    ApplyStyle();
}

void UiSetDpi(float dpi) {
    s_dpi = dpi;
    s_styleDirty = true;
}

float UiDpi() { return s_dpi; }

void UiPrepare() {
    if (s_styleDirty) {
        ApplyStyle();
        s_styleDirty = false;
    }
}

// refreshes what the views show; network reads are cheap, the egress probe runs in the background
static void Refresh(double t) {
    if (t - s_lastNet > 5.0) {
        s_lastNet = t;
        RefreshNetState();
        s_network = GetNetState();
    }
    if (t - s_lastKit > 1.0) {
        s_lastKit = t;
        s_kit = ReadKitState();
    }
    if (TakeRescanRequest()) s_appsDirty = true;
    if (s_appsDirty || t - s_lastApps > 5.0) {
        s_lastApps = t;
        s_appsDirty = false;
        s_apps = ScanApps(s_network, LoadSavedApps(), InstalledBrowsers(), s_kit.proxyPort);
    }
    if (t - s_lastEgress > 30.0) {
        s_lastEgress = t;
        StartEgressProbe();
    }
    s_egress = GetEgress();
}

void UiFrame() {
    Refresh(ImGui::GetTime());

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                          ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoSavedSettings;
    const ImGuiWindowFlags fixed = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("xvpn", nullptr, wf);
    ImGui::PopStyleVar();
    ImGui::PushFont(s_fUi, 13.0f);

    DrawHeader();

    ImVec2 wp = ImGui::GetWindowPos();
    float W = ImGui::GetWindowWidth(), H = ImGui::GetWindowHeight();
    float pad = S(12), cw = W - pad * 2;
    float y = wp.y + S(kHeaderHeight) + S(10);
    float topH = S(64) + S(8) + S(36);

    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, y));
    ImGui::BeginChild("top", ImVec2(cw, topH), ImGuiChildFlags_None, fixed);
    ImVec2 tp = ImGui::GetCursorScreenPos();
    DrawStatus();
    ImGui::SetCursorScreenPos(ImVec2(tp.x, tp.y + S(64) + S(8)));
    DrawNav({ { IC_HOME, "Dashboard", "" },
              { IC_APPS, "Apps", std::to_string(s_apps.size()) },
              { IC_FILTER, "Exceptions", s_kit.exceptions ? std::to_string(s_kit.exceptions) : std::string() },
              { IC_NETWORK, "Proxy", s_kit.proxyUp ? std::string("on") : std::string() } });
    ImGui::EndChild();
    y += topH + S(10);

    float logH = s_logOpen ? S(122) : S(32);
    float bodyH = std::max((wp.y + H - pad - logH - S(10)) - y, S(60));
    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, y));
    ImGui::BeginChild("body", ImVec2(cw, bodyH), ImGuiChildFlags_None, s_tab == TAB_APPS ? fixed : 0);
    switch (s_tab) {
    case TAB_APPS:       DrawApps(); break;
    case TAB_EXCEPTIONS: DrawExceptions(); break;
    case TAB_PROXY:      DrawProxy(); break;
    default:             DrawDashboard(); break;
    }
    ImGui::EndChild();

    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, y + bodyH + S(10)));
    ImGui::BeginChild("log", ImVec2(cw, logH), ImGuiChildFlags_None, fixed);
    DrawLog(logH);
    ImGui::EndChild();

    ImGui::PopFont();
    ImGui::End();
}
