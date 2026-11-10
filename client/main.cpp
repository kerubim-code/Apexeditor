

#define _CRT_SECURE_NO_WARNINGS

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#include "fonts.h"
#include "glass/backdrop.h"
#include "glass/glass.h"

#include "app.h"
#include "appres.h"
#include "crosshair.h"
#include "logo.h"
#include "overlay.h"
#include "settings.h"
#include "spotify.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <objbase.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "ole32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
                                                             WPARAM wParam,
                                                             LPARAM lParam);

namespace {

ID3D11Device*           g_device        = nullptr;
ID3D11DeviceContext*    g_context       = nullptr;
IDXGISwapChain*         g_swapChain     = nullptr;
ID3D11RenderTargetView* g_rtView        = nullptr;
bool                    g_occluded      = false;
UINT                    g_resizeW = 0, g_resizeH = 0;

bool  g_itemHover = false;
bool  g_dragging  = false;
POINT g_dragPt{};

constexpr int     kOvrW = 440, kOvrH = 196;
HWND             g_ovrHwnd      = nullptr;
IDXGISwapChain*  g_ovrSwap      = nullptr;
ID3D11RenderTargetView* g_ovrRT = nullptr;
ImGuiContext*    g_ovrCtx       = nullptr;
ImGuiContext*    g_mainCtx      = nullptr;
Glass::Renderer  g_ovrRenderer;
ImFont*          g_ovrIconFont   = nullptr;
ImFont*          g_ovrIconBrands = nullptr;
ImFont*          g_mainIconFont   = nullptr;
ImFont*          g_mainIconBrands = nullptr;
bool             g_ovrInited     = false;
bool             g_ovrDragging   = false;
POINT            g_ovrDragPt{};

bool IsDragZone(HWND hWnd, int x, int y) {
    RECT rc{};
    if (!GetClientRect(hWnd, &rc))
        return false;
    const int m = 46;
    const bool zone = (x < m || y < m || x > rc.right - m || y > rc.bottom - m) ||
                      (x < m + static_cast<int>(apex::kSidebarWidth));
    if (g_itemHover)
        return false;
    return zone;
}

Glass::Backdrop         g_backdrop;
Glass::Renderer         g_renderer;

constexpr int kWindowW = 1240;
constexpr int kWindowH = 780;

}

