#include "vibrance.h"

#include "adl_backend.h"
#include "gamma.h"

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
    GammaBackend::Get().Init();

    {
        std::lock_guard lock(mutex_);
        satCacheValid_ = false;
    }

    ApplyNow(true);

    running_ = true;
    watcher_ = std::thread(&VibranceEngine::WatchLoop, this);

    return AdlReady() || GammaReady();
}

void VibranceEngine::Stop() {
    const bool wasRunning = running_.exchange(false);
    if (watcher_.joinable())
        watcher_.join();
    if (wasRunning)
        Save();

    AdlBackend::Get().RestoreSaturation();
    GammaBackend::Get().Reset();

    {
        std::lock_guard lock(mutex_);
        satCacheValid_ = false;
        dirty_         = false;
    }

    AdlBackend::Get().Shutdown();
    GammaBackend::Get().Shutdown();
}

bool VibranceEngine::AdlReady() const {
    return AdlBackend::Get().Ready();
}

bool VibranceEngine::GammaReady() const {
    return GammaBackend::Get().Ready();
}

bool VibranceEngine::Running() const {
    return running_.load();
}

std::string VibranceEngine::BackendInfo() const {
    std::string info;
    if (AdlReady())
        info += AdlBackend::Get().BackendName() + " (" +
                std::to_string(AdlBackend::Get().Displays().size()) + " display(s))";
    else
        info += "AMD ADL: " + AdlBackend::Get().Error();

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
    auto& adl = AdlBackend::Get();
    if (!adl.Ready()) return;

    const auto displays = adl.Displays();
    if (displays.empty()) return;

    if (!force && satCacheValid_ && value == satCache_) return;

    bool needsWrite = false;
    for (const auto& target : displays) {
        ColorValue current;
        if (!adl.GetColor(target, kColorSaturation, current) ||
            current.current != AdlBackend::MapSaturation(target, value)) {
            needsWrite = true;
            break;
        }
    }
    if (needsWrite)
        adl.SetSaturationOnAll(value);

    satCache_      = value;
    satCacheValid_ = true;
}

void VibranceEngine::ApplyLocked(bool force) {
    const int target = TargetSaturationLocked();
    ApplySaturation(target, force);
    appliedSat_ = target;

    auto& gamma = GammaBackend::Get();
    if (gamma.Ready()) {
        float b = 1.0f, c = 1.0f, g = 1.0f;
        if (enabled_) {
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
        }
        if (!gamma.Matches(b, c, g))
            gamma.Apply(b, c, g);
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
            const auto comma = value.find(',');
            rule.saturation = std::max(
                kSaturationMin,
                std::min(atoi(value.substr(0, comma).c_str()), kSaturationMax));
            if (comma != std::string::npos) {
                std::istringstream rest(value.substr(comma + 1));
                std::string b, c, g;
                if (std::getline(rest, b, ',') && std::getline(rest, c, ',') &&
                    std::getline(rest, g, ',')) {
                    rule.levels     = true;
                    rule.brightness = ClampFloat(static_cast<float>(atof(b.c_str())), 0.50f, 1.50f);
                    rule.contrast   = ClampFloat(static_cast<float>(atof(c.c_str())), 0.50f, 1.50f);
                    rule.gamma      = ClampFloat(static_cast<float>(atof(g.c_str())), 0.50f, 2.00f);
                }
            }
            if (!rule.exe.empty()) loadedRules.push_back(rule);
            continue;
        }

        std::lock_guard lock(mutex_);
        if (key == "enabled")          enabled_ = (value != "0");
        else if (key == "saturation")   desktopSat_ = std::max(kSaturationMin,
                                                              std::min(atoi(value.c_str()), kSaturationMax));
        else if (key == "brightness")   brightness_ = ClampFloat(static_cast<float>(atof(value.c_str())), 0.50f, 1.50f);
        else if (key == "contrast")     contrast_   = ClampFloat(static_cast<float>(atof(value.c_str())), 0.50f, 1.50f);
        else if (key == "gamma")        gamma_      = ClampFloat(static_cast<float>(atof(value.c_str())), 0.50f, 2.00f);
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
    out << "\n; one profile per application\n";
    out << ";   exe=saturation\n";
    out << ";   exe=saturation,brightness,contrast,gamma   (per application levels)\n";
    out << "[apps]\n";
    for (const auto& rule : rules_) {
        out << rule.exe << '=' << rule.saturation;
        if (rule.levels)
            out << ',' << rule.brightness << ',' << rule.contrast << ',' << rule.gamma;
        out << '\n';
    }

    if (!out.good()) return false;
    dirty_ = false;
    return true;
}

}
