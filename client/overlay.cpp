#include "overlay.h"

#include "settings.h"

#include <objbase.h>
#include <gdiplus.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace apex::overlay {
namespace {

constexpr int kRes[4][2] = {{1920, 1080}, {1440, 1080}, {1280, 720},
                            {2560, 1440}};
constexpr int   kRegionMargin = 64;
constexpr int   kHandle       = 8;
constexpr int   kGrab         = 18;
constexpr int   kRotGap       = 34;
constexpr float kPi           = 3.14159265f;

enum Zone {
    Z_NONE = 0,
    Z_MOVE,
    Z_TOPLEFT,
    Z_TOPRIGHT,
    Z_BOTTOMLEFT,
    Z_BOTTOMRIGHT,
    Z_ROTATE,
};

struct Item {
    std::string        path;
    int                x = 0;
    int                y = 0;
    int                w = 300;
    int                h = 300;
    float              opacity = 1.0f;
    float              angle   = 0.0f;
    int                type    = 0;
    Gdiplus::Bitmap*   img     = nullptr;
    int                frames  = 1;
    std::vector<int>   delays;
    int                frame   = 0;
    ULONGLONG          frameAt = 0;
};

HWND                    g_hwnd        = nullptr;
ULONG_PTR               g_token       = 0;
bool                    g_class       = false;
bool                    g_on          = false;
bool                    g_click       = false;
bool                    g_lock        = false;
int                     g_resIndex    = 0;
int                     g_resW        = 1920;
int                     g_resH        = 1080;
int                     g_sel         = -1;
int                     g_zone        = Z_NONE;
POINT                   g_lastPt      = {0, 0};
int                     g_dragItem    = -1;
int                     g_dragX0      = 0;
int                     g_dragY0      = 0;
int                     g_dragW0      = 0;
int                     g_dragH0      = 0;
float                   g_dragAngle0  = 0.0f;
float                   g_dragCX      = 0.0f;
float                   g_dragCY      = 0.0f;
float                   g_dragFixedX  = 0.0f;
float                   g_dragFixedY  = 0.0f;
float                   g_dragMouse0  = 0.0f;
int                     g_dragSignX   = 1;
int                     g_dragSignY   = 1;
bool                    g_dirty       = true;
bool                    g_regionDirty = true;
std::string             g_lastPath;
std::vector<Item>       g_items;
static std::string      g_nameBuf;

int Clamp(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

std::string Utf8FromWide(const wchar_t* text) {
    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0)
        return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, &out[0], len, nullptr, nullptr);
    out.resize(static_cast<size_t>(len - 1));
    return out;
}

std::wstring WideFromUtf8(const std::string& text) {
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (len <= 0)
        return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &out[0], len);
    out.resize(static_cast<size_t>(len - 1));
    return out;
}

std::string OverlaysDir() {
    return settings::EnsureDir(settings::RootDir() + "\\overlays");
}

std::string DefaultConfigPath() {
    return OverlaysDir() + "\\config.json";
}

bool ReadAll(const std::string& path, std::string* out) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    const DWORD size = GetFileSize(h, nullptr);
    if (size == INVALID_FILE_SIZE || size > 16u * 1024u * 1024u) {
        CloseHandle(h);
        return false;
    }
    out->assign(static_cast<size_t>(size), '\0');
    DWORD read = 0;
    const bool ok = size == 0 ||
                    ReadFile(h, &(*out)[0], size, &read, nullptr) != FALSE;
    CloseHandle(h);
    return ok && read == size;
}

bool WriteAll(const std::string& path, const std::string& data) {
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    DWORD written = 0;
    const bool ok = data.empty() ||
                    WriteFile(h, data.data(), static_cast<DWORD>(data.size()),
                              &written, nullptr) != FALSE;
    CloseHandle(h);
    return ok && written == data.size();
}

size_t ScanObjectEnd(const std::string& text, size_t start) {
    bool inString = false;
    for (size_t i = start + 1; i < text.size(); ++i) {
        const char ch = text[i];
        if (inString) {
            if (ch == '\\') {
                ++i;
                continue;
            }
            if (ch == '"')
                inString = false;
        } else if (ch == '"') {
            inString = true;
        } else if (ch == '}') {
            return i;
        }
    }
    return std::string::npos;
}

size_t FindValue(const std::string& text, const char* key) {
    const std::string pattern = std::string("\"") + key + "\"";
    size_t p = text.find(pattern);
    if (p == std::string::npos)
        return std::string::npos;
    p = text.find(':', p + pattern.size());
    if (p == std::string::npos)
        return std::string::npos;
    ++p;
    while (p < text.size() && (text[p] == ' ' || text[p] == '\t' ||
                               text[p] == '\n' || text[p] == '\r'))
        ++p;
    return p;
}

