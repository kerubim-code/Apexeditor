#include "app.h"

#include "color/vibrance.h"
#include "logo.h"
#include "settings.h"

#include "glass/glass.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <tlhelp32.h>
#include <shellapi.h>
#include <vector>

namespace apex {

bool  g_playerOverlay  = settings::GetBool("spotify_overlay", false);
float g_playerOpacity  = static_cast<float>(
    std::max(25, std::min(100, settings::GetInt("spotify_overlay_opacity", 85))));
bool  g_playerLyrics   = settings::GetBool("spotify_lyrics", true);
bool  g_overlayItemHover = false;
ImFont* g_ovrFontSemi  = nullptr;

int g_navPendingSection = -1;
int g_navPendingSub     = -1;

void NavScrollTo(int section, int sub) {
    if (g_navPendingSection != section || g_navPendingSub != sub)
        return;
    g_navPendingSection = -1;
    g_navPendingSub     = -1;

    const float contentY = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y +
                           ImGui::GetScrollY();
    ImGui::SetScrollY(contentY - 16.0f);
}

namespace {

constexpr ImU32 kAccentText = IM_COL32(122, 152, 255, 255);
constexpr ImU32 kOkText     = IM_COL32(96, 220, 138, 255);
constexpr ImU32 kInk        = IM_COL32(240, 242, 248, 255);
constexpr ImU32 kDim        = IM_COL32(150, 153, 168, 255);

constexpr uint64_t kSplashMs = 1500;
constexpr uint64_t kFadeMs   = 500;

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

float EaseOutCubic(float t) {
    const float inv = 1.0f - Clamp01(t);
    return 1.0f - inv * inv * inv;
}

const char* SectionName(Section section) {
    switch (section) {
        case Section::VideoCfg:     return "apex video cfg";
        case Section::Custom:       return "Custom";
        case Section::Color:        return "Color";
        case Section::Optimization: return "Optimization";
        case Section::Config:       return "Config";
        case Section::Spotify:      return "Spotify";
        case Section::Crosshair:    return "Crosshair";
        case Section::Overlay:      return "Overlay";
    }
    return "Section";
}

const char* SectionSubtitle(Section section) {
    switch (section) {
        case Section::VideoCfg:     return "kerubimm files - download and install";
        case Section::Custom:       return "resolution, fov, textures and anti-aliasing";
        case Section::Color:        return "saturation, contrast and screen profile";
        case Section::Optimization: return "autoexec.cfg and system tuning";
        case Section::Config:       return "profiles, startup options and saved data";
        case Section::Spotify:      return "now playing, overlay player and lyrics";
        case Section::Crosshair:    return "screen crosshair - shape, colour, size";
        case Section::Overlay:      return "images and gifs on an always-on-top overlay";
    }
    return "";
}

void CenterLine(const char* text, ImFont* font, float fontSize, ImU32 color) {
    if (font != nullptr)
        ImGui::PushFont(font, fontSize);

    const ImVec2 ts = ImGui::CalcTextSize(text);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (ImGui::GetContentRegionAvail().x - ts.x) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();

    if (font != nullptr)
        ImGui::PopFont();
}

void TwoColumnRow(const char* left, const char* right, ImU32 leftColour,
                  ImU32 rightColour, float height = 22.0f) {
    const float w    = ImGui::GetContentRegionAvail().x;
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, height));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ls = ImGui::CalcTextSize(left);
    dl->AddText(ImVec2(cur.x, cur.y + (height - ls.y) * 0.5f), leftColour, left);
    const ImVec2 rs = ImGui::CalcTextSize(right);
    dl->AddText(ImVec2(cur.x + w - rs.x, cur.y + (height - rs.y) * 0.5f),
                rightColour, right);
}

const char* BootStage(float progress) {
    if (progress < 0.22f) return "starting the glass engine";
    if (progress < 0.46f) return "capturing the desktop";
    if (progress < 0.70f) return "building the blur chain";
    if (progress < 0.92f) return "warming the shaders";
    return "ready";
}

struct VibranceCtx {
    DWORD pid = 0;
    HWND  hwnd = nullptr;
};

