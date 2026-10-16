#include "ui.h"
#include "engine.h"
#include "net.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>

// ------------------------------------------------------------------ state ---
static HWND  s_hwnd = nullptr;
static float s_dpi = 1.0f;
static bool  s_styleDirty = false;
static bool  s_topmost = false;
static bool  s_logOpen = false;    // opens by itself when something goes wrong
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

static const ImVec4 V_ACCENT(0.53f, 0.45f, 0.80f, 1.00f);
static const ImVec4 V_OK    (0.45f, 0.76f, 0.58f, 1.00f);
static const ImVec4 V_OUT   (0.85f, 0.72f, 0.42f, 1.00f);
static const ImVec4 V_MIX   (0.78f, 0.64f, 0.84f, 1.00f);
static const ImVec4 V_CYAN  (0.53f, 0.76f, 0.81f, 1.00f);
static const ImVec4 V_DIM   (0.56f, 0.53f, 0.64f, 1.00f);
static const ImVec4 V_BAD   (0.85f, 0.47f, 0.52f, 1.00f);
static const ImVec4 V_LILAC (0.68f, 0.65f, 0.84f, 1.00f);

static const ImU32 K_BG     = IM_COL32(0x12, 0x11, 0x17, 255);
static const ImU32 K_SURF   = IM_COL32(0x1A, 0x18, 0x22, 255);
static const ImU32 K_SURF2  = IM_COL32(0x23, 0x21, 0x2E, 255);
static const ImU32 K_SUNK   = IM_COL32(0x0E, 0x0D, 0x12, 255);
static const ImU32 K_LINE   = IM_COL32(0x2B, 0x28, 0x38, 255);
static const ImU32 K_LINE2  = IM_COL32(0x44, 0x3F, 0x5A, 255);
static const ImU32 K_TEXT   = IM_COL32(0xE4, 0xE1, 0xEC, 255);
static const ImU32 K_SUB    = IM_COL32(0xA2, 0x9D, 0xB4, 255);
static const ImU32 K_DIM    = IM_COL32(0x72, 0x6D, 0x84, 255);
static const ImU32 K_ACC    = IM_COL32(0x76, 0x67, 0xC2, 255);
static const ImU32 K_ACC2   = IM_COL32(0x5F, 0x51, 0xA6, 255);
static const ImU32 K_ACCHI  = IM_COL32(0x87, 0x79, 0xCF, 255);
static const ImU32 K_ACCTX  = IM_COL32(0xBC, 0xB4, 0xE4, 255);
static const ImU32 K_WHITE  = IM_COL32(0xFF, 0xFF, 0xFF, 255);
static const ImU32 K_DANGER = IM_COL32(0xB8, 0x3A, 0x4E, 255);