std::string JsonString(const std::string& text, const char* key,
                       const std::string& fallback = {}) {
    size_t p = FindValue(text, key);
    if (p == std::string::npos || p >= text.size() || text[p] != '"')
        return fallback;
    ++p;
    std::string out;
    while (p < text.size() && text[p] != '"') {
        if (text[p] == '\\' && p + 1 < text.size()) {
            const char esc = text[p + 1];
            if (esc == 'n')
                out += '\n';
            else if (esc == 't')
                out += '\t';
            else
                out += esc;
            p += 2;
        } else {
            out += text[p];
            ++p;
        }
    }
    return out;
}

double JsonNumber(const std::string& text, const char* key, double fallback) {
    const size_t p = FindValue(text, key);
    if (p == std::string::npos || p >= text.size())
        return fallback;
    return std::strtod(text.c_str() + p, nullptr);
}

Gdiplus::Bitmap* LoadBitmap(const std::string& path, int type, int* frames,
                            std::vector<int>* delays) {
    const std::wstring wide = WideFromUtf8(path);
    if (wide.empty())
        return nullptr;
    Gdiplus::Bitmap* img = new Gdiplus::Bitmap(wide.c_str());
    if (img->GetLastStatus() != Gdiplus::Ok) {
        delete img;
        return nullptr;
    }
    if (type == 1) {
        const GUID dim = Gdiplus::FrameDimensionTime;
        const UINT count = img->GetFrameCount(&dim);
        if (count > 1) {
            *frames = static_cast<int>(count);
            delays->assign(static_cast<size_t>(count), 100);
            const UINT size = img->GetPropertyItemSize(0x5100);
            if (size > 0) {
                char* buffer = new char[size];
                std::memset(buffer, 0, size);
                if (img->GetPropertyItem(0x5100, size,
                                         reinterpret_cast<Gdiplus::PropertyItem*>(
                                             buffer)) == Gdiplus::Ok) {
                    const auto* item =
                        reinterpret_cast<const Gdiplus::PropertyItem*>(buffer);
                    if (item->type == 4 && item->length >= count * 4) {
                        const auto* values =
                            reinterpret_cast<const unsigned int*>(item->value);
                        for (UINT i = 0; i < count; ++i)
                            delays->at(static_cast<size_t>(i)) =
                                values[i] > 0
                                    ? static_cast<int>(values[i]) * 10
                                    : 100;
                    } else if (item->type == 3 && item->length >= count * 2) {
                        const auto* values =
                            reinterpret_cast<const unsigned short*>(item->value);
                        for (UINT i = 0; i < count; ++i)
                            delays->at(static_cast<size_t>(i)) =
                                values[i] > 0
                                    ? static_cast<int>(values[i]) * 10
                                    : 100;
                    }
                }
                delete[] buffer;
            }
            img->SelectActiveFrame(&dim, 0);
        }
    }
    return img;
}

void RotatePoint(float* x, float* y, float cx, float cy, float degrees) {
    const float r = degrees * kPi / 180.0f;
    const float c = std::cos(r);
    const float s = std::sin(r);
    const float dx = *x - cx;
    const float dy = *y - cy;
    *x = cx + dx * c - dy * s;
    *y = cy + dx * s + dy * c;
}

void ItemCenter(const Item& it, float* cx, float* cy) {
    *cx = static_cast<float>(it.x) + static_cast<float>(it.w) * 0.5f;
    *cy = static_cast<float>(it.y) + static_cast<float>(it.h) * 0.5f;
}

// Local, unrotated coordinates of a screen point relative to the item centre.
void ToLocal(const Item& it, int px, int py, float* lx, float* ly) {
    float cx = 0.0f, cy = 0.0f;
    ItemCenter(it, &cx, &cy);
    *lx = static_cast<float>(px);
    *ly = static_cast<float>(py);
    RotatePoint(lx, ly, cx, cy, -it.angle);
}

bool LocalInside(const Item& it, float lx, float ly) {
    return lx >= static_cast<float>(it.x) &&
           lx < static_cast<float>(it.x + it.w) &&
           ly >= static_cast<float>(it.y) &&
           ly < static_cast<float>(it.y + it.h);
}

bool InsideItem(const Item& it, int px, int py) {
    float lx = 0.0f, ly = 0.0f;
    ToLocal(it, px, py, &lx, &ly);
    return LocalInside(it, lx, ly);
}

// Rotation handle sits above the top edge in the item's own frame.
void RotationHandle(const Item& it, float* hx, float* hy) {
    float cx = 0.0f, cy = 0.0f;
    ItemCenter(it, &cx, &cy);
    *hx = cx;
    *hy = cy - static_cast<float>(it.h) * 0.5f - kRotGap;
    RotatePoint(hx, hy, cx, cy, it.angle);
}

