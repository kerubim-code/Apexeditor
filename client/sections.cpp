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
#include <commdlg.h>
#include <d3d11.h>
#include <dwmapi.h>
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

void Note(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(text);
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

BOOL CALLBACK CollectWindowExecutable(HWND window, LPARAM param) {
    auto* found = reinterpret_cast<std::vector<std::string>*>(param);

    if (!IsWindowVisible(window))
        return TRUE;

    const LONG_PTR exStyle = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0)
        return TRUE;

    if (GetWindow(window, GW_OWNER) != nullptr && (exStyle & WS_EX_APPWINDOW) == 0)
        return TRUE;

    if (GetWindowTextLengthW(window) <= 0)
        return TRUE;

    int cloaked = 0;
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) &&
        cloaked != 0)
        return TRUE;

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0)
        return TRUE;

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return TRUE;

    char  path[MAX_PATH] = {};
    DWORD size           = MAX_PATH;
    std::string exe;
    if (QueryFullProcessImageNameA(process, 0, path, &size)) {
        exe.assign(path, size);
        const size_t slash = exe.find_last_of("\\/");
        if (slash != std::string::npos)
            exe = exe.substr(slash + 1);
        exe = LowerCase(exe);
    }
    CloseHandle(process);

    if (exe.empty() || IsNoiseProcess(exe))
        return TRUE;

    found->push_back(exe);
    return TRUE;
}

std::vector<std::string> ScanRunningExecutables() {
    std::vector<std::string> found;
    EnumWindows(&CollectWindowExecutable, reinterpret_cast<LPARAM>(&found));

    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    return found;
}

struct IniEntry {
    std::string key;
    std::string value;
};

struct IniSection {
    std::string name;
    std::vector<IniEntry> entries;
};

using IniDoc = std::vector<IniSection>;

void Toast(Glass::Icon icon, ImU32 tile, const char* title,
           const std::string& message);

IniDoc ReadIniDoc(const std::string& path) {
    IniDoc doc;

    char names[32768] = {};
    if (GetPrivateProfileSectionNamesA(names, sizeof(names), path.c_str()) == 0)
        return doc;

    for (const char* name = names; *name != '\0'; name += strlen(name) + 1) {
        IniSection section;
        section.name = name;

        std::vector<char> buffer(32768, '\0');
        GetPrivateProfileSectionA(name, buffer.data(),
                                  static_cast<DWORD>(buffer.size()), path.c_str());
        for (const char* line = buffer.data(); *line != '\0';
             line += strlen(line) + 1) {
            const char* equals = strchr(line, '=');
            if (equals == nullptr)
                section.entries.push_back({line, ""});
            else
                section.entries.push_back(
                    {std::string(line, equals - line), std::string(equals + 1)});
        }

        if (!section.entries.empty())
            doc.push_back(std::move(section));
    }

    return doc;
}

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char code[8];
                    snprintf(code, sizeof(code), "\\u%04x", c);
                    out += code;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string JsonValues(const std::vector<IniEntry>& entries) {
    std::string out = "{\n";
    for (size_t i = 0; i < entries.size(); ++i) {
        out += "      \"" + JsonEscape(entries[i].key) + "\": \"" +
               JsonEscape(entries[i].value) + "\"";
        if (i + 1 < entries.size())
            out += ",";
        out += "\n";
    }
    out += "    }";
    return out;
}

std::string JsonDoc(const IniDoc& doc) {
    std::string out = "{\n";
    for (size_t i = 0; i < doc.size(); ++i) {
        out += "    \"" + JsonEscape(doc[i].name) + "\": " +
               JsonValues(doc[i].entries);
        if (i + 1 < doc.size())
            out += ",";
        out += "\n";
    }
    out += "  }";
    return out;
}

std::string BuildSettingsJson(const IniDoc& app, const IniDoc& vibrance) {
    return "{\n"
           "  \"format\": \"apex-settings\",\n"
           "  \"version\": 1,\n"
           "  \"app\": " + JsonDoc(app) + ",\n"
           "  \"vibrance\": " + JsonDoc(vibrance) + "\n"
           "}\n";
}

struct JsonReader {
    const std::string& text;
    size_t pos = 0;

    void SkipSpace() {
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++pos;
            else
                break;
        }
    }

    bool Eat(char expected) {
        SkipSpace();
        if (pos < text.size() && text[pos] == expected) {
            ++pos;
            return true;
        }
        return false;
    }

    bool ReadString(std::string& out) {
        SkipSpace();
        if (pos >= text.size() || text[pos] != '"')
            return false;
        ++pos;
        out.clear();
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos >= text.size())
                return false;
            const char escaped = text[pos++];
            switch (escaped) {
                case '"':  out.push_back('"');  break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/');  break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (pos + 4 > text.size())
                        return false;
                    unsigned value = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = text[pos++];
                        value <<= 4;
                        if (digit >= '0' && digit <= '9')
                            value |= static_cast<unsigned>(digit - '0');
                        else if (digit >= 'a' && digit <= 'f')
                            value |= static_cast<unsigned>(digit - 'a' + 10);
                        else if (digit >= 'A' && digit <= 'F')
                            value |= static_cast<unsigned>(digit - 'A' + 10);
                        else
                            return false;
                    }
                    if (value < 0x80) {
                        out.push_back(static_cast<char>(value));
                    } else if (value < 0x800) {
                        out.push_back(static_cast<char>(0xC0 | (value >> 6)));
                        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                    } else {
                        out.push_back(static_cast<char>(0xE0 | (value >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                    }
                    break;
                }
                default:
                    out.push_back(escaped);
                    break;
            }
        }
        return false;
    }

    bool ReadScalar(std::string& out) {
        SkipSpace();
        const size_t start = pos;
        while (pos < text.size()) {
            const char c = text[pos];
            if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' ||
                c == '\r' || c == '\n')
                break;
            ++pos;
        }
        out = text.substr(start, pos - start);
        return !out.empty();
    }

    bool SkipValue() {
        SkipSpace();
        if (pos >= text.size())
            return false;
        if (text[pos] == '"') {
            std::string ignored;
            return ReadString(ignored);
        }
        if (text[pos] == '{') {
            ++pos;
            if (Eat('}'))
                return true;
            do {
                std::string key;
                if (!ReadString(key) || !Eat(':'))
                    return false;
                if (!SkipValue())
                    return false;
            } while (Eat(','));
            return Eat('}');
        }
        if (text[pos] == '[') {
            ++pos;
            if (Eat(']'))
                return true;
            do {
                if (!SkipValue())
                    return false;
            } while (Eat(','));
            return Eat(']');
        }
        std::string ignored;
        return ReadScalar(ignored);
    }
};

