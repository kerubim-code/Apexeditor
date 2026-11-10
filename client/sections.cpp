#include "app.h"

#include "color/adl_backend.h"
#include "color/vibrance.h"
#include "crosshair.h"
#include "logo.h"
#include "overlay.h"
#include "settings.h"
#include "spotify.h"
#include "videocfg.h"

#include "glass/glass.h"

#include <imgui.h>

#include <windows.h>
#include <d3d11.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace apex {

namespace {

constexpr ImU32 kInk    = IM_COL32(240, 242, 248, 255);
constexpr ImU32 kDim    = IM_COL32(150, 153, 168, 255);
constexpr ImU32 kAccent = IM_COL32(122, 152, 255, 255);
constexpr ImU32 kOk     = IM_COL32(96, 220, 138, 255);
constexpr ImU32 kWarn   = IM_COL32(255, 196, 84, 255);

void Dim(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

std::string FitText(const char* s, float maxw) {
    if (s == nullptr || s[0] == '\0' || maxw <= 0.0f)
        return {};
    const size_t len = strlen(s);
    if (ImGui::CalcTextSize(s).x <= maxw)
        return std::string(s);
    static const char* kDots = "\xE2\x80\xA6";
    const float dots = ImGui::CalcTextSize(kDots).x;
    size_t lo = 0, hi = len;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (ImGui::CalcTextSize(s, s + mid).x + dots <= maxw)
            lo = mid;
        else
            hi = mid - 1;
    }
    while (lo > 0 && (static_cast<unsigned char>(s[lo]) & 0xC0) == 0x80)
        --lo;
    std::string text(s, lo);
    text += kDots;
    return text;
}

void TextFit(const char* s, float maxw, ImU32 col) {
    const std::string text = FitText(s, maxw);
    if (text.empty())
        return;
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

struct LyricAnim {
    int    idx = -99999;
    double t0  = -1.0;
};
LyricAnim g_lyrAnimSection;
LyricAnim g_lyrAnimOverlay;

void DrawLyricWriteOn(const char* text, float wrapW, ImU32 col, bool horizontal,
                      LyricAnim& anim, int idx) {
    const double now = ImGui::GetTime();
    if (text == nullptr || text[0] == '\0') {
        anim.idx = idx;
        anim.t0  = now;
        return;
    }
    if (anim.idx != idx) {
        anim.idx = idx;
        anim.t0  = now;
    }
    const double t   = now - anim.t0;
    const size_t len = strlen(text);

    const double dur = horizontal ? std::min(1.2, 0.35 + 0.020 * static_cast<double>(len))
                                  : std::min(1.4, 0.30 + 0.010 * static_cast<double>(len));
    float k = static_cast<float>(std::min(1.0, t / dur));
    k = 1.0f - (1.0f - k) * (1.0f - k);
    float fade = static_cast<float>(std::min(1.0, t / 0.20));
    fade = fade * fade * (3.0f - 2.0f * fade);

    const float baseA = static_cast<float>((col >> 24) & 0xFF);
    const ImU32 c     = (col & 0x00FFFFFFu) |
                    (static_cast<ImU32>(baseA * fade) << 24);

    ImDrawList* dl      = ImGui::GetWindowDrawList();
    const ImVec2 p0     = ImGui::GetCursorScreenPos();

    if (horizontal) {
        const ImVec2 sz   = ImGui::CalcTextSize(text);
        const float  slide = (1.0f - fade) * 5.0f;
        const ImVec2 p(p0.x, p0.y + slide);
        dl->PushClipRect(ImVec2(p0.x - 4.0f, p0.y - 8.0f),
                         ImVec2(p0.x + sz.x * k, p0.y + sz.y + 10.0f), true);
        dl->AddText(p, c, text);
        dl->PopClipRect();

        float caret = (k < 1.0f)
                          ? 1.0f
                          : static_cast<float>(
                                std::max(0.0, 1.0 - (t - dur) / 0.25));
        if (caret > 0.0f) {
            const ImU32 cc = (IM_COL32(30, 215, 96, 255) & 0x00FFFFFFu) |
                             (static_cast<ImU32>(255.0f * caret * fade) << 24);
            const float cx = p.x + sz.x * k;
            dl->AddRectFilled(ImVec2(cx + 1.0f, p.y + 1.0f),
                              ImVec2(cx + 3.0f, p.y + sz.y - 1.0f), cc);
        }
        ImGui::Dummy(sz);
    } else {
        const ImVec2 sz = ImGui::CalcTextSize(text, nullptr, false, wrapW);
        dl->PushClipRect(ImVec2(p0.x - 4.0f, p0.y - 4.0f),
                         ImVec2(p0.x + wrapW + 4.0f, p0.y + sz.y * k), true);

        ImGui::PushStyleColor(ImGuiCol_Text, c);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
        dl->PopClipRect();
    }
}

void InfoRow(const char* label, const char* value) {
    const float rowh = 26.0f;
    const float w    = ImGui::GetContentRegionAvail().x;
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(w, rowh));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 ls = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(cur.x + 2.0f, cur.y + (rowh - ls.y) * 0.5f), kDim, label);
    const ImVec2 vs = ImGui::CalcTextSize(value);
    dl->AddText(ImVec2(cur.x + w - vs.x - 2.0f, cur.y + (rowh - vs.y) * 0.5f), kInk,
                value);
}

void StatTile(ImFont* semi, float x, float y, float w, float h, const char* label,
              const char* value, bool accent) {
    Glass::Primitive tile;
    tile.cx            = x + w * 0.5f;
    tile.cy            = y + h * 0.5f;
    tile.hw            = w * 0.5f;
    tile.hh            = h * 0.5f;
    tile.corner_radius = 15.0f;
    tile.fade          = 1.0f;
    tile.material      = Glass::Material::Thin;
    if (Glass::g != nullptr)
        Glass::g->Submit(tile);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(ImVec2(x + 16.0f, y + 14.0f), kDim, label);

    if (accent) {
        Glass::Primitive bar;
        bar.cx            = x + 18.0f;
        bar.cy            = y + h - 12.0f;
        bar.hw            = 15.0f;
        bar.hh            = 2.5f;
        bar.corner_radius = 2.5f;
        bar.fade          = 1.0f;
        bar.material      = Glass::Material::Accent;
        if (Glass::g != nullptr)
            Glass::g->Submit(bar);
    }

    ImGui::SetCursorScreenPos(ImVec2(x + 16.0f, y + 38.0f));
    if (semi != nullptr)
        ImGui::PushFont(semi, 19.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::TextUnformatted(value);
    ImGui::PopStyleColor();
    if (semi != nullptr)
        ImGui::PopFont();
    ImGui::SetCursorScreenPos(ImVec2(x, y + h));
}

void StatTileRow(ImFont* semi, const char* const* labels, const char* const* values,
                 const bool* accents, int count) {
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float  avail = ImGui::GetContentRegionAvail().x;
    const float  gap   = 12.0f;
    const float  w     = (avail - gap * static_cast<float>(count - 1)) /
                    static_cast<float>(count);
    const float  h     = 84.0f;

    for (int i = 0; i < count; ++i) {
        const float x = start.x + static_cast<float>(i) * (w + gap);
        StatTile(semi, x, start.y, w, h, labels[i], values[i],
                 accents != nullptr && accents[i]);
    }

    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + h + 14.0f));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

bool RuleRow(const char* exe, float* saturation) {
    ImDrawList*      dl    = ImGui::GetWindowDrawList();
    const ImVec2     cur   = ImGui::GetCursorScreenPos();
    const float      avail = ImGui::GetContentRegionAvail().x;
    const float      nameH = 34.0f;

    ImGui::Dummy(ImVec2(avail, nameH));
    dl->AddText(ImVec2(cur.x + 2.0f, cur.y + 4.0f), kInk, exe);

    ImGui::SetCursorScreenPos(ImVec2(cur.x + avail - 32.0f, cur.y + 2.0f));
    const bool remove = Glass::IconButton("del", Glass::Icon::Trash, 30.0f);

    ImGui::SetCursorScreenPos(ImVec2(cur.x, cur.y + nameH + 6.0f));
    ImGui::SetNextItemWidth(avail - 190.0f);
    Glass::Slider("sat", saturation, 0.0f, 300.0f, "%.0f");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    return remove;
}

std::string FormatBytes(uint64_t bytes) {
    char buffer[64];
    if (bytes >= (1ULL << 30))
        snprintf(buffer, sizeof(buffer), "%.1f GB", static_cast<double>(bytes) / (1 << 30));
    else
        snprintf(buffer, sizeof(buffer), "%0.f MB", static_cast<double>(bytes) / (1 << 20));
    return buffer;
}

uint64_t FileTimeToU64(const FILETIME& value) {
    ULARGE_INTEGER integer;
    integer.LowPart  = value.dwLowDateTime;
    integer.HighPart = value.dwHighDateTime;
    return integer.QuadPart;
}

int CpuUsagePercent() {
    static uint64_t lastIdle   = 0;
    static uint64_t lastKernel = 0;
    static uint64_t lastUser   = 0;
    static uint64_t lastTick   = 0;

    FILETIME idleTime{}, kernelTime{}, userTime{};
    if (!GetSystemTimes(&idleTime, &kernelTime, &userTime)) return -1;

    const uint64_t idle   = FileTimeToU64(idleTime);
    const uint64_t kernel = FileTimeToU64(kernelTime);
    const uint64_t user   = FileTimeToU64(userTime);
    const uint64_t now    = GetTickCount64();

    int usage = -1;
    if (lastTick != 0 && now - lastTick >= 400) {
        const uint64_t deltaIdle   = idle - lastIdle;
        const uint64_t deltaTotal  = (kernel - lastKernel) + (user - lastUser);
        if (deltaTotal > 0)
            usage = static_cast<int>(100.0 * static_cast<double>(deltaTotal - deltaIdle) /
                                     static_cast<double>(deltaTotal));
        lastTick = now;
    } else if (lastTick == 0) {
        lastTick = now;
    }

    lastIdle   = idle;
    lastKernel = kernel;
    lastUser   = user;

    if (usage < 0) usage = 0;
    if (usage > 100) usage = 100;
    return usage;
}

std::string PrimaryAdapterName() {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(nullptr, 0, &device, 0)) {
        const int needed = WideCharToMultiByte(CP_UTF8, 0, device.DeviceString, -1, nullptr,
                                               0, nullptr, nullptr);
        if (needed > 1) {
            std::string name(static_cast<size_t>(needed - 1), '\0');
            WideCharToMultiByte(CP_UTF8, 0, device.DeviceString, -1, name.data(), needed,
                                nullptr, nullptr);
            return name;
        }
    }
    return "unknown adapter";
}

const char* SectionLabel(Section value) {
    switch (value) {
        case Section::VideoCfg:     return "apex video cfg";
        case Section::Custom:       return "Custom";
        case Section::Color:        return "Color";
        case Section::Optimization: return "Optimization";
        case Section::Config:       return "Config";
        case Section::Spotify:      return "Spotify";
        case Section::Crosshair:    return "Crosshair";
        case Section::Overlay:      return "Overlay";
    }
    return "-";
}

std::string LowerCase(std::string text) {
    for (auto& character : text)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return text;
}

std::string OwnExeName() {
    char path[MAX_PATH] = {};
    const DWORD length  = GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string name =
        (length > 0 && length < MAX_PATH) ? std::string(path) : "apexclient.exe";
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos)
        name = name.substr(slash + 1);
    return LowerCase(name);
}