static ImU32 Col(ImVec4 c) { return ImGui::ColorConvertFloat4ToU32(c); }
static ImU32 Alpha(ImVec4 c, float a) { c.w = a; return Col(c); }

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10) code points
enum : unsigned {
    IC_SHIELD = 0xEA18, IC_PIN = 0xE718, IC_PINNED = 0xE842, IC_MIN = 0xE921, IC_CLOSE = 0xE8BB,
    IC_HOME = 0xE80F, IC_APPS = 0xE71D, IC_FILTER = 0xE71C, IC_GLOBE = 0xE774, IC_REFRESH = 0xE72C,
    IC_COPY = 0xE8C8, IC_WIFI = 0xE701, IC_VPN = 0xE705, IC_NETWORK = 0xE968, IC_FOLDER = 0xE8B7,
    IC_UP = 0xE70E, IC_DOWN = 0xE70D, IC_SEARCH = 0xE721, IC_OUT = 0xE72A, IC_LOCK = 0xE72E,
    IC_DELETE = 0xE74D, IC_INFO = 0xE946, IC_PLAY = 0xE768, IC_STOP = 0xE71A, IC_TEST = 0xE9D9,
    IC_LIST = 0xE8FD, IC_HOTSPOT = 0xE704, IC_EYE = 0xE890, IC_ADD = 0xE710, IC_NEXT = 0xE76C,
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
    c[ImGuiCol_TextSelectedBg]       = ImVec4(0.46f, 0.40f, 0.76f, 0.50f);
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

static void StatusPill(bool on, const std::string& text, bool warn = false) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec4 tone = warn ? V_OUT : (on ? V_OK : V_DIM);
    ImVec2 ts = TSize(s_fSemi, 12.0f, text.c_str());
    float h = S(26), w = ts.x + S(30);
    dl->AddRectFilled(p, p + ImVec2(w, h), Alpha(tone, 0.10f), h * 0.5f);
    dl->AddRect(p, p + ImVec2(w, h), Alpha(tone, 0.32f), h * 0.5f);
    dl->AddCircleFilled(ImVec2(p.x + S(13), p.y + h * 0.5f), S(3.5f), Col(tone));
    TDraw(dl, s_fSemi, 12.0f, ImVec2(p.x + S(22), p.y + (h - ts.y) * 0.5f), (on || warn) ? K_TEXT : K_SUB, text.c_str());
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
                                IM_COL32(0x1F, 0x1C, 0x2B, 255), IM_COL32(0x15, 0x14, 0x1B, 255),
                                IM_COL32(0x15, 0x14, 0x1B, 255), IM_COL32(0x1F, 0x1C, 0x2B, 255));
    dl->AddLine(ImVec2(p0.x, p0.y + h - 1), ImVec2(p0.x + w, p0.y + h - 1), K_LINE);

    float b = S(26);
    ImVec2 bp(p0.x + S(12), p0.y + (h - b) * 0.5f);
    dl->AddRectFilled(bp - ImVec2(S(3), S(3)), bp + ImVec2(b + S(3), b + S(3)), IM_COL32(0x76, 0x67, 0xC2, 22), S(11));
    dl->AddRectFilled(bp, bp + ImVec2(b, b), K_ACC2, S(8));
    dl->AddRectFilled(bp, bp + ImVec2(b, b * 0.55f), K_ACC, S(8), ImDrawFlags_RoundCornersTop);
    dl->AddRect(bp, bp + ImVec2(b, b), IM_COL32(0x87, 0x79, 0xCF, 120), S(8));
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
    dl->AddRectFilledMultiColor(p, p + ImVec2(w * 0.6f, h), Alpha(tone, 0.06f), Alpha(tone, 0.0f), Alpha(tone, 0.0f), Alpha(tone, 0.06f));
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
    std::string ev = e.ip.empty() ? (e.busy ? std::string("checking") + ELL : std::string("unknown")) : e.ip;
    ImVec2 evs = TSize(s_fMono, 16.0f, ev.c_str());
    const char* via = "";
    if (!e.localIp.empty() && e.localIp == n.vpnIp)      via = "through the VPN";
    else if (!e.localIp.empty() && e.localIp == n.nicIp) via = "not through the VPN";
    const char* cap = "YOUR PUBLIC IP";
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
    if (IconButton("##egress", IC_REFRESH, rb, e.busy ? "checking" : "check again", true)) {
        StartEgressProbe();
        s_lastEgress = ImGui::GetTime();
    }

    // left: headline and detail, shortened so they never run into the egress block
    float lx = p.x + S(62), room = rx - lx - S(12);
    std::string title = ok ? ("Protected by " + (n.vpnAlias.empty() ? std::string("the VPN") : n.vpnAlias))
                           : std::string("VPN is off");
    std::string detail = ok ? std::string("apps use the VPN unless you send them OUT")
                            : ("everything uses " + (n.nicAlias.empty() ? std::string("your normal connection") : n.nicAlias) + DOT + "connect your VPN app");
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
            dl->AddRectFilled(ip, ip + is, IM_COL32(0x76, 0x67, 0xC2, 36), S(7));
            dl->AddRect(ip, ip + is, IM_COL32(0x76, 0x67, 0xC2, 96), S(7));
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
struct Card { unsigned icon; ImVec4 tint; const char* label; std::string value; ImU32 valueCol; std::string detail; int tab = -1; };

