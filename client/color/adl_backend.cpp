#include "adl_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objbase.h>

#include <algorithm>

namespace apex::color {

namespace {

void* AdlMemoryAlloc(int size) {
    if (size <= 0) return nullptr;
    return CoTaskMemAlloc(static_cast<SIZE_T>(size));
}

template <typename T>
T Resolve(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}

}

AdlBackend& AdlBackend::Get() {
    static AdlBackend instance;
    return instance;
}

bool AdlBackend::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_) return true;
    if (attempted_) return false;
    attempted_ = true;

    HMODULE module = LoadLibraryW(L"atiadlxx.dll");
    if (module == nullptr) {
        error_ = "atiadlxx.dll not found - AMD driver missing";
        return false;
    }
    module_ = module;

    create_        = Resolve<decltype(create_)>(module, "ADL_Main_Control_Create");
    destroy_       = Resolve<decltype(destroy_)>(module, "ADL_Main_Control_Destroy");
    adapterCount_  = Resolve<decltype(adapterCount_)>(module, "ADL_Adapter_NumberOfAdapters_Get");
    adapterInfo_   = Resolve<decltype(adapterInfo_)>(module, "ADL_Adapter_AdapterInfo_Get");
    adapterActive_ = Resolve<decltype(adapterActive_)>(module, "ADL_Adapter_Active_Get");
    displayInfo_   = Resolve<decltype(displayInfo_)>(module, "ADL_Display_DisplayInfo_Get");
    colorSet_      = Resolve<decltype(colorSet_)>(module, "ADL_Display_Color_Set");
    colorGet_      = Resolve<decltype(colorGet_)>(module, "ADL_Display_Color_Get");

    if (create_ == nullptr || adapterCount_ == nullptr || adapterInfo_ == nullptr ||
        displayInfo_ == nullptr || colorSet_ == nullptr || colorGet_ == nullptr) {
        error_ = "required ADL entry points are missing";
        return false;
    }

    if (create_(&AdlMemoryAlloc, 1) != kAdlSuccess) {
        error_ = "ADL_Main_Control_Create failed";
        return false;
    }

    int adapterTotal = 0;
    adapterCount_(&adapterTotal);
    adapterTotal = std::min(std::max(adapterTotal, 0), kAdlMaxAdapters);

    if (adapterTotal == 0) {
        error_ = "no AMD adapter reported by the driver";
        return false;
    }

    std::vector<AdlAdapterInfo> adapters(static_cast<size_t>(adapterTotal));
    for (auto& info : adapters)
        info.size = static_cast<int>(sizeof(AdlAdapterInfo));

    if (adapterInfo_(adapters.data(),
                     static_cast<int>(sizeof(AdlAdapterInfo) * adapters.size())) != kAdlSuccess) {
        error_ = "ADL_Adapter_AdapterInfo_Get failed";
        return false;
    }

    for (const auto& adapter : adapters) {
        int active = 0;
        if (adapterActive_ != nullptr &&
            adapterActive_(adapter.adapterIndex, &active) != kAdlSuccess)
            continue;

        int         displayCount = 0;
        void*       buffer       = nullptr;
        const int   status       = displayInfo_(adapter.adapterIndex, &displayCount, &buffer, 1);
        if (status != kAdlSuccess || buffer == nullptr || displayCount <= 0)
            continue;

        const auto* infos = static_cast<const AdlDisplayInfo*>(buffer);
        for (int i = 0; i < displayCount; ++i) {
            if (infos[i].displayID.logicalAdapterIndex == -1) continue;

            DisplayTarget target;
            target.adapterIndex = adapter.adapterIndex;
            target.displayIndex = infos[i].displayID.logicalIndex;
            target.name = infos[i].displayName[0] ? infos[i].displayName : "display";
            target.adapterName = adapter.displayName[0] ? adapter.displayName : "AMD adapter";

            int current = 0, fallback = 0, minimum = 0, maximum = 0, settable = 0;
            if (colorGet_ != nullptr &&
                colorGet_(target.adapterIndex, target.displayIndex, kColorSaturation,
                          &current, &fallback, &minimum, &maximum, &settable) ==
                    kAdlSuccess &&
                minimum <= maximum) {
                target.saturationDefault = fallback;
                target.saturationMin     = minimum;
                target.saturationMax     = maximum;
                target.saturationKnown   = true;
            }

            displays_.push_back(target);
        }

        CoTaskMemFree(buffer);
    }

    if (displays_.empty()) {
        error_ = "no usable AMD display found";
        return false;
    }

    ready_ = true;
    error_.clear();
    return true;
}

