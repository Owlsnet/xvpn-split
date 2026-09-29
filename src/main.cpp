// xvpn - split tunneling for full-tunnel VPNs on Windows.
// Window, Direct3D 11 device and message loop; the contents live in ui.cpp.
#include "common.h"
#include "engine.h"
#include "resource.h"
#include "ui.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <windowsx.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

static const wchar_t* kWindowClass = L"xvpnWindow";

static ID3D11Device*           g_dev = nullptr;
static ID3D11DeviceContext*    g_ctx = nullptr;
static IDXGISwapChain*         g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT                    g_resizeW = 0, g_resizeH = 0;

static inline float S(float v) { return v * UiDpi(); }

// ---------------------------------------------------------------- direct3d ---
static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    if (SUCCEEDED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
        g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDeviceD3D(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                               D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (hr == DXGI_ERROR_UNSUPPORTED)   // no usable GPU: fall back to the software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swap) { g_swap->Release(); g_swap = nullptr; }
    if (g_ctx)  { g_ctx->Release();  g_ctx = nullptr; }
    if (g_dev)  { g_dev->Release();  g_dev = nullptr; }
}

// ------------------------------------------------------------------ window ---
// Borderless window that keeps the Windows 11 rounded corners, shadow and a coloured edge.
// The attributes are ignored on Windows 10, which then shows square corners.
static void StyleWindowFrame(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    int corner = 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
    COLORREF edge = RGB(0x3A, 0x2E, 0x5C);
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &edge, sizeof(edge));
    MARGINS m = { 0, 0, 0, 1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp == TRUE) {
            // the whole window is client area; when maximised, keep it inside the monitor
            if (IsZoomed(hwnd)) {
                auto* p = (NCCALCSIZE_PARAMS*)lp;
                UINT dpi = GetDpiForWindow(hwnd);
                int f = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += f;
                p->rgrc[0].right -= f;
                p->rgrc[0].top += f;
                p->rgrc[0].bottom -= f;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        POINT p{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &p);
        RECT rc;
        GetClientRect(hwnd, &rc);
        const int edge = (int)S(6);
        bool left = p.x < edge, right = p.x >= rc.right - edge;
        bool top = p.y < edge, bottom = p.y >= rc.bottom - edge;
        if (!IsZoomed(hwnd)) {
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
        }
        if (p.y < (int)S(kHeaderHeight) && p.x < rc.right - (int)S(kWindowButtonsWidth)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = (MINMAXINFO*)lp;
        mmi->ptMinTrackSize.x = (LONG)S(420);
        mmi->ptMinTrackSize.y = (LONG)S(380);
        return 0;
    }
    case WM_DPICHANGED: {
        UiSetDpi(HIWORD(wp) / 96.0f);
        const RECT* r = (const RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) return 0;
        g_resizeW = LOWORD(lp);
        g_resizeH = HIWORD(lp);
        return 0;
    case WM_SYSCOMMAND:
        if ((wp & 0xfff0) == SC_KEYMENU) return 0;   // no Alt menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int StartTabFromCommandLine() {
    std::string cmd = Narrow(GetCommandLineW());
    if (cmd.find("--apps") != std::string::npos) return TAB_APPS;
    if (cmd.find("--exceptions") != std::string::npos) return TAB_EXCEPTIONS;
    if (cmd.find("--proxy") != std::string::npos) return TAB_PROXY;
    return TAB_DASHBOARD;
}

// ------------------------------------------------------------------- main ---
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // one window per user session: a second start brings the first one forward
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\xvpn-ui");
    if (single && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    ImGui_ImplWin32_EnableDpiAwareness();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hbrBackground = CreateSolidBrush(RGB(0x10, 0x0C, 0x17));
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, kWindowClass, L"xvpn",
                                WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
                                120, 120, 560, 580, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    float dpi = ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd);
    if (dpi <= 0.0f) dpi = 1.0f;
    SetWindowPos(hwnd, nullptr, 0, 0, (int)(560 * dpi), (int)(580 * dpi), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    StyleWindowFrame(hwnd);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        MessageBoxW(nullptr, L"Direct3D 11 is not available on this machine.", L"xvpn", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    UiInit(hwnd, dpi, StartTabFromCommandLine());
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    Log("xvpn started - data folder %s", DataDir().c_str());
    if (!InstallEngine()) Log("!! the engine scripts could not be unpacked; OUT/IN and the proxy will not work");

    const float clear[4] = { 0.063f, 0.047f, 0.090f, 1.0f };
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_resizeW != 0 && g_resizeH != 0) {
            CleanupRenderTarget();
            g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        UiPrepare();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        UiFrame();
        ImGui::Render();
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, hInst);
    WSACleanup();
    if (single) CloseHandle(single);
    return 0;
}