BOOL CALLBACK FindVibranceWindow(HWND hwnd, LPARAM lparam) {
    auto* ctx = reinterpret_cast<VibranceCtx*>(lparam);

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid && IsWindowVisible(hwnd)) {
        ctx->hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}

int CloseVibranceGUI() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    std::vector<DWORD> pids;
    PROCESSENTRY32W    entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"vibranceGUI.exe") == 0)
                pids.push_back(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    int closed = 0;
    for (DWORD pid : pids) {
        VibranceCtx ctx;
        ctx.pid = pid;
        EnumWindows(FindVibranceWindow, reinterpret_cast<LPARAM>(&ctx));

        if (ctx.hwnd != nullptr)
            PostMessageA(ctx.hwnd, WM_CLOSE, 0, 0);

        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (process == nullptr) continue;

        if (WaitForSingleObject(process, 700) == WAIT_TIMEOUT) {

            if (TerminateProcess(process, 0))
                WaitForSingleObject(process, 300);
        }
        CloseHandle(process);
        ++closed;
    }
    return closed;
}

}

App& App::Get() {
    static App instance;
    return instance;
}

void App::Init(HWND hwnd) {
    hwnd_ = hwnd;

    if (settings::GetBool("boot_animation", true)) {
        bootStart_ = GetTickCount64();
    } else {
        bootStart_ = 0;
        splashDone_ = true;
    }

    vibranceClosed_ = CloseVibranceGUI();

    if (settings::GetBool("start_engine", true))
        color::VibranceEngine::Get().Start();

    if (settings::GetBool("remember_section", true)) {

        if (!settings::GetBool("section_shifted", false)) {
            const int old = settings::GetInt("section", -1);
            if (old >= 1 && old <= 4)
                settings::SetInt("section", old + 1);
            settings::SetBool("section_shifted", true);
        }
        const int saved = settings::GetInt("section", static_cast<int>(Section::Color));
        if (saved >= 0 && saved < kSectionCount)
            section = static_cast<Section>(saved);
    }
}

void App::Shutdown() {
    color::VibranceEngine::Get().Stop();
}

void App::ReplayIntro() {
    bootStart_  = GetTickCount64();
    splashDone_ = false;
    dashSlide_  = 26.0f;
}

void App::Draw() {
    ImGuiStyle& style   = ImGui::GetStyle();
    const float  base   = style.Alpha;

    float splashAlpha = 1.0f;
    float dashAlpha   = 0.0f;

    if (splashDone_) {
        splashAlpha = 0.0f;
        dashAlpha   = 1.0f;
    } else if (bootStart_ != 0) {
        const uint64_t elapsed = GetTickCount64() - bootStart_;
        if (elapsed >= kSplashMs) {
            const float u = Clamp01(static_cast<float>(elapsed - kSplashMs) /
                                    static_cast<float>(kFadeMs));
            const float e = EaseOutCubic(u);
            splashAlpha   = 1.0f - e;
            dashAlpha     = e;
            if (u >= 1.0f) {
                splashDone_ = true;
                splashAlpha = 0.0f;
                dashAlpha   = 1.0f;
            }
        }
    }

    dashSlide_ = (1.0f - dashAlpha) * 26.0f;

    const ImVec2 display = ImGui::GetIO().DisplaySize;

    if (splashAlpha > 0.002f) {
        style.Alpha = base * splashAlpha;
        if (Glass::g != nullptr)
            Glass::g->SetSubmitFade(splashAlpha);
        DrawSplash(display);
    }

    if (dashAlpha > 0.002f) {
        style.Alpha = base * dashAlpha;
        if (Glass::g != nullptr)
            Glass::g->SetSubmitFade(dashAlpha);
        DrawDashboard(display);
    }

    if (splashDone_ && !startupNote_) {
        startupNote_ = true;
        if (vibranceClosed_ > 0) {
            char text[160];
            snprintf(text, sizeof(text),
                     "%d instance%s of vibranceGUI closed so APEX owns the "
                     "colours on this PC",
                     vibranceClosed_, vibranceClosed_ == 1 ? "" : "s");
            Glass::Notify(Glass::Icon::CheckCircle, kOkText, "APEX",
                          "vibranceGUI closed", text);
        }
    }

    style.Alpha = base;
    if (Glass::g != nullptr)
        Glass::g->SetSubmitFade(1.0f);
}