static void DrawCard(ImVec2 pos, ImVec2 size, const Card& cd) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool hov = ImGui::IsMouseHoveringRect(pos, pos + size) && ImGui::IsWindowHovered();
    bool link = cd.tab >= 0;   // cards with a tab open it when clicked
    dl->AddRectFilled(pos, pos + size, (hov && link) ? K_SURF2 : K_SURF, S(10));
    dl->AddRect(pos, pos + size, (hov && link) ? K_LINE2 : K_LINE, S(10));
    if (hov && link) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) s_tab = cd.tab;
        std::string chev = Ic(IC_NEXT);
        ImVec2 cs = TSize(s_fUi, 10.0f, chev.c_str());
        TDraw(dl, s_fUi, 10.0f, ImVec2(pos.x + size.x - S(12) - cs.x, pos.y + (size.y - cs.y) * 0.5f), K_SUB, chev.c_str());
    }

    float t = S(32);
    ImVec2 tp(pos.x + S(11), pos.y + (size.y - t) * 0.5f);
    dl->AddRectFilled(tp, tp + ImVec2(t, t), Alpha(cd.tint, 0.13f), S(8));
    std::string g = Ic(cd.icon);
    ImVec2 gs = TSize(s_fUi, 15.0f, g.c_str());
    TDraw(dl, s_fUi, 15.0f, tp + ImVec2((t - gs.x) * 0.5f, (t - gs.y) * 0.5f), Col(cd.tint), g.c_str());

    float x = tp.x + t + S(11), room = pos.x + size.x - S(link ? 26.0f : 10.0f) - x;
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
    cards.push_back({ IC_VPN, n.vpnUp ? V_OK : V_BAD, "VPN",
                      n.vpnUp ? std::string("connected") : std::string("not connected"), n.vpnUp ? K_TEXT : Col(V_BAD),
                      n.vpnUp ? (n.vpnAlias + DOT + n.vpnIp) : std::string("connect your VPN app first") });
    cards.push_back({ IC_WIFI, V_LILAC, "YOUR NETWORK",
                      n.nicAlias.empty() ? std::string("none") : n.nicAlias, K_TEXT,
                      n.nicIp.empty() ? std::string("no connection found") : (n.nicIp + (n.nicGw.empty() ? "" : DOT + ("router " + n.nicGw))) });
    {
        std::string d = "checking" + std::string(ELL);
        if (!e.ip.empty() && !e.localIp.empty()) {
            if (!n.vpnIp.empty() && e.localIp == n.vpnIp)      d = "sites see the VPN's address";
            else if (!n.nicIp.empty() && e.localIp == n.nicIp) d = "sites see your real address";
            else                                               d = "from " + e.localIp;
        } else if (!e.busy) {
            d = "could not check";
        }
        cards.push_back({ IC_GLOBE, V_CYAN, "PUBLIC IP",
                          e.ip.empty() ? (e.busy ? std::string("checking") + ELL : std::string("unknown")) : e.ip, Col(V_CYAN), d });
    }
    int outApps = 0;
    for (const auto& a : s_apps)
        if (a.mode == "OUT") outApps++;
    cards.push_back({ IC_APPS, outApps ? V_OUT : V_ACCENT, "APPS SENT OUT",
                      outApps ? (std::to_string(outApps) + (outApps == 1 ? " app" : " apps")) : std::string("none"),
                      K_TEXT,
                      outApps ? (k.proxyUp ? std::string("they skip the VPN")
                                           : std::string("start the proxy, or they cannot load anything"))
                              : std::string("every app uses the VPN"), TAB_APPS });
    cards.push_back({ IC_FILTER, k.exceptions ? V_OUT : V_ACCENT, "WEBSITES THAT SKIP THE VPN",
                      k.exceptions ? (std::to_string(k.exceptions) + " active") : std::string("none"), K_TEXT,
                      k.exceptions ? std::string("for every app") : std::string("add sites that should skip the VPN"), TAB_EXCEPTIONS });
    {
        std::string v = "off", d = "use the VPN on your phone";
        ImVec4 tint = V_ACCENT;
        if (k.hotspotShared && k.hotspotLive) {
            v = "on, through the VPN";
            d = std::to_string(k.hotspotClients) + (k.hotspotClients == 1 ? " device" : " devices") + DOT +
                k.hotspotHost + ":" + std::to_string(k.hotspotPort);
            tint = V_OK;
        } else if (k.hotspotShared) {
            v = "paused";
            d = k.hotspotProblem.empty() ? std::string("the VPN is not available right now") : k.hotspotProblem;
            tint = V_OUT;
        }
        cards.push_back({ IC_HOTSPOT, tint, "HOTSPOT", v, K_TEXT, d, TAB_HOTSPOT });
    }

    ImVec2 start = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    int cols = (avail < S(380)) ? 1 : 2;
    float gap = S(8), cw = (avail - gap * (cols - 1)) / cols, ch = S(60);
    for (int i = 0; i < (int)cards.size(); ++i)
        DrawCard(ImVec2(start.x + (i % cols) * (cw + gap), start.y + (i / cols) * (ch + gap)), ImVec2(cw, ch), cards[i]);
    int rows = ((int)cards.size() + cols - 1) / cols;
    ImGui::Dummy(ImVec2(avail, rows * (ch + gap)));
    Legend({ { V_OK, "IN  uses the VPN" }, { V_OUT, "OUT  skips the VPN" }, { V_MIX, "MIXED  both" },
             { ImVec4(0, 0, 0, 0), "click a card to open it" } });
}

// ------------------------------------------------------------------- apps ---
static bool ContainsI(const std::string& hay, const char* needle) {
    return !*needle || Lower(hay).find(Lower(needle)) != std::string::npos;
}

