#include "vibrance.h"

#include "adl_backend.h"
#include "gamma.h"
#include "nvidia_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace apex::color {

namespace {

std::string LocalAppData() {
    char buffer[MAX_PATH] = {};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", buffer, MAX_PATH);
    return (length > 0 && length < MAX_PATH) ? std::string(buffer, length) : std::string(".");
}

std::string ToLower(const std::string& text) {
    std::string out = text;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string BaseName(const std::string& path) {
    const auto slash = path.find_last_of("\\/");
    const std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    return ToLower(name);
}

std::string Narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 1) return std::string();
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

struct NativeMonitor {
    std::string device;
    std::string name;
    int         width   = 0;
    int         height  = 0;
    int         left    = 0;
    int         top     = 0;
    bool        primary = false;
};

BOOL CALLBACK CollectMonitorProc(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    auto* list = reinterpret_cast<std::vector<NativeMonitor>*>(param);
    if (list == nullptr) return TRUE;

    MONITORINFOEXW info = {};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info))
        return TRUE;

    NativeMonitor entry;
    entry.device  = Narrow(info.szDevice);
    entry.width   = info.rcMonitor.right - info.rcMonitor.left;
    entry.height  = info.rcMonitor.bottom - info.rcMonitor.top;
    entry.left    = info.rcMonitor.left;
    entry.top     = info.rcMonitor.top;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    DISPLAY_DEVICEW display = {};
    display.cb = sizeof(display);
    if (EnumDisplayDevicesW(info.szDevice, 0, &display, 0))
        entry.name = Narrow(display.DeviceString);

    list->push_back(entry);
    return TRUE;
}

std::vector<MonitorInfo> EnumerateMonitors() {
    std::vector<NativeMonitor> native;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitorProc,
                        reinterpret_cast<LPARAM>(&native));

    std::stable_sort(native.begin(), native.end(),
                     [](const NativeMonitor& a, const NativeMonitor& b) {
                         if (a.primary != b.primary) return a.primary;
                         if (a.left != b.left) return a.left < b.left;
                         return a.top < b.top;
                     });

    std::vector<MonitorInfo> out;
    out.reserve(native.size());
    for (size_t i = 0; i < native.size(); ++i) {
        MonitorInfo info;
        info.index   = static_cast<int>(i);
        info.label   = "screen " + std::to_string(i + 1);
        info.name    = native[i].name;
        info.device  = native[i].device;
        info.width   = native[i].width;
        info.height  = native[i].height;
        info.primary = native[i].primary;
        out.push_back(info);
    }
    return out;
}

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && (text[begin] == ' ' || text[begin] == '\t' || text[begin] == '\r'))
        ++begin;
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r'))
        --end;
    return text.substr(begin, end - begin);
}

float ClampFloat(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

std::string NormalizeApp(std::string name) {
    name = ToLower(Trim(name));
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".exe") == 0)
        name.resize(name.size() - 4);
    return name;
}

std::string ForegroundExecutable() {
    HWND window = GetForegroundWindow();
    if (window == nullptr) return std::string();

    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) return std::string();

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) return std::string();

    char buffer[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    std::string result;
    if (QueryFullProcessImageNameA(process, 0, buffer, &size))
        result = BaseName(std::string(buffer, size));

    CloseHandle(process);
    return result;
}

}

bool SameApp(const std::string& a, const std::string& b) {
    const std::string na = NormalizeApp(a);
    if (na.empty()) return false;
    return na == NormalizeApp(b);
}

VibranceEngine& VibranceEngine::Get() {
    static VibranceEngine instance;
    return instance;
}

bool VibranceEngine::Start() {
    if (running_.load()) return true;

    Load();

    AdlBackend::Get().Init();
    NvidiaBackend::Get().Init();
    GammaBackend::Get().Init();

    {
        std::lock_guard lock(mutex_);
        RefreshMonitorsLocked();
        satCacheValid_  = false;
        hueCacheValid_  = false;
        tempCacheValid_ = false;
        selectionChanged_ = false;
        disabledRestored_ = false;
    }

    ApplyNow(true);

    running_ = true;
    watcher_ = std::thread(&VibranceEngine::WatchLoop, this);

    return AdlReady() || NvidiaReady() || GammaReady();
}