bool IsNoiseProcess(const std::string& exe) {
    static const char* const kNoise[] = {
        "system",            "registry",         "memory compression",
        "secure system",     "csrss.exe",        "wininit.exe",
        "winlogon.exe",      "services.exe",     "lsass.exe",
        "lsaiso.exe",        "smss.exe",         "svchost.exe",
        "fontdrvhost.exe",   "dwm.exe",          "sihost.exe",
        "taskhostw.exe",     "ctfmon.exe",       "shellexperiencehost.exe",
        "startmenuexperiencehost.exe",           "searchapp.exe",
        "searchhost.exe",    "applicationframehost.exe",
        "runtimebroker.exe", "systemsettings.exe",
        "consent.exe",       "securityhealthservice.exe",
        "msmpeng.exe",       "spoolsv.exe",      "taskeng.exe",
        "wmiprvse.exe",      "dllhost.exe",      "backgroundtaskhost.exe",
        "devicecensus.exe",  "smartscreen.exe",  "useroobebroker.exe",
        "explorer.exe",      "textinputhost.exe", "lockapp.exe",
    };
    for (const char* candidate : kNoise)
        if (exe == candidate)
            return true;
    return exe == OwnExeName();
}

std::vector<std::string> ScanRunningExecutables() {
    std::vector<std::string> found;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return found;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            char name[MAX_PATH] = {};
            if (WideCharToMultiByte(CP_UTF8, 0, entry.szExeFile, -1, name,
                                    MAX_PATH, nullptr, nullptr) <= 1)
                continue;
            std::string exe = LowerCase(name);
            if (IsNoiseProcess(exe))
                continue;
            found.push_back(exe);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

std::vector<std::string> ListProfiles() {
    std::vector<std::string> files;

    WIN32_FIND_DATAA data{};
    HANDLE handle = FindFirstFileA((settings::ConfigsDir() + "\\*.ini").c_str(), &data);
    if (handle != INVALID_HANDLE_VALUE) {
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                files.push_back(data.cFileName);
        } while (FindNextFileA(handle, &data));
        FindClose(handle);
    }

    std::sort(files.begin(), files.end());
    return files;
}

std::string ProfileLabel(const std::string& file) {
    const size_t dot = file.find_last_of('.');
    return dot == std::string::npos ? file : file.substr(0, dot);
}

std::string SanitizeName(const char* raw) {
    std::string name = raw != nullptr ? raw : "";
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
        name.erase(name.begin());
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.pop_back();
    for (auto& character : name)
        if (character == '\\' || character == '/' || character == ':' ||
            character == '*' || character == '?' || character == '"' ||
            character == '<' || character == '>' || character == '|')
            character = '_';
    return name;
}

void Toast(Glass::Icon icon, ImU32 tile, const char* title, const std::string& message) {
    Glass::Notify(icon, tile, "APEX", title, message.c_str());
}

class GlassClip {
public:
    GlassClip() {
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        x0_ = p.x;
        y0_ = p.y;
        x1_ = p.x + s.x;
        y1_ = p.y + s.y;
    }

    void ClipHere() const {
        if (Glass::g == nullptr) return;
        const ImVec2 p = ImGui::GetWindowPos();
        const ImVec2 s = ImGui::GetWindowSize();
        Glass::g->SetClipRect(p.x, p.y, p.x + s.x, p.y + s.y);
    }

    void Restore() const {
        if (Glass::g != nullptr) Glass::g->SetClipRect(x0_, y0_, x1_, y1_);
    }

private:
    float x0_ = 0.0f, y0_ = 0.0f, x1_ = 0.0f, y1_ = 0.0f;
};