static ImU32 AvatarTone(const std::string& name) {
    static const ImU32 tones[] = { IM_COL32(0x95, 0x88, 0xD6, 255), IM_COL32(0x7F, 0xBF, 0xCC, 255), IM_COL32(0xCF, 0x94, 0xB4, 255),
                                   IM_COL32(0xD4, 0xB4, 0x6A, 255), IM_COL32(0x76, 0xC2, 0x96, 255), IM_COL32(0x95, 0x9C, 0xD6, 255) };
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

    // An OUT app is launched with --proxy-server pointing at the bypass proxy. While the
    // proxy is not running that browser cannot load anything at all and reports
    // ERR_TUNNEL_CONNECTION_FAILED for every address, so say it here and offer to start it.
    int outApps = 0;
    std::string outFirst;
    for (const auto& a : s_apps)
        if (a.mode == "OUT") { outApps++; if (outFirst.empty()) outFirst = a.name; }
    if (outApps > 0 && !s_kit.proxyUp) {
        float h = S(30), bw = ActionButtonWidth(IC_PLAY, "Start proxy");
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddRectFilled(p, p + ImVec2(avail, h), Alpha(V_OUT, 0.12f), S(8));
        dl->AddRect(p, p + ImVec2(avail, h), Alpha(V_OUT, 0.38f), S(8));
        std::string msg = std::to_string(outApps) + (outApps == 1 ? " app is" : " apps are") +
                          " set OUT but cannot load anything until the proxy runs";
        if (!outFirst.empty()) msg += " (" + outFirst + (outApps > 1 ? ", ..." : "") + ")";
        msg = Elide(s_fSemi, 11.5f, msg, avail - bw - S(24));
        TDraw(dl, s_fSemi, 11.5f, ImVec2(p.x + S(10), p.y + (h - TSize(s_fSemi, 11.5f, msg.c_str()).y) * 0.5f), Col(V_OUT), msg.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x + avail - bw - S(6), p.y + S(1)));
        if (ActionButton("##outstart", IC_PLAY, "Start proxy", !EngineBusy(), BTN_PRIMARY))
            RunEngine("app-chooser.ps1", "-StartProxy", "starting the bypass proxy");
        ImGui::SetCursorScreenPos(p);
        ImGui::Dummy(ImVec2(avail, h + S(6)));
    }

    ImVec2 hp = ImGui::GetCursorScreenPos();
    const float liveW = S(96), modeW = S(58);
    float liveX = hp.x + avail - S(8) - liveW, modeX = liveX - modeW;
    TDraw(dl, s_fSemi, 10.0f, ImVec2(hp.x + S(10), hp.y + S(2)), K_DIM, "APP");
    TDraw(dl, s_fSemi, 10.0f, ImVec2(modeX, hp.y + S(2)), K_DIM, "SET TO");
    TDraw(dl, s_fSemi, 10.0f, ImVec2(liveX, hp.y + S(2)), K_DIM, "RIGHT NOW");
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
                ldl->AddRectFilled(rp, rp + ImVec2(rw, rh), IM_COL32(0x76, 0x67, 0xC2, 32), S(7));
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
            const char* msg = s_appFilter[0] ? "no app matches the filter" : "no app is online right now";
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
        Legend({ { V_OK, "IN  uses the VPN" }, { V_OUT, "OUT  skips the VPN" },
                 { ImVec4(0, 0, 0, 0), "click an app to change it" } });
    }
}

// ------------------------------------------------------------- exceptions ---
static char s_exInput[512] = { 0 };

// addresses, networks and hostnames: letters, digits and . - _ / : plus separators
static int ExceptionCharFilter(ImGuiInputTextCallbackData* d) {
    ImWchar c = d->EventChar;
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-' || c == '_' || c == '/' || c == ':' || c == ',' || c == ' ' || c == ';';
    return ok ? 0 : 1;
}

static std::vector<std::string> SplitTargets(const char* text) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* q = text;; ++q) {
        if (*q == 0 || *q == ',' || *q == ' ' || *q == ';') {
            if (!cur.empty()) {
                std::string t = Lower(cur);
                if (t.compare(0, 8, "https://") == 0) t = t.substr(8);   // a pasted link
                else if (t.compare(0, 7, "http://") == 0) t = t.substr(7);
                size_t slash = t.find('/');
                bool cidr = slash != std::string::npos && t.find_first_not_of("0123456789./") == std::string::npos;
                if (slash != std::string::npos && !cidr) t = t.substr(0, slash);
                if (!t.empty() && std::find(out.begin(), out.end(), t) == out.end()) out.push_back(t);
            }
            cur.clear();
            if (*q == 0) break;
        } else {
            cur += *q;
        }
    }
    return out;
}