void VibranceEngine::Stop() {
    const bool wasRunning = running_.exchange(false);
    if (watcher_.joinable())
        watcher_.join();
    if (wasRunning)
        Save();

    AdlBackend::Get().RestoreSaturation();
    AdlBackend::Get().RestoreColorOnAll(kColorHue);
    AdlBackend::Get().RestoreColorOnAll(kColorTemperature);
    NvidiaBackend::Get().RestoreSaturation();
    GammaBackend::Get().Reset();

    {
        std::lock_guard lock(mutex_);
        satCacheValid_  = false;
        hueCacheValid_  = false;
        tempCacheValid_ = false;
        selectionChanged_ = false;
        disabledRestored_ = false;
        dirty_          = false;
    }

    AdlBackend::Get().Shutdown();
    NvidiaBackend::Get().Shutdown();
    GammaBackend::Get().Shutdown();
}

bool VibranceEngine::AdlReady() const {
    return AdlBackend::Get().Ready();
}

bool VibranceEngine::NvidiaReady() const {
    return NvidiaBackend::Get().Ready();
}

bool VibranceEngine::GammaReady() const {
    return GammaBackend::Get().Ready();
}

bool VibranceEngine::Running() const {
    return running_.load();
}

std::string VibranceEngine::BackendInfo() const {
    std::string info;
    bool gpuBackend = false;

    if (AdlReady()) {
        info += AdlBackend::Get().BackendName() + " (" +
                std::to_string(AdlBackend::Get().Displays().size()) + " display(s))";
        gpuBackend = true;
    }

    if (NvidiaReady()) {
        if (gpuBackend) info += "  |  ";
        info += NvidiaBackend::Get().BackendName() + " (" +
                std::to_string(NvidiaBackend::Get().DisplayCount()) + " display(s))";
        gpuBackend = true;
    }

    if (!gpuBackend) {
        info += "AMD ADL: " + AdlBackend::Get().Error();
        info += "  |  NVIDIA: " + NvidiaBackend::Get().Error();
    }

    if (GammaReady())
        info += "  |  gamma ramp (" + std::to_string(GammaBackend::Get().DisplayCount()) +
                " display(s))";
    else
        info += "  |  gamma: " + GammaBackend::Get().Error();

    return info;
}

bool VibranceEngine::Enabled() const {
    std::lock_guard lock(mutex_);
    return enabled_;
}

void VibranceEngine::SetEnabled(bool enabled) {
    std::lock_guard lock(mutex_);
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    pendingForce_ = true;
    pending_      = true;
    MarkDirtyLocked();
}

int VibranceEngine::DesktopSaturation() const {
    std::lock_guard lock(mutex_);
    return desktopSat_;
}

void VibranceEngine::SetDesktopSaturation(int value) {
    std::lock_guard lock(mutex_);
    desktopSat_ = std::max(kSaturationMin, std::min(value, kSaturationMax));
    pending_ = true;
    MarkDirtyLocked();
}

float VibranceEngine::Brightness() const {
    std::lock_guard lock(mutex_);
    return brightness_;
}

void VibranceEngine::SetBrightness(float value) {
    std::lock_guard lock(mutex_);
    brightness_ = ClampFloat(value, 0.50f, 1.50f);
    pending_ = true;
    MarkDirtyLocked();
}

float VibranceEngine::Contrast() const {
    std::lock_guard lock(mutex_);
    return contrast_;
}

void VibranceEngine::SetContrast(float value) {
    std::lock_guard lock(mutex_);
    contrast_ = ClampFloat(value, 0.50f, 1.50f);
    pending_ = true;
    MarkDirtyLocked();
}

float VibranceEngine::Gamma() const {
    std::lock_guard lock(mutex_);
    return gamma_;
}

void VibranceEngine::SetGamma(float value) {
    std::lock_guard lock(mutex_);
    gamma_ = ClampFloat(value, 0.50f, 2.00f);
    pending_ = true;
    MarkDirtyLocked();
}

int VibranceEngine::Hue() const {
    std::lock_guard lock(mutex_);
    return hue_;
}

void VibranceEngine::SetHue(int value) {
    std::lock_guard lock(mutex_);
    hue_ = std::max(kSaturationMin, std::min(value, kSaturationMax));
    pending_ = true;
    MarkDirtyLocked();
}

int VibranceEngine::Temperature() const {
    std::lock_guard lock(mutex_);
    return temperature_;
}

void VibranceEngine::SetTemperature(int value) {
    std::lock_guard lock(mutex_);
    temperature_ = std::max(kSaturationMin, std::min(value, kSaturationMax));
    pending_ = true;
    MarkDirtyLocked();
}