bool DrawAppProfileWindow(color::VibranceEngine& engine, const std::string& exe,
                          color::AppRule& draft, bool& loaded) {
    static float satEdit = 150.0f;

    if (!loaded) {
        draft = color::AppRule{};
        draft.exe        = exe;
        draft.saturation = color::VibranceEngine::kSaturationDefault;
        for (const auto& rule : engine.Rules())
            if (color::SameApp(rule.exe, exe)) {
                draft = rule;
                break;
            }
        satEdit = static_cast<float>(draft.saturation);
        loaded  = true;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    bool         close   = false;

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("apex_profile_dim", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);

    ImDrawList* dimDl = ImGui::GetWindowDrawList();
    dimDl->AddRectFilled(ImVec2(0.0f, 0.0f), display, IM_COL32(5, 6, 9, 155));
    ImGui::SetCursorScreenPos(ImVec2(0.0f, 0.0f));
    ImGui::InvisibleButton("##profile_catcher", display, ImGuiButtonFlags_AllowOverlap);
    if (ImGui::IsItemDeactivated() && ImGui::IsItemHovered())
        close = true;

    ImGui::End();
    ImGui::PopStyleVar();

    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f, 18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(12.0f, 9.0f));
    ImGui::SetNextWindowFocus();
    ImGui::Begin("apex_profile_win", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    if (ws.x > 80.0f && ws.y > 80.0f) {
        const ImVec2 pmin = wp;
        const ImVec2 pmax(wp.x + ws.x, wp.y + ws.y);

        if (Glass::g != nullptr) {
            Glass::Primitive shadow;
            shadow.cx            = wp.x + ws.x * 0.5f;
            shadow.cy            = wp.y + ws.y * 0.5f;
            shadow.hw            = ws.x * 0.5f;
            shadow.hh            = ws.y * 0.5f;
            shadow.corner_radius = 24.0f;
            shadow.fade          = 1.0f;
            shadow.material      = Glass::Material::Thick;
            Glass::g->SubmitOverlay(shadow);
        }

        ImDrawList* pdl = ImGui::GetWindowDrawList();
        pdl->AddRectFilled(pmin, pmax, IM_COL32(18, 19, 25, 246), 24.0f);
        Glass::DrawUIBlurPanel(pdl, pmin, pmax, 24.0f, 1.0f, IM_COL32(26, 27, 34, 128));
        pdl->AddRect(pmin, pmax, IM_COL32(255, 255, 255, 46), 24.0f, 0, 1.4f);
    }

    const float innerW = ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::TextUnformatted(("profile  -  " + exe).c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("applied as soon as this window comes to the front, and only "
                       "for this application.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const float satBefore = satEdit;
    ImGui::SetNextItemWidth(innerW - 170.0f);
    Glass::Slider("saturation", &satEdit, 0.0f, 300.0f, "%.0f");
    if (satEdit != satBefore)
        draft.saturation = static_cast<int>(satEdit + 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted("100 = normal, 150 = vivid, 300 = maximum vibrance");
    ImGui::PopStyleColor();

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (Glass::Toggle("use my own brightness / contrast / gamma here", &draft.levels)) {
        if (draft.levels) {
            draft.brightness = engine.Brightness();
            draft.contrast   = engine.Contrast();
            draft.gamma      = engine.Gamma();
        }
    }

    if (draft.levels) {
        ImGui::SetNextItemWidth(innerW - 170.0f);
        Glass::Slider("brightness", &draft.brightness, 0.50f, 1.50f, "%.2f");
        ImGui::SetNextItemWidth(innerW - 170.0f);
        Glass::Slider("contrast", &draft.contrast, 0.50f, 1.50f, "%.2f");
        ImGui::SetNextItemWidth(innerW - 170.0f);
        Glass::Slider("gamma", &draft.gamma, 0.50f, 2.00f, "%.2f");
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("1.00 = neutral on every count.");
        ImGui::PopStyleColor();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const float half = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const bool  save = Glass::Button("save profile", true, ImVec2(half, 44.0f));
    ImGui::SameLine(0.0f, 12.0f);
    if (Glass::Button("close", false, ImVec2(half, 44.0f)))
        close = true;

    ImGui::End();
    ImGui::PopStyleVar(2);

    if (save) {
        std::vector<color::AppRule> rules = engine.Rules();
        bool                        found = false;
        for (auto& rule : rules) {
            if (color::SameApp(rule.exe, exe)) {
                rule        = draft;
                rule.exe    = draft.exe;
                found       = true;
                break;
            }
        }
        if (!found)
            rules.push_back(draft);
        engine.SetRules(rules);
        engine.Save();
        Toast(Glass::Icon::CheckCircle, kOk, "profile saved", exe);
        return false;
    }

    return !close;
}

}

void App::DrawVideoCfg() {
    using namespace videocfg;

    const Report report = Snapshot();

    char installedText[32];
    snprintf(installedText, sizeof(installedText), "%d / 3", InstalledCount());

    char gameText[24];
    snprintf(gameText, sizeof(gameText), "%s", GameFound() ? "found" : "not found");

    char statusText[32];
    if (report.running)
        snprintf(statusText, sizeof(statusText), "%d / %d", report.done, report.total);
    else if (report.finished)
        snprintf(statusText, sizeof(statusText), "%s",
                 report.ok ? "installed" : "partial");
    else
        snprintf(statusText, sizeof(statusText), "idle");

    const char* labels[3] = {"files installed", "apex", "status"};
    const char* values[3] = {installedText, gameText, statusText};
    const bool  accents[3] = {InstalledCount() > 0, GameFound(),
                              report.running || (report.finished && report.ok)};
    StatTileRow(fontSemi, labels, values, accents, 3);

    NavScrollTo(static_cast<int>(Section::VideoCfg), 0);
    Glass::BeginPanel("apex legends");

    Dim("Starts the game straight from here : APEXClient runs r5apex.exe "
        "directly when the install folder is known, and falls back to the "
        "Steam protocol when it is not.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    if (!GameFound())
        ImGui::BeginDisabled();
    const bool launchPressed =
        Glass::Button("launch apex legends", true,
                      ImVec2(ImGui::GetContentRegionAvail().x, 50.0f));
    if (!GameFound())
        ImGui::EndDisabled();

    if (launchPressed) {
        std::string detail;
        const bool  launched = LaunchGame(&detail);
        Toast(launched ? Glass::Icon::CheckCircle : Glass::Icon::XCircle,
              launched ? kOk : kWarn,
              launched ? "launching apex legends" : "could not launch apex",
              detail);
    }

    if (!GameFound()) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        Dim("Apex Legends was not found on this PC - install it on Steam first.");
    }

    InfoRow("game folder", GameFound() ? GameDirFound().c_str()
                                       : "not installed");
    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    NavScrollTo(static_cast<int>(Section::VideoCfg), 1);
    Glass::BeginPanel("apex video cfg  -  download and install");

    Dim("Grabs the kerubimm files from the public APEX-SETTINGS repository and "
        "drops them exactly where Apex Legends reads them. Everything happens "
        "on this PC : no key, no account, no launcher.");

    InfoRow("repository", "kerubim-code / APEX-SETTINGS");
    InfoRow("game folder", SavedGamesDir().c_str());
    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    const float half = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    if (report.running)
        ImGui::BeginDisabled();
    const bool installPressed =
        Glass::Button("download and install", true, ImVec2(half, 46.0f));
    if (report.running)
        ImGui::EndDisabled();
    if (installPressed)
        StartInstall();

    ImGui::SameLine(0.0f, 12.0f);
    if (Glass::Button("open the game folder", false, ImVec2(half, 46.0f)))
        ShellExecuteA(nullptr, "open", SavedGamesDir().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    if (report.running) {
        const float frac =
            report.total > 0 ? static_cast<float>(report.done) / report.total : 0.0f;
        Glass::ProgressBar(frac, ImVec2(ImGui::GetContentRegionAvail().x, 8.0f));
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    } else if (report.finished) {
        Glass::StatusPill(report.ok ? "all files installed" : "some files failed",
                          report.ok ? kOk : kWarn);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

    if (!report.lines.empty()) {
        Glass::SectionHeader("download log");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        const GlassClip parent;
        float logH = 14.0f + static_cast<float>(report.lines.size()) * 19.0f;
        if (logH > 224.0f) logH = 224.0f;

        ImGui::BeginChild("apex_cfg_log", ImVec2(ImGui::GetContentRegionAvail().x, logH),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        parent.ClipHere();

        for (const auto& line : report.lines) {
            const bool failed = line.rfind("failed", 0) == 0;
            ImGui::PushStyleColor(ImGuiCol_Text, failed ? kWarn : kInk);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        parent.Restore();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    Dim("videoconfig.txt, voice_volumes.dat and steam_autocloud.vdf are the three "
        "files installed here - keybinds and profile.cfg are never touched. "
        "autoexec.cfg lives inside the Steam folder, so it has its own button in "
        "the Optimization section. To change resolution, FOV, textures or "
        "anti-aliasing by hand, use the Custom section.");

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::VideoCfg), 2);
    Glass::BeginPanel("where the files land");

    InfoRow("video settings", LocalDir().c_str());
    InfoRow("profile", ProfileDir().c_str());
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    Dim("Apex reads these folders every time it starts, so launch the game once "
        "after installing to pick the new settings up.");

    Glass::EndPanel();
}

namespace {

std::string ReadTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool WriteTextFile(const std::string& path, const std::string& data) {

    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) {
        for (size_t i = 3; i + 1 < slash; ++i)
            if (path[i] == '\\' || path[i] == '/')
                CreateDirectoryA(path.substr(0, i).c_str(), nullptr);
        CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << data;
    return out.good();
}

std::string VideoCfgGet(const std::string& text, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return {};
    pos = text.find('"', pos + needle.size());
    if (pos == std::string::npos) return {};
    const size_t end = text.find('"', pos + 1);
    if (end == std::string::npos) return {};
    return text.substr(pos + 1, end - pos - 1);
}

bool VideoCfgSet(std::string& text, const char* key, const std::string& value) {
    const std::string needle = std::string("\"") + key + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;
    pos = text.find('"', pos + needle.size());
    if (pos == std::string::npos) return false;
    const size_t end = text.find('"', pos + 1);
    if (end == std::string::npos) return false;
    text.replace(pos + 1, end - pos - 1, value);
    return true;
}

std::string ProfileFovGet(const std::string& text) {
    const size_t key = text.find("cl_fovScale");
    if (key == std::string::npos) return {};
    const size_t open = text.find('"', key + 11);
    if (open == std::string::npos) return {};
    const size_t end = text.find('"', open + 1);
    if (end == std::string::npos) return {};
    return text.substr(open + 1, end - open - 1);
}

bool ProfileFovSet(const std::string& path, const std::string& scale) {
    std::string text = ReadTextFile(path);
    const size_t key = text.find("cl_fovScale");
    if (key != std::string::npos) {
        const size_t open = text.find('"', key + 11);
        if (open == std::string::npos) return false;
        const size_t end = text.find('"', open + 1);
        if (end == std::string::npos) return false;
        text.replace(open + 1, end - open - 1, scale);
        return WriteTextFile(path, text);
    }
    if (!text.empty() && text.back() != '\n') text += "\n";
    text += "cl_fovScale \"" + scale + "\"\n";
    return WriteTextFile(path, text);
}

const char* FovScaleFor(int fov) {
    switch (fov) {
        case 70:  return "1.0";
        case 90:  return "1.3";
        case 100: return "1.4";
        case 110: return "1.5";
        case 120: return "1.7";
        default:  return nullptr;
    }
}

constexpr int kFovPresets[] = {70, 90, 100, 110, 120};

bool ChipRow(const std::vector<std::pair<std::string, bool>>& chips,
             std::vector<int>* clicked) {
    const float avail = ImGui::GetContentRegionAvail().x;
    float       rowW  = 0.0f;
    bool        start = true;
    bool        any   = false;
    for (size_t i = 0; i < chips.size(); ++i) {
        const float w = ImGui::CalcTextSize(chips[i].first.c_str()).x + 34.0f;
        if (!start && rowW + w > avail) {
            start = true;
            rowW  = 0.0f;
        }
        if (!start)
            ImGui::SameLine(0.0f, 8.0f);
        if (Glass::Chip(chips[i].first.c_str(), chips[i].second)) {
            clicked->push_back(static_cast<int>(i));
            any = true;
        }
        rowW += w;
        start = false;
    }
    return any;
}

}

void App::DrawCustom() {
    using namespace videocfg;

    const std::string videoPath   = LocalDir() + "\\videoconfig.txt";
    const std::string profilePath = ProfileDir() + "\\profile.cfg";

    const std::string videoText   = ReadTextFile(videoPath);
    const std::string profileText = ReadTextFile(profilePath);
    const bool        haveVideo   = !videoText.empty();

    const std::string resW      = VideoCfgGet(videoText, "setting.defaultres");
    const std::string resH      = VideoCfgGet(videoText, "setting.defaultresheight");
    const std::string streamMem = VideoCfgGet(videoText, "setting.stream_memory");
    const std::string aa        = VideoCfgGet(videoText, "setting.mat_antialias_mode");
    const std::string fovScale  = ProfileFovGet(profileText);

    auto writeVideoKey = [&](const char* key, const char* value,
                             const char* what) {
        std::string next = videoText;
        if (VideoCfgSet(next, key, value) && WriteTextFile(videoPath, next)) {
            Toast(Glass::Icon::CheckCircle, kOk, what,
                  std::string(key) + " set to \"" + value + "\"");
        } else {
            Toast(Glass::Icon::XCircle, kWarn, "could not write", videoPath);
        }
    };

    char resText[48];
    if (!resW.empty() && !resH.empty())
        snprintf(resText, sizeof(resText), "%s x %s", resW.c_str(), resH.c_str());
    else
        snprintf(resText, sizeof(resText), "not found");

    char fovText[32];
    if (fovScale.empty()) {
        snprintf(fovText, sizeof(fovText), "default");
    } else {
        bool known = false;
        for (const int fov : kFovPresets)
            if (fovScale == FovScaleFor(fov)) {
                snprintf(fovText, sizeof(fovText), "%d", fov);
                known = true;
                break;
            }
        if (!known)
            snprintf(fovText, sizeof(fovText), "%s", fovScale.c_str());
    }

    char texText[48];
    if (streamMem.empty())
        snprintf(texText, sizeof(texText), "not found");
    else if (streamMem == "0")
        snprintf(texText, sizeof(texText), "low");
    else if (streamMem == "300000")
        snprintf(texText, sizeof(texText), "high");
    else
        snprintf(texText, sizeof(texText), "custom (%s)", streamMem.c_str());

    char aaText[32];
    if (aa.empty())
        snprintf(aaText, sizeof(aaText), "not found");
    else if (aa == "12")
        snprintf(aaText, sizeof(aaText), "on");
    else if (aa == "0")
        snprintf(aaText, sizeof(aaText), "off");
    else
        snprintf(aaText, sizeof(aaText), "%s", aa.c_str());

    const char* labels[4] = {"resolution", "fov", "textures", "antialiasing"};
    const char* values[4] = {resText, fovText, texText, aaText};
    const bool  accents[4] = {!resW.empty() && !resH.empty(),
                             !fovScale.empty(),
                             streamMem == "0" || streamMem == "300000",
                             aa == "12"};
    StatTileRow(fontSemi, labels, values, accents, 4);

    Dim("These are the two Apex config files, edited in place on this PC. "
        "Close the game first : Apex rewrites both files when it quits and "
        "would undo whatever is changed here.");

    if (!haveVideo) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        Dim("videoconfig.txt was not found - install Apex Legends and the "
            "settings first from the apex video cfg section.");
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 0);
    Glass::BeginPanel("resolution  -  setting.defaultres");

    Dim("Standard 16:9 modes plus the two stretched ones used in "
        "competitive play, then a custom field for anything else.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    struct ResPreset { const char* label; const char* w; const char* h; };
    static const ResPreset kRes[] = {
        {"1280 x 720",  "1280", "720"},
        {"1600 x 900",  "1600", "900"},
        {"1920 x 1080", "1920", "1080"},
        {"2560 x 1440", "2560", "1440"},
        {"3840 x 2160", "3840", "2160"},
        {"1728 x 1080", "1728", "1080"},
        {"1440 x 1080", "1440", "1080"},
    };

    std::vector<std::pair<std::string, bool>> resChips;
    for (const ResPreset& preset : kRes)
        resChips.emplace_back(preset.label,
                              resW == preset.w && resH == preset.h);

    if (!haveVideo)
        ImGui::BeginDisabled();
    std::vector<int> resClicked;
    if (ChipRow(resChips, &resClicked)) {
        const ResPreset& preset = kRes[resClicked.front()];
        std::string      next   = videoText;
        if (VideoCfgSet(next, "setting.defaultres", preset.w) &&
            VideoCfgSet(next, "setting.defaultresheight", preset.h) &&
            WriteTextFile(videoPath, next)) {
            Toast(Glass::Icon::CheckCircle, kOk, "resolution applied",
                  std::string(preset.label) + " written to videoconfig.txt");
        } else {
            Toast(Glass::Icon::XCircle, kWarn, "could not write", videoPath);
        }
    }
    if (!haveVideo)
        ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    Glass::SectionHeader("custom resolution");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    static char widthField[8]  = "";
    static char heightField[8] = "";
    static bool fieldsSeeded   = false;
    if (!fieldsSeeded && !resW.empty() && !resH.empty()) {
        snprintf(widthField, sizeof(widthField), "%s", resW.c_str());
        snprintf(heightField, sizeof(heightField), "%s", resH.c_str());
        fieldsSeeded = true;
    }

    if (!haveVideo)
        ImGui::BeginDisabled();
    Glass::TextField("width", widthField, sizeof(widthField), "e.g. 1920");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    Glass::TextField("height", heightField, sizeof(heightField), "e.g. 1080");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    if (Glass::Button("apply custom resolution", true,
                      ImVec2(ImGui::GetContentRegionAvail().x, 46.0f))) {
        const int w = atoi(widthField);
        const int h = atoi(heightField);
        if (w < 320 || w > 7680 || h < 240 || h > 4320) {
            Toast(Glass::Icon::XCircle, kWarn, "invalid resolution",
                  "width must be 320..7680, height 240..4320.");
        } else {
            char ws[8], hs[8];
            snprintf(ws, sizeof(ws), "%d", w);
            snprintf(hs, sizeof(hs), "%d", h);
            std::string next = videoText;
            if (VideoCfgSet(next, "setting.defaultres", ws) &&
                VideoCfgSet(next, "setting.defaultresheight", hs) &&
                WriteTextFile(videoPath, next)) {
                Toast(Glass::Icon::CheckCircle, kOk, "resolution applied",
                      std::string(ws) + " x " + hs +
                      " written to videoconfig.txt");
            } else {
                Toast(Glass::Icon::XCircle, kWarn, "could not write", videoPath);
            }
        }
    }
    if (!haveVideo)
        ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    InfoRow("videoconfig.txt", videoPath.c_str());
    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 1);
    Glass::BeginPanel("field of view  -  cl_fovScale  (profile.cfg)");

    Dim("Apex stores the FOV as cl_fovScale in profile.cfg : 110 is 1.5, "
        "120 is 1.7 and 120 is the maximum. The file is created here if "
        "Apex never wrote one.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    std::vector<std::pair<std::string, bool>> fovChips;
    for (const int fov : kFovPresets) {
        char label[12];
        snprintf(label, sizeof(label), "%d", fov);
        fovChips.emplace_back(label, fovScale == FovScaleFor(fov));
    }

    std::vector<int> fovClicked;
    if (ChipRow(fovChips, &fovClicked)) {
        const int   fov   = kFovPresets[fovClicked.front()];
        const char* scale = FovScaleFor(fov);
        if (ProfileFovSet(profilePath, scale)) {
            Toast(Glass::Icon::CheckCircle, kOk, "field of view applied",
                  std::to_string(fov) + " written as cl_fovScale \"" +
                  scale + "\"");
        } else {
            Toast(Glass::Icon::XCircle, kWarn, "could not write", profilePath);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    InfoRow("cl_fovScale", fovScale.empty() ? "not set (game default)"
                                             : fovScale.c_str());
    InfoRow("profile.cfg", profilePath.c_str());
    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 2);
    Glass::BeginPanel("texture quality  -  setting.stream_memory");

    Dim("stream_memory is the texture budget in kilobytes : 0 is the low "
        "preset, 300000 the high one - Apex only knows these two, there is "
        "no free number here.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const bool isLow  = streamMem == "0";
    const bool isHigh = streamMem == "300000";

    if (!haveVideo)
        ImGui::BeginDisabled();
    if (Glass::Chip("low  (0)", isLow))
        writeVideoKey("setting.stream_memory", "0", "texture quality applied");
    ImGui::SameLine(0.0f, 8.0f);
    if (Glass::Chip("high  (300000)", isHigh))
        writeVideoKey("setting.stream_memory", "300000",
                      "texture quality applied");
    if (!haveVideo)
        ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    InfoRow("current value", streamMem.empty() ? "not found"
                                               : streamMem.c_str());
    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 3);
    Glass::BeginPanel("anti-aliasing  -  setting.mat_antialias_mode");

    Dim("mat_antialias_mode is 12 when anti-aliasing is on and 0 when it "
        "is off - the toggle only ever writes one of those two values.");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    bool aaOn = aa == "12";
    if (!haveVideo)
        ImGui::BeginDisabled();
    if (Glass::Toggle("anti-aliasing", &aaOn))
        writeVideoKey("setting.mat_antialias_mode", aaOn ? "12" : "0",
                      "anti-aliasing updated");
    if (!haveVideo)
        ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    InfoRow("current value", aa.empty() ? "not found" : aa.c_str());
    Glass::EndPanel();
}

void App::DrawColor() {
    auto& engine = color::VibranceEngine::Get();

    const std::string frontApp = engine.ForegroundApp();

    const bool adlReady   = engine.AdlReady();
    const bool gammaReady = engine.GammaReady();

    const char* pillText = "no colour backend available";
    ImU32       pillTone = kWarn;
    if (engine.Running() && adlReady) {
        pillText = "AMD colour backend ready";
        pillTone = kOk;
    } else if (engine.Running() && gammaReady) {
        pillText = "AMD driver unreachable - gamma fallback";
        pillTone = kWarn;
    } else if (!engine.Running()) {
        pillText = "colour engine stopped";
        pillTone = kWarn;
    }

    Glass::StatusPill(pillText, pillTone);
    ImGui::SameLine(0.0f, 14.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(engine.StatusLine().c_str());
    ImGui::PopStyleColor();

    {
        const std::string detail =
            engine.Running()
                ? engine.BackendInfo()
                : "the engine is not running - start it from the Config section.";
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", detail.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    NavScrollTo(static_cast<int>(Section::Color), 0);
    Glass::BeginPanel("screen profile  -  desktop level");

    bool enabled = engine.Enabled();
    if (Glass::Toggle("enable colour engine", &enabled))
        engine.SetEnabled(enabled);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (!enabled) {
        Dim("The engine is off: the screen is back to the driver defaults. Turn it on to "
            "use the sliders below.");
    } else {

        float saturation = static_cast<float>(engine.DesktopSaturation());
        const float satBefore = saturation;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
        Glass::Slider("saturation", &saturation, 0.0f, 300.0f, "%.0f");
        if (saturation != satBefore)
            engine.SetDesktopSaturation(static_cast<int>(saturation + 0.5f));
        Dim("100 = normal, 150 = vivid, 300 = maximum vibrance");
        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        float brightness = engine.Brightness();
        const float briBefore = brightness;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
        Glass::Slider("brightness", &brightness, 0.50f, 1.50f, "%.2f");
        if (brightness != briBefore) engine.SetBrightness(brightness);

        float contrast = engine.Contrast();
        const float conBefore = contrast;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
        Glass::Slider("contrast", &contrast, 0.50f, 1.50f, "%.2f");
        if (contrast != conBefore) engine.SetContrast(contrast);

        float gamma = engine.Gamma();
        const float gamBefore = gamma;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
        Glass::Slider("gamma", &gamma, 0.50f, 2.00f, "%.2f");
        if (gamma != gamBefore) engine.SetGamma(gamma);

        ImGui::Dummy(ImVec2(0.0f, 12.0f));

        if (Glass::Button("reset to defaults", false,
                          ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
            engine.SetBrightness(1.0f);
            engine.SetContrast(1.0f);
            engine.SetGamma(1.0f);
            engine.SetDesktopSaturation(color::VibranceEngine::kSaturationDefault);
        }

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        Dim("Saturation goes through the AMD driver (ADL). Brightness, contrast and "
            "gamma use the Windows gamma ramp on every active display.");
    }

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Color), 1);
    Glass::BeginPanel("running software  -  detect and colour");

    static std::vector<std::string> running;
    static uint64_t                 scannedAt = 0;
    const uint64_t                  nowMs     = GetTickCount64();
    if (scannedAt == 0 || nowMs - scannedAt > 3000ULL) {
        running   = ScanRunningExecutables();
        scannedAt = nowMs;
    }

    char countText[32];
    snprintf(countText, sizeof(countText), "%d", static_cast<int>(running.size()));
    InfoRow("detected", countText);
    InfoRow("in front", frontApp.empty() ? "-" : frontApp.c_str());

    static float processSat = 150.0f;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 52.0f);
    Glass::Slider("saturation to apply", &processSat, 0.0f, 300.0f, "%.0f");
    ImGui::SameLine(0.0f, 8.0f);
    if (Glass::IconButton("scan", Glass::Icon::Refresh, 34.0f)) {
        running   = ScanRunningExecutables();
        scannedAt = nowMs;
    }

    static char filter[64] = "";
    Glass::SearchField(filter, sizeof(filter), "filter  (e.g. cs2.exe)");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    const std::vector<color::AppRule> known  = engine.Rules();
    const std::string                 needle = LowerCase(filter);

    static std::string profileExe;
    static bool        profileLoaded = false;
    static color::AppRule profileDraft{};

    const GlassClip runningClip;
    ImGui::BeginChild("apex_running_software",
                      ImVec2(ImGui::GetContentRegionAvail().x, 244.0f),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    runningClip.ClipHere();

    std::string addExe;
    std::string dropExe;
    int         visible = 0;

    for (const auto& exe : running) {
        if (!needle.empty() && exe.find(needle) == std::string::npos)
            continue;
        ++visible;

        const color::AppRule* rule = nullptr;
        for (const auto& candidate : known)
            if (color::SameApp(candidate.exe, exe)) {
                rule = &candidate;
                break;
            }

        const bool inFront = !frontApp.empty() && color::SameApp(frontApp, exe);

        ImGui::PushID(exe.c_str());

        const float  w    = ImGui::GetContentRegionAvail().x;
        const float  rowH = 32.0f;
        const ImVec2 p    = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, rowH));

        ImDrawList* dl     = ImGui::GetWindowDrawList();
        const float textY  = p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        const float btnY   = p.y + (rowH - 30.0f) * 0.5f;
        const float btnX   = p.x + w - 34.0f;
        const float profX  = p.x + w - 68.0f;

        if (inFront)
            dl->AddRectFilled(ImVec2(p.x - 4.0f, p.y),
                              ImVec2(p.x + w - 40.0f, p.y + rowH),
                              IM_COL32(122, 152, 255, 30), 8.0f);

        dl->AddText(ImVec2(p.x + 6.0f, textY), inFront ? kAccent : (rule ? kOk : kInk),
                    exe.c_str());

        if (rule != nullptr) {
            char tag[32];
            snprintf(tag, sizeof(tag), "rule %d", rule->saturation);
            const ImVec2 ts = ImGui::CalcTextSize(tag);
            dl->AddText(ImVec2(profX - 10.0f - ts.x, textY), kOk, tag);

            ImGui::SetCursorScreenPos(ImVec2(btnX, btnY));
            if (Glass::IconButton("unrule", Glass::Icon::Minus, 30.0f))
                dropExe = exe;
        } else {
            ImGui::SetCursorScreenPos(ImVec2(btnX, btnY));
            if (Glass::IconButton("ruleit", Glass::Icon::Plus, 30.0f))
                addExe = exe;
        }

        ImGui::SetCursorScreenPos(ImVec2(profX, btnY));
        if (Glass::IconButton("profile", Glass::Icon::Sliders, 30.0f)) {
            profileExe    = exe;
            profileLoaded = false;
        }

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 4.0f));

        ImGui::Dummy(ImVec2(w, 0.0f));
        ImGui::PopID();
    }

    if (visible == 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(needle.empty() ? "no process detected yet"
                                              : "no process matches this filter");
        ImGui::PopStyleColor();
    }

    if (!addExe.empty() || !dropExe.empty()) {
        std::vector<color::AppRule> updated = known;
        if (!addExe.empty() &&
            std::none_of(updated.begin(), updated.end(), [&](const color::AppRule& entry) {
                return color::SameApp(entry.exe, addExe);
            })) {
            color::AppRule fresh{};
            fresh.exe        = addExe;
            fresh.saturation = static_cast<int>(processSat + 0.5f);
            updated.push_back(fresh);
        }
        if (!dropExe.empty())
            updated.erase(
                std::remove_if(updated.begin(), updated.end(),
                               [&](const color::AppRule& entry) {
                                   return color::SameApp(entry.exe, dropExe);
                               }),
                updated.end());
        engine.SetRules(updated);
    }

    ImGui::EndChild();
    runningClip.Restore();

    if (!profileExe.empty()) {
        if (!DrawAppProfileWindow(engine, profileExe, profileDraft, profileLoaded))
            profileExe.clear();
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    Dim("Press + to give an application its own saturation : it is applied as soon as "
        "that window comes to the front. The list refreshes by itself every few "
        "seconds.");

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Color), 2);
    Glass::BeginPanel("per application  -  saturation rules");

    std::vector<color::AppRule> rules = engine.Rules();
    const std::string&          foreground = frontApp;

    InfoRow("in front", foreground.empty() ? "-" : foreground.c_str());
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (rules.empty()) {
        Dim("No rule yet. Every window uses the desktop saturation value.");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    bool rulesChanged = false;
    int  removeIndex  = -1;

    for (size_t i = 0; i < rules.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        float value     = static_cast<float>(rules[i].saturation);
        const float before = value;
        if (RuleRow(rules[i].exe.c_str(), &value)) {
            removeIndex = static_cast<int>(i);
        } else if (value != before) {
            rules[i].saturation = static_cast<int>(value + 0.5f);
            rulesChanged        = true;
        }

        ImGui::PopID();
    }

    if (removeIndex >= 0) {
        rules.erase(rules.begin() + removeIndex);
        rulesChanged = true;
    }

    if (rulesChanged)
        engine.SetRules(rules);

    Glass::Divider();
    Glass::SectionHeader("add a rule");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    static char exeField[64]      = "";
    static float newSaturation    = 150.0f;

    Glass::TextField("exe", exeField, sizeof(exeField), "executable  (e.g. game.exe)",
                     Glass::Icon::File);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    ImGui::PushID("newrule");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
    Glass::Slider("saturation", &newSaturation, 0.0f, 300.0f, "%.0f");
    ImGui::PopID();
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    std::string exeName = exeField;
    for (auto& character : exeName)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');

    const bool addPressed = Glass::Button("add", true, ImVec2(140.0f, 44.0f));
    ImGui::SameLine(0.0f, 10.0f);
    const bool useForeground =
        !foreground.empty() && Glass::Button("use current app", false, ImVec2(190.0f, 44.0f));

    bool addNow = addPressed;
    if (addPressed && exeName.empty())
        addNow = false;

    if ((addNow || useForeground) && Glass::g != nullptr) {
        const std::string name = useForeground ? foreground : exeName;
        bool exists = false;
        for (const auto& rule : rules)
            if (rule.exe == name) exists = true;
        if (!exists) {
            rules.push_back({name, static_cast<int>(newSaturation + 0.5f)});
            engine.SetRules(rules);
            if (addNow) exeField[0] = '\0';
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    Dim("When the matching window comes to the front, its saturation is applied "
        "automatically and restored when it loses focus.");

    Glass::EndPanel();
}

void App::DrawOptimization() {
    const int cpu = CpuUsagePercent();

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatusEx(&memory);

    const uint64_t totalRam = memory.ullTotalPhys;
    const uint64_t usedRam  = totalRam - memory.ullAvailPhys;

    const uint64_t uptimeSeconds = GetTickCount64() / 1000ULL;
    char uptime[64];
    snprintf(uptime, sizeof(uptime), "%llu h %02llu min",
             static_cast<unsigned long long>(uptimeSeconds / 3600),
             static_cast<unsigned long long>((uptimeSeconds % 3600) / 60));

    char cpuText[32];
    snprintf(cpuText, sizeof(cpuText), "%d %%", cpu < 0 ? 0 : cpu);

    const std::string usedText    = FormatBytes(usedRam);
    const std::string totalText   = FormatBytes(totalRam);

    const char* labels[4]  = {"cpu usage", "memory", "total memory", "uptime"};
    const char* values[4]  = {cpuText, usedText.c_str(), totalText.c_str(), uptime};
    const bool  accents[4] = {true, false, false, false};
    StatTileRow(fontSemi, labels, values, accents, 4);
    NavScrollTo(static_cast<int>(Section::Optimization), 0);

    const auto execReport = videocfg::SnapshotAutoexec();
    const bool execOnDisk = videocfg::AutoexecInstalled();

    char execState[32];
    if (execReport.running)
        snprintf(execState, sizeof(execState), "%d / %d", execReport.done,
                 execReport.total);
    else if (execReport.finished)
        snprintf(execState, sizeof(execState), "%s",
                 execReport.ok ? "installed" : "failed");
    else
        snprintf(execState, sizeof(execState), "%s",
                 execOnDisk ? "installed" : "not installed");

    Glass::BeginPanel("autoexec.cfg  -  kerubimm optimisation file");

    Dim("autoexec.cfg comes from the same GitHub repository as the video "
        "settings, but it lands inside the Steam game folder (\\cfg) - so it "
        "needs an administrator instance of APEXClient. Nothing else in this "
        "section touches your files.");

    InfoRow("repository", "kerubim-code / APEX-SETTINGS / autoexec.cfg");
    InfoRow("target", videocfg::SteamFound() ? videocfg::SteamCfgDir().c_str()
                                             : "steam not installed");
    InfoRow("state", execState);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const float halfExec = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const bool  needAdmin = !videocfg::Elevated();
    const bool  canInstall =
        videocfg::SteamFound() && !needAdmin && !execReport.running;

    if (!canInstall)
        ImGui::BeginDisabled();
    const bool execPressed =
        Glass::Button("download and install autoexec.cfg", true,
                      ImVec2(needAdmin ? halfExec : ImGui::GetContentRegionAvail().x,
                             46.0f));
    if (!canInstall)
        ImGui::EndDisabled();
    if (execPressed)
        videocfg::StartAutoexec();

    if (needAdmin) {
        ImGui::SameLine(0.0f, 12.0f);
        if (Glass::Button("restart as administrator", false,
                          ImVec2(halfExec, 46.0f))) {
            char exePath[MAX_PATH] = {};
            GetModuleFileNameA(nullptr, exePath, MAX_PATH);
            const HINSTANCE relaunched = ShellExecuteA(
                nullptr, "runas", exePath, nullptr, nullptr, SW_SHOWNORMAL);
            if (reinterpret_cast<INT_PTR>(relaunched) > 32) {
                Toast(Glass::Icon::CheckCircle, kOk, "restarting as admin",
                      "APEXClient is restarting with administrator rights.");
                RequestClose();
            } else {
                Toast(Glass::Icon::XCircle, kWarn, "elevation declined",
                      "Windows refused the administrator request.");
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    if (execReport.running) {
        const float frac =
            execReport.total > 0
                ? static_cast<float>(execReport.done) / execReport.total
                : 0.0f;
        Glass::ProgressBar(frac,
                           ImVec2(ImGui::GetContentRegionAvail().x, 8.0f));
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    } else if (execReport.finished) {
        Glass::StatusPill(execReport.ok ? "autoexec.cfg installed"
                                        : "autoexec.cfg not installed",
                          execReport.ok ? kOk : kWarn);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    } else {
        Glass::StatusPill(execOnDisk ? "already on disk" : "not installed yet",
                          execOnDisk ? kOk : kDim);
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

    if (!execReport.lines.empty()) {
        Glass::SectionHeader("install log");
        ImGui::Dummy(ImVec2(0.0f, 4.0f));

        const GlassClip parent;
        float logH = 14.0f + static_cast<float>(execReport.lines.size()) * 19.0f;
        if (logH > 180.0f) logH = 180.0f;

        ImGui::BeginChild("apex_autoexec_log",
                          ImVec2(ImGui::GetContentRegionAvail().x, logH),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        parent.ClipHere();

        for (const auto& line : execReport.lines) {
            const bool failed = line.rfind("failed", 0) == 0 ||
                                line.rfind("skip", 0) == 0;
            ImGui::PushStyleColor(ImGuiCol_Text, failed ? kWarn : kInk);
            ImGui::TextUnformatted(line.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::EndChild();
        parent.Restore();
    }

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    Glass::BeginPanel("tuning  -  module coming soon");
    Dim("This module will host the system optimisations (priority tweaks, service "
        "profiles, game mode, ...). The layout is ready, the controls will be added in "
        "the next build.");
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    const float half = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    ImGui::BeginDisabled();
    Glass::Button("performance profile", false, ImVec2(half, 44.0f));
    ImGui::SameLine(0.0f, 12.0f);
    Glass::Button("apply", false, ImVec2(half, 44.0f));
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    Dim("Disabled until the module is implemented.");
    Glass::EndPanel();
}

void App::DrawConfig() {
    auto& engine = color::VibranceEngine::Get();

    const std::vector<std::string> profiles = ListProfiles();

    char profileCount[32];
    snprintf(profileCount, sizeof(profileCount), "%d",
             static_cast<int>(profiles.size()));

    char engineState[16];
    snprintf(engineState, sizeof(engineState), "%s", engine.Running() ? "on" : "off");

    char bootState[16];
    snprintf(bootState, sizeof(bootState), "%s",
             settings::GetBool("boot_animation", true) ? "on" : "off");

    const char* labels[4] = {"saved profiles", "colour engine", "boot animation",
                             "current section"};
    const char* values[4] = {profileCount, engineState, bootState,
                             SectionLabel(section)};
    const bool  accents[4] = {true, engine.Running(), true, false};
    StatTileRow(fontSemi, labels, values, accents, 4);

    NavScrollTo(static_cast<int>(Section::Config), 0);
    Glass::BeginPanel("profiles  -  save and load");

    InfoRow("settings file", engine.SettingsPath().c_str());
    InfoRow("profile folder", settings::ConfigsDir().c_str());

    Glass::Divider();
    Glass::SectionHeader("save the current settings as");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    static char nameField[64] = "";
    Glass::TextField("profilename", nameField, sizeof(nameField),
                     "profile name  (e.g. gaming)", Glass::Icon::File);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const std::string wanted = SanitizeName(nameField);
    if (wanted.empty())
        ImGui::BeginDisabled();
    const bool savePressed =
        Glass::Button("save profile", true, ImVec2(190.0f, 44.0f));
    if (wanted.empty())
        ImGui::EndDisabled();

    ImGui::SameLine(0.0f, 10.0f);
    const bool flushPressed =
        Glass::Button("write settings now", false, ImVec2(200.0f, 44.0f));

    if (savePressed && !wanted.empty()) {
        engine.Save();
        const std::string target = settings::ConfigsDir() + "\\" + wanted + ".ini";
        if (CopyFileA(engine.SettingsPath().c_str(), target.c_str(), FALSE)) {
            nameField[0] = '\0';
            Toast(Glass::Icon::CheckCircle, kOk, "profile saved", wanted);
        } else {
            Toast(Glass::Icon::XCircle, kWarn, "could not save the profile", wanted);
        }
    }

    if (flushPressed) {
        engine.Save();
        Toast(Glass::Icon::CheckCircle, kOk, "settings written", engine.SettingsPath());
    }

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    Glass::SectionHeader("saved profiles");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (profiles.empty())
        Dim("No profile yet. Name the current settings above and press save profile.");

    std::string loadFile;
    std::string deleteFile;

    for (size_t i = 0; i < profiles.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));

        const std::string label = ProfileLabel(profiles[i]);
        const float       w     = ImGui::GetContentRegionAvail().x;
        const float       rowH  = 46.0f;
        const ImVec2      p     = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, rowH));

        const float tileH   = rowH - 8.0f;
        const float lineH   = ImGui::GetTextLineHeight();

        Glass::Primitive tile;
        tile.cx            = p.x + w * 0.5f;
        tile.cy            = p.y + tileH * 0.5f;
        tile.hw            = w * 0.5f;
        tile.hh            = tileH * 0.5f;
        tile.corner_radius = 10.0f;
        tile.fade          = 1.0f;
        tile.material      = Glass::Material::Thin;
        if (Glass::g != nullptr)
            Glass::g->Submit(tile);

        ImGui::GetWindowDrawList()->AddText(
            ImVec2(p.x + 14.0f, p.y + (tileH - lineH) * 0.5f), kInk, label.c_str());

        ImGui::SetCursorScreenPos(
            ImVec2(p.x + w - 190.0f, p.y + (tileH - 34.0f) * 0.5f));
        const bool loadPressed = Glass::Button("load", false, ImVec2(92.0f, 34.0f));

        ImGui::SetCursorScreenPos(
            ImVec2(p.x + w - 46.0f, p.y + (tileH - 34.0f) * 0.5f));
        const bool removePressed = Glass::IconButton("del", Glass::Icon::Trash, 34.0f);

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 4.0f));

        ImGui::Dummy(ImVec2(w, 0.0f));

        if (loadPressed)
            loadFile = profiles[i];
        if (removePressed)
            deleteFile = profiles[i];

        ImGui::PopID();
    }

    if (!loadFile.empty()) {
        const std::string source = settings::ConfigsDir() + "\\" + loadFile;
        if (CopyFileA(source.c_str(), engine.SettingsPath().c_str(), FALSE) &&
            engine.Load()) {
            engine.ApplyNow();
            Toast(Glass::Icon::CheckCircle, kOk, "profile loaded",
                  ProfileLabel(loadFile));
        } else {
            Toast(Glass::Icon::XCircle, kWarn, "could not load the profile",
                  ProfileLabel(loadFile));
        }
    }

    if (!deleteFile.empty()) {
        const std::string target = settings::ConfigsDir() + "\\" + deleteFile;
        if (DeleteFileA(target.c_str()))
            Toast(Glass::Icon::XCircle, kWarn, "profile deleted",
                  ProfileLabel(deleteFile));
        else
            Toast(Glass::Icon::XCircle, kWarn, "could not delete the profile",
                  ProfileLabel(deleteFile));
    }

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Config), 1);
    Glass::BeginPanel("application  -  startup behaviour");

    bool option = settings::GetBool("start_engine", true);
    if (Glass::Toggle("start the colour engine at launch", &option))
        settings::SetBool("start_engine", option);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    option = settings::GetBool("boot_animation", true);
    if (Glass::Toggle("play the boot animation", &option))
        settings::SetBool("boot_animation", option);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    option = settings::GetBool("remember_section", true);
    if (Glass::Toggle("remember the last section", &option)) {
        settings::SetBool("remember_section", option);
        if (option)
            settings::SetInt("section", static_cast<int>(section));
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    Dim("These options are applied the next time APEX starts.");

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    const float engineW = ImGui::GetContentRegionAvail().x;
    if (engine.Running()) {
        if (Glass::Button("stop the colour engine now", false, ImVec2(engineW, 44.0f)))
            engine.Stop();
    } else if (Glass::Button("start the colour engine now", true,
                             ImVec2(engineW, 44.0f))) {
        engine.Start();
        Toast(Glass::Icon::CheckCircle, kOk, "colour engine",
              engine.AdlReady() ? "AMD backend ready" : "gamma fallback active");
    }

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Config), 2);
    Glass::BeginPanel("branding  -  logo artwork");

    char    exePath[MAX_PATH] = {};
    const DWORD exeLength     = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string logoFile      = "logo.jpg";
    if (exeLength > 0 && exeLength < MAX_PATH) {
        const std::string path(exePath, exeLength);
        const size_t      slash = path.find_last_of("\\/");
        if (slash != std::string::npos)
            logoFile = path.substr(0, slash + 1) + "logo.jpg";
    }

    InfoRow("artwork file", logoFile.c_str());
    InfoRow("state", !logo::Ready()        ? "not found"
                     : logo::FromDisk()    ? "loaded from logo.jpg"
                                           : "loaded (built into the exe)");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    Dim("The artwork ships inside APEXClient.exe, so the program is fully "
        "portable and needs no files next to it. To use your own picture, "
        "drop any jpeg or png beside the exe, rename it logo.jpg and "
        "restart APEX - it replaces the built-in one for the boot screen, "
        "the sidebar badge and the footer avatar.");
    if (!logo::Ready())
        Dim("No artwork is available, so the vector emblem is shown instead.");

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Config), 3);
    Glass::BeginPanel("maintenance  -  files and data");

    const float half = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;

    if (Glass::Button("reload settings from disk", false, ImVec2(half, 44.0f))) {
        engine.Load();
        engine.ApplyNow();
        Toast(Glass::Icon::CheckCircle, kOk, "reloaded", engine.SettingsPath());
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (Glass::Button("open the settings folder", false, ImVec2(half, 44.0f)))
        ShellExecuteA(nullptr, "open", settings::RootDir().c_str(), nullptr, nullptr,
                      SW_SHOWNORMAL);

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    if (Glass::Button("save everything now", false, ImVec2(half, 44.0f))) {
        engine.Save();
        Toast(Glass::Icon::CheckCircle, kOk, "saved", engine.SettingsPath());
    }
    ImGui::SameLine(0.0f, 12.0f);
    if (Glass::Button("reset to factory defaults", false, ImVec2(half, 44.0f))) {
        engine.SetEnabled(true);
        engine.SetDesktopSaturation(color::VibranceEngine::kSaturationDefault);
        engine.SetBrightness(1.0f);
        engine.SetContrast(1.0f);
        engine.SetGamma(1.0f);
        engine.SetRules({});
        engine.Save();
        Toast(Glass::Icon::CheckCircle, kOk, "reset done",
              "saturation, brightness, contrast and gamma are back to normal");
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    Dim("Everything lives in %LOCALAPPDATA%\\APEX : vibrance.ini holds the colour "
        "settings, app.ini the startup options, configs\\ your saved profiles.");

    Glass::EndPanel();
}

namespace {

struct ArtTexture {
    ID3D11Texture2D*          tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    unsigned serial = 0;
    int w = 0, h = 0;
};
ArtTexture g_art;

void UpdateArtTexture(unsigned curSerial) {
    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    if (!spotify::CopyArt(g_art.serial, rgba, w, h))
        return;
    ID3D11Device* dev = Device();
    if (dev == nullptr || rgba.empty())
        return;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width            = static_cast<UINT>(w);
    desc.Height           = static_cast<UINT>(h);
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_DEFAULT;
    desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA data{};
    data.pSysMem     = rgba.data();
    data.SysMemPitch = static_cast<UINT>(w * 4);

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(dev->CreateTexture2D(&desc, &data, &tex)) || tex == nullptr)
        return;
    ID3D11ShaderResourceView* srv = nullptr;
    dev->CreateShaderResourceView(tex, nullptr, &srv);
    if (srv == nullptr) {
        tex->Release();
        return;
    }

    if (g_art.srv != nullptr) g_art.srv->Release();
    if (g_art.tex != nullptr) g_art.tex->Release();
    g_art.tex    = tex;
    g_art.srv    = srv;
    g_art.w      = w;
    g_art.h      = h;
    g_art.serial = curSerial;
}

void FormatTime(long long ms, char* out, size_t n) {
    if (ms < 0)
        ms = 0;
    const long long s = ms / 1000;
    snprintf(out, n, "%lld:%02lld", s / 60, s % 60);
}

int ActiveLyricIndex(const std::vector<spotify::LyricLine>& lines, long long pos) {
    int idx = -1;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (lines[i].tMs < 0)
            break;
        if (lines[i].tMs <= pos)
            idx = i;
        else
            break;
    }
    return idx;
}

void DrawArt(const ImVec2& a, const ImVec2& b, float rounding, bool big) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (g_art.srv != nullptr) {
        dl->AddImageRounded((ImTextureID)(intptr_t)g_art.srv, a, b,
                            ImVec2(0, 0), ImVec2(1, 1),
                            IM_COL32(255, 255, 255, 255), rounding);
        return;
    }
    dl->AddRectFilled(a, b, IM_COL32(30, 215, 96, 36), rounding);
    Glass::DrawIcon(dl, Glass::Icon::Music,
                    ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                    (b.x - a.x) * (big ? 0.30f : 0.40f),
                    IM_COL32(30, 215, 96, 210), 2.4f);
}

void SyncLyricsRequest(const spotify::NowPlaying& np) {
    if (np.available && !np.title.empty())
        spotify::RequestLyrics(np.artist, np.title);
}

}

void App::DrawSpotify() {
    spotify::NowPlaying np;
    spotify::Snapshot(np);
    UpdateArtTexture(np.artSerial);
    SyncLyricsRequest(np);

    std::vector<spotify::LyricLine> lines;
    std::string lyrStatus, lyrKey;
    spotify::LyricsSnapshot(lines, lyrStatus, lyrKey);

    char posStr[24], durStr[24];
    FormatTime(np.posMs, posStr, sizeof(posStr));
    FormatTime(np.durMs, durStr, sizeof(durStr));
    char position[48];
    snprintf(position, sizeof(position), "%s / %s", posStr, durStr);

    const char* status = !np.available ? "no session"
                         : np.playing   ? "playing"
                                        : "paused";
    const char* lyricsVal = !g_playerLyrics ? "off"
                            : lyrStatus == "ok"       ? "ready"
                            : lyrStatus == "loading"  ? "loading"
                            : lyrStatus == "none"     ? "none"
                                                      : "-";

    const char* labels[4] = {"media session", "position", "lyrics",
                             "overlay player"};
    const char* values[4] = {status, position, lyricsVal,
                             g_playerOverlay ? "shown" : "hidden"};
    const bool  accents[4] = {np.available, np.available,
                              lyrStatus == "ok", g_playerOverlay};
    StatTileRow(fontSemi, labels, values, accents, 4);
    NavScrollTo(static_cast<int>(Section::Spotify), 0);

    Glass::BeginPanel("now playing");

    if (!np.available) {
        Dim("No media session is publishing right now. Start a track in "
            "Spotify (or any player that speaks the Windows media controls) "
            "and it shows up here instantly - APEX never touches the player "
            "itself.");
    } else {
        const float  avail = ImGui::GetContentRegionAvail().x;
        const float  art   = 148.0f;
        const ImVec2 p     = ImGui::GetCursorScreenPos();
        DrawArt(p, ImVec2(p.x + art, p.y + art), 16.0f, true);

        const float tx = p.x + art + 18.0f;
        ImGui::SetCursorScreenPos(ImVec2(tx, p.y + 6.0f));
        if (fontSemi != nullptr)
            ImGui::PushFont(fontSemi, 24.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        ImGui::TextWrapped("%s",
                           np.title.empty() ? "unknown track" : np.title.c_str());
        ImGui::PopStyleColor();
        if (fontSemi != nullptr)
            ImGui::PopFont();

        auto line = [&](const char* text) {
            const ImVec2 c = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(tx, c.y));
            ImGui::TextWrapped("%s", text);
        };
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        line(np.artist.empty() ? "unknown artist" : np.artist.c_str());
        if (!np.album.empty())
            line(np.album.c_str());
        ImGui::PopStyleColor();

        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + art + 16.0f));
        float frac = (np.durMs > 0)
                         ? static_cast<float>(np.posMs) /
                               static_cast<float>(np.durMs)
                         : 0.0f;
        static bool scrubbing = false;
        const bool  dragging  = Glass::Scrubber("spotify_scrub", &frac, 0.0f,
                                                1.0f, avail);
        if (scrubbing && !dragging && np.durMs > 0)
            spotify::Seek(static_cast<long long>(
                static_cast<double>(frac) * static_cast<double>(np.durMs)));
        scrubbing = dragging;

        const float ty = p.y + art + 52.0f;
        ImGui::SetCursorScreenPos(ImVec2(p.x, ty));
        ImGui::TextDisabled("%s", posStr);
        const char* rem = nullptr;
        char remBuf[24];
        if (np.durMs > 0) {
            FormatTime(np.durMs - np.posMs, remBuf, sizeof(remBuf));
            snprintf(remBuf + strlen(remBuf), sizeof(remBuf) - strlen(remBuf),
                     " left");
            rem = remBuf;
        }
        if (rem != nullptr) {
            const ImVec2 rs = ImGui::CalcTextSize(rem);
            ImGui::SetCursorScreenPos(ImVec2(p.x + avail - rs.x, ty));
            ImGui::TextDisabled("%s", rem);
        }

        const float cy   = ty + 46.0f;
        const float mid  = p.x + avail * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(mid - 84.0f, cy - 20.0f));
        if (Glass::IconButton("spotify_prev", Glass::Icon::Prev, 40.0f))
            spotify::Post(spotify::Cmd::Prev);
        ImGui::SetCursorScreenPos(ImVec2(mid - 28.0f, cy - 28.0f));
        if (Glass::IconButton("spotify_pp", np.playing ? Glass::Icon::Pause
                                                       : Glass::Icon::Play,
                              56.0f))
            spotify::Post(spotify::Cmd::PlayPause);
        ImGui::SetCursorScreenPos(ImVec2(mid + 44.0f, cy - 20.0f));
        if (Glass::IconButton("spotify_next", Glass::Icon::Next, 40.0f))
            spotify::Post(spotify::Cmd::Next);

        ImGui::SetCursorScreenPos(ImVec2(p.x, cy + 34.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
    }

    Glass::EndPanel();

    Glass::BeginPanel("detached overlay player");

    bool on = g_playerOverlay;
    if (Glass::Toggle("show the overlay player", &on)) {
        g_playerOverlay = on;
        settings::SetBool("spotify_overlay", on);
        Toast(Glass::Icon::CheckCircle, on ? kOk : kWarn, "overlay player",
              on ? "shown on the desktop - drag it anywhere"
                 : "hidden");
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    float opacity = g_playerOpacity;
    if (Glass::Slider("overlay opacity", &opacity, 25.0f, 100.0f, "%.0f %%")) {
        g_playerOpacity = opacity;
        settings::SetInt("spotify_overlay_opacity",
                         static_cast<int>(opacity + 0.5f));
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    bool lyrics = g_playerLyrics;
    if (Glass::Toggle("lyrics line on the overlay", &lyrics)) {
        g_playerLyrics = lyrics;
        settings::SetBool("spotify_lyrics", lyrics);
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    Dim("The overlay is a small always-on-top glass window with the cover, "
        "title, position and transport buttons. Drag it anywhere; it never "
        "takes focus away from your game, and the opacity slider controls "
        "the whole card.");

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Spotify), 2);

    Glass::BeginPanel("global keybinds");

    static int    recSlot  = -1;
    static bool   recArmed = false;
    static double recLast  = -1.0;
    if (recSlot >= 0) {

        if (ImGui::GetTime() - recLast > 0.5)
            recSlot = -1;
        recLast = ImGui::GetTime();
    }
    if (recSlot >= 0 && GetForegroundWindow() == Window()) {
        bool anyDown = false;
        int  hit     = 0;
        for (int vk = 1; vk <= 255; ++vk) {
            if ((GetAsyncKeyState(vk) & 0x8000) == 0)
                continue;
            anyDown = true;

            if (vk >= 8 && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                vk != VK_LWIN && vk != VK_RWIN && vk != VK_APPS &&
                vk != VK_CAPITAL && vk != VK_NUMLOCK && vk != VK_SCROLL)
                hit = vk;
        }
        if (!recArmed) {
            if (!anyDown)
                recArmed = true;
        } else if (hit == VK_ESCAPE) {
            recSlot = -1;
        } else if (hit != 0) {
            const int  slot = recSlot;
            const bool ok   = HotkeyBind(slot, hit);
            Toast(ok ? Glass::Icon::CheckCircle : Glass::Icon::XCircle,
                  ok ? kOk : kWarn, "global keybind",
                  ok ? HotkeyKeyName(hit) + " is now bound"
                     : HotkeyKeyName(hit) + " is used by another program");
            recSlot = -1;
        }
    }

    Dim("Click a button, then press the key you want. The keys are global : "
        "they fire even inside a game. Right-click clears a binding, Esc "
        "cancels while recording.");

    static const char* kBindName[3] = {"play / pause", "previous track",
                                       "next track"};
    for (int s = 0; s < 3; ++s) {
        const int    vk  = HotkeyVk(s);
        const ImVec2 row = ImGui::GetCursorScreenPos();

        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        ImGui::TextUnformatted(kBindName[s]);
        ImGui::PopStyleColor();

        const std::string label = (recSlot == s)      ? "press a key ..."
                                  : (vk != 0)         ? HotkeyKeyName(vk)
                                                      : "not set";
        ImGui::SetCursorScreenPos(ImVec2(row.x + 230.0f, row.y - 2.0f));
        ImGui::PushID(s);
        if (Glass::Button(label.c_str(), recSlot == s, ImVec2(210.0f, 0.0f))) {
            if (recSlot == s)
                recSlot = -1;
            else {
                recSlot  = s;
                recArmed = false;
                recLast  = ImGui::GetTime();
            }
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && vk != 0) {
            HotkeyBind(s, 0);
            recSlot = -1;
            Toast(Glass::Icon::CheckCircle, kWarn, "global keybind",
                  std::string(kBindName[s]) + " cleared");
        }
        ImGui::PopID();

        ImGui::SetCursorScreenPos(ImVec2(row.x + 456.0f, row.y + 2.0f));
        if (recSlot == s)
            ImGui::TextDisabled("press a key - esc to cancel");
        else if (vk != 0 && !HotkeyRegistered(s))
            ImGui::TextDisabled("in use by another program");
        else if (vk != 0)
            ImGui::TextDisabled("global");
        ImGui::Dummy(ImVec2(0.0f, 34.0f));
    }

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Spotify), 1);

    Glass::BeginPanel("lyrics");

    if (!g_playerLyrics) {
        Dim("Lyrics are switched off. Turn the toggle above back on to see "
            "the synchronised lines here and on the overlay.");
    } else if (!np.available) {
        Dim("Start a track and the lyrics for it appear here.");
    } else if (lyrStatus == "loading") {
        Dim("Looking the lyrics up on lrclib.net ...");
    } else if (lyrStatus == "none") {
        Dim("No lyrics found for this track on lrclib.net.");
    } else if (lines.empty()) {
        Dim("No lyrics available.");
    } else {
        int idx = ActiveLyricIndex(lines, np.posMs);
        const int count = static_cast<int>(lines.size());
        const int from  = std::max(0, idx - 1);
        const int to    = std::min(count - 1, from + 5);
        for (int i = from; i <= to; ++i) {
            const bool current = (i == idx);
            if (current && fontSemi != nullptr)
                ImGui::PushFont(fontSemi, 20.0f);
            if (current) {

                DrawLyricWriteOn(lines[i].text.c_str(),
                                 ImGui::GetContentRegionAvail().x,
                                 IM_COL32(30, 215, 96, 255), false,
                                 g_lyrAnimSection, idx);
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped("%s", lines[i].text.c_str());
                ImGui::PopStyleColor();
            }
            if (current && fontSemi != nullptr)
                ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, 2.0f));
        }
        if (idx < 0)
            Dim("waiting for the first line ...");
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    Dim("Lyrics come from lrclib.net : one plain request per track, no key, "
        "no account. Without network the panel simply stays empty - nothing "
        "else in APEX ever connects.");

    Glass::EndPanel();
}