bool ParseIniDoc(JsonReader& reader, IniDoc& doc) {
    if (!reader.Eat('{'))
        return false;
    reader.SkipSpace();
    if (reader.Eat('}'))
        return true;
    do {
        IniSection section;
        if (!reader.ReadString(section.name) || !reader.Eat(':'))
            return false;
        if (!reader.Eat('{'))
            return false;
        reader.SkipSpace();
        if (!reader.Eat('}')) {
            do {
                IniEntry entry;
                if (!reader.ReadString(entry.key) || !reader.Eat(':'))
                    return false;
                if (!reader.ReadString(entry.value) &&
                    !reader.ReadScalar(entry.value))
                    return false;
                section.entries.push_back(std::move(entry));
            } while (reader.Eat(','));
            if (!reader.Eat('}'))
                return false;
        }
        doc.push_back(std::move(section));
    } while (reader.Eat(','));
    return reader.Eat('}');
}

bool ParseSettingsJson(const std::string& text, IniDoc& app, IniDoc& vibrance) {
    JsonReader reader{text, 0};
    if (!reader.Eat('{'))
        return false;
    reader.SkipSpace();
    if (reader.Eat('}'))
        return false;
    do {
        std::string key;
        if (!reader.ReadString(key) || !reader.Eat(':'))
            return false;
        if (key == "app") {
            if (!ParseIniDoc(reader, app))
                return false;
        } else if (key == "vibrance") {
            if (!ParseIniDoc(reader, vibrance))
                return false;
        } else if (!reader.SkipValue()) {
            return false;
        }
    } while (reader.Eat(','));
    return reader.Eat('}');
}

void WriteIniDoc(const std::string& path, const IniDoc& doc) {
    DeleteFileA(path.c_str());
    for (const auto& section : doc) {
        for (const auto& entry : section.entries) {
            WritePrivateProfileStringA(section.name.c_str(), entry.key.c_str(),
                                       entry.value.c_str(), path.c_str());
        }
    }
}

bool ReadTextFile(const std::wstring& path, std::string& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());

    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF &&
        static_cast<unsigned char>(out[1]) == 0xBB &&
        static_cast<unsigned char>(out[2]) == 0xBF)
        out.erase(0, 3);

    return true;
}

bool WriteTextFile(const std::wstring& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return out.good();
}

std::string NarrowPath(const std::wstring& path) {
    if (path.empty())
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr,
                                           0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, out.data(), needed, nullptr,
                        nullptr);
    return out;
}

bool ChooseJsonPath(HWND owner, bool save, std::wstring& path) {
    wchar_t file[MAX_PATH] = L"apex-settings.json";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = owner;
    ofn.lpstrFilter = L"JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrTitle  = save ? L"Export all APEX settings"
                           : L"Import APEX settings";
    ofn.lpstrDefExt = L"json";
    ofn.Flags       = OFN_NOCHANGEDIR |
                (save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST)
                      : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST));

    if (save) {
        if (!GetSaveFileNameW(&ofn))
            return false;
    } else {
        file[0] = L'\0';
        if (!GetOpenFileNameW(&ofn))
            return false;
    }

    path = file;
    return true;
}

void ApplyImportedSettings() {
    g_guiTransparency = settings::GetBool("gui_transparency", false);
    g_showOverlayCategory = settings::GetBool("overlay_category", false);
    g_showSpotifyCategory = settings::GetBool("spotify_category", false);
    if (!g_showOverlayCategory)
        settings::SetBool("overlay_on", false);
    if (!g_showSpotifyCategory) {
        settings::SetBool("spotify_overlay", false);
    }
    g_playerOverlay   = settings::GetBool("spotify_overlay", false);
    g_playerOpacity   = static_cast<float>(std::max(
        25, std::min(100, settings::GetInt("spotify_overlay_opacity", 85))));
    g_playerLyrics    = settings::GetBool("spotify_lyrics", true);
    ReapplyHotkeys();

    crosshair::SetEnabled(settings::GetBool("crosshair_on", false));
    crosshair::SetShape(settings::GetInt("crosshair_shape", 0));
    crosshair::SetSize(settings::GetInt("crosshair_size", 10));
    crosshair::SetThickness(settings::GetInt("crosshair_thickness", 1));
    crosshair::ReloadGrid();

    const int packed = settings::GetInt("crosshair_color", 0xFFFFFF);
    float*    rgb    = crosshair::Rgb();
    if (rgb != nullptr) {
        rgb[0] = static_cast<float>((packed >> 16) & 0xFF) / 255.0f;
        rgb[1] = static_cast<float>((packed >> 8) & 0xFF) / 255.0f;
        rgb[2] = static_cast<float>(packed & 0xFF) / 255.0f;
    }

    overlay::Show(settings::GetBool("overlay_on", false));
    overlay::SetClickThrough(settings::GetBool("overlay_click", false));
    overlay::SetLocked(settings::GetBool("overlay_lock", false));
    overlay::SetResolutionIndex(settings::GetInt("overlay_res", 0));

    App& app = App::Get();
    if ((app.section == Section::Overlay && !g_showOverlayCategory) ||
        (app.section == Section::Spotify && !g_showSpotifyCategory))
        app.section = Section::Misc;

    auto& engine = color::VibranceEngine::Get();
    engine.Load();
    engine.ApplyNow();
}