bool VibranceEngine::HueAvailable() const {
    return AdlBackend::Get().HueAvailable();
}

bool VibranceEngine::TemperatureAvailable() const {
    return AdlBackend::Get().TemperatureAvailable();
}

std::vector<MonitorInfo> VibranceEngine::Monitors() const {
    std::lock_guard lock(mutex_);
    return monitors_;
}

int VibranceEngine::SelectedMonitor() const {
    std::lock_guard lock(mutex_);
    return selectedMonitor_;
}

void VibranceEngine::SetSelectedMonitor(int index) {
    std::lock_guard lock(mutex_);
    if (index >= static_cast<int>(monitors_.size()))
        index = -1;
    if (index < -1) index = -1;
    if (selectedMonitor_ == index) return;

    selectedMonitor_  = index;
    selectionChanged_ = true;
    satCacheValid_    = false;
    hueCacheValid_    = false;
    tempCacheValid_   = false;
    pendingForce_     = true;
    pending_          = true;
    MarkDirtyLocked();
}

void VibranceEngine::ResetToUnity() {
    std::lock_guard lock(mutex_);
    desktopSat_  = kSaturationDefault;
    hue_         = kSaturationDefault;
    temperature_ = kSaturationDefault;
    brightness_  = 1.0f;
    contrast_    = 1.0f;
    gamma_       = 1.0f;
    pendingForce_ = true;
    pending_      = true;
    MarkDirtyLocked();
}

void VibranceEngine::RefreshMonitorsLocked() {
    std::vector<MonitorInfo> found = EnumerateMonitors();

    if (found.empty()) {
        // Headless/fallback case: derive a screen count from the backends.
        const size_t count = std::max({
            AdlBackend::Get().Displays().size(),
            static_cast<size_t>(NvidiaBackend::Get().DisplayCount()),
            static_cast<size_t>(GammaBackend::Get().DisplayCount())});
        for (size_t i = 0; i < count; ++i) {
            MonitorInfo info;
            info.index  = static_cast<int>(i);
            info.label  = "screen " + std::to_string(i + 1);
            info.device = GammaBackend::Get().DeviceName(static_cast<int>(i));
            found.push_back(info);
        }
    }

    monitors_ = std::move(found);
    if (selectedMonitor_ >= static_cast<int>(monitors_.size()))
        selectedMonitor_ = -1;
    if (selectedMonitor_ < -1)
        selectedMonitor_ = -1;
}

void VibranceEngine::RestoreAllTargetsLocked() {
    AdlBackend::Get().RestoreSaturation();
    AdlBackend::Get().RestoreColorOnAll(kColorHue);
    AdlBackend::Get().RestoreColorOnAll(kColorTemperature);
    NvidiaBackend::Get().RestoreSaturation();
    GammaBackend::Get().Reset();

    satCacheValid_  = false;
    hueCacheValid_  = false;
    tempCacheValid_ = false;
}

std::string VibranceEngine::GammaDeviceForLocked(int monitor) const {
    if (monitor < 0 || monitor >= static_cast<int>(monitors_.size())) {
        // -1 (all screens) is handled by the caller with an empty device.
        return std::string();
    }
    const std::string& device = monitors_[monitor].device;
    if (!device.empty())
        return device;
    return GammaBackend::Get().DeviceName(monitor);
}

int VibranceEngine::AdlDisplayForLocked(int monitor) const {
    if (monitor < 0) return -1;

    const std::vector<DisplayTarget> displays = AdlBackend::Get().Displays();
    if (monitor >= static_cast<int>(displays.size()) ||
        monitor >= static_cast<int>(monitors_.size()))
        return monitor;

    const std::string& device = monitors_[monitor].device;
    if (device.empty())
        return monitor;

    // ADL reports the Windows device path (for example \\.\DISPLAY2) for each
    // output. Matching on it keeps "screen N" stable even when the driver
    // enumerates outputs in a different order than Windows sorts monitors.
    for (size_t i = 0; i < displays.size(); ++i) {
        if (_stricmp(displays[i].name.c_str(), device.c_str()) == 0)
            return static_cast<int>(i);
    }
    return monitor;
}