Zone DetectZone(const Item& it, int px, int py) {
    float hx = 0.0f, hy = 0.0f;
    RotationHandle(it, &hx, &hy);
    const float rdx = static_cast<float>(px) - hx;
    const float rdy = static_cast<float>(py) - hy;
    if (rdx * rdx + rdy * rdy <= static_cast<float>(kGrab * kGrab))
        return Z_ROTATE;

    float lx = 0.0f, ly = 0.0f;
    ToLocal(it, px, py, &lx, &ly);
    const float gx0 = static_cast<float>(it.x) - kGrab * 0.5f;
    const float gx1 = static_cast<float>(it.x + it.w) + kGrab * 0.5f;
    const float gy0 = static_cast<float>(it.y) - kGrab * 0.5f;
    const float gy1 = static_cast<float>(it.y + it.h) + kGrab * 0.5f;

    if (lx >= gx0 && lx <= gx1 && ly >= gy0 && ly <= gy1) {
        const bool l = lx <= static_cast<float>(it.x) + kGrab * 0.5f;
        const bool r = lx >= static_cast<float>(it.x + it.w) - kGrab * 0.5f;
        const bool t = ly <= static_cast<float>(it.y) + kGrab * 0.5f;
        const bool b = ly >= static_cast<float>(it.y + it.h) - kGrab * 0.5f;
        if (t && l) return Z_TOPLEFT;
        if (t && r) return Z_TOPRIGHT;
        if (b && l) return Z_BOTTOMLEFT;
        if (b && r) return Z_BOTTOMRIGHT;
    }
    return LocalInside(it, lx, ly) ? Z_MOVE : Z_NONE;
}

void ItemAabb(const Item& it, float* l, float* t, float* r, float* b) {
    const float cx = static_cast<float>(it.x) + static_cast<float>(it.w) * 0.5f;
    const float cy = static_cast<float>(it.y) + static_cast<float>(it.h) * 0.5f;
    float xs[4] = {static_cast<float>(it.x), static_cast<float>(it.x + it.w),
                   static_cast<float>(it.x + it.w), static_cast<float>(it.x)};
    float ys[4] = {static_cast<float>(it.y), static_cast<float>(it.y),
                   static_cast<float>(it.y + it.h),
                   static_cast<float>(it.y + it.h)};
    *l = *t = 1e9f;
    *r = *b = -1e9f;
    for (int i = 0; i < 4; ++i) {
        RotatePoint(&xs[i], &ys[i], cx, cy, it.angle);
        *l = std::min(*l, xs[i]);
        *t = std::min(*t, ys[i]);
        *r = std::max(*r, xs[i]);
        *b = std::max(*b, ys[i]);
    }
}