void ExportSettingsJson(HWND owner) {
    auto& engine = color::VibranceEngine::Get();
    engine.Save();

    std::wstring path;
    if (!ChooseJsonPath(owner, true, path))
        return;

    const std::string json =
        BuildSettingsJson(ReadIniDoc(settings::AppIniPath()),
                          ReadIniDoc(engine.SettingsPath()));
    if (WriteTextFile(path, json))
        Toast(Glass::Icon::CheckCircle, kOk, "settings exported", NarrowPath(path));
    else
        Toast(Glass::Icon::XCircle, kWarn, "could not export the settings",
              NarrowPath(path));
}

void ImportSettingsJson(HWND owner) {
    std::wstring path;
    if (!ChooseJsonPath(owner, false, path))
        return;

    std::string text;
    if (!ReadTextFile(path, text)) {
        Toast(Glass::Icon::XCircle, kWarn, "could not read the file",
              NarrowPath(path));
        return;
    }

    IniDoc app;
    IniDoc vibrance;
    if (!ParseSettingsJson(text, app, vibrance) ||
        (app.empty() && vibrance.empty())) {
        Toast(Glass::Icon::XCircle, kWarn, "not an APEX settings file",
              NarrowPath(path));
        return;
    }

    if (!app.empty())
        WriteIniDoc(settings::AppIniPath(), app);
    if (!vibrance.empty())
        WriteIniDoc(color::VibranceEngine::Get().SettingsPath(), vibrance);

    ApplyImportedSettings();
    Toast(Glass::Icon::CheckCircle, kOk, "settings imported", NarrowPath(path));
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

bool ModalSlider(const char* label, float* value, float vmin, float vmax,
                 const char* fmt, float width) {
    ImGui::PushID(label);

    const float  rowH  = 30.0f;
    const float  knobR = 7.0f;
    const ImVec2 cur   = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##slider", ImVec2(width, rowH));
    const bool active  = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    if (active) {
        const float span = std::max(1.0f, width - knobR * 2.0f);
        float       t    = (ImGui::GetIO().MousePos.x - cur.x - knobR) / span;
        t                = std::max(0.0f, std::min(1.0f, t));
        *value           = vmin + t * (vmax - vmin);
    }

    float t = (*value - vmin) / (vmax - vmin);
    t       = std::max(0.0f, std::min(1.0f, t));

    const float cy = cur.y + rowH * 0.5f;
    const float x0 = cur.x + knobR;
    const float x1 = cur.x + width - knobR;
    const float kx = x0 + (x1 - x0) * t;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(x0, cy - 3.0f), ImVec2(x1, cy + 3.0f),
                      IM_COL32(255, 255, 255, 26), 3.0f);
    if (kx > x0)
        dl->AddRectFilled(ImVec2(x0, cy - 3.0f), ImVec2(kx, cy + 3.0f),
                          hovered || active ? IM_COL32(146, 172, 255, 235)
                                            : IM_COL32(122, 152, 255, 205),
                          3.0f);
    dl->AddCircleFilled(ImVec2(kx, cy), knobR + (hovered ? 1.2f : 0.0f),
                        IM_COL32(245, 247, 255, 255), 24);

    char buf[64];
    snprintf(buf, sizeof(buf), fmt, *value);

    const float textY  = cur.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
    const float labelX = cur.x + width + 12.0f;
    dl->AddText(ImVec2(labelX, textY), kDim, label);
    const float labelW = ImGui::CalcTextSize(label).x;
    dl->AddText(ImVec2(labelX + labelW + 8.0f, textY), kInk, buf);

    ImGui::PopID();
    return active;
}

bool ModalToggle(const char* label, bool* value) {
    ImGui::PushID(label);

    const float  rowH  = 30.0f;
    const float  w     = ImGui::GetContentRegionAvail().x;
    const ImVec2 cur   = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##toggle", ImVec2(w, rowH));
    const bool clicked = ImGui::IsItemClicked();
    const bool hovered = ImGui::IsItemHovered();
    if (clicked)
        *value = !*value;

    const float pillW = 46.0f;
    const float pillH = 24.0f;
    const float py    = cur.y + (rowH - pillH) * 0.5f;
    const float kx    = *value ? cur.x + pillW - pillH * 0.5f
                               : cur.x + pillH * 0.5f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(cur.x, py), ImVec2(cur.x + pillW, py + pillH),
                      *value ? IM_COL32(97, 128, 255, 240)
                             : IM_COL32(255, 255, 255, hovered ? 34 : 24),
                      pillH * 0.5f);
    dl->AddCircleFilled(ImVec2(kx, py + pillH * 0.5f), pillH * 0.5f - 3.0f,
                        IM_COL32(245, 247, 255, 255), 24);
    dl->AddText(ImVec2(cur.x + pillW + 10.0f,
                       cur.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f),
                kInk, label);

    ImGui::PopID();
    return clicked;
}

bool ModalButton(const char* label, bool primary, ImVec2 size) {
    ImGui::PushID(label);

    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##button", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemDeactivated() && hovered;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = primary
                           ? (hovered ? IM_COL32(128, 158, 255, 255)
                                      : IM_COL32(104, 134, 246, 236))
                           : IM_COL32(255, 255, 255, hovered ? 32 : 18);
    dl->AddRectFilled(cur, ImVec2(cur.x + size.x, cur.y + size.y), fill, 12.0f);
    if (!primary)
        dl->AddRect(cur, ImVec2(cur.x + size.x, cur.y + size.y),
                    IM_COL32(255, 255, 255, 42), 12.0f);

    const ImVec2 textSize = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(cur.x + (size.x - textSize.x) * 0.5f,
                       cur.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f),
                IM_COL32(245, 247, 255, 255), label);

    ImGui::PopID();
    return clicked;
}