namespace apex {
namespace {
HWND g_hkHwnd   = nullptr;
int  g_hkVk[5]  = {0, 0, 0, 0, 0};
bool g_hkReg[5] = {false, false, false, false, false};
constexpr int      kHkId[5]   = {0x5001, 0x5002, 0x5003, 0x5004, 0x5005};
constexpr const char* kHkKey[5] = {"key_play", "key_prev", "key_next",
                                   "key_crosshair", "key_overlay"};

void ApplySlot(int slot) {
    if (g_hkHwnd == nullptr || slot < 0 || slot > 4)
        return;
    if (g_hkReg[slot]) {
        UnregisterHotKey(g_hkHwnd, kHkId[slot]);
        g_hkReg[slot] = false;
    }
    if (g_hkVk[slot] > 0)
        g_hkReg[slot] = RegisterHotKey(g_hkHwnd, kHkId[slot], MOD_NOREPEAT,
                                       static_cast<UINT>(g_hkVk[slot])) != FALSE;
}
}

void ReapplyHotkeys() {
    if (g_hkHwnd == nullptr)
        g_hkHwnd = App::Get().Window();
    for (int s = 0; s < 5; ++s) {
        g_hkVk[s] = settings::GetInt(kHkKey[s], 0);
        ApplySlot(s);
    }
}

void UnregisterHotkeys() {
    for (int s = 0; s < 5; ++s)
        if (g_hkReg[s] && g_hkHwnd != nullptr) {
            UnregisterHotKey(g_hkHwnd, kHkId[s]);
            g_hkReg[s] = false;
        }
    g_hkHwnd = nullptr;
}

bool HotkeyBind(int slot, int vk) {
    if (slot < 0 || slot > 4)
        return false;
    settings::SetInt(kHkKey[slot], vk);
    g_hkVk[slot] = vk;
    ApplySlot(slot);
    return vk == 0 || g_hkReg[slot];
}

int HotkeyVk(int slot) {
    return (slot >= 0 && slot < 5) ? g_hkVk[slot] : 0;
}

bool HotkeyRegistered(int slot) {
    return (slot >= 0 && slot < 5) ? g_hkReg[slot] : false;
}

std::string HotkeyKeyName(int vk) {
    if (vk <= 0)
        return "not set";
    switch (vk) {
    case VK_ESCAPE:  return "Esc";
    case VK_SPACE:   return "Space";
    case VK_TAB:     return "Tab";
    case VK_RETURN:  return "Enter";
    case VK_BACK:    return "Backspace";
    case VK_DELETE:  return "Delete";
    case VK_INSERT:  return "Insert";
    case VK_HOME:    return "Home";
    case VK_END:     return "End";
    case VK_PRIOR:   return "Page Up";
    case VK_NEXT:    return "Page Down";
    case VK_LEFT:    return "Left";
    case VK_RIGHT:   return "Right";
    case VK_UP:      return "Up";
    case VK_DOWN:    return "Down";
    case VK_CAPITAL: return "Caps Lock";
    default: break;
    }
    if (vk >= 'A' && vk <= 'Z')
        return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9')
        return std::string(1, static_cast<char>(vk));
    if (vk >= VK_F1 && vk <= VK_F24)
        return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return "Numpad " + std::to_string(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_MULTIPLY:  return "Numpad *";
    case VK_ADD:       return "Numpad +";
    case VK_SUBTRACT:  return "Numpad -";
    case VK_DECIMAL:   return "Numpad .";
    case VK_DIVIDE:    return "Numpad /";
    default: break;
    }

    const UINT scan = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    if (scan != 0) {
        char name[64] = {};
        if (GetKeyNameTextA(static_cast<LONG>(scan << 16), name,
                            static_cast<int>(sizeof(name))) > 0)
            return name;
    }
    return "Key " + std::to_string(vk);
}
}

namespace {

void CreateRenderTarget();

LRESULT WINAPI OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount            = 2;
    sd.BufferDesc.Format      = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate  = {60, 1};
    sd.Flags                  = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage            = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow           = hWnd;
    sd.SampleDesc.Count       = 1;
    sd.Windowed               = TRUE;
    sd.SwapEffect             = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level{};
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
        &g_swapChain, &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &sd,
            &g_swapChain, &g_device, &level, &g_context);
    if (FAILED(hr))
        return false;

    CreateRenderTarget();
    return true;
}

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back != nullptr) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtView);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_rtView != nullptr) {
        g_rtView->Release();
        g_rtView = nullptr;
    }
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain != nullptr)  { g_swapChain->Release();  g_swapChain = nullptr; }
    if (g_context != nullptr)    { g_context->Release();    g_context = nullptr; }
    if (g_device != nullptr)     { g_device->Release();     g_device = nullptr; }
}