void RebuildRegion() {
    if (g_hwnd == nullptr)
        return;

    const bool pass = g_click || g_lock;
    LONG_PTR style = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (pass)
        style |= WS_EX_TRANSPARENT;
    else
        style &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, style);

    HRGN region = CreateRectRgn(0, 0, 0, 0);
    for (size_t i = 0; i < g_items.size(); ++i) {
        const Item& it = g_items[i];
        float l = 0.0f, t = 0.0f, r = 0.0f, b = 0.0f;
        ItemAabb(it, &l, &t, &r, &b);
        const int m = (static_cast<int>(i) == g_sel && !g_lock)
                          ? kRegionMargin
                          : 0;
        HRGN part = CreateRectRgn(static_cast<int>(std::floor(l)) - m,
                                  static_cast<int>(std::floor(t)) - m,
                                  static_cast<int>(std::ceil(r)) + m,
                                  static_cast<int>(std::ceil(b)) + m);
        CombineRgn(region, region, part, RGN_OR);
        DeleteObject(part);
    }
    if (SetWindowRgn(g_hwnd, region, TRUE) == 0)
        DeleteObject(region);
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
    bmi.bmiHeader.biWidth       = g_resW;
    bmi.bmiHeader.biHeight      = -g_resH;
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

    std::memset(bits, 0, static_cast<size_t>(g_resW) *
                             static_cast<size_t>(g_resH) * 4);
    HGDIOBJ old = SelectObject(mem, dib);

    {
        Gdiplus::Graphics g(mem);
        g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        for (const Item& it : g_items) {
            if (it.img == nullptr)
                continue;
            Gdiplus::ImageAttributes attrs;
            Gdiplus::ColorMatrix matrix = {};
            matrix.m[0][0] = 1.0f;
            matrix.m[1][1] = 1.0f;
            matrix.m[2][2] = 1.0f;
            matrix.m[3][3] = it.opacity;
            matrix.m[4][4] = 1.0f;
            attrs.SetColorMatrix(&matrix);
            attrs.SetWrapMode(Gdiplus::WrapModeTileFlipXY);

            const Gdiplus::RectF dst(static_cast<Gdiplus::REAL>(it.x),
                                     static_cast<Gdiplus::REAL>(it.y),
                                     static_cast<Gdiplus::REAL>(it.w),
                                     static_cast<Gdiplus::REAL>(it.h));
            float cx = 0.0f, cy = 0.0f;
            ItemCenter(it, &cx, &cy);
            Gdiplus::Matrix saved;
            Gdiplus::Matrix transform;
            transform.Translate(cx, cy);
            transform.Rotate(it.angle);
            transform.Translate(-cx, -cy);
            g.GetTransform(&saved);
            g.SetTransform(&transform);
            g.DrawImage(it.img, dst, 0.0f, 0.0f,
                        static_cast<Gdiplus::REAL>(it.img->GetWidth()),
                        static_cast<Gdiplus::REAL>(it.img->GetHeight()),
                        Gdiplus::UnitPixel, &attrs);
            g.SetTransform(&saved);
        }

        if (g_sel >= 0 && g_sel < static_cast<int>(g_items.size()) && !g_lock) {
            const Item& it = g_items[static_cast<size_t>(g_sel)];
            float cx = 0.0f, cy = 0.0f;
            ItemCenter(it, &cx, &cy);
            Gdiplus::Matrix saved;
            Gdiplus::Matrix transform;
            transform.Translate(cx, cy);
            transform.Rotate(it.angle);
            transform.Translate(-cx, -cy);
            g.GetTransform(&saved);
            g.SetTransform(&transform);

            const Gdiplus::Color cyan(255, 0, 255, 255);
            Gdiplus::Pen pen(cyan, 2.0f);
            pen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawRectangle(&pen, it.x, it.y, it.w, it.h);

            Gdiplus::SolidBrush brush(cyan);
            const int hs = kHandle;
            const int corners[4][2] = {{it.x, it.y},
                                       {it.x + it.w, it.y},
                                       {it.x, it.y + it.h},
                                       {it.x + it.w, it.y + it.h}};
            for (const auto& corner : corners)
                g.FillRectangle(&brush, corner[0] - hs / 2, corner[1] - hs / 2,
                                hs, hs);
            g.SetTransform(&saved);

            float hx = 0.0f, hy = 0.0f;
            RotationHandle(it, &hx, &hy);
            float tx = cx, ty = static_cast<float>(it.y);
            RotatePoint(&tx, &ty, cx, cy, it.angle);
            Gdiplus::Pen stem(cyan, 2.0f);
            g.DrawLine(&stem, tx, ty, hx, hy);
            g.FillEllipse(&brush, hx - hs * 0.75f, hy - hs * 0.75f,
                          hs * 1.5f, hs * 1.5f);
        }
    }

    POINT origin = {0, 0};
    SIZE   size   = {g_resW, g_resH};
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

    g_dirty = false;
}

void FreeItems() {
    for (Item& it : g_items)
        delete it.img;
    g_items.clear();
    g_sel = -1;
}

void PushItem(const std::string& path, int x, int y, int w, int h,
              float opacity, int type, float angle) {
    Item it;
    it.path    = path;
    it.x       = x;
    it.y       = y;
    it.w       = w > 0 ? w : 300;
    it.h       = h > 0 ? h : 300;
    it.opacity = opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    it.angle   = angle;
    it.type    = type == 1 ? 1 : 0;
    it.img     = LoadBitmap(path, it.type, &it.frames, &it.delays);
    if (it.img == nullptr) {
        if (w <= 0 || h <= 0) {
            it.w = 300;
            it.h = 300;
        }
    } else if (w <= 0 || h <= 0) {
        it.w = static_cast<int>(it.img->GetWidth());
        it.h = static_cast<int>(it.img->GetHeight());
        if (it.w <= 0) it.w = 300;
        if (it.h <= 0) it.h = 300;
    }
    g_items.push_back(std::move(it));
}

bool LoadPath(const std::string& path, int* qtKeybind) {
    std::string data;
    if (!ReadAll(path, &data))
        return false;
    const size_t itemsPos = data.find("\"items\"");
    if (itemsPos == std::string::npos)
        return false;

    if (qtKeybind != nullptr)
        *qtKeybind = static_cast<int>(JsonNumber(data, "keybind", 16777248.0));

    const size_t open = data.find('[', itemsPos);
    if (open == std::string::npos)
        return false;
    const size_t close = data.find(']', open);
    const size_t limit = close == std::string::npos ? data.size() : close;

    FreeItems();
    size_t p = open;
    while (true) {
        const size_t objStart = data.find('{', p);
        if (objStart == std::string::npos || objStart >= limit)
            break;
        const size_t objEnd = ScanObjectEnd(data, objStart);
        if (objEnd == std::string::npos || objEnd > limit)
            break;
        const std::string obj = data.substr(objStart, objEnd - objStart + 1);
        const std::string file = JsonString(obj, "path");
        if (!file.empty()) {
            const int x = static_cast<int>(JsonNumber(obj, "x", 100));
            const int y = static_cast<int>(JsonNumber(obj, "y", 100));
            const int w = static_cast<int>(JsonNumber(obj, "w", 300));
            const int h = static_cast<int>(JsonNumber(obj, "h", 300));
            const float opacity =
                static_cast<float>(JsonNumber(obj, "opacity", 1.0));
            const float angle =
                static_cast<float>(JsonNumber(obj, "angle", 0.0));
            const int type = static_cast<int>(JsonNumber(obj, "type", 0));
            PushItem(file, x, y, w, h, opacity, type, angle);
        }
        p = objEnd + 1;
    }

    g_dirty       = true;
    g_regionDirty = true;
    return true;
}