static void DrawExceptions() {
    bool idle = !EngineBusy();
    SectionHead(IC_FILTER, "Websites that skip the VPN");
    Wrapped(s_fUi, 12.5f, K_SUB, "Traffic to these websites or addresses uses your normal connection instead of the VPN, "
                                 "for every app. Handy for local devices, or sites and games that block VPNs.");
    ImGui::Dummy(ImVec2(0, S(4)));

    std::vector<std::string> current = InstalledExceptionTargets();
    StatusPill(s_kit.exceptions > 0, s_kit.exceptions ? (std::to_string(s_kit.exceptions) + " active") : std::string("none yet"));
    if (!current.empty()) {
        std::string list;
        for (const auto& t : current) list += (list.empty() ? "" : ", ") + t;
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
        Wrapped(s_fUi, 12.0f, K_SUB, list.c_str());
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    ImGui::PushFont(s_fSemi, 11.5f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(K_SUB), "%s", "Add websites or IP addresses");
    ImGui::PopFont();
    float fh = S(30);
    float addW = ActionButtonWidth(IC_ADD, "Add");
    ImGui::PushFont(s_fUi, 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), (fh - ImGui::GetFontSize()) * 0.5f));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addW - S(6));
    bool enter = ImGui::InputTextWithHint("##exinput", "example.com, 192.168.1.20", s_exInput, sizeof(s_exInput),
                                          ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue, ExceptionCharFilter);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    ImGui::SameLine(0, S(6));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(1));
    std::vector<std::string> add = SplitTargets(s_exInput);
    bool clicked = ActionButton("##exadd", IC_ADD, "Add", idle && !add.empty(), BTN_PRIMARY);
    if ((clicked || (enter && idle)) && !add.empty()) {
        // the script records only what one run installs, so every run gets the full list
        std::vector<std::string> all = current;
        for (const auto& t : add)
            if (std::find(all.begin(), all.end(), t) == all.end()) all.push_back(t);
        std::string csv;
        for (const auto& t : all) csv += (csv.empty() ? "" : ",") + t;
        if (RunEngineElevated("split-tunnel.ps1", "-Exclude " + csv + " -Apply", "adding websites that skip the VPN"))
            s_exInput[0] = 0;
    }
    ImGui::Dummy(ImVec2(0, S(6)));

    if (ActionButton("##exundo", IC_DELETE, "Remove all", idle && s_kit.exceptions > 0))
        RunEngineElevated("split-tunnel.ps1", "-Undo", "removing all websites that skip the VPN");
    ImGui::SameLine(0, S(6));
    if (ActionButton("##exstatus", IC_LIST, "Details", idle))
        RunEngine("split-tunnel.ps1", "-Status", "reading installed exceptions");
    ImGui::Dummy(ImVec2(0, S(8)));
    Note("Windows asks for permission when you add or remove websites. They stay until you restart the PC or "
         "click Remove all. A website is looked up when you add it; if it moves to a new address, add it again.");
}

// ------------------------------------------------------------------ proxy ---
static void DrawProxy() {
    SectionHead(IC_NETWORK, "Bypass proxy");
    Wrapped(s_fUi, 12.5f, K_SUB, "A small helper on this PC that lets the apps you send OUT skip the VPN, while everything "
                                 "else stays in it. It starts by itself when you send an app OUT.");
    ImGui::Dummy(ImVec2(0, S(4)));
    StatusPill(s_kit.proxyUp, s_kit.proxyUp
        ? ("running on port " + std::to_string(s_kit.proxyPort))
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
        RunEngine("app-chooser.ps1", "-Probe", "testing the proxy (public IP with and without it)");
    ImGui::Dummy(ImVec2(0, S(8)));
    Note("Browsers sent OUT from the Apps tab use this proxy automatically. Other apps can use it if they "
         "have a proxy setting: HTTP proxy 127.0.0.1 on the port shown above. Test shows your public IP with "
         "and without the proxy.");
}

// ---------------------------------------------------------------- hotspot ---
static char s_hsName[33] = { 0 };    // an SSID is at most 32 bytes
static char s_hsPass[64] = { 0 };
static bool s_hsShowPass = false;
static bool s_hsLoaded = false;      // the saved name and password are read once

// the values end up inside "..." on a command line, where a quote or a backslash would break it
static int HotspotCharFilter(ImGuiInputTextCallbackData* d) {
    return (d->EventChar == '"' || d->EventChar == '\\') ? 1 : 0;
}

static void HotspotField(const char* id, const char* label, const char* hint, char* buf, size_t size, bool password) {
    ImGui::PushFont(s_fSemi, 11.5f);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(K_SUB), "%s", label);
    ImGui::PopFont();
    float fh = S(30);
    float w = std::min(ImGui::GetContentRegionAvail().x, S(340));
    ImGui::PushFont(s_fUi, 13.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(S(10), (fh - ImGui::GetFontSize()) * 0.5f));
    ImGui::SetNextItemWidth(password ? w - fh - S(6) : w);
    ImGuiInputTextFlags f = ImGuiInputTextFlags_CallbackCharFilter;
    if (password && !s_hsShowPass) f |= ImGuiInputTextFlags_Password;
    ImGui::InputTextWithHint(id, hint, buf, size, f, HotspotCharFilter);
    ImGui::PopStyleVar();
    ImGui::PopFont();
    if (password) {
        ImGui::SameLine(0, S(6));
        if (IconButton("##hseye", IC_EYE, ImVec2(fh, fh), s_hsShowPass ? "hide the password" : "show the password", true, s_hsShowPass))
            s_hsShowPass = !s_hsShowPass;
    }
}