bool DrawAppProfileWindow(color::VibranceEngine& engine, const std::string& exe,
                          color::AppRule& draft, bool& loaded) {
    static float satEdit  = 150.0f;
    static float hueEdit  = 100.0f;
    static float tempEdit = 100.0f;

    if (!loaded) {
        draft = color::AppRule{};
        draft.exe        = exe;
        draft.saturation = color::VibranceEngine::kSaturationDefault;
        for (const auto& rule : engine.Rules())
            if (color::SameApp(rule.exe, exe)) {
                draft = rule;
                break;
            }
        satEdit  = static_cast<float>(draft.saturation);
        hueEdit  = static_cast<float>(draft.hue);
        tempEdit = static_cast<float>(draft.temperature);
        loaded   = true;
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
    ImGui::Dummy(ImVec2(500.0f, 0.0f));

    const float sliderW = std::max(160.0f, innerW - 170.0f);

    const float satBefore = satEdit;
    ModalSlider("saturation", &satEdit, 0.0f, 300.0f, "%.0f", sliderW);
    if (satEdit != satBefore)
        draft.saturation = static_cast<int>(satEdit + 0.5f);

    const float hueBefore = hueEdit;
    ModalSlider("hue", &hueEdit, 0.0f, 300.0f, "%.0f", sliderW);
    if (hueEdit != hueBefore)
        draft.hue = static_cast<int>(hueEdit + 0.5f);

    const float tempBefore = tempEdit;
    ModalSlider("temperature", &tempEdit, 0.0f, 300.0f, "%.0f", sliderW);
    if (tempEdit != tempBefore)
        draft.temperature = static_cast<int>(tempEdit + 0.5f);

    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    if (ModalToggle("custom brightness / contrast / gamma", &draft.levels)) {
        if (draft.levels) {
            draft.brightness = engine.Brightness();
            draft.contrast   = engine.Contrast();
            draft.gamma      = engine.Gamma();
        }
    }

    if (draft.levels) {
        ModalSlider("brightness", &draft.brightness, 0.50f, 1.50f, "%.2f", sliderW);
        ModalSlider("contrast", &draft.contrast, 0.50f, 1.50f, "%.2f", sliderW);
        ModalSlider("gamma", &draft.gamma, 0.50f, 2.00f, "%.2f", sliderW);
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    const float half = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;
    const bool  save = ModalButton("save profile", true, ImVec2(half, 44.0f));
    ImGui::SameLine(0.0f, 12.0f);
    if (ModalButton("close", false, ImVec2(half, 44.0f)))
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
        Note("Apex Legends not found.");
    }

    InfoRow("game folder", GameFound() ? GameDirFound().c_str()
                                       : "not installed");
    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    NavScrollTo(static_cast<int>(Section::VideoCfg), 1);
    Glass::BeginPanel("apex video cfg");

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

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::VideoCfg), 2);
    Glass::BeginPanel("files");

    InfoRow("video settings", LocalDir().c_str());
    InfoRow("profile", ProfileDir().c_str());

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

float FovToScale(float fov) {
    if (fov <= static_cast<float>(kFovPresets[0]))
        return 1.0f;
    for (size_t i = 1; i < sizeof(kFovPresets) / sizeof(kFovPresets[0]); ++i) {
        const float hi = static_cast<float>(kFovPresets[i]);
        if (fov <= hi) {
            const float lo    = static_cast<float>(kFovPresets[i - 1]);
            const float loS   = static_cast<float>(atof(FovScaleFor(kFovPresets[i - 1])));
            const float hiS   = static_cast<float>(atof(FovScaleFor(kFovPresets[i])));
            const float ratio = (fov - lo) / (hi - lo);
            return loS + (hiS - loS) * ratio;
        }
    }
    return 1.7f;
}

std::string FormatScale(float scale) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.3f", scale);
    std::string out = buf;
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out;
}