bool SavePath(const std::string& path, int qtKeybind) {
    std::string out = "{\"items\":[";
    for (size_t i = 0; i < g_items.size(); ++i) {
        const Item& it = g_items[i];
        if (i > 0)
            out += ',';
        out += "{\"path\":\"";
        for (const char ch : it.path) {
            if (ch == '"' || ch == '\\')
                out += '\\';
            out += ch;
        }
        char number[64];
        out += "\",\"x\":";
        std::snprintf(number, sizeof(number), "%d", it.x);
        out += number;
        out += ",\"y\":";
        std::snprintf(number, sizeof(number), "%d", it.y);
        out += number;
        out += ",\"w\":";
        std::snprintf(number, sizeof(number), "%d", it.w);
        out += number;
        out += ",\"h\":";
        std::snprintf(number, sizeof(number), "%d", it.h);
        out += number;
        out += ",\"opacity\":";
        std::snprintf(number, sizeof(number), "%g", it.opacity);
        out += number;
        out += ",\"angle\":";
        std::snprintf(number, sizeof(number), "%g", it.angle);
        out += number;
        out += ",\"type\":";
        std::snprintf(number, sizeof(number), "%d", it.type);
        out += number;
        out += '}';
    }
    out += "],\"keybind\":";
    char number[32];
    std::snprintf(number, sizeof(number), "%d", qtKeybind);
    out += number;
    out += '}';
    return WriteAll(path, out);
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_LBUTTONDOWN: {
            if (g_click || g_lock)
                break;
            const int px = GET_X_LPARAM(lParam);
            const int py = GET_Y_LPARAM(lParam);

            int hit = -1;
            if (g_sel >= 0 && g_sel < static_cast<int>(g_items.size())) {
                const Item& sel = g_items[static_cast<size_t>(g_sel)];
                if (DetectZone(sel, px, py) != Z_NONE)
                    hit = g_sel;
            }
            if (hit < 0) {
                for (int i = static_cast<int>(g_items.size()) - 1; i >= 0; --i) {
                    if (InsideItem(g_items[static_cast<size_t>(i)], px, py)) {
                        hit = i;
                        break;
                    }
                }
            }

            if (hit < 0) {
                if (g_sel != -1) {
                    g_sel         = -1;
                    g_dirty       = true;
                    g_regionDirty = true;
                }
                return 0;
            }

            g_sel  = hit;
            Item& it = g_items[static_cast<size_t>(hit)];
            g_zone = DetectZone(it, px, py);
            if (g_zone == Z_NONE)
                g_zone = Z_MOVE;

            g_dragItem = hit;
            g_dragX0   = it.x;
            g_dragY0   = it.y;
            g_dragW0   = it.w;
            g_dragH0   = it.h;
            g_dragAngle0 = it.angle;
            g_lastPt.x   = px;
            g_lastPt.y   = py;

            float cx = 0.0f, cy = 0.0f;
            ItemCenter(it, &cx, &cy);
            g_dragCX = cx;
            g_dragCY = cy;
            g_dragMouse0 =
                std::atan2(static_cast<float>(py) - cy,
                           static_cast<float>(px) - cx);

            if (g_zone == Z_TOPLEFT || g_zone == Z_TOPRIGHT ||
                g_zone == Z_BOTTOMLEFT || g_zone == Z_BOTTOMRIGHT) {
                g_dragSignX = (g_zone == Z_TOPLEFT ||
                               g_zone == Z_BOTTOMLEFT)
                                  ? -1
                                  : 1;
                g_dragSignY = (g_zone == Z_TOPLEFT ||
                               g_zone == Z_TOPRIGHT)
                                  ? -1
                                  : 1;
                const float fx =
                    cx + static_cast<float>(-g_dragSignX * it.w) * 0.5f;
                const float fy =
                    cy + static_cast<float>(-g_dragSignY * it.h) * 0.5f;
                g_dragFixedX = fx;
                g_dragFixedY = fy;
                RotatePoint(&g_dragFixedX, &g_dragFixedY, cx, cy, it.angle);
            }

            SetCapture(hWnd);
            g_dirty       = true;
            g_regionDirty = true;
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g_click || g_lock || GetCapture() != hWnd ||
                g_zone == Z_NONE || g_dragItem < 0 ||
                g_dragItem >= static_cast<int>(g_items.size()))
                break;
            const int px = GET_X_LPARAM(lParam);
            const int py = GET_Y_LPARAM(lParam);
            Item& it = g_items[static_cast<size_t>(g_dragItem)];
            constexpr int minW = 24;
            constexpr int minH = 24;

            if (g_zone == Z_MOVE) {
                it.x = g_dragX0 + (px - g_lastPt.x);
                it.y = g_dragY0 + (py - g_lastPt.y);
            } else if (g_zone == Z_ROTATE) {
                const float mouseAngle =
                    std::atan2(static_cast<float>(py) - g_dragCY,
                               static_cast<float>(px) - g_dragCX);
                float degrees =
                    g_dragAngle0 +
                    (mouseAngle - g_dragMouse0) * 180.0f / kPi;
                while (degrees > 180.0f)
                    degrees -= 360.0f;
                while (degrees < -180.0f)
                    degrees += 360.0f;
                if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
                    degrees = std::round(degrees / 15.0f) * 15.0f;
                if (std::fabs(degrees) < 2.0f)
                    degrees = 0.0f;
                it.angle = degrees;
            } else {
                const float rad = g_dragAngle0 * kPi / 180.0f;
                const float c   = std::cos(rad);
                const float s   = std::sin(rad);
                const float dx  = static_cast<float>(px) - g_dragFixedX;
                const float dy  = static_cast<float>(py) - g_dragFixedY;
                const float lx  = dx * c + dy * s;
                const float ly  = -dx * s + dy * c;
                float newW = static_cast<float>(g_dragSignX) * lx;
                float newH = static_cast<float>(g_dragSignY) * ly;
                newW = std::max(static_cast<float>(minW), newW);
                newH = std::max(static_cast<float>(minH), newH);
                const float hx = static_cast<float>(g_dragSignX) * newW * 0.5f;
                const float hy = static_cast<float>(g_dragSignY) * newH * 0.5f;
                const float wcx = g_dragFixedX + hx * c - hy * s;
                const float wcy = g_dragFixedY + hx * s + hy * c;
                it.w = static_cast<int>(std::lround(newW));
                it.h = static_cast<int>(std::lround(newH));
                it.x = static_cast<int>(std::lround(wcx - newW * 0.5f));
                it.y = static_cast<int>(std::lround(wcy - newH * 0.5f));
            }
            g_dirty       = true;
            g_regionDirty = true;
            return 0;
        }
        case WM_LBUTTONUP: {
            if (GetCapture() == hWnd)
                ReleaseCapture();
            if (g_zone != Z_NONE) {
                g_zone        = Z_NONE;
                g_dragItem    = -1;
                g_regionDirty = true;
                g_dirty       = true;
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            g_zone     = Z_NONE;
            g_dragItem = -1;
            break;
        default:
            break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

}

bool Create() {
    if (g_hwnd != nullptr)
        return true;

    Gdiplus::GdiplusStartupInput input;
    if (GdiplusStartup(&g_token, &input, nullptr) != Gdiplus::Ok)
        return false;

    g_on       = settings::GetBool("overlay_on", false);
    g_click    = settings::GetBool("overlay_click", false);
    g_lock     = settings::GetBool("overlay_lock", false);
    g_resIndex = Clamp(settings::GetInt("overlay_res", 0), 0, 3);
    g_resW     = kRes[g_resIndex][0];
    g_resH     = kRes[g_resIndex][1];
    g_lastPath = settings::GetString("overlay_last", "");

    if (!g_class) {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style         = CS_CLASSDC;
        wc.lpfnWndProc   = WndProc;
        wc.hInstance     = GetModuleHandle(nullptr);
        wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"APEXImageOverlayWindow";
        RegisterClassExW(&wc);
        g_class = true;
    }

    DWORD ex = WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
    if (g_click || g_lock)
        ex |= WS_EX_TRANSPARENT;

    g_hwnd = CreateWindowExW(ex, L"APEXImageOverlayWindow", nullptr, WS_POPUP, 0,
                             0, g_resW, g_resH, nullptr, nullptr,
                             GetModuleHandle(nullptr), nullptr);
    if (g_hwnd == nullptr) {
        Gdiplus::GdiplusShutdown(g_token);
        g_token = 0;
        return false;
    }

    if (LoadPath(DefaultConfigPath(), nullptr))
        g_lastPath = DefaultConfigPath();

    Render();
    RebuildRegion();
    if (g_on)
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    return true;
}

void Destroy() {
    if (g_hwnd != nullptr) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
    FreeItems();
    if (g_token != 0) {
        Gdiplus::GdiplusShutdown(g_token);
        g_token = 0;
    }
}

void Tick() {
    if (g_hwnd == nullptr)
        return;

    const ULONGLONG now = GetTickCount64();
    bool animate = false;
    for (Item& it : g_items) {
        if (it.img == nullptr || it.frames <= 1)
            continue;
        int delay = 100;
        if (it.frame < static_cast<int>(it.delays.size()) &&
            it.delays[static_cast<size_t>(it.frame)] > 0)
            delay = it.delays[static_cast<size_t>(it.frame)];
        if (now - it.frameAt >= static_cast<ULONGLONG>(delay)) {
            it.frame   = (it.frame + 1) % it.frames;
            it.frameAt = now;
            const GUID dim = Gdiplus::FrameDimensionTime;
            it.img->SelectActiveFrame(&dim, static_cast<UINT>(it.frame));
            animate = true;
        }
    }

    if (g_dirty || animate)
        Render();
    if (g_regionDirty) {
        g_regionDirty = false;
        RebuildRegion();
    }
}

HWND Window() {
    return g_hwnd;
}

void Toggle() {
    Show(!g_on);
}

void Show(bool on) {
    g_on = on;
    settings::SetBool("overlay_on", on);
    if (g_hwnd == nullptr)
        return;
    if (on) {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        g_dirty = true;
    } else {
        ShowWindow(g_hwnd, SW_HIDE);
    }
    g_regionDirty = true;
}

bool Visible() {
    return g_on;
}

int Count() {
    return static_cast<int>(g_items.size());
}

const char* Name(int index) {
    if (index < 0 || index >= static_cast<int>(g_items.size()))
        return "";
    const std::string& path = g_items[static_cast<size_t>(index)].path;
    const size_t slash = path.find_last_of("\\/");
    g_nameBuf = slash == std::string::npos ? path : path.substr(slash + 1);
    return g_nameBuf.c_str();
}

int Type(int index) {
    if (index < 0 || index >= static_cast<int>(g_items.size()))
        return 0;
    return g_items[static_cast<size_t>(index)].type;
}

int Selected() {
    return g_sel;
}

void Select(int index) {
    if (index < -1 || index >= static_cast<int>(g_items.size()))
        index = -1;
    if (g_sel == index)
        return;
    g_sel         = index;
    g_dirty       = true;
    g_regionDirty = true;
}

void Deselect() {
    Select(-1);
}

void RemoveSelected() {
    if (g_sel < 0 || g_sel >= static_cast<int>(g_items.size()))
        return;
    delete g_items[static_cast<size_t>(g_sel)].img;
    g_items.erase(g_items.begin() + g_sel);
    if (g_sel >= static_cast<int>(g_items.size()))
        g_sel = static_cast<int>(g_items.size()) - 1;
    g_dirty       = true;
    g_regionDirty = true;
}

float Opacity() {
    if (g_sel < 0 || g_sel >= static_cast<int>(g_items.size()))
        return 1.0f;
    return g_items[static_cast<size_t>(g_sel)].opacity;
}

void SetOpacity(float opacity) {
    if (g_sel < 0 || g_sel >= static_cast<int>(g_items.size()))
        return;
    Item& it = g_items[static_cast<size_t>(g_sel)];
    const float clamped =
        opacity < 0.0f ? 0.0f : (opacity > 1.0f ? 1.0f : opacity);
    if (std::fabs(it.opacity - clamped) < 0.0001f)
        return;
    it.opacity = clamped;
    g_dirty    = true;
}

float Rotation() {
    if (g_sel < 0 || g_sel >= static_cast<int>(g_items.size()))
        return 0.0f;
    return g_items[static_cast<size_t>(g_sel)].angle;
}

void SetRotation(float degrees) {
    if (g_sel < 0 || g_sel >= static_cast<int>(g_items.size()))
        return;
    while (degrees > 180.0f)
        degrees -= 360.0f;
    while (degrees < -180.0f)
        degrees += 360.0f;
    Item& it = g_items[static_cast<size_t>(g_sel)];
    if (std::fabs(it.angle - degrees) < 0.01f)
        return;
    it.angle      = degrees;
    g_dirty       = true;
    g_regionDirty = true;
}

bool AddFromDialog(int type) {
    if (g_hwnd == nullptr)
        return false;

    wchar_t file[1024] = L"";
    const std::wstring initialDir = WideFromUtf8(OverlaysDir());
    OPENFILENAMEW ofn  = {};
    ofn.lStructSize   = sizeof(ofn);
    ofn.hwndOwner     = g_hwnd;
    ofn.lpstrFilter =
        type == 1
            ? L"GIF (*.gif)\0*.gif\0All files (*.*)\0*.*\0\0"
            : L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.webp)\0*.png;*.jpg;*.jpeg;*.bmp;*.webp\0"
              L"All files (*.*)\0*.*\0\0";
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle   = type == 1 ? L"Add a GIF" : L"Add an image";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags        = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == FALSE)
        return false;

    const std::string path = Utf8FromWide(file);
    if (path.empty())
        return false;

    Item it;
    it.path = path;
    it.type = type == 1 ? 1 : 0;
    it.img  = LoadBitmap(path, it.type, &it.frames, &it.delays);
    if (it.img != nullptr) {
        const int pw = static_cast<int>(it.img->GetWidth());
        const int ph = static_cast<int>(it.img->GetHeight());
        it.w = pw > 0 ? pw : 300;
        it.h = ph > 0 ? ph : 300;
    } else {
        it.w = 300;
        it.h = 300;
    }
    it.x = (g_resW - it.w) / 2;
    it.y = (g_resH - it.h) / 2;
    g_items.push_back(std::move(it));
    g_sel         = static_cast<int>(g_items.size()) - 1;
    g_dirty       = true;
    g_regionDirty = true;

    if (!g_on)
        Show(true);
    return true;
}