void App::DrawSpotifyOverlay(float width, float height) {
    g_overlayItemHover = false;

    spotify::NowPlaying np;
    spotify::Snapshot(np);
    UpdateArtTexture(np.artSerial);
    if (g_playerLyrics)
        SyncLyricsRequest(np);

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(width, height));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##apex_player_overlay", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();

    if (Glass::g != nullptr) {
        Glass::Primitive bg;
        bg.cx            = width * 0.5f;
        bg.cy            = height * 0.5f;
        bg.hw            = width * 0.5f;
        bg.hh            = height * 0.5f;
        bg.corner_radius = 20.0f;
        bg.fade          = 1.0f;
        bg.material      = Glass::Material::Regular;
        Glass::g->Submit(bg);
    }

    const float pad = 14.0f;

    const float art = std::max(
        96.0f, std::min(std::min(height - pad * 2.0f, width * 0.40f), 240.0f));
    const ImVec2 p(pad,
                   pad + std::max(0.0f, (height - pad * 2.0f - art) * 0.5f));
    DrawArt(p, ImVec2(p.x + art, p.y + art), 14.0f, false);

    const float tx = p.x + art + 14.0f;
    const float tw = width - tx - pad;

    ImGui::SetCursorScreenPos(ImVec2(tx, p.y + 4.0f));    if (g_ovrFontSemi != nullptr)
        ImGui::PushFont(g_ovrFontSemi, 19.0f);
    TextFit(np.available ? (np.title.empty() ? "unknown track"
                                             : np.title.c_str())
                         : "nothing playing",
            tw, kInk);
    if (g_ovrFontSemi != nullptr)
        ImGui::PopFont();
    if (ImGui::IsItemHovered())
        g_overlayItemHover = true;

    ImGui::SetCursorScreenPos(ImVec2(tx, p.y + 28.0f));
    TextFit(np.available ? (np.artist.empty() ? "unknown artist"
                                              : np.artist.c_str())
                         : "open a player and press play",
            tw, kDim);

    if (g_playerLyrics) {
        std::vector<spotify::LyricLine> lines;
        std::string status, key;
        spotify::LyricsSnapshot(lines, status, key);
        const int idx = ActiveLyricIndex(lines, np.posMs);
        const char* line = (idx >= 0) ? lines[idx].text.c_str() : nullptr;
        const char* text = line != nullptr ? line
                                           : (status == "loading" ? "..." : "");
        if (text[0] != '\0') {
            const std::string shown = FitText(text, tw);
            ImGui::SetCursorScreenPos(ImVec2(tx, p.y + 50.0f));
            DrawLyricWriteOn(shown.c_str(), tw,
                             line != nullptr ? IM_COL32(30, 215, 96, 255) : kDim,
                             true, g_lyrAnimOverlay, idx);
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(tx, p.y + 72.0f));
    float frac = (np.durMs > 0)
                     ? static_cast<float>(np.posMs) /
                           static_cast<float>(np.durMs)
                     : 0.0f;
    static bool ovrScrubbing = false;
    const bool  ovrDragging  =
        Glass::Scrubber("ovr_scrub", &frac, 0.0f, 1.0f, tw);
    if (ImGui::IsItemHovered() || ovrDragging)
        g_overlayItemHover = true;
    if (ovrScrubbing && !ovrDragging && np.durMs > 0)
        spotify::Seek(static_cast<long long>(
            static_cast<double>(frac) * static_cast<double>(np.durMs)));
    ovrScrubbing = ovrDragging;

    char posStr[24], durStr[24];
    FormatTime(np.posMs, posStr, sizeof(posStr));
    FormatTime(np.durMs, durStr, sizeof(durStr));
    ImGui::SetCursorScreenPos(ImVec2(tx, p.y + 104.0f));
    ImGui::TextDisabled("%s", posStr);
    {
        char rem[32], shown[36];
        if (np.durMs > 0)
            FormatTime(np.durMs - np.posMs, rem, sizeof(rem));
        else
            snprintf(rem, sizeof(rem), "0:00");
        snprintf(shown, sizeof(shown), "-%s", rem);
        const ImVec2 rs = ImGui::CalcTextSize(shown);
        ImGui::SetCursorScreenPos(ImVec2(tx + tw - rs.x, p.y + 104.0f));
        ImGui::TextDisabled("%s", shown);
    }

    const float  rowY    = p.y + 126.0f;
    float startX = tx + (tw - 140.0f) * 0.5f;

    startX = std::min(startX, width - pad - 28.0f - 6.0f - 140.0f);
    startX = std::max(startX, tx);
    ImGui::SetCursorScreenPos(ImVec2(startX, rowY));
    if (Glass::IconButton("ovr_prev", Glass::Icon::Prev, 36.0f))
        spotify::Post(spotify::Cmd::Prev);
    if (ImGui::IsItemHovered())
        g_overlayItemHover = true;
    ImGui::SetCursorScreenPos(ImVec2(startX + 48.0f, rowY - 4.0f));
    if (Glass::IconButton("ovr_pp", np.playing ? Glass::Icon::Pause
                                               : Glass::Icon::Play,
                          44.0f))
        spotify::Post(spotify::Cmd::PlayPause);
    if (ImGui::IsItemHovered())
        g_overlayItemHover = true;
    ImGui::SetCursorScreenPos(ImVec2(startX + 104.0f, rowY));
    if (Glass::IconButton("ovr_next", Glass::Icon::Next, 36.0f))
        spotify::Post(spotify::Cmd::Next);
    if (ImGui::IsItemHovered())
        g_overlayItemHover = true;

    ImGui::SetCursorScreenPos(ImVec2(width - pad - 28.0f, height - pad - 28.0f));
    if (Glass::IconButton("ovr_hide", Glass::Icon::Xmark, 28.0f)) {
        g_playerOverlay = false;
        settings::SetBool("spotify_overlay", false);
        Toast(Glass::Icon::CheckCircle, kWarn, "overlay player", "hidden");
    }
    if (ImGui::IsItemHovered())
        g_overlayItemHover = true;

    ImGui::End();
}