float ScaleToFov(float scale) {
    if (scale <= 1.0f)
        return 70.0f;
    for (size_t i = 1; i < sizeof(kFovPresets) / sizeof(kFovPresets[0]); ++i) {
        const float hiS = static_cast<float>(atof(FovScaleFor(kFovPresets[i])));
        if (scale <= hiS) {
            const float loS   = static_cast<float>(atof(FovScaleFor(kFovPresets[i - 1])));
            const float ratio = (scale - loS) / (hiS - loS);
            return static_cast<float>(kFovPresets[i - 1]) +
                   (static_cast<float>(kFovPresets[i]) -
                    static_cast<float>(kFovPresets[i - 1])) *
                       ratio;
        }
    }
    return 120.0f;
}

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
    const std::string gam       = VideoCfgGet(videoText, "setting.gamma");
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

    if (!haveVideo) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        Note("videoconfig.txt not found.");
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 0);
    Glass::BeginPanel("resolution");

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
                const auto  launch = videocfg::EnsureLaunchOptions();
                std::string message = std::string(ws) + " x " + hs;
                if (launch.ok) {
                    if (launch.changed)
                        message += "  +  launch options updated";
                    else if (launch.alreadyPresent)
                        message += "  +  launch options already set";
                    Toast(Glass::Icon::CheckCircle, kOk, "resolution applied",
                          message);
                } else {
                    message += "  +  ";
                    message += launch.detail.empty() ? "launch options not updated"
                                                     : launch.detail;
                    Toast(Glass::Icon::XCircle, kWarn, "resolution applied",
                          message);
                }
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
    Glass::BeginPanel("field of view");

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

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    Glass::SectionHeader("exact value");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    static char fovField[16] = "";
    static bool fovSeeded    = false;
    if (!fovSeeded) {
        const float current =
            fovScale.empty() ? 70.0f
                             : ScaleToFov(static_cast<float>(atof(fovScale.c_str())));
        snprintf(fovField, sizeof(fovField), "%.0f", current);
        fovSeeded = true;
    }

    Glass::TextField("fov", fovField, sizeof(fovField), "70 - 120");
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (Glass::Button("apply fov", true,
                      ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
        const float fov = static_cast<float>(atof(fovField));
        if (fov < 70.0f || fov > 120.0f) {
            Toast(Glass::Icon::XCircle, kWarn, "invalid fov",
                  "fov must be between 70 and 120.");
        } else {
            const std::string scale = FormatScale(FovToScale(fov));
            if (ProfileFovSet(profilePath, scale)) {
                Toast(Glass::Icon::CheckCircle, kOk, "field of view applied",
                      "fov " + std::string(fovField) +
                          " written as cl_fovScale \"" + scale + "\"");
            } else {
                Toast(Glass::Icon::XCircle, kWarn, "could not write",
                      profilePath);
            }
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    InfoRow("cl_fovScale", fovScale.empty() ? "not set (game default)"
                                             : fovScale.c_str());
    InfoRow("profile.cfg", profilePath.c_str());
    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 2);
    Glass::BeginPanel("texture quality");

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

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    Glass::SectionHeader("exact value");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    static char streamField[24] = "";
    static bool streamSeeded    = false;
    if (!streamSeeded && !streamMem.empty()) {
        snprintf(streamField, sizeof(streamField), "%s", streamMem.c_str());
        streamSeeded = true;
    }

    if (!haveVideo)
        ImGui::BeginDisabled();
    Glass::TextField("stream memory", streamField, sizeof(streamField),
                     "e.g. 300000");
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    if (Glass::Button("apply texture value", true,
                      ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
        const long value = atol(streamField);
        if (value < 0 || value > 2000000) {
            Toast(Glass::Icon::XCircle, kWarn, "invalid value",
                  "stream memory must be between 0 and 2000000.");
        } else {
            char normalized[24];
            snprintf(normalized, sizeof(normalized), "%ld", value);
            writeVideoKey("setting.stream_memory", normalized,
                          "texture quality applied");
        }
    }
    if (!haveVideo)
        ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    InfoRow("current value", streamMem.empty() ? "not found"
                                               : streamMem.c_str());
    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 3);
    Glass::BeginPanel("anti-aliasing");

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

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Custom), 4);
    Glass::BeginPanel("gamma");

    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    static char gammaField[24] = "";
    static bool gammaSeeded    = false;
    if (!gammaSeeded) {
        snprintf(gammaField, sizeof(gammaField), "%s",
                 gam.empty() ? "1" : gam.c_str());
        gammaSeeded = true;
    }

    if (!haveVideo)
        ImGui::BeginDisabled();
    Glass::TextField("setting.gamma", gammaField, sizeof(gammaField),
                     "base is 1");
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    if (Glass::Button("apply gamma", true,
                      ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
        char* end = nullptr;
        const double value = strtod(gammaField, &end);
        if (end == gammaField || value < 0.05 || value > 10.0) {
            Toast(Glass::Icon::XCircle, kWarn, "invalid gamma",
                  "gamma must be between 0.05 and 10.");
        } else {
            char text[32];
            snprintf(text, sizeof(text), "%.6f", value);
            std::string normalized = text;
            while (!normalized.empty() && normalized.back() == '0')
                normalized.pop_back();
            if (!normalized.empty() && normalized.back() == '.')
                normalized.pop_back();
            writeVideoKey("setting.gamma", normalized.c_str(),
                          "gamma applied");
        }
    }
    if (!haveVideo)
        ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    InfoRow("current value", gam.empty() ? "not found" : gam.c_str());
    InfoRow("videoconfig.txt", videoPath.c_str());
    Glass::EndPanel();
}

bool MonitorCard(const char* id, const std::string& label,
                 const std::string& detail, bool selected,
                 const ImVec2& size) {
    ImGui::PushID(id);
    const ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##card", size);
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemDeactivated() && hovered;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 max(cur.x + size.x, cur.y + size.y);
    const float  rounding = 13.0f;

    dl->AddRectFilled(cur, max,
                      selected ? IM_COL32(104, 134, 246, 62)
                               : IM_COL32(255, 255, 255, hovered ? 26 : 14),
                      rounding);
    dl->AddRect(cur, max,
                selected ? IM_COL32(150, 174, 255, 220)
                         : IM_COL32(255, 255, 255, 40),
                rounding, 0, selected ? 1.6f : 1.0f);

    const float iconX = cur.x + 27.0f;
    const float iconY = cur.y + size.y * 0.5f;
    Glass::DrawIcon(dl, Glass::Icon::Display, ImVec2(iconX, iconY), 23.0f,
                    selected ? IM_COL32(255, 255, 255, 255) : kDim, 1.9f);

    dl->AddText(ImVec2(cur.x + 49.0f, cur.y + 11.0f), kInk, label.c_str());
    if (!detail.empty())
        dl->AddText(ImVec2(cur.x + 49.0f, cur.y + 31.0f), kDim, detail.c_str());

    if (selected && size.x > 122.0f) {
        Glass::DrawIcon(dl, Glass::Icon::CheckCircle,
                        ImVec2(max.x - 19.0f, cur.y + 19.0f), 13.0f,
                        IM_COL32(206, 219, 255, 255), 1.7f);
    }

    ImGui::PopID();
    return clicked;
}