bool ClickThrough() {
    return g_click;
}

void SetClickThrough(bool on) {
    g_click = on;
    settings::SetBool("overlay_click", on);
    g_regionDirty = true;
}

bool Locked() {
    return g_lock;
}

void SetLocked(bool on) {
    g_lock = on;
    settings::SetBool("overlay_lock", on);
    if (on)
        g_sel = -1;
    g_dirty       = true;
    g_regionDirty = true;
}

int ResolutionIndex() {
    return g_resIndex;
}

void SetResolutionIndex(int index) {
    index = Clamp(index, 0, 3);
    if (index == g_resIndex)
        return;
    g_resIndex = index;
    g_resW     = kRes[index][0];
    g_resH     = kRes[index][1];
    settings::SetInt("overlay_res", index);
    if (g_hwnd != nullptr)
        SetWindowPos(g_hwnd, nullptr, 0, 0, g_resW, g_resH,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_dirty       = true;
    g_regionDirty = true;
}

void Resolution(int* w, int* h) {
    if (w != nullptr)
        *w = g_resW;
    if (h != nullptr)
        *h = g_resH;
}

void Place(int mode) {
    if (g_sel < 0 || g_sel >= static_cast<int>(g_items.size()))
        return;
    Item& it = g_items[static_cast<size_t>(g_sel)];
    it.angle = 0.0f;
    switch (mode) {
        case 0:
            it.x = 0;
            it.y = 0;
            it.w = g_resW;
            it.h = g_resH;
            break;
        case 1:
            it.x = (g_resW - it.w) / 2;
            it.y = (g_resH - it.h) / 2;
            break;
        case 2:
            it.x = 0;
            it.y = 0;
            break;
        case 3:
            it.x = g_resW - it.w;
            it.y = 0;
            break;
        case 4:
            it.x = 0;
            it.y = g_resH - it.h;
            break;
        case 5:
            it.x = g_resW - it.w;
            it.y = g_resH - it.h;
            break;
        default:
            break;
    }
    g_dirty = true;
}

bool SaveDialog(int qtKeybind) {
    if (g_hwnd == nullptr)
        return false;

    wchar_t file[1024] = L"config.json";
    const std::wstring initialDir = WideFromUtf8(OverlaysDir());

    OPENFILENAMEW ofn  = {};
    ofn.lStructSize   = sizeof(ofn);
    ofn.hwndOwner     = g_hwnd;
    ofn.lpstrFilter   = L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile     = file;
    ofn.nMaxFile      = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle    = L"Save the overlay configuration";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.lpstrDefExt   = L"json";
    ofn.Flags         = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn) == FALSE)
        return false;

    const std::string path = Utf8FromWide(file);
    if (path.empty() || !SavePath(path, qtKeybind))
        return false;

    g_lastPath = path;
    settings::SetString("overlay_last", path);
    return true;
}