void VibranceEngine::RefreshMonitorsIfChanged() {
    std::vector<MonitorInfo> found = EnumerateMonitors();
    if (found.empty())
        return;  // keep the last known list if enumeration fails transiently

    std::lock_guard lock(mutex_);

    bool changed = found.size() != monitors_.size();
    if (!changed) {
        for (size_t i = 0; i < found.size(); ++i) {
            const MonitorInfo& a = found[i];
            const MonitorInfo& b = monitors_[i];
            if (a.device != b.device || a.width != b.width ||
                a.height != b.height || a.primary != b.primary) {
                changed = true;
                break;
            }
        }
    }
    if (!changed) return;

    monitors_ = std::move(found);

    // Display numbering may have shifted: fall back to all screens so the
    // current values are never left applied to a screen the user did not pick.
    selectedMonitor_  = -1;
    selectionChanged_ = false;
    satCacheValid_    = false;
    hueCacheValid_    = false;
    tempCacheValid_   = false;
    pendingForce_     = true;
    pending_          = true;
    MarkDirtyLocked();
}

std::vector<AppRule> VibranceEngine::Rules() const {
    std::lock_guard lock(mutex_);
    return rules_;
}

void VibranceEngine::SetRules(const std::vector<AppRule>& rules) {
    std::lock_guard lock(mutex_);
    rules_ = rules;
    for (auto& rule : rules_)
        rule.exe = BaseName(rule.exe);
    pendingForce_ = true;
    pending_      = true;
    MarkDirtyLocked();
}

bool VibranceEngine::ActiveRule(AppRule& out) const {
    std::lock_guard lock(mutex_);
    if (!enabled_) return false;
    const AppRule* rule = RuleInFrontLocked();
    if (rule == nullptr) return false;
    out = *rule;
    return true;
}

int VibranceEngine::TargetSaturation() const {
    std::lock_guard lock(mutex_);
    return TargetSaturationLocked();
}

std::string VibranceEngine::ForegroundApp() const {
    std::lock_guard lock(mutex_);
    return foreground_;
}

int VibranceEngine::LastAppliedSaturation() const {
    std::lock_guard lock(mutex_);
    return appliedSat_;
}

std::string VibranceEngine::StatusLine() const {
    std::lock_guard lock(mutex_);

    std::string text;
    text += enabled_ ? "color engine: ON" : "color engine: OFF";
    text += "  |  saturation " + std::to_string(TargetSaturationLocked());
    if (enabled_) {
        const AppRule* rule = RuleInFrontLocked();
        if (rule != nullptr)
            text += "  (profile: " + rule->exe + ")";
    }
    if (selectedMonitor_ >= 0 &&
        selectedMonitor_ < static_cast<int>(monitors_.size()))
        text += "  |  " + monitors_[selectedMonitor_].label;
    else
        text += "  |  all screens";
    text += "  |  front: " + (foreground_.empty() ? std::string("-") : foreground_);
    return text;
}

const AppRule* VibranceEngine::RuleInFrontLocked() const {
    if (foreground_.empty()) return nullptr;
    for (const auto& rule : rules_)
        if (SameApp(rule.exe, foreground_))
            return &rule;
    return nullptr;
}

int VibranceEngine::TargetSaturationLocked() const {
    if (!enabled_) return kSaturationDefault;

    const AppRule* rule = RuleInFrontLocked();
    if (rule != nullptr)
        return std::max(kSaturationMin, std::min(rule->saturation, kSaturationMax));

    return desktopSat_;
}

void VibranceEngine::ApplySaturation(int value, bool force) {
    auto& adl    = AdlBackend::Get();
    auto& nvidia = NvidiaBackend::Get();

    const bool adlReady    = adl.Ready();
    const bool nvidiaReady = nvidia.Ready();
    if (!adlReady && !nvidiaReady) return;

    if (!force && satCacheValid_ && value == satCache_) return;

    const int adlTarget = AdlDisplayForLocked(selectedMonitor_);
    bool needsWrite = false;

    if (adlReady) {
        const auto displays = adl.Displays();
        if (adlTarget < static_cast<int>(displays.size())) {
            for (size_t i = 0; i < displays.size(); ++i) {
                if (adlTarget >= 0 &&
                    static_cast<int>(i) != adlTarget)
                    continue;
                const auto& target = displays[i];
                ColorValue current;
                if (!adl.GetColor(target, kColorSaturation, current) ||
                    current.current != AdlBackend::MapSaturation(target, value)) {
                    needsWrite = true;
                    break;
                }
            }
        }
    }

    if (!needsWrite && nvidiaReady)
        needsWrite = !nvidia.MatchesSaturation(value, selectedMonitor_);

    if (needsWrite) {
        if (adlReady)    adl.SetSaturation(value, adlTarget);
        if (nvidiaReady) nvidia.SetSaturationOnAll(value, selectedMonitor_);
    }

    satCache_      = value;
    satCacheValid_ = true;
}

