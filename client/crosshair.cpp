#include "crosshair.h"

#include "settings.h"

#include <objbase.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstring>

namespace apex::crosshair {
namespace {

constexpr int kW = 100;
constexpr int kH = 100;

HWND      g_hwnd    = nullptr;
ULONG_PTR g_token   = 0;
bool      g_on      = false;
int       g_shape   = 0;
int       g_size    = 10;
int       g_thick   = 1;
float     g_rgb[3]  = {1.0f, 1.0f, 1.0f};
bool      g_dirty   = true;
int       g_lastCol = -1;
bool      g_class   = false;

int PackedColor() {
    const int r = static_cast<int>(g_rgb[0] * 255.0f + 0.5f);
    const int g = static_cast<int>(g_rgb[1] * 255.0f + 0.5f);
    const int b = static_cast<int>(g_rgb[2] * 255.0f + 0.5f);
    return (r << 16) | (g << 8) | b;
}

void ApplyColor(int packed) {
    g_rgb[0] = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
    g_rgb[1] = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
    g_rgb[2] = static_cast<float>(packed & 0xFF) / 255.0f;
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void Render() {
    if (g_hwnd == nullptr)
        return;

    HDC screen = GetDC(nullptr);
    if (screen == nullptr)
        return;
    HDC mem = CreateCompatibleDC(screen);

    BITMAPINFO bmi;
    std::memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = kW;
    bmi.bmiHeader.biHeight      = -kH;
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void*   bits = nullptr;
    HBITMAP dib  = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr)
            DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        return;
    }

    std::memset(bits, 0, static_cast<size_t>(kW) * static_cast<size_t>(kH) * 4);
    HGDIOBJ old = SelectObject(mem, dib);

    {
        Gdiplus::Graphics g(mem);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        const int packed = PackedColor();
        const Gdiplus::Color col(
            255, static_cast<BYTE>((packed >> 16) & 0xFF),
            static_cast<BYTE>((packed >> 8) & 0xFF),
            static_cast<BYTE>(packed & 0xFF));

        const float c = 50.0f;
        const float s = static_cast<float>(g_size);
        const float t = static_cast<float>(g_thick);

        switch (g_shape) {
            case 1: {
                Gdiplus::Pen pen(col, t);
                g.DrawEllipse(&pen, c - s, c - s, s * 2.0f, s * 2.0f);
                break;
            }
            case 2: {
                Gdiplus::SolidBrush brush(col);
                g.FillEllipse(&brush, c - s * 0.5f, c - s * 0.5f, s, s);
                break;
            }
            case 3: {
                Gdiplus::Pen pen(col, t);
                g.DrawRectangle(&pen, c - s, c - s, s * 2.0f, s * 2.0f);
                break;
            }
            case 4: {
                Gdiplus::Pen pen(col, t);
                const Gdiplus::PointF pts[5] = {
                    {c, c - s}, {c + s, c}, {c, c + s}, {c - s, c}, {c, c - s}};
                g.DrawLines(&pen, pts, 5);
                break;
            }
            default: {
                Gdiplus::Pen pen(col, t);
                g.DrawLine(&pen, c, c - s, c, c + s);
                g.DrawLine(&pen, c - s, c, c + s, c);
                break;
            }
        }
    }

    POINT origin = {0, 0};
    SIZE   size   = {kW, kH};
    BLENDFUNCTION blend = {};
    blend.BlendOp             = AC_SRC_OVER;
    blend.BlendFlags          = 0;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat         = AC_SRC_ALPHA;
    UpdateLayeredWindow(g_hwnd, screen, nullptr, &size, mem, &origin, 0, &blend,
                        ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);

    g_lastCol = PackedColor();
}

}

bool Create() {
    if (g_hwnd != nullptr)
        return true;

    Gdiplus::GdiplusStartupInput input;
    if (GdiplusStartup(&g_token, &input, nullptr) != Gdiplus::Ok)
        return false;

    g_on    = settings::GetBool("crosshair_on", false);
    g_shape = std::max(0, std::min(4, settings::GetInt("crosshair_shape", 0)));
    g_size  = std::max(2, std::min(50, settings::GetInt("crosshair_size", 10)));
    g_thick = std::max(1, std::min(5, settings::GetInt("crosshair_thickness", 1)));
    ApplyColor(settings::GetInt("crosshair_color", 0xFFFFFF));

    if (!g_class) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style         = CS_CLASSDC;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"APEXCrosshairWindow";
        RegisterClassExW(&wc);
        g_class = true;
    }

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    g_hwnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                                 WS_EX_TOPMOST | WS_EX_TRANSPARENT,
                             L"APEXCrosshairWindow", nullptr, WS_POPUP,
                             sw / 2 - kW / 2, sh / 2 - kH / 2, kW, kH, nullptr,
                             nullptr, GetModuleHandle(nullptr), nullptr);
    if (g_hwnd == nullptr) {
        Gdiplus::GdiplusShutdown(g_token);
        g_token = 0;
        return false;
    }

    Render();
    if (g_on)
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void Destroy() {
    if (g_hwnd != nullptr) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    if (g_token != 0) {
        Gdiplus::GdiplusShutdown(g_token);
        g_token = 0;
    }
}

void Tick() {
    if (g_hwnd == nullptr)
        return;

    const int packed = PackedColor();
    if (packed != g_lastCol) {
        settings::SetInt("crosshair_color", packed);
        Render();
    } else if (g_dirty) {
        g_dirty = false;
        Render();
    }

    if (g_on) {
        RECT rc{};
        if (GetWindowRect(g_hwnd, &rc)) {
            const int sw = GetSystemMetrics(SM_CXSCREEN);
            const int sh = GetSystemMetrics(SM_CYSCREEN);
            const int x  = sw / 2 - kW / 2;
            const int y  = sh / 2 - kH / 2;
            if (rc.left != x || rc.top != y || rc.right - rc.left != kW ||
                rc.bottom - rc.top != kH)
                SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, kW, kH,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
}

HWND Window() {
    return g_hwnd;
}

void Toggle() {
    SetEnabled(!g_on);
}

void SetEnabled(bool on) {
    g_on = on;
    settings::SetBool("crosshair_on", on);
    if (g_hwnd == nullptr)
        return;
    if (on) {
        Render();
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
}

bool Enabled() {
    return g_on;
}

int Shape() {
    return g_shape;
}

void SetShape(int shape) {
    g_shape = std::max(0, std::min(4, shape));
    settings::SetInt("crosshair_shape", g_shape);
    g_dirty = true;
}

int Size() {
    return g_size;
}

void SetSize(int size) {
    g_size = std::max(2, std::min(50, size));
    settings::SetInt("crosshair_size", g_size);
    g_dirty = true;
}

int Thickness() {
    return g_thick;
}

void SetThickness(int thickness) {
    g_thick = std::max(1, std::min(5, thickness));
    settings::SetInt("crosshair_thickness", g_thick);
    g_dirty = true;
}

float* Rgb() {
    return g_rgb;
}

}