bool LoadDialog(int* qtKeybind) {
    if (g_hwnd == nullptr)
        return false;

    wchar_t file[1024] = L"";
    const std::wstring initialDir = WideFromUtf8(OverlaysDir());

    OPENFILENAMEW ofn  = {};
    ofn.lStructSize   = sizeof(ofn);
    ofn.hwndOwner     = g_hwnd;
    ofn.lpstrFilter   = L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile     = file;
    ofn.nMaxFile      = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
    ofn.lpstrTitle    = L"Load an overlay configuration";
    ofn.lpstrInitialDir = initialDir.c_str();
    ofn.Flags         = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn) == FALSE)
        return false;

    const std::string path = Utf8FromWide(file);
    if (path.empty())
        return false;

    int key = 16777248;
    if (!LoadPath(path, &key))
        return false;

    g_lastPath = path;
    settings::SetString("overlay_last", path);
    if (qtKeybind != nullptr)
        *qtKeybind = key;
    return true;
}

const char* LastPath() {
    return g_lastPath.c_str();
}

int QtKeyToVk(int qtKey) {
    if (qtKey >= 0x01000020 && qtKey <= 0x01000023)
        return VK_F1 + (qtKey - 0x01000020);
    return 0;
}

int VkToQtKey(int vk) {
    if (vk >= VK_F1 && vk <= VK_F4)
        return 0x01000020 + (vk - VK_F1);
    return 0x01000020;
}

}
