#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace apex::color {

struct AppRule {
    std::string exe;
    int         saturation = 150;

    bool  levels    = false;
    float brightness = 1.0f;
    float contrast   = 1.0f;
    float gamma      = 1.0f;
};

bool SameApp(const std::string& a, const std::string& b);

class VibranceEngine {
public:
    static VibranceEngine& Get();

    bool Start();
    void Stop();

    bool AdlReady() const;
    bool GammaReady() const;
    bool Running() const;
    std::string BackendInfo() const;

    bool   Enabled() const;
    void   SetEnabled(bool enabled);

    int    DesktopSaturation() const;
    void   SetDesktopSaturation(int value);

    float  Brightness() const;
    void   SetBrightness(float value);

    float  Contrast() const;
    void   SetContrast(float value);

    float  Gamma() const;
    void   SetGamma(float value);

    std::vector<AppRule> Rules() const;
    void   SetRules(const std::vector<AppRule>& rules);

    bool   ActiveRule(AppRule& out) const;

    int    TargetSaturation() const;

    std::string ForegroundApp() const;
    std::string StatusLine() const;
    int         LastAppliedSaturation() const;

    bool        Load();
    bool        Save() const;
    std::string SettingsPath() const;

    void ApplyNow(bool force = false);

    static constexpr int kSaturationMin = 0;
    static constexpr int kSaturationMax = 300;
    static constexpr int kSaturationDefault = 100;

private:
    VibranceEngine() = default;

    void   WatchLoop();
    int    TargetSaturationLocked() const;
    const AppRule* RuleInFrontLocked() const;
    void   ApplyLocked(bool force);
    void   ApplySaturation(int value, bool force);

    void   MarkDirtyLocked();
    void   FlushDirty() const;

    mutable std::recursive_mutex mutex_;
    std::thread        watcher_;
    std::atomic<bool>  running_{false};

    bool   enabled_         = true;
    int    desktopSat_      = kSaturationDefault;
    float  brightness_      = 1.0f;
    float  contrast_        = 1.0f;
    float  gamma_           = 1.0f;
    std::vector<AppRule> rules_;

    std::string foreground_;
    int         appliedSat_ = kSaturationDefault;

    int         satCache_      = kSaturationDefault;
    bool        satCacheValid_ = false;

    mutable bool     dirty_     = false;
    mutable uint64_t dirtyTick_ = 0;

    std::atomic<bool> pending_{false};
    std::atomic<bool> pendingForce_{false};
};

}