void VibranceEngine::ApplyExtraColor(int flag, int value, bool force) {
    auto& adl = AdlBackend::Get();
    if (!adl.Ready()) return;

    int*  cache = (flag == kColorHue) ? &hueCache_ : &tempCache_;
    bool* valid = (flag == kColorHue) ? &hueCacheValid_ : &tempCacheValid_;

    if (!force && *valid && value == *cache) return;

    adl.SetColorOnTargets(flag, value, AdlDisplayForLocked(selectedMonitor_));
    *cache = value;
    *valid = true;
}

void VibranceEngine::ApplyLocked(bool force) {
    if (!enabled_) {
        if (!disabledRestored_) {
            RestoreAllTargetsLocked();
            disabledRestored_ = true;
        }
        appliedSat_ = kSaturationDefault;
        return;
    }
    disabledRestored_ = false;

    if (selectionChanged_) {
        selectionChanged_ = false;
        RestoreAllTargetsLocked();
        force = true;
    }

    const int target = TargetSaturationLocked();
    ApplySaturation(target, force);
    appliedSat_ = target;

    int hue  = kSaturationDefault;
    int temp = kSaturationDefault;
    if (enabled_) {
        const AppRule* rule = RuleInFrontLocked();
        if (rule != nullptr) {
            hue  = std::max(kSaturationMin, std::min(rule->hue, kSaturationMax));
            temp = std::max(kSaturationMin, std::min(rule->temperature, kSaturationMax));
        } else {
            hue  = hue_;
            temp = temperature_;
        }
    }
    ApplyExtraColor(kColorHue, hue, force);
    ApplyExtraColor(kColorTemperature, temp, force);

    auto& gamma = GammaBackend::Get();
    if (gamma.Ready()) {
        float b = 1.0f, c = 1.0f, g = 1.0f;
        const AppRule* rule = RuleInFrontLocked();
        if (rule != nullptr && rule->levels) {
            b = rule->brightness;
            c = rule->contrast;
            g = rule->gamma;
        } else {
            b = brightness_;
            c = contrast_;
            g = gamma_;
        }

        const std::string device =
            (selectedMonitor_ >= 0) ? GammaDeviceForLocked(selectedMonitor_)
                                    : std::string();
        if (selectedMonitor_ < 0 || !device.empty()) {
            if (!gamma.Matches(b, c, g, device))
                gamma.Apply(b, c, g, device);
        }
    }
}

void VibranceEngine::ApplyNow(bool force) {
    std::lock_guard lock(mutex_);
    ApplyLocked(force);
}

void VibranceEngine::WatchLoop() {
    uint64_t lastFullApply = 0;

    while (running_.load()) {

        Sleep(pending_.load() ? 15 : 60);

        const std::string foreground = ForegroundExecutable();

        bool changed = false;
        {
            std::lock_guard lock(mutex_);
            if (foreground != foreground_) {
                foreground_ = foreground;
                changed = true;
            }
        }

        const bool force = pendingForce_.exchange(false);
        const bool need  = pending_.exchange(false) || changed || force;

        const uint64_t now = GetTickCount64();

        // Hot-plug / resolution changes: keep the screen cards in sync. The
        // scan is cheap and only runs twice a second.
        if (now - lastMonitorScan_ >= 2000) {
            lastMonitorScan_ = now;
            RefreshMonitorsIfChanged();
        }

        if (need || now - lastFullApply >= 1500) {
            lastFullApply = now;
            ApplyNow(force || changed);
        }

        FlushDirty();
    }
}

std::string VibranceEngine::SettingsPath() const {
    return LocalAppData() + "\\APEX\\vibrance.ini";
}

void VibranceEngine::MarkDirtyLocked() {
    dirty_     = true;
    dirtyTick_ = GetTickCount64();
}

void VibranceEngine::FlushDirty() const {
    std::lock_guard lock(mutex_);
    if (!dirty_) return;
    if (dirtyTick_ != 0 && GetTickCount64() - dirtyTick_ < 400) return;
    if (Save()) dirty_ = false;
}

