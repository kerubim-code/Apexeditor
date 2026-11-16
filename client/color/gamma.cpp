#include "gamma.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cmath>
#include <cstring>

namespace apex::color {

namespace {

constexpr double kEpsilon = 1e-6;

std::string Narrow(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return std::string();
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

double Clamp01(double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
}

uint16_t ToWord(double value) {
    const double scaled = Clamp01(value) * 65535.0;
    const long   rounded = static_cast<long>(scaled + 0.5);
    return static_cast<uint16_t>(rounded < 0 ? 0 : (rounded > 65535 ? 65535 : rounded));
}

}

GammaBackend& GammaBackend::Get() {
    static GammaBackend instance;
    return instance;
}

bool GammaBackend::Init() {
    if (ready_) return true;

    Shutdown();

    for (DWORD index = 0; index < 16; ++index) {
        DISPLAY_DEVICEW device = {};
        device.cb = sizeof(device);
        if (!EnumDisplayDevicesW(nullptr, index, &device, 0))
            continue;
        if ((device.StateFlags & DISPLAY_DEVICE_ACTIVE) == 0)
            continue;

        HDC dc = CreateDCW(device.DeviceName, nullptr, nullptr, nullptr);
        if (dc == nullptr) continue;

        Entry entry;
        entry.device = Narrow(device.DeviceName);
        entry.label  = Narrow(device.DeviceString);
        entry.dc     = dc;

        GammaRamp ramp;
        if (GetDeviceGammaRamp(dc, &ramp)) {
            entry.factory    = ramp;
            entry.hasFactory = true;
        }

        displays_.push_back(entry);
    }

    if (displays_.empty()) {
        error_ = "no display accepts a gamma ramp";
        return false;
    }

    ready_ = true;
    error_.clear();
    return true;
}

void GammaBackend::Shutdown() {

    if (ready_)
        Reset();

    for (auto& entry : displays_) {
        if (entry.dc != nullptr) {
            DeleteDC(static_cast<HDC>(entry.dc));
            entry.dc = nullptr;
        }
    }
    displays_.clear();
    ready_ = false;
}

bool GammaBackend::Apply(float brightness, float contrast, float gamma,
                         const std::string& device) {
    if (!ready_) return false;

    if (std::fabs(brightness - 1.0f) < kEpsilon &&
        std::fabs(contrast - 1.0f) < kEpsilon &&
        std::fabs(gamma - 1.0f) < kEpsilon)
        return Reset(device);

    GammaRamp ramp;
    BuildRamp(brightness, contrast, gamma, ramp);
    return WriteRamp(ramp, device);
}

void GammaBackend::BuildRamp(float brightness, float contrast, float gamma, GammaRamp& out) {
    const double b = brightness <= 0.0f ? 0.05 : static_cast<double>(brightness);
    const double c = contrast   <= 0.0f ? 0.05 : static_cast<double>(contrast);
    const double g = gamma      <= 0.0f ? 0.05 : static_cast<double>(gamma);
    const double invGamma = 1.0 / g;

    GammaRamp ramp;
    for (int i = 0; i < 256; ++i) {
        double v = static_cast<double>(i) / 255.0;
        v *= b;
        v = (v - 0.5) * c + 0.5;
        v = Clamp01(v);
        v = std::pow(v, invGamma);

        const uint16_t word = ToWord(v);
        ramp.red[i]   = word;
        ramp.green[i] = word;
        ramp.blue[i]  = word;
    }
    out = ramp;
}

bool GammaBackend::Matches(float brightness, float contrast, float gamma,
                           const std::string& device) const {
    if (!ready_) return false;

    const bool neutral = std::fabs(brightness - 1.0f) < kEpsilon &&
                         std::fabs(contrast - 1.0f) < kEpsilon &&
                         std::fabs(gamma - 1.0f) < kEpsilon;

    const Entry* only = device.empty() ? nullptr : FindEntry(device);
    if (!device.empty() && only == nullptr)
        return true;

    GammaRamp target;
    if (!neutral)
        BuildRamp(brightness, contrast, gamma, target);

    for (const auto& entry : displays_) {
        if (only != nullptr && &entry != only)
            continue;
        if (entry.dc == nullptr)
            continue;

        GammaRamp current;
        if (!GetDeviceGammaRamp(static_cast<HDC>(entry.dc), &current))
            return false;

        const GammaRamp& expected = neutral ? entry.factory : target;
        if (neutral && !entry.hasFactory)
            continue;
        if (memcmp(&current, &expected, sizeof(GammaRamp)) != 0)
            return false;
    }
    return true;
}

bool GammaBackend::Reset(const std::string& device) {
    if (!ready_) return false;

    Entry* only = device.empty() ? nullptr : FindEntry(device);
    if (!device.empty() && only == nullptr)
        return true;

    bool ok = true;
    for (auto& entry : displays_) {
        if (only != nullptr && &entry != only)
            continue;
        if (!entry.hasFactory) continue;
        if (!SetDeviceGammaRamp(static_cast<HDC>(entry.dc), &entry.factory))
            ok = false;
    }
    return ok;
}

bool GammaBackend::WriteRamp(const GammaRamp& ramp, const std::string& device) {
    if (!ready_) return false;

    Entry* only = device.empty() ? nullptr : FindEntry(device);
    if (!device.empty() && only == nullptr)
        return false;

    void* buffer = const_cast<GammaRamp*>(&ramp);

    bool ok = false;
    for (auto& entry : displays_) {
        if (only != nullptr && &entry != only)
            continue;
        if (SetDeviceGammaRamp(static_cast<HDC>(entry.dc), buffer))
            ok = true;
    }
    return ok;
}

std::string GammaBackend::DeviceName(int index) const {
    if (index < 0 || index >= static_cast<int>(displays_.size()))
        return std::string();
    return displays_[index].device;
}

GammaBackend::Entry* GammaBackend::FindEntry(const std::string& device) {
    for (auto& entry : displays_)
        if (entry.device == device)
            return &entry;
    return nullptr;
}

const GammaBackend::Entry* GammaBackend::FindEntry(const std::string& device) const {
    for (const auto& entry : displays_)
        if (entry.device == device)
            return &entry;
    return nullptr;
}

}