bool SliderReset(const char* id) {
    ImGui::SameLine(0.0f, 5.0f);
    const bool clicked = Glass::IconButton(id, Glass::Icon::Refresh, 24.0f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("reset to unity");
    return clicked;
}

void App::DrawColor() {
    auto& engine = color::VibranceEngine::Get();

    const std::string frontApp = engine.ForegroundApp();

    const bool adlReady    = engine.AdlReady();
    const bool nvidiaReady = engine.NvidiaReady();
    const bool gammaReady  = engine.GammaReady();

    const char* pillText = "no colour backend available";
    ImU32       pillTone = kWarn;
    if (engine.Running() && nvidiaReady) {
        pillText = "NVIDIA colour backend ready";
        pillTone = kOk;
    } else if (engine.Running() && adlReady) {
        pillText = "AMD colour backend ready";
        pillTone = kOk;
    } else if (engine.Running() && gammaReady) {
        pillText = "GPU driver unreachable - gamma fallback";
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
            engine.Running() ? engine.BackendInfo() : "engine stopped";
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", detail.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));

    NavScrollTo(static_cast<int>(Section::Color), 0);
    Glass::BeginPanel("screen profile");

    bool enabled = engine.Enabled();
    if (Glass::Toggle("enable colour engine", &enabled))
        engine.SetEnabled(enabled);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (enabled) {
        ImGui::PushID("screen_profile");

        const std::vector<color::MonitorInfo> monitors = engine.Monitors();
        const int selectedMonitor = engine.SelectedMonitor();

        if (!monitors.empty()) {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float gap   = 8.0f;
            const int   count = 1 + static_cast<int>(monitors.size());
            float cardW = (avail - gap * static_cast<float>(count - 1)) /
                          static_cast<float>(count);
            cardW = std::min(190.0f, std::max(104.0f, cardW));
            const ImVec2 cardSize(cardW, 58.0f);

            if (MonitorCard("all_screens", "all screens", "every display",
                            selectedMonitor < 0, cardSize))
                engine.SetSelectedMonitor(-1);

            for (size_t i = 0; i < monitors.size(); ++i) {
                char id[24];
                snprintf(id, sizeof(id), "screen_card_%d", static_cast<int>(i));
                char detail[32];
                if (monitors[i].width > 0 && monitors[i].height > 0)
                    snprintf(detail, sizeof(detail), "%d x %d",
                             monitors[i].width, monitors[i].height);
                else
                    detail[0] = '\0';

                ImGui::SameLine(0.0f, gap);
                if (MonitorCard(id, monitors[i].label, detail,
                                selectedMonitor == static_cast<int>(i), cardSize))
                    engine.SetSelectedMonitor(static_cast<int>(i));
            }

            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (selectedMonitor >= 0 &&
                selectedMonitor < static_cast<int>(monitors.size()))
                ImGui::TextWrapped(
                    "colour values are applied only to %s. The other monitors "
                    "stay at their original levels.",
                    monitors[selectedMonitor].label.c_str());
            else
                ImGui::TextWrapped(
                    "colour values are applied to every detected screen. Pick a "
                    "screen card to isolate one monitor.");
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
        }

        float saturation = static_cast<float>(engine.DesktopSaturation());
        const float satBefore = saturation;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210.0f);
        Glass::Slider("saturation", &saturation, 0.0f, 300.0f, "%.0f");
        if (saturation != satBefore)
            engine.SetDesktopSaturation(static_cast<int>(saturation + 0.5f));
        if (SliderReset("reset_saturation"))
            engine.SetDesktopSaturation(color::VibranceEngine::kSaturationDefault);

        float hue = static_cast<float>(engine.Hue());
        const float hueBefore = hue;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210.0f);
        Glass::Slider("hue", &hue, 0.0f, 300.0f, "%.0f");
        if (hue != hueBefore)
            engine.SetHue(static_cast<int>(hue + 0.5f));
        if (SliderReset("reset_hue"))
            engine.SetHue(color::VibranceEngine::kSaturationDefault);

        float temperature = static_cast<float>(engine.Temperature());
        const float tempBefore = temperature;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210.0f);
        Glass::Slider("temperature", &temperature, 0.0f, 300.0f, "%.0f");
        if (temperature != tempBefore)
            engine.SetTemperature(static_cast<int>(temperature + 0.5f));
        if (SliderReset("reset_temperature"))
            engine.SetTemperature(color::VibranceEngine::kSaturationDefault);

        ImGui::Dummy(ImVec2(0.0f, 8.0f));

        float brightness = engine.Brightness();
        const float briBefore = brightness;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210.0f);
        Glass::Slider("brightness", &brightness, 0.50f, 1.50f, "%.2f");
        if (brightness != briBefore) engine.SetBrightness(brightness);
        if (SliderReset("reset_brightness")) engine.SetBrightness(1.0f);

        float contrast = engine.Contrast();
        const float conBefore = contrast;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210.0f);
        Glass::Slider("contrast", &contrast, 0.50f, 1.50f, "%.2f");
        if (contrast != conBefore) engine.SetContrast(contrast);
        if (SliderReset("reset_contrast")) engine.SetContrast(1.0f);

        float gamma = engine.Gamma();
        const float gamBefore = gamma;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 210.0f);
        Glass::Slider("gamma", &gamma, 0.50f, 2.00f, "%.2f");
        if (gamma != gamBefore) engine.SetGamma(gamma);
        if (SliderReset("reset_gamma")) engine.SetGamma(1.0f);

        ImGui::Dummy(ImVec2(0.0f, 12.0f));

        if (Glass::Button("reset every value to unity", false,
                          ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
            engine.ResetToUnity();
            Toast(Glass::Icon::CheckCircle, kOk, "reset to unity",
                  "brightness, contrast, gamma and colour values are back to neutral.");
        }

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "tip: the refresh icon next to a value resets that single value to "
            "unity (1.00 or 100).");
        ImGui::PopStyleColor();

        ImGui::PopID();
    }

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Color), 1);
    Glass::BeginPanel("running software");

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
    ImGui::PushID("process_saturation");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 190.0f);
    Glass::Slider("saturation", &processSat, 0.0f, 300.0f, "%.0f");
    ImGui::PopID();
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

        const float  w    = ImGui::GetContentRegionAvail().x - 10.0f;
        const float  rowH = 32.0f;
        const ImVec2 p    = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(w, rowH));

        const ImVec2 childPos  = ImGui::GetWindowPos();
        const ImVec2 childSize = ImGui::GetWindowSize();
        const bool   rowInside = p.y >= childPos.y &&
                                 p.y + rowH <= childPos.y + childSize.y;

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

            if (rowInside) {
                ImGui::SetCursorScreenPos(ImVec2(btnX, btnY));
                if (Glass::IconButton("unrule", Glass::Icon::Minus, 30.0f))
                    dropExe = exe;
            }
        } else if (rowInside) {
            ImGui::SetCursorScreenPos(ImVec2(btnX, btnY));
            if (Glass::IconButton("ruleit", Glass::Icon::Plus, 30.0f))
                addExe = exe;
        }

        if (rowInside) {
            ImGui::SetCursorScreenPos(ImVec2(profX, btnY));
            if (Glass::IconButton("profile", Glass::Icon::Sliders, 30.0f)) {
                profileExe    = exe;
                profileLoaded = false;
            }
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

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Color), 2);
    Glass::BeginPanel("per application rules");

    std::vector<color::AppRule> rules = engine.Rules();
    const std::string&          foreground = frontApp;

    InfoRow("in front", foreground.empty() ? "-" : foreground.c_str());
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    if (rules.empty()) {
        Note("no rule yet");
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

    Glass::BeginPanel("autoexec.cfg");

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
    if (execPressed) {
        videocfg::StartAutoexec();

        const auto launch = videocfg::EnsureAutoexecLaunchOption();
        if (launch.ok) {
            if (launch.changed) {
                Toast(Glass::Icon::CheckCircle, kOk, "launch option added",
                      "+exec autoexec.cfg");
            } else {
                Toast(Glass::Icon::CheckCircle, kOk, "launch option already set",
                      "+exec autoexec.cfg");
            }
        } else {
            Toast(Glass::Icon::XCircle, kWarn, "launch option not updated",
                  launch.detail.empty() ? "steam localconfig not found"
                                        : launch.detail);
        }
    }

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
}