void App::DrawSplash(const ImVec2& displaySize) {
    const uint64_t elapsed =
        bootStart_ != 0 ? GetTickCount64() - bootStart_ : kSplashMs;
    const float raw = Clamp01(static_cast<float>(elapsed) /
                              static_cast<float>(kSplashMs));
    const float progress = EaseOutCubic(raw);

    ImGui::SetNextWindowPos(ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (Glass::BeginCard("splash", Glass::Material::Regular, ImVec2(470.0f, 0))) {
        const float   width = ImGui::GetContentRegionAvail().x;
        ImDrawList*   dl    = ImGui::GetWindowDrawList();

        const float ringD = 112.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - ringD) * 0.5f);
        const ImVec2 ringPos = ImGui::GetCursorScreenPos();
        Glass::ProgressRing("boot", progress, ringD * 0.5f, nullptr);

        const ImVec2 c(ringPos.x + ringD * 0.5f, ringPos.y + ringD * 0.5f);
        const float  pulse = 1.0f + 0.035f *
                                std::sin(static_cast<float>(ImGui::GetTime()) * 3.0f);

        Glass::Primitive halo;
        halo.cx = c.x;
        halo.cy = c.y;
        halo.hw = halo.hh = 47.0f * pulse;
        halo.corner_radius = 19.0f;
        halo.fade          = 0.55f;
        halo.material      = Glass::Material::Thin;
        if (Glass::g != nullptr)
            Glass::g->Submit(halo);

        const ImTextureID logoTex = logo::Texture();
        if (logoTex != ImTextureID_Invalid) {

            const float  d     = 86.0f * pulse;
            const float  fade  = Clamp01(ImGui::GetStyle().Alpha);
            const ImU32  ink   = IM_COL32(255, 255, 255, static_cast<int>(255 * fade + 0.5f));
            const ImU32  edge  = IM_COL32(255, 255, 255, static_cast<int>(80 * fade + 0.5f));
            const ImVec2 mn(c.x - d * 0.5f, c.y - d * 0.5f);
            const ImVec2 mx(c.x + d * 0.5f, c.y + d * 0.5f);

            dl->AddImageRounded(logoTex, mn, mx, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                ink, d * 0.5f, ImDrawFlags_None);
            dl->AddCircle(c, d * 0.5f, edge, 64, 1.5f);
        } else {
            Glass::Primitive badge;
            badge.cx = c.x;
            badge.cy = c.y;
            badge.hw = badge.hh = 36.0f;
            badge.corner_radius = 13.0f;
            badge.fade          = 1.0f;
            badge.material      = Glass::Material::Accent;
            if (Glass::g != nullptr)
                Glass::g->Submit(badge);

            Glass::DrawIcon(dl, Glass::Icon::ShieldHalf, c, 41.0f,
                            IM_COL32(255, 255, 255, 255), 3.0f);
        }

        ImGui::Dummy(ImVec2(0.0f, 14.0f));
        CenterLine("APEX", fontSemi, 30.0f, kInk);
        CenterLine("ALL IN ONE", nullptr, 0.0f, kDim);

        ImGui::Dummy(ImVec2(0.0f, 24.0f));

        Glass::ProgressBar(progress, ImVec2(width, 7.0f));

        ImGui::Dummy(ImVec2(0.0f, 14.0f));

        char percent[8];
        snprintf(percent, sizeof(percent), "%d%%",
                 static_cast<int>(progress * 100.0f + 0.5f));
        TwoColumnRow(BootStage(progress), percent, kDim, kAccentText);

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        Glass::Divider();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));

        TwoColumnRow("liquid glass engine", "v1.0", kDim, kDim, 18.0f);
    }
    Glass::EndCard();
}

