#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <string>

#include "imgui.h"

struct ID3D11Device;

namespace apex {

enum class Section {
    VideoCfg     = 0,
    Custom       = 1,
    Color        = 2,
    Optimization = 3,
    Config       = 4,
    Spotify      = 5,
    Crosshair    = 6,
    Overlay      = 7,
};

constexpr int   kSectionCount   = 8;
constexpr float kSidebarWidth  = 231.0f;

ID3D11Device* Device();

extern bool  g_playerOverlay;
extern float g_playerOpacity;
extern bool  g_playerLyrics;
extern bool  g_overlayItemHover;
extern ImFont* g_ovrFontSemi;

bool        HotkeyBind(int slot, int vk);
int         HotkeyVk(int slot);
bool        HotkeyRegistered(int slot);
std::string HotkeyKeyName(int vk);
void        ReapplyHotkeys();
void        UnregisterHotkeys();

extern int g_navPendingSection;
extern int g_navPendingSub;
void NavScrollTo(int section, int sub);

class App {
public:
    static App& Get();

    void Init(HWND hwnd);
    void Shutdown();

    void Draw();
    bool CloseRequested() const { return closeRequested_; }
    void RequestClose() { closeRequested_ = true; }
    HWND Window() const { return hwnd_; }

    void DrawVideoCfg();
    void DrawCustom();
    void DrawColor();
    void DrawOptimization();
    void DrawConfig();
    void DrawSpotify();
    void DrawCrosshair();
    void DrawOverlay();

    void DrawSpotifyOverlay(float width, float height);

    Section section = Section::Color;

    ImFont* fontSemi = nullptr;

private:
    App() = default;

    void DrawSplash(const ImVec2& display);
    void DrawDashboard(const ImVec2& display);
    void DrawSidebar(const ImVec2& origin, float height);
    void DrawContent(const ImVec2& origin, float width, float height);

    void ReplayIntro();

    HWND      hwnd_ = nullptr;
    bool      closeRequested_ = false;

    int  vibranceClosed_ = 0;
    bool startupNote_    = false;

    uint64_t  bootStart_  = 0;
    bool      splashDone_ = false;
    float     dashSlide_  = 26.0f;
};

}