static void DrawHotspot() {
    const KitState& k = s_kit;
    bool idle = !EngineBusy();
    if (!s_hsLoaded) {
        s_hsLoaded = true;
        HotspotSettings saved;
        if (LoadHotspotSettings(saved)) {
            snprintf(s_hsName, sizeof(s_hsName), "%s", saved.name.c_str());
            snprintf(s_hsPass, sizeof(s_hsPass), "%s", saved.pass.c_str());
            SecureZeroMemory(&saved.pass[0], saved.pass.size());
        }
    }
    SectionHead(IC_HOTSPOT, "Use the VPN on your phone");
    Wrapped(s_fUi, 12.5f, K_SUB, "Turns on your PC's Mobile Hotspot and runs a proxy on it that only uses the VPN. "
                                 "Phones and other devices that join and set the proxy browse through the VPN.");
    ImGui::Dummy(ImVec2(0, S(4)));

    // status
    if (k.hotspotShared && k.hotspotLive) {
        StatusPill(true, "On" + std::string(DOT) + "through " + (k.hotspotTunnel.empty() ? std::string("the VPN") : k.hotspotTunnel));
        std::string d = std::to_string(k.hotspotClients) + (k.hotspotClients == 1 ? " device" : " devices") + " connected";
        if (!k.hotspotSsid.empty()) d += DOT + ("\"" + k.hotspotSsid + "\"");
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
        Wrapped(s_fUi, 12.0f, K_SUB, d.c_str());
    } else if (k.hotspotShared) {
        StatusPill(false, "Paused", true);
        ImGui::SameLine(0, S(10));
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(5));
        std::string why = k.hotspotProblem.empty() ? std::string("the VPN is not available right now") : k.hotspotProblem;
        if (!k.hotspotWatcher) why += " - press Start again";
        else                   why += " - it resumes by itself";
        Wrapped(s_fUi, 12.0f, Col(V_OUT), why.c_str());
    } else {
        StatusPill(false, "Off");
    }
    ImGui::Dummy(ImVec2(0, S(8)));

    // what to set on the phone, once the proxy runs
    if (k.hotspotShared && k.hotspotWatcher && !k.hotspotHost.empty()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x, h = S(158);
        dl->AddRectFilled(p, p + ImVec2(w, h), K_SURF, S(10));
        dl->AddRect(p, p + ImVec2(w, h), K_LINE, S(10));
        float x = p.x + S(14), y = p.y + S(12);
        TDraw(dl, s_fSemi, 12.5f, ImVec2(x, y), K_TEXT, "On your phone");
        y += S(24);
        std::string ssid = k.hotspotSsid.empty() ? std::string(s_hsName) : k.hotspotSsid;
        std::string s1 = "1.  Join the Wi-Fi \"" + ssid + "\"";
        TDraw(dl, s_fUi, 12.0f, ImVec2(x, y), K_SUB, Elide(s_fUi, 12.0f, s1, w - S(28)).c_str());
        y += S(20);
        TDraw(dl, s_fUi, 12.0f, ImVec2(x, y), K_SUB, "2.  In that network's settings, set Proxy to Manual:");
        y += S(24);
        std::string server = k.hotspotHost, port = std::to_string(k.hotspotPort);
        TDraw(dl, s_fUi, 12.0f, ImVec2(x + S(22), y + S(2)), K_DIM, "Server");
        TDraw(dl, s_fMono, 15.0f, ImVec2(x + S(76), y), Col(V_CYAN), server.c_str());
        float px = x + S(76) + TSize(s_fMono, 15.0f, server.c_str()).x + S(28);
        TDraw(dl, s_fUi, 12.0f, ImVec2(px, y + S(2)), K_DIM, "Port");
        TDraw(dl, s_fMono, 15.0f, ImVec2(px + S(38), y), Col(V_CYAN), port.c_str());
        y += S(28);
        // Without the proxy setting the phone still gets internet from the hotspot, but over
        // the real connection; a count that stays at 0 is how that shows up.
        std::string seen = std::to_string(k.hotspotRequests) + (k.hotspotRequests == 1 ? " connection" : " connections") +
                           " through the VPN so far";
        ImU32 seenCol = K_SUB;
        if (k.hotspotRequests == 0 && k.hotspotClients > 0) {
            seen += " - the phone is not using the proxy yet";
            seenCol = Col(V_OUT);
        }
        TDraw(dl, s_fUi, 11.5f, ImVec2(x, y), seenCol, Elide(s_fUi, 11.5f, seen, w - S(28)).c_str());
        y += S(18);
        TDraw(dl, s_fUi, 11.5f, ImVec2(x, y), K_DIM, Elide(s_fUi, 11.5f, "Without the proxy setting, the phone uses your real connection, not the VPN.", w - S(28)).c_str());
        ImGui::Dummy(ImVec2(w, h));
        ImGui::Dummy(ImVec2(0, S(4)));
        if (k.hotspotBand.compare(0, 3, "2.4") == 0) {
            Note("Slow on the phone? This PC is on a 2.4 GHz Wi-Fi network, and the hotspot has to share that "
                 "channel. Connecting the PC to a 5 GHz network or a cable makes the hotspot much faster. "
                 "A VPN server close to you also helps.");
            ImGui::Dummy(ImVec2(0, S(6)));
        }
    }

    // actions first, so Stop stays in view while the phone instructions are shown
    size_t passLen = strlen(s_hsPass);
    bool passOk = passLen == 0 || (passLen >= 8 && passLen <= 63);
    bool canStart = idle && passOk;
    if (ActionButton("##hsstart", IC_PLAY, k.hotspotShared ? "Restart" : "Start", canStart, BTN_PRIMARY)) {
        std::string args = "-Start";
        if (s_hsName[0]) args += std::string(" -Ssid \"") + s_hsName + "\"";
        if (s_hsPass[0]) args += std::string(" -Passphrase \"") + s_hsPass + "\"";
        if (RunEngineElevated("hotspot.ps1", args, "starting the hotspot proxy") && (s_hsName[0] || s_hsPass[0])) {
            if (!SaveHotspotSettings({ s_hsName, s_hsPass })) Log("!! could not save the hotspot name and password");
        }
    }
    ImGui::SameLine(0, S(6));
    if (ActionButton("##hsstop", IC_STOP, "Stop", idle && k.hotspotShared))
        RunEngineElevated("hotspot.ps1", "-Stop", "stopping the hotspot proxy");
    ImGui::SameLine(0, S(6));
    if (ActionButton("##hsstatus", IC_LIST, "Details", idle))
        RunEngine("hotspot.ps1", "-Status", "reading the hotspot status");
    ImGui::Dummy(ImVec2(0, S(10)));

    // name and password, used by Start
    std::string nameHint = k.hotspotSsid.empty() ? std::string("keep the name Windows uses") : k.hotspotSsid;
    HotspotField("##hsname", "Hotspot name", nameHint.c_str(), s_hsName, sizeof(s_hsName), false);
    ImGui::Dummy(ImVec2(0, S(2)));
    HotspotField("##hspass", "Password", "keep the password Windows uses", s_hsPass, sizeof(s_hsPass), true);
    if (!passOk) Wrapped(s_fUi, 11.5f, Col(V_BAD), "The password needs at least 8 characters.");
    ImGui::Dummy(ImVec2(0, S(8)));
    Note("Windows asks for permission when you start or stop. Browsing and most apps use the proxy; games and calls "
         "that ignore it are not guaranteed to use the VPN. If the VPN drops, the proxy blocks traffic instead of "
         "using your real connection. The name and password are remembered on this PC.");
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

    // the newest line, shown in the header while the log is closed; a new problem opens it
    static size_t seen = 0;
    std::string last;
    size_t idx = 0;
    bool problem = false;
    ForEachLogLine([&](const std::string& l) {
        if (idx++ >= seen && l.size() > 10 && l.compare(10, 2, "!!") == 0) problem = true;
        last = l;
    });
    if (count < seen) seen = 0;
    if (problem && count > seen) s_logOpen = true;
    seen = count;

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
    float busyW = 0.0f;
    if (EngineBusy()) {
        const char* busy = "working";
        ImVec2 bs = TSize(s_fUi, 11.0f, busy);
        busyW = bs.x + S(12);
        TDraw(dl, s_fUi, 11.0f, ImVec2(p.x + w - S(40) - bs.x, p.y + (hh - bs.y) * 0.5f), K_ACCTX, busy);
    }
    if (!s_logOpen && last.size() > 10) {
        std::string msg = last.substr(10);
        ImU32 tone = LogTone(msg);
        if (msg.compare(0, 3, "ok ") == 0 || msg.compare(0, 3, "!! ") == 0 || msg.compare(0, 3, ".. ") == 0) msg = msg.substr(3);
        float x = bp.x + ns.x + S(10) + S(12);
        msg = Elide(s_fUi, 11.5f, msg, (p.x + w - S(44) - busyW) - x);
        TDraw(dl, s_fUi, 11.5f, ImVec2(x, p.y + (hh - TSize(s_fUi, 11.5f, "A").y) * 0.5f), tone, msg.c_str());
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x + w - S(34), p.y + S(3)));
    if (IconButton("##logclear", IC_DELETE, ImVec2(S(26), S(26)), "clear the log")) ClearLog();

    if (s_logOpen) {
        dl->AddLine(ImVec2(p.x + S(1), p.y + hh), ImVec2(p.x + w - S(1), p.y + hh), K_LINE);
        ImGui::SetCursorScreenPos(ImVec2(p.x + S(12), p.y + hh + S(5)));
        ImGui::BeginChild("logscroll", ImVec2(w - S(18), h - hh - S(9)));
        static size_t lastCount = 0;
        bool atEnd = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - S(4);
        // time | status dot | message; the message wraps in its own column, with a gap
        // between entries so a wrapped entry does not run into the next one
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(S(8), S(6)));
        ImDrawList* ldl = ImGui::GetWindowDrawList();
        float stampW = TSize(s_fMono, 11.0f, "00:00:00").x;
        float lineH = TSize(s_fUi, 12.0f, "A").y;
        float wrapRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - S(10);   // clear of the scrollbar
        ForEachLogLine([&](const std::string& l) {
            std::string stamp = l.substr(0, std::min<size_t>(8, l.size()));
            std::string msg = l.size() > 10 ? l.substr(10) : std::string();
            ImU32 tone = LogTone(msg);
            bool marked = msg.compare(0, 2, "ok") == 0 || msg.compare(0, 2, "!!") == 0;
            if (msg.compare(0, 3, "ok ") == 0 || msg.compare(0, 3, "!! ") == 0 || msg.compare(0, 3, ".. ") == 0) msg = msg.substr(3);
            ImVec2 rp = ImGui::GetCursorScreenPos();
            TDraw(ldl, s_fMono, 11.0f, ImVec2(rp.x, rp.y + (lineH - TSize(s_fMono, 11.0f, "0").y) * 0.5f), K_DIM, stamp.c_str());
            if (marked) ldl->AddCircleFilled(ImVec2(rp.x + stampW + S(10), rp.y + lineH * 0.5f), S(2.6f), tone);
            float tx = stampW + S(20);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + tx);
            ImGui::PushFont(s_fUi, 12.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, tone == K_DIM ? K_SUB : tone);
            ImGui::PushTextWrapPos(wrapRight);
            ImGui::TextUnformatted(msg.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
            ImGui::PopFont();
        });
        ImGui::PopStyleVar();
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
              { IC_NETWORK, "Proxy", s_kit.proxyUp ? std::string("on") : std::string() },
              { IC_HOTSPOT, "Hotspot", !s_kit.hotspotShared ? std::string() : s_kit.hotspotLive ? std::string("on") : std::string("paused") } });
    ImGui::EndChild();
    y += topH + S(10);

    float logH = s_logOpen ? S(170) : S(32);
    float bodyH = std::max((wp.y + H - pad - logH - S(10)) - y, S(60));
    ImGui::SetCursorScreenPos(ImVec2(wp.x + pad, y));
    ImGui::BeginChild("body", ImVec2(cw, bodyH), ImGuiChildFlags_None, s_tab == TAB_APPS ? fixed : 0);
    switch (s_tab) {
    case TAB_APPS:       DrawApps(); break;
    case TAB_EXCEPTIONS: DrawExceptions(); break;
    case TAB_PROXY:      DrawProxy(); break;
    case TAB_HOTSPOT:    DrawHotspot(); break;
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
