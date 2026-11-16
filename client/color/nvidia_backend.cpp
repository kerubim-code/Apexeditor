#include "nvidia_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <cstdio>

namespace apex::color {

namespace {

// Interface IDs used by the NVIDIA driver's nvapi64.dll. The DVC calls are
// private NVAPI entry points, matching the ones used by VibranceGUI.
constexpr uint32_t kIdInitialize        = 0x0150E828;
constexpr uint32_t kIdUnload            = 0xD22BDD7E;
constexpr uint32_t kIdEnumDisplay       = 0x9ABDD40D;
constexpr uint32_t kIdGetPhysicalGpus   = 0x34EF9506;
constexpr uint32_t kIdGetSystemType     = 0xBAAABFCC;
constexpr uint32_t kIdGetSystemTypeOld  = 0x593E8644;
constexpr uint32_t kIdGetDvcInfo        = 0x4085DE45;
constexpr uint32_t kIdSetDvcLevel       = 0x172409B4;

constexpr int kNvSystemTypeLaptop = 1;

template <typename T, typename Query>
T Resolve(Query query, uint32_t id) {
    return reinterpret_cast<T>(query(id));
}

std::string StatusText(int status) {
    char text[48];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(status));
    return text;
}

}

NvidiaBackend& NvidiaBackend::Get() {
    static NvidiaBackend instance;
    return instance;
}

bool NvidiaBackend::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_) return true;
    if (attempted_) return false;
    attempted_ = true;

    HMODULE module = LoadLibraryW(L"nvapi64.dll");
    if (module == nullptr)
        module = LoadLibraryW(L"nvapi.dll");
    if (module == nullptr) {
        error_ = "nvapi64.dll not found - NVIDIA driver missing";
        return false;
    }
    module_ = module;

    query_ = reinterpret_cast<QueryInterfaceFn>(
        GetProcAddress(module, "nvapi_QueryInterface"));
    if (query_ == nullptr) {
        error_ = "nvapi_QueryInterface missing - unsupported NVIDIA driver";
        return false;
    }

    initialize_      = Resolve<InitializeFn>(query_, kIdInitialize);
    unload_          = Resolve<UnloadFn>(query_, kIdUnload);
    enumDisplay_     = Resolve<EnumDisplayFn>(query_, kIdEnumDisplay);
    getPhysicalGpus_ = Resolve<GetPhysicalGpusFn>(query_, kIdGetPhysicalGpus);
    getDvcInfo_      = Resolve<GetDvcInfoFn>(query_, kIdGetDvcInfo);
    setDvcLevel_     = Resolve<SetDvcLevelFn>(query_, kIdSetDvcLevel);
    getSystemType_   = Resolve<GetSystemTypeFn>(query_, kIdGetSystemType);
    if (getSystemType_ == nullptr)
        getSystemType_ = Resolve<GetSystemTypeFn>(query_, kIdGetSystemTypeOld);

    if (initialize_ == nullptr || enumDisplay_ == nullptr ||
        getDvcInfo_ == nullptr || setDvcLevel_ == nullptr) {
        error_ = "required NVIDIA DVC entry points are missing";
        return false;
    }

    const int status = initialize_();
    if (status != 0) {
        error_ = "NvAPI_Initialize failed (" + StatusText(status) + ")";
        return false;
    }
    initialized_ = true;

    int laptopDisplays = 0;

    for (uint32_t index = 0; index < 64; ++index) {
        void* handle = nullptr;
        if (enumDisplay_(index, &handle) != 0) break;
        if (handle == nullptr) continue;

        bool laptop = false;
        if (getPhysicalGpus_ != nullptr && getSystemType_ != nullptr) {
            void*    gpus[64] = {};
            uint32_t count    = 0;
            if (getPhysicalGpus_(handle, gpus, &count) == 0) {
                for (uint32_t gpu = 0; gpu < count && gpu < 64; ++gpu) {
                    int systemType = 0;
                    if (getSystemType_(gpus[gpu], &systemType) == 0 &&
                        systemType == kNvSystemTypeLaptop) {
                        laptop = true;
                        break;
                    }
                }
            }
        }
        if (laptop) {
            ++laptopDisplays;
            continue;
        }

        NvidiaDvcInfo info;
        if (getDvcInfo_(handle, 0, &info) != 0) continue;
        if (info.maxLevel < info.minLevel) continue;

        NvidiaDisplayTarget target;
        target.handle        = handle;
        target.originalLevel = std::max(info.minLevel,
                                        std::min(info.currentLevel, info.maxLevel));
        target.minLevel      = info.minLevel;
        target.maxLevel      = info.maxLevel;
        target.known         = true;
        displays_.push_back(target);
    }

    if (displays_.empty()) {
        error_ = laptopDisplays > 0
                     ? "NVIDIA DVC is not supported on laptop GPUs"
                     : "no NVIDIA display with DVC found";

        if (unload_ != nullptr)
            unload_();
        initialized_ = false;

        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
        return false;
    }

    ready_ = true;
    error_.clear();
    return true;
}