void AdlBackend::Shutdown() {

    RestoreSaturation();

    std::lock_guard<std::mutex> lock(mutex_);
    if (module_ != nullptr) {
        if (destroy_ != nullptr) destroy_();
        FreeLibrary(static_cast<HMODULE>(module_));
        module_ = nullptr;
    }
    ready_ = false;
    displays_.clear();
}

bool AdlBackend::Ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

std::vector<DisplayTarget> AdlBackend::Displays() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return displays_;
}

bool AdlBackend::GetColor(const DisplayTarget& target, int flag, ColorValue& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || colorGet_ == nullptr) return false;

    int current = 0, fallback = 0, minimum = 0, maximum = 0, settable = 0;
    if (colorGet_(target.adapterIndex, target.displayIndex, flag,
                  &current, &fallback, &minimum, &maximum, &settable) != kAdlSuccess)
        return false;

    out.current  = current;
    out.fallback = fallback;
    out.minimum  = minimum;
    out.maximum  = maximum;
    out.settable = settable != 0;
    return true;
}

bool AdlBackend::SetColor(const DisplayTarget& target, int flag, int value) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ready_ || colorSet_ == nullptr) return false;
    return colorSet_(target.adapterIndex, target.displayIndex, flag, value) == kAdlSuccess;
}

int AdlBackend::MapSaturation(const DisplayTarget& target, int appValue) {
    if (!target.saturationKnown)
        return target.saturationDefault;

    double value = static_cast<double>(appValue);
    if (value < kAmdValueMin) value = kAmdValueMin;
    if (value > kAmdValueMax) value = kAmdValueMax;

    const double neutral = static_cast<double>(target.saturationDefault);
    double       mapped  = neutral;

    if (value <= static_cast<double>(kAmdValueDefault)) {
        const double t = (static_cast<double>(kAmdValueDefault) - value) /
                         static_cast<double>(kAmdValueDefault);
        mapped = neutral - t * static_cast<double>(neutral - target.saturationMin);
    } else {
        const double t = (value - static_cast<double>(kAmdValueDefault)) /
                         static_cast<double>(kAmdValueMax - kAmdValueDefault);
        mapped = neutral + t * static_cast<double>(target.saturationMax - neutral);
    }

    const int rounded = static_cast<int>(mapped >= 0.0 ? mapped + 0.5 : mapped - 0.5);
    if (rounded < target.saturationMin) return target.saturationMin;
    if (rounded > target.saturationMax) return target.saturationMax;
    return rounded;
}

bool AdlBackend::SetSaturationOnAll(int appValue) const {
    const auto targets = Displays();
    bool any = false;
    for (const auto& target : targets) {
        if (!target.saturationKnown)
            continue;
        const int mapped = MapSaturation(target, appValue);
        any = SetColor(target, kColorSaturation, mapped) || any;
    }
    return any;
}

bool AdlBackend::RestoreSaturation() const {
    const auto targets = Displays();
    bool any = false;
    for (const auto& target : targets) {
        if (!target.saturationKnown)
            continue;
        any = SetColor(target, kColorSaturation, target.saturationDefault) || any;
    }
    return any;
}

std::string AdlBackend::BackendName() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_) return "AMD ADL (atiadlxx.dll)";
    return error_.empty() ? "not initialised" : error_;
}

}