bool VibranceEngine::Load() {
    std::ifstream in(SettingsPath());
    if (!in) return false;

    std::vector<AppRule> loadedRules;
    std::string line;
    bool inApps = false;

    while (std::getline(in, line)) {
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;

        if (line.front() == '[') {
            inApps = (line.find("[apps]") != std::string::npos);
            continue;
        }

        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = ToLower(Trim(line.substr(0, eq)));
        const std::string value = Trim(line.substr(eq + 1));

        if (inApps) {

            AppRule rule;
            rule.exe = key;

            std::vector<std::string> parts;
            {
                std::istringstream stream(value);
                std::string        part;
                while (std::getline(stream, part, ','))
                    parts.push_back(Trim(part));
            }

            const auto clampNeutral = [](const std::string& text) {
                return std::max(kSaturationMin,
                                std::min(atoi(text.c_str()), kSaturationMax));
            };
            const auto readLevels = [&](size_t offset) {
                rule.levels     = true;
                rule.brightness = ClampFloat(static_cast<float>(atof(parts[offset].c_str())), 0.50f, 1.50f);
                rule.contrast   = ClampFloat(static_cast<float>(atof(parts[offset + 1].c_str())), 0.50f, 1.50f);
                rule.gamma      = ClampFloat(static_cast<float>(atof(parts[offset + 2].c_str())), 0.50f, 2.00f);
            };

            if (!parts.empty())
                rule.saturation = clampNeutral(parts[0]);

            if (parts.size() == 3) {
                rule.hue         = clampNeutral(parts[1]);
                rule.temperature = clampNeutral(parts[2]);
            } else if (parts.size() == 6) {
                rule.hue         = clampNeutral(parts[1]);
                rule.temperature = clampNeutral(parts[2]);
                readLevels(3);
            } else if (parts.size() == 4) {
                rule.hue         = kSaturationDefault;
                rule.temperature = kSaturationDefault;
                readLevels(1);
            }

            if (!rule.exe.empty()) loadedRules.push_back(rule);
            continue;
        }

        std::lock_guard lock(mutex_);
        if (key == "enabled")          enabled_ = (value != "0");
        else if (key == "saturation")   desktopSat_ = std::max(kSaturationMin,
                                                              std::min(atoi(value.c_str()), kSaturationMax));
        else if (key == "hue")          hue_ = std::max(kSaturationMin,
                                                       std::min(atoi(value.c_str()), kSaturationMax));
        else if (key == "temperature")  temperature_ = std::max(kSaturationMin,
                                                               std::min(atoi(value.c_str()), kSaturationMax));
        else if (key == "brightness")   brightness_ = ClampFloat(static_cast<float>(atof(value.c_str())), 0.50f, 1.50f);
        else if (key == "contrast")     contrast_   = ClampFloat(static_cast<float>(atof(value.c_str())), 0.50f, 1.50f);
        else if (key == "gamma")        gamma_      = ClampFloat(static_cast<float>(atof(value.c_str())), 0.50f, 2.00f);
        else if (key == "monitor")      selectedMonitor_ = std::max(-1, atoi(value.c_str()));
    }

    std::lock_guard lock(mutex_);
    rules_ = loadedRules;
    return true;
}

bool VibranceEngine::Save() const {
    const std::string path = SettingsPath();

    const auto slash = path.find_last_of("\\/");
    if (slash != std::string::npos)
        CreateDirectoryA(path.substr(0, slash).c_str(), nullptr);

    std::lock_guard lock(mutex_);

    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "; APEX color settings\n";
    out << "[main]\n";
    out << "enabled=" << (enabled_ ? 1 : 0) << "\n";
    out << "saturation=" << desktopSat_ << "\n";
    out << "brightness=" << brightness_ << "\n";
    out << "contrast=" << contrast_ << "\n";
    out << "gamma=" << gamma_ << "\n";
    out << "hue=" << hue_ << "\n";
    out << "temperature=" << temperature_ << "\n";
    out << "; -1 = all screens, 0 = screen 1, 1 = screen 2 ...\n";
    out << "monitor=" << selectedMonitor_ << "\n";
    out << "\n; one profile per application\n";
    out << ";   exe=saturation,hue,temperature\n";
    out << ";   exe=saturation,hue,temperature,brightness,contrast,gamma   (with levels)\n";
    out << "[apps]\n";
    for (const auto& rule : rules_) {
        out << rule.exe << '=' << rule.saturation
            << ',' << rule.hue
            << ',' << rule.temperature;
        if (rule.levels)
            out << ',' << rule.brightness << ',' << rule.contrast << ',' << rule.gamma;
        out << '\n';
    }

    if (!out.good()) return false;
    dirty_ = false;
    return true;
}

}