std::string ExeDir() {
    char buf[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string path(buf, (n > 0 && n < MAX_PATH) ? n : 0u);
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

bool FileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::string AssetPath(const char* rel) {
    std::string dir = ExeDir();
    for (int up = 0; up <= 6; ++up) {
        const std::string candidate = dir + "\\" + rel;
        if (FileExists(candidate))
            return candidate;
        dir += "\\..";
    }
    const std::string dev = std::string("..\\liquidDX11-main\\") + rel;
    if (FileExists(dev))
        return dev;

    char env[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableA("GLASS_ASSET_DIR", env, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const std::string candidate = std::string(env) + "\\" + rel;
        if (FileExists(candidate))
            return candidate;
    }
    return rel;
}

const void* EmbeddedResource(int id, DWORD* outSize) {
    const HRSRC res = FindResource(nullptr, MAKEINTRESOURCE(id), RT_RCDATA);
    if (res == nullptr)
        return nullptr;
    const HGLOBAL block = LoadResource(nullptr, res);
    if (block == nullptr)
        return nullptr;
    if (outSize != nullptr)
        *outSize = SizeofResource(nullptr, res);
    return LockResource(block);
}

ImFont* AddIconFont(ImFontAtlas* atlas, const char* rel, int resId, float size) {
    if (atlas == nullptr)
        return nullptr;

    const std::string path = AssetPath(rel);
    if (FileExists(path))
        return atlas->AddFontFromFileTTF(path.c_str(), size);

    DWORD bytes = 0;
    const void* raw = EmbeddedResource(resId, &bytes);
    if (raw == nullptr || bytes == 0)
        return nullptr;

    static std::vector<std::vector<unsigned char>> store;
    const auto* begin = static_cast<const unsigned char*>(raw);
    store.emplace_back(begin, begin + bytes);

    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    return atlas->AddFontFromMemoryTTF(store.back().data(),
                                       static_cast<int>(bytes), size, &cfg);
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {

    switch (msg) {
        case WM_HOTKEY: {

            const int slot = static_cast<int>(wParam) - 0x5001;
            if (slot >= 0 && slot < 3) {
                apex::spotify::Post(slot == 0 ? apex::spotify::Cmd::PlayPause
                                    : slot == 1 ? apex::spotify::Cmd::Prev
                                                : apex::spotify::Cmd::Next);
                return 0;
            }
            if (slot == 3) {
                apex::crosshair::Toggle();
                return 0;
            }
            if (slot == 4) {
                apex::overlay::Toggle();
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (IsDragZone(hWnd, x, y)) {
                g_dragging = true;
                g_dragPt.x = x;
                g_dragPt.y = y;
                SetCapture(hWnd);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (g_dragging) {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);

                RECT now{};
                GetWindowRect(hWnd, &now);
                int nx = now.left + (x - g_dragPt.x);
                int ny = now.top + (y - g_dragPt.y);

                const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int ww = now.right - now.left;
                const int wh = now.bottom - now.top;
                const int keep = 80;
                if (nx > vx + vw - keep) nx = vx + vw - keep;
                if (ny > vy + vh - keep) ny = vy + vh - keep;
                if (nx < vx + keep - ww) nx = vx + keep - ww;
                if (ny < vy + keep - wh) ny = vy + keep - wh;
                SetWindowPos(hWnd, nullptr, nx, ny, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (g_dragging) {
                g_dragging = false;
                ReleaseCapture();
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            g_dragging = false;
            break;
    }

    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED)
                return 0;
            g_resizeW = LOWORD(lParam);
            g_resizeH = HIWORD(lParam);
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU)
                return 0;
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ::PostMessageW(hWnd, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void CreateOverlayRT() {
    if (g_ovrSwap == nullptr)
        return;
    ID3D11Texture2D* back = nullptr;
    g_ovrSwap->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back != nullptr) {
        g_device->CreateRenderTargetView(back, nullptr, &g_ovrRT);
        back->Release();
    }
}

void CleanupOverlayRT() {
    if (g_ovrRT != nullptr) {
        g_ovrRT->Release();
        g_ovrRT = nullptr;
    }
}

bool CreateOverlayWindow(HINSTANCE inst, float scale, int x, int y,
                         int w, int h) {
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, OverlayWndProc, 0L, 0L, inst,
                      nullptr, nullptr, nullptr, nullptr, L"APEXOverlayWindow",
                      nullptr};
    RegisterClassExW(&wc);

    if (w <= 0)
        w = static_cast<int>(kOvrW * scale);
    if (h <= 0)
        h = static_cast<int>(kOvrH * scale);

    g_ovrHwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED |
                                    WS_EX_NOACTIVATE,
                                L"APEXOverlayWindow", L"APEX player",
                                WS_POPUP,
                                x, y, w, h, nullptr, nullptr, inst, nullptr);
    if (g_ovrHwnd == nullptr)
        return false;

    SetLayeredWindowAttributes(g_ovrHwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS margins = {-1};
    DwmExtendFrameIntoClientArea(g_ovrHwnd, &margins);

    const DWORD cornerPref = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(g_ovrHwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPref,
                          sizeof(cornerPref));

    IDXGIDevice*   dxgiDev = nullptr;
    IDXGIAdapter*  adapter = nullptr;
    IDXGIFactory*  factory = nullptr;
    g_device->QueryInterface(IID_PPV_ARGS(&dxgiDev));
    if (dxgiDev != nullptr) {
        dxgiDev->GetAdapter(&adapter);
        if (adapter != nullptr)
            adapter->GetParent(IID_PPV_ARGS(&factory));
    }

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount       = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow      = g_ovrHwnd;
    sd.SampleDesc.Count  = 1;
    sd.Windowed          = TRUE;
    sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

    const HRESULT hr = (factory != nullptr)
                           ? factory->CreateSwapChain(g_device, &sd, &g_ovrSwap)
                           : E_FAIL;
    if (factory != nullptr)  factory->Release();
    if (adapter != nullptr)  adapter->Release();
    if (dxgiDev != nullptr)  dxgiDev->Release();
    if (FAILED(hr))
        return false;

    CreateOverlayRT();
    return true;
}

void InitOverlayImGui(float scale) {
    (void)scale;
    if (g_ovrHwnd == nullptr)
        return;

    const ImGuiStyle tuned = ImGui::GetStyle();

    ImGuiContext* prev = ImGui::GetCurrentContext();
    g_ovrCtx = ImGui::CreateContext();

    ImGui::SetCurrentContext(g_ovrCtx);

    ImGuiIO& oio = ImGui::GetIO();
    oio.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    oio.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui::GetStyle() = tuned;

    ImGui_ImplWin32_Init(g_ovrHwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ImFontConfig font_cfg;
    font_cfg.FontDataOwnedByAtlas = false;
    ImFont* medium = oio.Fonts->AddFontFromMemoryTTF(
        inter_medium.data(), static_cast<int>(inter_medium.size()), 17.0f,
        &font_cfg);
    apex::g_ovrFontSemi = oio.Fonts->AddFontFromMemoryTTF(
        inter_semibold.data(), static_cast<int>(inter_semibold.size()), 26.0f,
        &font_cfg);
    if (medium != nullptr)
        oio.FontDefault = medium;

    g_ovrIconFont = AddIconFont(oio.Fonts, "data\\fa\\fa-solid-900.ttf",
                                IDR_FA_SOLID, 32.0f);
    g_ovrIconBrands = AddIconFont(oio.Fonts, "data\\fa\\fa-brands-400.ttf",
                                  IDR_FA_BRANDS, 32.0f);

    g_ovrInited = g_ovrRenderer.Init(g_device, g_context);
    if (!g_ovrInited) {

        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_ovrCtx);
        g_ovrCtx = nullptr;
    }
    ImGui::SetCurrentContext(prev);
}

LRESULT WINAPI OverlayWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_NCHITTEST: {

            return HTCLIENT;
        }
        case WM_LBUTTONDOWN: {

            if (!apex::g_overlayItemHover) {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                g_ovrDragging = true;
                g_ovrDragPt.x = x;
                g_ovrDragPt.y = y;
                SetCapture(hWnd);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE:
            if (g_ovrDragging) {
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);

                RECT now{};
                GetWindowRect(hWnd, &now);
                int nx = now.left + (x - g_ovrDragPt.x);
                int ny = now.top + (y - g_ovrDragPt.y);
                const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int ww = now.right - now.left;
                const int wh = now.bottom - now.top;
                const int keep = 60;
                if (nx > vx + vw - keep) nx = vx + vw - keep;
                if (ny > vy + vh - keep) ny = vy + vh - keep;
                if (nx < vx + keep - ww) nx = vx + keep - ww;
                if (ny < vy + keep - wh) ny = vy + keep - wh;
                SetWindowPos(hWnd, nullptr, nx, ny, 0, 0,
                             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            break;
        case WM_LBUTTONUP:
            if (g_ovrDragging) {
                g_ovrDragging = false;
                ReleaseCapture();
                RECT wr{};
                if (GetWindowRect(hWnd, &wr)) {
                    apex::settings::SetInt("player_x", wr.left);
                    apex::settings::SetInt("player_y", wr.top);
                }
                return 0;
            }
            break;
        case WM_CAPTURECHANGED:
            g_ovrDragging = false;
            break;
    }

    if (g_ovrCtx != nullptr) {
        ImGuiContext* prev = ImGui::GetCurrentContext();
        if (prev != g_ovrCtx)
            ImGui::SetCurrentContext(g_ovrCtx);
        const bool handled =
            ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam);
        if (prev != g_ovrCtx)
            ImGui::SetCurrentContext(prev);
        if (handled)
            return true;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

void RenderOverlayFrame(apex::App& app, bool allowShow) {
    if (g_ovrHwnd == nullptr || !g_ovrInited)
        return;

    const bool want = apex::g_playerOverlay && allowShow;
    const bool visible = IsWindowVisible(g_ovrHwnd) != FALSE;
    if (want != visible)
        ShowWindow(g_ovrHwnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (!want)
        return;

    ImGui::SetCurrentContext(g_ovrCtx);

    RECT rc{};
    GetClientRect(g_ovrHwnd, &rc);
    const int ow = rc.right - rc.left;
    const int oh = rc.bottom - rc.top;
    if (g_ovrSwap != nullptr && ow > 0 && oh > 0) {
        DXGI_SWAP_CHAIN_DESC sd{};
        if (SUCCEEDED(g_ovrSwap->GetDesc(&sd)) &&
            (sd.BufferDesc.Width != static_cast<UINT>(ow) ||
             sd.BufferDesc.Height != static_cast<UINT>(oh))) {

            ID3D11RenderTargetView* none = nullptr;
            g_context->OMSetRenderTargets(1, &none, nullptr);
            CleanupOverlayRT();
            if (SUCCEEDED(g_ovrSwap->ResizeBuffers(0, ow, oh,
                                                   DXGI_FORMAT_UNKNOWN, 0)))
                CreateOverlayRT();
        }
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    RECT wr{};
    GetWindowRect(g_ovrHwnd, &wr);
    POINT cp{};
    GetCursorPos(&cp);
    const ImVec2 cursor(static_cast<float>(cp.x - wr.left),
                        static_cast<float>(cp.y - wr.top));

    const float opacity = apex::g_playerOpacity * 0.01f;
    ImGui::GetStyle().Alpha = opacity;

    Glass::Renderer* prevRenderer = Glass::g;
    Glass::g = &g_ovrRenderer;
    Glass::SetIconFont(g_ovrIconFont);
    Glass::SetIconFontBrands(g_ovrIconBrands);

    g_ovrRenderer.BeginFrame(ow, oh, wr.left - g_backdrop.originX(),
                             wr.top - g_backdrop.originY(), g_backdrop.width(),
                             g_backdrop.height(), cursor);
    g_ovrRenderer.SetSubmitFade(opacity);

    app.DrawSpotifyOverlay(static_cast<float>(ow), static_cast<float>(oh));

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsAnyItemHovered() &&
        !Glass::SfxConsumed())
        Glass::PlaySfx(Glass::Sfx::Tap);
    Glass::SfxResetConsumed();

    ImGui::Render();

    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    g_ovrRenderer.SetRenderScale(1.0f);
    g_context->OMSetRenderTargets(1, &g_ovrRT, nullptr);
    g_context->ClearRenderTargetView(g_ovrRT, clear);
    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(ow);
    vp.Height   = static_cast<float>(oh);
    vp.MaxDepth = 1.0f;
    g_context->RSSetViewports(1, &vp);
    g_ovrRenderer.Render(g_backdrop.heavySRV(), g_backdrop.softSRV());
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ovrSwap->Present(0, 0);

    Glass::g = prevRenderer;
    Glass::SetIconFont(g_mainIconFont);
    Glass::SetIconFontBrands(g_mainIconBrands);
    ImGui::SetCurrentContext(g_mainCtx);
}

}

namespace apex {
ID3D11Device* Device() { return g_device; }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\APEX_ALL_IN_ONE_CLIENT");
    if (mutex != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND previous = FindWindowW(L"APEXClientWindow", nullptr))
            SetForegroundWindow(previous);
        return 0;
    }

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale =
        ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0},
                                                               MONITOR_DEFAULTTOPRIMARY));

    HICON appIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandle(nullptr), MAKEINTRESOURCEW(IDI_APEX), IMAGE_ICON, 0, 0,
        LR_DEFAULTSIZE | LR_SHARED));

    WNDCLASSEXW wc = {sizeof(wc),     CS_CLASSDC,        WndProc,         0L, 0L,
                      GetModuleHandle(nullptr), appIcon,  nullptr,        nullptr,
                      nullptr,        L"APEXClientWindow", appIcon};
    RegisterClassExW(&wc);

    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int winW = static_cast<int>(kWindowW * scale);
    const int winH = static_cast<int>(kWindowH * scale);
    const int posX = work.left + ((work.right - work.left) - winW) / 2;
    const int posY = work.top + ((work.bottom - work.top) - winH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, wc.lpszClassName,
                                L"APEX ALL IN ONE", WS_POPUP, posX, posY, winW, winH,
                                nullptr, nullptr, wc.hInstance, nullptr);
    SetLayeredWindowAttributes(hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS margins = {-1};
    DwmExtendFrameIntoClientArea(hwnd, &margins);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxA(hwnd, "Direct3D 11 initialisation failed", "APEX", MB_OK | MB_ICONERROR);
        return 1;
    }

    {
        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int ovrX = apex::settings::GetInt(
            "player_x", static_cast<int>(work.right - kOvrW * scale - 40));
        const int ovrY = apex::settings::GetInt(
            "player_y", static_cast<int>(work.bottom - kOvrH * scale - 40));

        const int defW = static_cast<int>(kOvrW * scale);
        const int defH = static_cast<int>(kOvrH * scale);
        int ovrW = apex::settings::GetInt("player_w", defW);
        int ovrH = apex::settings::GetInt("player_h", defH);
        const int maxW = (workArea.right - workArea.left) - 40;
        const int maxH = (workArea.bottom - workArea.top) - 40;
        if (ovrW < 320) ovrW = defW;
        if (ovrH < 160) ovrH = defH;
        if (ovrW > maxW) ovrW = maxW;
        if (ovrH > maxH) ovrH = maxH;
        CreateOverlayWindow(GetModuleHandle(nullptr), scale, ovrX, ovrY,
                            ovrW, ovrH);
        g_backdrop.SetProtectedWindow(g_ovrHwnd);
    }

    const bool backdropOk = g_backdrop.Init(g_device, g_context, hwnd);
    g_backdrop.SetUIBlurSpread(3.0f);

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding     = 0.0f;
    style.WindowBorderSize   = 0.0f;
    style.FrameRounding      = 10.0f;
    style.GrabRounding       = 10.0f;
    style.PopupRounding      = 13.0f;
    style.ChildRounding      = 13.0f;
    style.ScrollbarRounding  = 12.0f;
    style.FrameBorderSize    = 0.0f;
    style.PopupBorderSize    = 0.0f;
    style.ScrollbarSize      = 10.0f;
    style.AntiAliasedLines       = true;
    style.AntiAliasedLinesUseTex = true;
    style.AntiAliasedFill        = true;
    style.CircleTessellationMaxError = 0.10f;
    style.CurveTessellationTol       = 0.80f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;

    auto frost = [](float a) { return ImVec4(1.0f, 1.0f, 1.0f, a); };
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text]             = ImVec4(0.925f, 0.929f, 0.953f, 1.00f);
    colors[ImGuiCol_TextDisabled]     = ImVec4(0.592f, 0.604f, 0.659f, 1.00f);
    colors[ImGuiCol_NavCursor]        = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_Border]           = frost(0.07f);
    colors[ImGuiCol_Separator]        = frost(0.10f);
    colors[ImGuiCol_Button]           = frost(0.06f);
    colors[ImGuiCol_ButtonHovered]    = frost(0.12f);
    colors[ImGuiCol_ButtonActive]     = frost(0.18f);
    colors[ImGuiCol_FrameBg]          = frost(0.06f);
    colors[ImGuiCol_FrameBgHovered]   = frost(0.10f);
    colors[ImGuiCol_FrameBgActive]    = frost(0.14f);
    colors[ImGuiCol_Header]           = frost(0.10f);
    colors[ImGuiCol_HeaderHovered]    = frost(0.14f);
    colors[ImGuiCol_HeaderActive]     = frost(0.18f);
    colors[ImGuiCol_CheckMark]        = ImVec4(1, 1, 1, 0.95f);
    colors[ImGuiCol_SliderGrab]       = frost(0.55f);
    colors[ImGuiCol_SliderGrabActive] = frost(0.75f);
    colors[ImGuiCol_PopupBg]          = ImVec4(0.10f, 0.105f, 0.125f, 0.94f);
    colors[ImGuiCol_ScrollbarBg]      = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_ScrollbarGrab]    = frost(0.16f);
    colors[ImGuiCol_ScrollbarGrabHovered] = frost(0.24f);
    colors[ImGuiCol_ScrollbarGrabActive]  = frost(0.30f);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    ImFontConfig font_cfg;
    font_cfg.FontDataOwnedByAtlas = false;
    ImFont* font_medium = io.Fonts->AddFontFromMemoryTTF(
        inter_medium.data(), static_cast<int>(inter_medium.size()), 17.0f, &font_cfg);
    ImFont* font_semibold = io.Fonts->AddFontFromMemoryTTF(
        inter_semibold.data(), static_cast<int>(inter_semibold.size()), 26.0f, &font_cfg);
    IM_ASSERT(font_medium != nullptr && font_semibold != nullptr);
    io.FontDefault = font_medium;

    if (ImFont* icons = AddIconFont(io.Fonts, "data\\fa\\fa-solid-900.ttf",
                                    IDR_FA_SOLID, 32.0f)) {
        Glass::SetIconFont(icons);
        g_mainIconFont = icons;
    }
    if (ImFont* brands = AddIconFont(io.Fonts, "data\\fa\\fa-brands-400.ttf",
                                     IDR_FA_BRANDS, 32.0f)) {
        Glass::SetIconFontBrands(brands);
        g_mainIconBrands = brands;
    }

    if (!g_renderer.Init(g_device, g_context)) {
        MessageBoxA(hwnd, "Glass renderer init failed (shader compile?)", "APEX",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    Glass::g = &g_renderer;
    g_mainCtx = ImGui::GetCurrentContext();

    (void)backdropOk;

    Glass::SetGlobalMaterial(0.70f, 0.25f, 1.35f, 12.0f, 0.9f, 0.22f, 0.10f);

    Glass::SetLiquidFlow(0.0f);
    Glass::SetDensity(1.0f);
    Glass::SetAccent(0.33f, 0.45f, 1.0f);

    InitOverlayImGui(scale);

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    apex::spotify::Start();

    {
        const std::string logoFile = AssetPath("logo.jpg");
        bool logoOk = false;
        if (FileExists(logoFile))
            logoOk = apex::logo::Load(g_device, logoFile.c_str());
        if (!logoOk) {
            DWORD logoBytes = 0;
            if (const void* raw = EmbeddedResource(IDR_LOGO, &logoBytes))
                logoOk = apex::logo::LoadFromMemory(g_device, raw, logoBytes);
        }
        (void)logoOk;
    }

    apex::App& app = apex::App::Get();
    app.Init(hwnd);
    app.fontSemi = font_semibold;

    apex::ReapplyHotkeys();

    apex::crosshair::Create();
    apex::overlay::Create();
    g_backdrop.AddFreezeWindow(apex::crosshair::Window());
    g_backdrop.AddFreezeWindow(apex::overlay::Window());

    LARGE_INTEGER qpf{};
    QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER lastBlur{};

    const ULONGLONG startTick = GetTickCount64();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        apex::crosshair::Tick();
        apex::overlay::Tick();

        const bool ovrAllowed = (GetTickCount64() - startTick) > 3000;
        if (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            RenderOverlayFrame(app, ovrAllowed);
            Sleep(10);
            continue;
        }
        g_occluded = false;

        if (g_resizeW != 0 && g_resizeH != 0) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double sinceBlur =
            lastBlur.QuadPart ? static_cast<double>(now.QuadPart - lastBlur.QuadPart) /
                                     static_cast<double>(qpf.QuadPart)
                              : 1.0;
        const bool doBlur = sinceBlur >= (1.0 / 15.0);

        if (doBlur) {
            lastBlur = now;
            if (backdropOk)
                g_backdrop.Capture();
        }

        RECT client{};
        GetClientRect(hwnd, &client);
        RECT windowRect{};
        GetWindowRect(hwnd, &windowRect);
        const int cw = client.right - client.left;
        const int ch = client.bottom - client.top;
        const int originX = windowRect.left - g_backdrop.originX();
        const int originY = windowRect.top - g_backdrop.originY();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();

        if (io.DeltaTime > 0.05f)
            io.DeltaTime = 1.0f / 30.0f;
        ImGui::NewFrame();

        const ImVec2 cursorLocal(io.MousePos.x - static_cast<float>(windowRect.left),
                                 io.MousePos.y - static_cast<float>(windowRect.top));
        g_renderer.BeginFrame(cw, ch, originX, originY, g_backdrop.width(),
                              g_backdrop.height(), cursorLocal);

        const float entrance = std::max(0.0f, std::min(1.0f, g_renderer.Entrance()));
        style.Alpha = entrance;
        g_renderer.SetSubmitFade(1.0f);

        app.Draw();

        Glass::DrawNotifications(static_cast<float>(cw), static_cast<float>(ch));

        g_itemHover = ImGui::IsAnyItemHovered();

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && ImGui::IsAnyItemHovered() &&
            !Glass::SfxConsumed())
            Glass::PlaySfx(Glass::Sfx::Tap);
        Glass::SfxResetConsumed();

        ImGui::Render();

        RenderOverlayFrame(app, ovrAllowed);

        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        g_renderer.SetRenderScale(1.0f);
        g_context->OMSetRenderTargets(1, &g_rtView, nullptr);
        g_context->ClearRenderTargetView(g_rtView, clear);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(cw);
        vp.Height   = static_cast<float>(ch);
        vp.MaxDepth = 1.0f;
        g_context->RSSetViewports(1, &vp);
        g_renderer.Render(g_backdrop.heavySRV(), g_backdrop.softSRV());
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (doBlur && backdropOk) {
            ID3D11Texture2D* backBuffer = nullptr;
            if (SUCCEEDED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) &&
                backBuffer != nullptr) {
                g_backdrop.BlurUI(backBuffer, cw, ch);
                backBuffer->Release();
                Glass::g_uiBlurTex    = reinterpret_cast<ImTextureID>(
                    g_backdrop.uiSRV());
                Glass::g_uiBlurOrigin = ImVec2(0.0f, 0.0f);
                Glass::g_uiBlurSize   = ImVec2(static_cast<float>(cw),
                                               static_cast<float>(ch));
            }
        }

        const HRESULT hr = g_swapChain->Present(1, 0);
        g_occluded = (hr == DXGI_STATUS_OCCLUDED);

        if (app.CloseRequested())
            done = true;
    }

    apex::spotify::Stop();
    apex::UnregisterHotkeys();
    apex::overlay::Destroy();
    apex::crosshair::Destroy();

    if (g_ovrHwnd != nullptr) {
        RECT wr{};
        if (GetWindowRect(g_ovrHwnd, &wr)) {
            apex::settings::SetInt("player_x", wr.left);
            apex::settings::SetInt("player_y", wr.top);
            apex::settings::SetInt("player_w", wr.right - wr.left);
            apex::settings::SetInt("player_h", wr.bottom - wr.top);
        }
    }

    app.Shutdown();

    apex::logo::Release();

    g_backdrop.Shutdown();
    g_renderer.Shutdown();
    if (g_ovrInited) {
        g_ovrRenderer.Shutdown();
        g_ovrInited = false;
    }
    Glass::g = nullptr;

    ImGui::SetCurrentContext(g_mainCtx);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (g_ovrCtx != nullptr) {
        ImGui::SetCurrentContext(g_ovrCtx);
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext(g_ovrCtx);
        g_ovrCtx = nullptr;
    }

    CleanupOverlayRT();
    if (g_ovrSwap != nullptr) {
        g_ovrSwap->Release();
        g_ovrSwap = nullptr;
    }

    CleanupDeviceD3D();
    CoUninitialize();
    DestroyWindow(hwnd);
    if (g_ovrHwnd != nullptr) {
        DestroyWindow(g_ovrHwnd);
        g_ovrHwnd = nullptr;
    }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (mutex != nullptr)
        CloseHandle(mutex);
    return 0;
}