void App::DrawDashboard(const ImVec2& displaySize) {
    const float margin = 46.0f;
    ImVec2 size(displaySize.x - margin * 2.0f, displaySize.y - margin * 2.0f);
    if (size.x < 760.0f)
        size.x = displaySize.x - 40.0f;
    if (size.y < 520.0f)
        size.y = displaySize.y - 40.0f;

    ImGui::SetNextWindowPos(
        ImVec2(displaySize.x * 0.5f, displaySize.y * 0.5f + dashSlide_),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    if (Glass::BeginCard("dash", Glass::Material::Regular, size)) {
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float innerW  = ImGui::GetContentRegionAvail().x;
        const float innerH  = ImGui::GetContentRegionAvail().y;

        DrawSidebar(origin, innerH);

        const float contentX = origin.x + kSidebarWidth + 26.0f;
        const float contentW = innerW - kSidebarWidth - 52.0f;

        const float tlX = origin.x + innerW - (3 * 13.0f + 2 * 8.0f) - 6.0f;
        const int   tl  = Glass::TrafficLights("wtl", tlX, origin.y + 8.0f);
        if (tl == 0)
            RequestClose();
        else if (tl == 1)
            ShowWindow(hwnd_, SW_MINIMIZE);
        else if (tl == 2) {
            WINDOWPLACEMENT placement{};
            placement.length = sizeof(placement);
            GetWindowPlacement(hwnd_, &placement);
            if (placement.showCmd == SW_SHOWMAXIMIZED) {
                ShowWindow(hwnd_, SW_RESTORE);
            } else {
                MONITORINFO info{};
                info.cbSize = sizeof(info);
                GetMonitorInfo(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
                SetWindowPos(hwnd_, nullptr, info.rcWork.left, info.rcWork.top,
                             info.rcWork.right - info.rcWork.left,
                             info.rcWork.bottom - info.rcWork.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }

        ImGui::SetCursorScreenPos(ImVec2(contentX, origin.y + 2.0f));
        if (fontSemi != nullptr) {
            ImGui::PushFont(fontSemi, 24.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, kInk);
            ImGui::TextUnformatted(SectionName(section));
            ImGui::PopStyleColor();
            ImGui::PopFont();
        } else {
            ImGui::TextUnformatted(SectionName(section));
        }

        ImGui::SetCursorScreenPos(ImVec2(contentX, origin.y + 34.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(SectionSubtitle(section));
        ImGui::PopStyleColor();

        {
            const char* stateText = "local mode";
            const float pillW = ImGui::CalcTextSize(stateText).x + 34.0f;
            ImGui::SetCursorScreenPos(
                ImVec2(contentX + contentW - pillW, origin.y + 28.0f));
            Glass::StatusPill(stateText, kOkText);
        }

        const float bodyY = origin.y + 68.0f;
        const float bodyH = innerH - 68.0f - 12.0f;
        DrawContent(ImVec2(contentX, bodyY), contentW, bodyH);
    }
    Glass::EndCard();
}

void App::DrawSidebar(const ImVec2& origin, float height) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float sideW = kSidebarWidth;

    const float sepX = origin.x + sideW + 8.0f;
    dl->AddLine(ImVec2(sepX, origin.y + 4.0f), ImVec2(sepX, origin.y + height - 4.0f),
                IM_COL32(0, 0, 0, 30), 1.0f);
    dl->AddLine(ImVec2(sepX + 1.0f, origin.y + 4.0f),
                ImVec2(sepX + 1.0f, origin.y + height - 4.0f),
                IM_COL32(255, 255, 255, 40), 1.0f);

    {
        const float box = 40.0f;
        Glass::Primitive badge;
        badge.cx            = origin.x + 16.0f + box * 0.5f;
        badge.cy            = origin.y + 6.0f + box * 0.5f;
        badge.hw            = box * 0.5f;
        badge.hh            = box * 0.5f;
        badge.corner_radius = 12.0f;
        badge.fade          = 1.0f;
        badge.material      = Glass::Material::Thin;
        if (Glass::g != nullptr)
            Glass::g->Submit(badge);

        const ImTextureID logoTex = logo::Texture();
        if (logoTex != ImTextureID_Invalid) {
            const float  d    = box - 6.0f;
            const float  fade = Clamp01(ImGui::GetStyle().Alpha);
            const ImU32  ink  = IM_COL32(255, 255, 255, static_cast<int>(255 * fade + 0.5f));
            const ImVec2 mn(badge.cx - d * 0.5f, badge.cy - d * 0.5f);
            const ImVec2 mx(badge.cx + d * 0.5f, badge.cy + d * 0.5f);
            dl->AddImageRounded(logoTex, mn, mx, ImVec2(0.0f, 0.0f), ImVec2(1.0f, 1.0f),
                                ink, 9.0f, ImDrawFlags_None);
        } else {
            Glass::DrawIcon(dl, Glass::Icon::ShieldHalf, ImVec2(badge.cx, badge.cy),
                            box * 0.58f, kInk, 2.6f);
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 16.0f + box + 12.0f, origin.y + 4.0f));
        if (fontSemi != nullptr)
            ImGui::PushFont(fontSemi, 22.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        ImGui::TextUnformatted("APEX");
        ImGui::PopStyleColor();
        if (fontSemi != nullptr)
            ImGui::PopFont();

        ImGui::SetCursorScreenPos(ImVec2(origin.x + 16.0f + box + 12.0f, origin.y + 32.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("ALL IN ONE");
        ImGui::PopStyleColor();
    }

    static const Glass::Icon navIcons[kSectionCount] = {
        Glass::Icon::Download, Glass::Icon::Sliders, Glass::Icon::Palette,
        Glass::Icon::Gauge, Glass::Icon::Gear, Glass::Icon::Spotify,
        Glass::Icon::Crosshairs, Glass::Icon::Images};
    static const char* navLabels[kSectionCount] = {"apex video cfg", "Custom", "Color",
                                                   "Optimization", "Config", "Spotify",
                                                   "Crosshair", "Overlay"};
    static const ImU32 navTints[kSectionCount]  = {IM_COL32(10, 132, 255, 255),
                                                   IM_COL32(255, 64, 128, 255),
                                                   IM_COL32(52, 199, 89, 255),
                                                   IM_COL32(255, 149, 0, 255),
                                                   IM_COL32(176, 102, 255, 255),
                                                   IM_COL32(30, 215, 96, 255),
                                                   IM_COL32(0, 224, 255, 255),
                                                   IM_COL32(255, 69, 58, 255)};

    static const char* kSubsVideo[]   = {"launch", "install", "files"};
    static const char* kSubsCustom[]  = {"resolution", "fov", "textures",
                                         "antialiasing"};
    static const char* kSubsColor[]   = {"profile", "detect", "rules"};
    static const char* kSubsOpt[]     = {"autoexec"};
    static const char* kSubsConfig[]  = {"profiles", "startup", "branding",
                                         "maintenance"};
    static const char* kSubsSpotify[] = {"song", "lyrics", "keybind"};
    static const char* kSubsCross[]   = {"appearance", "hotkey"};
    static const char* kSubsOverlay[] = {"items", "position", "settings",
                                         "configuration"};
    static const char* const* kNavSubs[kSectionCount] = {
        kSubsVideo, kSubsCustom, kSubsColor, kSubsOpt, kSubsConfig, kSubsSpotify,
        kSubsCross, kSubsOverlay};
    static const int kNavSubCount[kSectionCount] = {3, 4, 3, 1, 4, 3, 2, 4};
    static int navExpanded = -1;

    const float navTop  = origin.y + 74.0f;
    const float navH    = height - 74.0f - 124.0f - 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 8.0f, navTop));
    ImGui::BeginChild("apexnavscroll", ImVec2(sideW - 16.0f, navH),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    const int current = static_cast<int>(section);
    const int picked =
        Glass::SidebarNav("apexnav", navIcons, navLabels, navTints, kSectionCount,
                          current, sideW - 16.0f, 50.0f,
                          kNavSubs, kNavSubCount, navExpanded);
    ImGui::EndChild();
    if (picked >= 0) {
        if (picked < kSectionCount) {

            if (picked == current) {
                navExpanded = (navExpanded == picked) ? -1 : picked;
            } else {
                section = static_cast<Section>(picked);
                navExpanded = picked;
                if (settings::GetBool("remember_section", true))
                    settings::SetInt("section", picked);
            }
        } else {

            const int flat = picked - kSectionCount;
            int sec = 0, sub = 0, acc = 0;
            for (; sec < kSectionCount; ++sec) {
                if (flat < acc + kNavSubCount[sec]) {
                    sub = flat - acc;
                    break;
                }
                acc += kNavSubCount[sec];
            }
            if (sec < kSectionCount) {
                section = static_cast<Section>(sec);
                navExpanded = sec;
                g_navPendingSection = sec;
                g_navPendingSub     = sub;
                if (settings::GetBool("remember_section", true))
                    settings::SetInt("section", sec);
            }
        }
    }

    const float footY = origin.y + height - 124.0f;
    dl->AddLine(ImVec2(origin.x + 16.0f, footY), ImVec2(origin.x + sideW - 16.0f, footY),
                IM_COL32(255, 255, 255, 26), 1.0f);

    {
        const float avatar = 36.0f;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 16.0f, footY + 16.0f));

        const ImTextureID logoTex = logo::Texture();
        if (logoTex != ImTextureID_Invalid) {
            const ImVec2 avPos = ImGui::GetCursorScreenPos();
            const float  fade  = Clamp01(ImGui::GetStyle().Alpha);
            const ImVec2 avMax(avPos.x + avatar, avPos.y + avatar);

            dl->AddImageRounded(logoTex, avPos, avMax, ImVec2(0.0f, 0.0f),
                                ImVec2(1.0f, 1.0f),
                                IM_COL32(255, 255, 255, static_cast<int>(255 * fade + 0.5f)),
                                avatar * 0.5f, ImDrawFlags_None);
            dl->AddCircle(ImVec2(avPos.x + avatar * 0.5f, avPos.y + avatar * 0.5f),
                          avatar * 0.5f,
                          IM_COL32(255, 255, 255, static_cast<int>(70 * fade + 0.5f)),
                          48, 1.0f);
            ImGui::Dummy(ImVec2(avatar, avatar));
        } else {
            Glass::Avatar("AP", avatar, IM_COL32(74, 108, 240, 255));
        }

        const float textX = origin.x + 16.0f + avatar + 12.0f;
        dl->AddText(ImVec2(textX, footY + 18.0f), kInk, "APEX ALL IN ONE");
        dl->AddText(ImVec2(textX, footY + 38.0f), kOkText, "no account needed");
    }

    {
        const float bx = origin.x + 16.0f;
        const float by = footY + 68.0f;
        const float bs = 36.0f;
        const float gap = 10.0f;

        ImGui::SetCursorScreenPos(ImVec2(bx, by));
        if (Glass::IconButton("tiktok", Glass::Icon::TikTok, bs))
            ShellExecuteA(nullptr, "open", "https://www.tiktok.com/@kerubimm_",
                          nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("tiktok  -  @kerubimm_");

        ImGui::SetCursorScreenPos(ImVec2(bx + bs + gap, by));
        if (Glass::IconButton("discord", Glass::Icon::Discord, bs))
            ShellExecuteA(nullptr, "open", "https://discord.gg/vqRvCprmD8",
                          nullptr, nullptr, SW_SHOWNORMAL);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("discord server");
    }
}

void App::DrawContent(const ImVec2& origin, float width, float height) {
    ImGui::SetCursorScreenPos(origin);
    ImGui::BeginChild("content", ImVec2(width, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);

    ImGui::PushClipRect(origin, ImVec2(origin.x + width, origin.y + height), true);
    if (Glass::g != nullptr)
        Glass::g->SetClipRect(origin.x, origin.y, origin.x + width, origin.y + height);

    switch (section) {
        case Section::VideoCfg:     DrawVideoCfg();     break;
        case Section::Custom:       DrawCustom();       break;
        case Section::Color:        DrawColor();        break;
        case Section::Optimization: DrawOptimization(); break;
        case Section::Config:       DrawConfig();       break;
        case Section::Spotify:      DrawSpotify();      break;
        case Section::Crosshair:    DrawCrosshair();    break;
        case Section::Overlay:      DrawOverlay();      break;
    }

    Glass::DrawColorPickerModal();

    ImGui::Dummy(ImVec2(0.0f, 40.0f));

    if (Glass::g != nullptr)
        Glass::g->ClearClipRect();
    ImGui::PopClipRect();
    ImGui::EndChild();
}

}