void App::DrawCrosshair() {
    static const char* kShapes[5] = {"cross", "circle", "point", "square",
                                     "diamond"};

    char state[16];
    snprintf(state, sizeof(state), "%s", crosshair::Enabled() ? "on" : "off");
    char shape[16];
    snprintf(shape, sizeof(shape), "%s", kShapes[crosshair::Shape()]);
    char size[16];
    snprintf(size, sizeof(size), "%d px", crosshair::Size());
    char thick[16];
    snprintf(thick, sizeof(thick), "%d", crosshair::Thickness());

    const char* labels[4] = {"state", "shape", "size", "thickness"};
    const char* values[4] = {state, shape, size, thick};
    const bool  accents[4] = {crosshair::Enabled(), true, true, false};
    StatTileRow(fontSemi, labels, values, accents, 4);

    NavScrollTo(static_cast<int>(Section::Crosshair), 0);

    Glass::BeginPanel("appearance  -  shape, colour, size");

    bool on = crosshair::Enabled();
    if (Glass::Toggle("show the crosshair", &on)) {
        crosshair::SetEnabled(on);
        Toast(Glass::Icon::Crosshairs, on ? kOk : kWarn, "crosshair",
              on ? "visible at the centre of the screen" : "hidden");
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    std::vector<std::pair<std::string, bool>> chips;
    for (int i = 0; i < 5; ++i)
        chips.push_back({kShapes[i], i == crosshair::Shape()});
    std::vector<int> hits;
    if (ChipRow(chips, &hits))
        crosshair::SetShape(hits[0]);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    Glass::ColorButton("crosshair colour", crosshair::Rgb());
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    {
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        ImGui::TextUnformatted("size");
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(row.x + 230.0f, row.y - 2.0f));
        if (Glass::Button("- 2", false, ImVec2(100.0f, 0.0f)))
            crosshair::SetSize(crosshair::Size() - 2);
        ImGui::SameLine(0.0f, 8.0f);
        if (Glass::Button("+ 2", false, ImVec2(100.0f, 0.0f)))
            crosshair::SetSize(crosshair::Size() + 2);
        ImGui::SetCursorScreenPos(ImVec2(row.x + 456.0f, row.y + 2.0f));
        ImGui::TextDisabled("%d px", crosshair::Size());
        ImGui::Dummy(ImVec2(0.0f, 34.0f));
    }

    {
        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, kInk);
        ImGui::TextUnformatted("thickness");
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(row.x + 230.0f, row.y - 2.0f));
        char label[32];
        snprintf(label, sizeof(label), "line width %d", crosshair::Thickness());
        if (Glass::Button(label, false, ImVec2(210.0f, 0.0f)))
            crosshair::SetThickness((crosshair::Thickness() % 5) + 1);
        ImGui::SetCursorScreenPos(ImVec2(row.x + 456.0f, row.y + 2.0f));
        ImGui::TextDisabled("cycles 1 to 5");
        ImGui::Dummy(ImVec2(0.0f, 34.0f));
    }

    {
        const ImVec2 p    = ImGui::GetCursorScreenPos();
        const float  box  = 150.0f;
        ImGui::Dummy(ImVec2(box, box));
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box),
                          IM_COL32(8, 10, 16, 255), 12.0f);
        const ImVec2 c(p.x + box * 0.5f, p.y + box * 0.5f);
        const float  k = (box * 0.5f - 10.0f) / 50.0f;
        const float  s = static_cast<float>(crosshair::Size()) * k;
        const float  t = static_cast<float>(crosshair::Thickness()) * k;
        float* rgb = crosshair::Rgb();
        const ImU32 col =
            IM_COL32(static_cast<int>(rgb[0] * 255.0f),
                     static_cast<int>(rgb[1] * 255.0f),
                     static_cast<int>(rgb[2] * 255.0f), 255);
        switch (crosshair::Shape()) {
            case 1:
                dl->AddCircle(c, s, col, 48, t);
                break;
            case 2:
                dl->AddCircleFilled(c, s * 0.5f, col, 32);
                break;
            case 3:
                dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s),
                            col, 0.0f, 0, t);
                break;
            case 4: {
                const ImVec2 pts[4] = {
                    ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y),
                    ImVec2(c.x, c.y + s), ImVec2(c.x - s, c.y)};
                dl->AddPolyline(pts, 4, col, t, ImDrawFlags_Closed);
                break;
            }
            default:
                dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y + s), col, t);
                dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), col, t);
                break;
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

    Dim("The crosshair is its own always-on-top window at the exact centre "
        "of the screen, transparent to clicks and never taking focus. Size "
        "moves in steps of 2 from 2 to 50 px, thickness cycles through five "
        "line widths.");

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Crosshair), 1);

    Glass::BeginPanel("global keybind");

    static int    recSlot  = -1;
    static bool   recArmed = false;
    static double recLast  = -1.0;
    if (recSlot >= 0) {
        if (ImGui::GetTime() - recLast > 0.5)
            recSlot = -1;
        recLast = ImGui::GetTime();
    }
    if (recSlot >= 0 && GetForegroundWindow() == Window()) {
        bool anyDown = false;
        int  hit     = 0;
        for (int vk = 1; vk <= 255; ++vk) {
            if ((GetAsyncKeyState(vk) & 0x8000) == 0)
                continue;
            anyDown = true;
            if (vk >= 8 && vk != VK_SHIFT && vk != VK_CONTROL && vk != VK_MENU &&
                vk != VK_LWIN && vk != VK_RWIN && vk != VK_APPS &&
                vk != VK_CAPITAL && vk != VK_NUMLOCK && vk != VK_SCROLL)
                hit = vk;
        }
        if (!recArmed) {
            if (!anyDown)
                recArmed = true;
        } else if (hit == VK_ESCAPE) {
            recSlot = -1;
        } else if (hit != 0) {
            const bool ok = HotkeyBind(recSlot, hit);
            Toast(ok ? Glass::Icon::CheckCircle : Glass::Icon::XCircle,
                  ok ? kOk : kWarn, "crosshair keybind",
                  ok ? HotkeyKeyName(hit) + " toggles the crosshair"
                     : HotkeyKeyName(hit) + " is used by another program");
            recSlot = -1;
        }
    }

    Dim("Click the button, then press the key you want. The key is global : "
        "it fires even inside a game. Right-click clears the binding, Esc "
        "cancels while recording.");

    const int cvk = HotkeyVk(3);
    const ImVec2 row = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Text, kInk);
    ImGui::TextUnformatted("show / hide crosshair");
    ImGui::PopStyleColor();
    const std::string label =
        recSlot == 3 ? "press a key ..."
                     : (cvk != 0 ? HotkeyKeyName(cvk) : "not set");
    ImGui::SetCursorScreenPos(ImVec2(row.x + 230.0f, row.y - 2.0f));
    ImGui::PushID(3);
    if (Glass::Button(label.c_str(), recSlot == 3, ImVec2(210.0f, 0.0f))) {
        if (recSlot == 3)
            recSlot = -1;
        else {
            recSlot  = 3;
            recArmed = false;
            recLast  = ImGui::GetTime();
        }
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && cvk != 0) {
        HotkeyBind(3, 0);
        recSlot = -1;
        Toast(Glass::Icon::CheckCircle, kWarn, "crosshair keybind",
              "binding cleared");
    }
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(row.x + 456.0f, row.y + 2.0f));
    if (recSlot == 3)
        ImGui::TextDisabled("press a key - esc to cancel");
    else if (cvk != 0 && !HotkeyRegistered(3))
        ImGui::TextDisabled("in use by another program");
    else if (cvk != 0)
        ImGui::TextDisabled("global");
    ImGui::Dummy(ImVec2(0.0f, 34.0f));

    Glass::EndPanel();
}