void App::DrawConfig() {
    auto& engine = color::VibranceEngine::Get();

    NavScrollTo(static_cast<int>(Section::Config), 0);
    Glass::BeginPanel("settings");

    InfoRow("app settings", settings::AppIniPath().c_str());
    InfoRow("colour settings", engine.SettingsPath().c_str());
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    const float half = (ImGui::GetContentRegionAvail().x - 12.0f) * 0.5f;

    if (Glass::Button("export all settings", true, ImVec2(half, 44.0f)))
        ExportSettingsJson(Window());
    ImGui::SameLine(0.0f, 12.0f);
    if (Glass::Button("import settings", false, ImVec2(half, 44.0f)))
        ImportSettingsJson(Window());

    ImGui::Dummy(ImVec2(0.0f, 12.0f));

    if (Glass::Button("save everything now", false,
                      ImVec2(ImGui::GetContentRegionAvail().x, 44.0f))) {
        engine.Save();
        Toast(Glass::Icon::CheckCircle, kOk, "saved",
              settings::AppIniPath() + " + " + engine.SettingsPath());
    }

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    NavScrollTo(static_cast<int>(Section::Config), 1);
    Glass::BeginPanel("startup");

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

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    const float engineW = ImGui::GetContentRegionAvail().x;
    if (engine.Running()) {
        if (Glass::Button("stop the colour engine now", false, ImVec2(engineW, 44.0f)))
            engine.Stop();
    } else if (Glass::Button("start the colour engine now", true,
                             ImVec2(engineW, 44.0f))) {
        engine.Start();
        const char* backend = engine.NvidiaReady() ? "NVIDIA backend ready"
                              : engine.AdlReady() ? "AMD backend ready"
                                                  : "gamma fallback active";
        Toast(Glass::Icon::CheckCircle, kOk, "colour engine", backend);
    }

    Glass::EndPanel();
}