void NvidiaBackend::Shutdown() {
    RestoreSaturation();

    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_ && unload_ != nullptr)
        unload_();
    initialized_ = false;

    if (module_ != nullptr) {
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }

    ready_ = false;
    attempted_ = false;
    displays_.clear();
}

bool NvidiaBackend::Ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

std::string NvidiaBackend::Error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

int NvidiaBackend::DisplayCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(displays_.size());
}

int NvidiaBackend::MapSaturation(int appValue, int minLevel, int maxLevel) {
    if (maxLevel < minLevel)
        std::swap(minLevel, maxLevel);

    const int neutral = (minLevel <= 0 && maxLevel >= 0) ? 0 : minLevel;
    if (maxLevel <= neutral)
        return neutral;

    if (appValue < 0) appValue = 0;
    if (appValue > 300) appValue = 300;

    // NVIDIA DVC cannot desaturate below its neutral level, so the lower half
    // of the app slider maps to neutral and 100..300 maps across the DVC range.
    if (appValue <= 100)
        return neutral;

    double t = static_cast<double>(appValue - 100) / 200.0;
    if (t > 1.0) t = 1.0;

    const double scaled = static_cast<double>(neutral) +
                          t * static_cast<double>(maxLevel - neutral);
    int level = static_cast<int>(scaled + 0.5);
    if (level < minLevel) level = minLevel;
    if (level > maxLevel) level = maxLevel;
    return level;
}

bool NvidiaBackend::MatchesSaturation(int appValue, int onlyDisplay) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || getDvcInfo_ == nullptr) return false;
    if (onlyDisplay >= static_cast<int>(displays_.size())) return true;

    bool checked = false;
    for (size_t i = 0; i < displays_.size(); ++i) {
        if (onlyDisplay >= 0 && static_cast<int>(i) != onlyDisplay)
            continue;
        const auto& target = displays_[i];
        if (!target.known) continue;

        NvidiaDvcInfo info;
        if (getDvcInfo_(target.handle, 0, &info) != 0) return false;

        checked = true;
        const int expected = MapSaturation(appValue, info.minLevel, info.maxLevel);
        if (info.currentLevel != expected)
            return false;
    }

    return checked;
}

bool NvidiaBackend::SetSaturationOnAll(int appValue, int onlyDisplay) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || setDvcLevel_ == nullptr) return false;
    if (onlyDisplay >= static_cast<int>(displays_.size())) return false;

    bool any = false;
    for (size_t i = 0; i < displays_.size(); ++i) {
        if (onlyDisplay >= 0 && static_cast<int>(i) != onlyDisplay)
            continue;
        const auto& target = displays_[i];
        if (!target.known) continue;
        const int level = MapSaturation(appValue, target.minLevel, target.maxLevel);
        any = setDvcLevel_(target.handle, 0, level) == 0 || any;
    }
    return any;
}

bool NvidiaBackend::RestoreSaturation() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || setDvcLevel_ == nullptr) return false;

    bool any = false;
    for (const auto& target : displays_) {
        if (!target.known) continue;
        any = setDvcLevel_(target.handle, 0, target.originalLevel) == 0 || any;
    }
    return any;
}

std::string NvidiaBackend::BackendName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_) return "NVIDIA DVC (nvapi64.dll)";
    return error_.empty() ? "not initialised" : error_;
}

}