void App::DrawOverlay() {
    char count[16];
    snprintf(count, sizeof(count), "%d", overlay::Count());
    int rw = 0, rh = 0;
    overlay::Resolution(&rw, &rh);
    char res[32];
    snprintf(res, sizeof(res), "%dx%d", rw, rh);
    const int ovk = HotkeyVk(4);
    const std::string keyName = HotkeyKeyName(ovk);
    char key[32];
    snprintf(key, sizeof(key), "%s", keyName.c_str());
    char state[16];
    snprintf(state, sizeof(state), "%s", overlay::Visible() ? "on" : "off");

    const char* labels[4] = {"items", "resolution", "keybind", "state"};
    const char* values[4] = {count, res, key, state};
    const bool  accents[4] = {overlay::Count() > 0, true, ovk != 0,
                              overlay::Visible()};
    StatTileRow(fontSemi, labels, values, accents, 4);

    NavScrollTo(static_cast<int>(Section::Overlay), 0);

    Glass::BeginPanel("items  -  add images and gifs");

    if (Glass::Button("add an image", false, ImVec2(210.0f, 0.0f))) {
        if (overlay::AddFromDialog(0))
            Toast(Glass::Icon::Image, kOk, "overlay", "image added");
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (Glass::Button("add a gif", false, ImVec2(210.0f, 0.0f))) {
        if (overlay::AddFromDialog(1))
            Toast(Glass::Icon::Film, kOk, "overlay", "gif added");
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const int itemCount = overlay::Count();
    if (itemCount == 0)
        Dim("Nothing yet. Add an image (png, jpg, jpeg, bmp, webp) or a gif "
            "- it lands in the middle of the overlay and starts shown.");
    bool removed = false;
    for (int i = 0; i < itemCount && !removed; ++i) {
        ImGui::PushID(i);
        const bool sel = overlay::Selected() == i;
        const std::string label =
            std::string(overlay::Type(i) == 1 ? "[gif] " : "[img] ") +
            overlay::Name(i);
        const float w = ImGui::GetContentRegionAvail().x;
        if (Glass::Button(label.c_str(), sel, ImVec2(w - 60.0f, 0.0f))) {
            if (sel)
                overlay::Deselect();
            else
                overlay::Select(i);
        }
        ImGui::SameLine(0.0f, 8.0f);
        if (Glass::IconButton("ovritemdel", Glass::Icon::Trash, 36.0f)) {
            overlay::Select(i);
            overlay::RemoveSelected();
            removed = true;
            Toast(Glass::Icon::Trash, kWarn, "overlay", "item removed");
        }
        ImGui::PopID();
    }

    if (overlay::Selected() >= 0) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        float op = overlay::Opacity() * 100.0f;
        if (Glass::Slider("item opacity", &op, 0.0f, 100.0f, "%.0f %%"))
            overlay::SetOpacity(op / 100.0f);
    }

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Overlay), 1);

    Glass::BeginPanel("position  -  quick placement");

    if (overlay::Selected() < 0) {
        Dim("Select an item in the list above first.");
    } else {
        std::vector<std::pair<std::string, bool>> place = {
            {"fullscreen", false}, {"centre", false},     {"top left", false},
            {"top right", false},  {"bottom left", false}, {"bottom right", false}};
        std::vector<int> hits;
        if (ChipRow(place, &hits)) {
            overlay::Place(hits[0]);
            Toast(Glass::Icon::CheckCircle, kOk, "overlay", "item placed");
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        Dim("Drag the item on the overlay to move it, pull a corner handle "
            "to resize it (20 px minimum). The selected item shows a cyan "
            "dashed outline with handles.");
    }

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Overlay), 2);

    Glass::BeginPanel("settings  -  resolution, keybind and behaviour");

    static const char* kRes[4] = {"1920x1080", "1440x1080", "1280x720",
                                  "2560x1440"};
    const int resPick =
        Glass::SegmentedControl("ovr_res", kRes, 4, overlay::ResolutionIndex());
    if (resPick != overlay::ResolutionIndex())
        overlay::SetResolutionIndex(resPick);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    static const char* kKeys[4] = {"F1", "F2", "F3", "F4"};
    const int curKey = (ovk >= VK_F1 && ovk <= VK_F4) ? ovk - VK_F1 : -1;
    const int keyPick = Glass::SegmentedControl("ovr_key", kKeys, 4, curKey);
    if (keyPick != curKey) {
        const bool ok = HotkeyBind(4, VK_F1 + keyPick);
        Toast(ok ? Glass::Icon::CheckCircle : Glass::Icon::XCircle,
              ok ? kOk : kWarn, "overlay keybind",
              ok ? std::string(kKeys[keyPick]) + " shows and hides the overlay"
                 : std::string(kKeys[keyPick]) + " is used by another program");
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    bool click = overlay::ClickThrough();
    if (Glass::Toggle("click through - the overlay never blocks the game",
                      &click))
        overlay::SetClickThrough(click);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    bool lock = overlay::Locked();
    if (Glass::Toggle("lock - no dragging, no selection", &lock))
        overlay::SetLocked(lock);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    bool show = overlay::Visible();
    if (Glass::Toggle("show the overlay", &show))
        overlay::Show(show);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    Dim("The overlay is a single always-on-top window sized to the chosen "
        "resolution, so it lines up with a game set to the same one. Click "
        "through hands every mouse click to what is underneath, lock freezes "
        "the items in place.");

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Overlay), 3);

    Glass::BeginPanel("configuration  -  save and load");

    if (Glass::Button("save configuration", false, ImVec2(210.0f, 0.0f))) {
        if (overlay::SaveDialog(overlay::VkToQtKey(HotkeyVk(4))))
            Toast(Glass::Icon::CheckCircle, kOk, "overlay",
                  "configuration saved");
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (Glass::Button("load configuration", false, ImVec2(210.0f, 0.0f))) {
        int qtKey = 0;
        if (overlay::LoadDialog(&qtKey)) {
            const int newVk = overlay::QtKeyToVk(qtKey);
            if (newVk > 0 && newVk != HotkeyVk(4))
                HotkeyBind(4, newVk);
            Toast(Glass::Icon::CheckCircle, kOk, "overlay",
                  "configuration loaded");
        }
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const std::string folder =
        settings::EnsureDir(settings::RootDir() + "\\overlays");
    InfoRow("last file", overlay::LastPath()[0] ? overlay::LastPath() : "none");
    InfoRow("folder", folder.c_str());
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    Dim("The file keeps every item (path, position, size, opacity, type) and "
        "the keybind in the same JSON schema as the original OverlayApp, so "
        "configs stay interchangeable. The default config.json in the folder "
        "above loads automatically at startup.");

    Glass::EndPanel();
}

}