void App::DrawMisc() {
    NavScrollTo(static_cast<int>(Section::Misc), 0);
    Glass::BeginPanel("interface");

    bool transparency = g_guiTransparency;
    if (Glass::Toggle("transparent gui", &transparency)) {
        g_guiTransparency = transparency;
        settings::SetBool("gui_transparency", transparency);
        Toast(Glass::Icon::CheckCircle, kOk,
              transparency ? "gui transparency on" : "clean black background",
              transparency ? "desktop blur" : "black glass");
    }

    Glass::EndPanel();

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    Glass::BeginPanel("categories");

    bool overlayCategory = g_showOverlayCategory;
    if (Glass::Toggle("overlay category", &overlayCategory)) {
        g_showOverlayCategory = overlayCategory;
        settings::SetBool("overlay_category", overlayCategory);
        if (!overlayCategory) {
            overlay::Show(false);
            if (section == Section::Overlay) {
                section = Section::Misc;
                if (settings::GetBool("remember_section", true))
                    settings::SetInt("section", static_cast<int>(section));
            }
        }
        Toast(Glass::Icon::CheckCircle, kOk, "overlay",
              overlayCategory ? "category shown" : "category hidden");
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    bool spotifyCategory = g_showSpotifyCategory;
    if (Glass::Toggle("spotify category", &spotifyCategory)) {
        g_showSpotifyCategory = spotifyCategory;
        settings::SetBool("spotify_category", spotifyCategory);
        if (!spotifyCategory) {
            g_playerOverlay = false;
            settings::SetBool("spotify_overlay", false);
            if (section == Section::Spotify) {
                section = Section::Misc;
                if (settings::GetBool("remember_section", true))
                    settings::SetInt("section", static_cast<int>(section));
            }
        }
        Toast(Glass::Icon::CheckCircle, kOk, "spotify",
              spotifyCategory ? "category shown" : "category hidden");
    }

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
        Note("no media session");
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
        Note("lyrics off");
    } else if (!np.available) {
        Note("start a track");
    } else if (lyrStatus == "loading") {
        Note("loading lyrics ...");
    } else if (lyrStatus == "none") {
        Note("no lyrics found");
    } else if (lines.empty()) {
        Note("no lyrics available");
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
            Note("waiting for the first line ...");
    }

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
    static const char* kShapes[6] = {"cross", "circle", "point", "square",
                                     "diamond", "custom"};

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

    Glass::BeginPanel("appearance");

    bool on = crosshair::Enabled();
    if (Glass::Toggle("show the crosshair", &on)) {
        crosshair::SetEnabled(on);
        Toast(Glass::Icon::Crosshairs, on ? kOk : kWarn, "crosshair",
              on ? "visible at the centre of the screen" : "hidden");
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    std::vector<std::pair<std::string, bool>> chips;
    for (int i = 0; i < 6; ++i)
        chips.push_back({kShapes[i], i == crosshair::Shape()});
    std::vector<int> hits;
    if (ChipRow(chips, &hits))
        crosshair::SetShape(hits[0]);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (crosshair::Shape() == 5) {
        static int paintMode = 0;
        if (Glass::Chip("draw", paintMode == 0))
            paintMode = 0;
        ImGui::SameLine(0.0f, 8.0f);
        if (Glass::Chip("erase", paintMode == 1))
            paintMode = 1;
        ImGui::SameLine(0.0f, 8.0f);
        if (Glass::Chip("clear", false)) {
            crosshair::ClearGrid();
            crosshair::SaveGrid();
        }
        ImGui::Dummy(ImVec2(0.0f, 10.0f));

        const int   n     = crosshair::GridSize();
        const float cell  = 9.0f;
        const float side  = cell * static_cast<float>(n);
        const float avail = ImGui::GetContentRegionAvail().x;
        if (avail > side)
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - side) * 0.5f);

        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##xhair_grid", ImVec2(side, side));
        const bool hover = ImGui::IsItemHovered() || ImGui::IsItemActive();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(p, ImVec2(p.x + side, p.y + side),
                          IM_COL32(8, 10, 16, 255), 12.0f);
        for (int i = 1; i < n; ++i) {
            const float o    = static_cast<float>(i) * cell;
            const ImU32 line = (i % 4 == 0) ? IM_COL32(255, 255, 255, 26)
                                            : IM_COL32(255, 255, 255, 12);
            dl->AddLine(ImVec2(p.x + o, p.y), ImVec2(p.x + o, p.y + side), line);
            dl->AddLine(ImVec2(p.x, p.y + o), ImVec2(p.x + side, p.y + o), line);
        }

        float* rgb = crosshair::Rgb();
        const ImU32 col =
            IM_COL32(static_cast<int>(rgb[0] * 255.0f),
                     static_cast<int>(rgb[1] * 255.0f),
                     static_cast<int>(rgb[2] * 255.0f), 255);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (crosshair::GridPixel(x, y))
                    dl->AddRectFilled(
                        ImVec2(p.x + x * cell, p.y + y * cell),
                        ImVec2(p.x + (x + 1) * cell, p.y + (y + 1) * cell), col);

        dl->AddLine(ImVec2(p.x + side * 0.5f, p.y),
                    ImVec2(p.x + side * 0.5f, p.y + side),
                    IM_COL32(122, 152, 255, 90));
        dl->AddLine(ImVec2(p.x, p.y + side * 0.5f),
                    ImVec2(p.x + side, p.y + side * 0.5f),
                    IM_COL32(122, 152, 255, 90));

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const int    mx    = static_cast<int>((mouse.x - p.x) / cell);
        const int    my    = static_cast<int>((mouse.y - p.y) / cell);
        if (hover && mx >= 0 && mx < n && my >= 0 && my < n)
            dl->AddRect(ImVec2(p.x + mx * cell, p.y + my * cell),
                        ImVec2(p.x + (mx + 1) * cell, p.y + (my + 1) * cell),
                        IM_COL32(255, 255, 255, 120));

        static int lastX = -1;
        static int lastY = -1;
        const bool left  = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        const bool right = ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (hover && (left || right) && mx >= 0 && mx < n && my >= 0 &&
            my < n) {
            const bool on = right ? false : paintMode == 0;
            if (lastX >= 0 && lastY >= 0) {
                const int steps = std::max(std::abs(mx - lastX),
                                           std::abs(my - lastY));
                for (int s = 1; s <= steps; ++s) {
                    const int x = lastX + (mx - lastX) * s / steps;
                    const int y = lastY + (my - lastY) * s / steps;
                    crosshair::SetGridPixel(x, y, on);
                }
            } else {
                crosshair::SetGridPixel(mx, my, on);
            }
            lastX = mx;
            lastY = my;
        }
        if ((!left && !right) || !hover) {
            lastX = -1;
            lastY = -1;
            crosshair::SaveGrid();
        }

        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

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
            case 5: {
                const int   n    = crosshair::GridSize();
                const float span = s * 2.0f;
                const float cs   = span / static_cast<float>(n);
                const float left = c.x - s;
                const float top  = c.y - s;
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x)
                        if (crosshair::GridPixel(x, y))
                            dl->AddRectFilled(
                                ImVec2(left + x * cs, top + y * cs),
                                ImVec2(left + (x + 1) * cs + 0.4f,
                                       top + (y + 1) * cs + 0.4f),
                                col);
                break;
            }
            default:
                dl->AddLine(ImVec2(c.x, c.y - s), ImVec2(c.x, c.y + s), col, t);
                dl->AddLine(ImVec2(c.x - s, c.y), ImVec2(c.x + s, c.y), col, t);
                break;
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
    }

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
    if (overlay::Window() == nullptr)
        overlay::Create();

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

    Glass::BeginPanel("items");

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
        Note("nothing added yet");
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

        float rot = overlay::Rotation();
        if (Glass::Slider("item rotation", &rot, -180.0f, 180.0f, "%.0f deg"))
            overlay::SetRotation(rot);
    }

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Overlay), 1);

    Glass::BeginPanel("position");

    if (overlay::Selected() < 0) {
        Note("select an item first");
    } else {
        std::vector<std::pair<std::string, bool>> place = {
            {"fullscreen", false}, {"centre", false},     {"top left", false},
            {"top right", false},  {"bottom left", false}, {"bottom right", false}};
        std::vector<int> hits;
        if (ChipRow(place, &hits)) {
            overlay::Place(hits[0]);
            Toast(Glass::Icon::CheckCircle, kOk, "overlay", "item placed");
        }
    }

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Overlay), 2);

    Glass::BeginPanel("settings");

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
    if (Glass::Toggle("click through", &click))
        overlay::SetClickThrough(click);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    bool lock = overlay::Locked();
    if (Glass::Toggle("lock", &lock))
        overlay::SetLocked(lock);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    bool show = overlay::Visible();
    if (Glass::Toggle("show the overlay", &show))
        overlay::Show(show);

    Glass::EndPanel();

    NavScrollTo(static_cast<int>(Section::Overlay), 3);

    Glass::BeginPanel("configuration");

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

    Glass::EndPanel();
}

}
