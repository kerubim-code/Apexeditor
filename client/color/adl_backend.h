#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace apex::color {

constexpr int kAdlMaxPath     = 256;
constexpr int kAdlMaxAdapters = 40;
constexpr int kAdlSuccess     = 0;

enum AdlColorFlag : int {
    kColorBrightness  = 0x00000001,
    kColorContrast    = 0x00000002,
    kColorSaturation  = 0x00000004,
    kColorHue         = 0x00000008,
    kColorTemperature = 0x00000010,
};

constexpr int kAmdValueMin     = 0;
constexpr int kAmdValueMax     = 300;
constexpr int kAmdValueDefault = 100;

#pragma pack(push, 4)

struct AdlAdapterInfo {
    int  size;
    int  adapterIndex;
    char udid[kAdlMaxPath];
    int  busNumber;
    int  driverNumber;
    int  functionNumber;
    int  vendorID;
    char adapterName[kAdlMaxPath];
    char displayName[kAdlMaxPath];
    int  present;
    int  exist;
    char driverPath[kAdlMaxPath];
    char driverPathExt[kAdlMaxPath];
    char pnpString[kAdlMaxPath];
    int  oemID;
};

struct AdlDisplayID {
    int logicalIndex;
    int physicalIndex;
    int logicalAdapterIndex;
    int physicalAdapterIndex;
};

struct AdlDisplayInfo {
    AdlDisplayID displayID;
    int          displayControllerIndex;
    char         displayName[kAdlMaxPath];
    char         displayManufacturerName[kAdlMaxPath];
    int          displayType;
    int          displayOutputType;
    int          displayConnector;
    int          displayInfoMask;
    int          displayInfoValue;
};

#pragma pack(pop)

static_assert(sizeof(AdlAdapterInfo) == 1572, "ADL adapter info layout");
static_assert(sizeof(AdlDisplayInfo) == 552, "ADL display info layout");

struct DisplayTarget {
    int         adapterIndex = -1;
    int         displayIndex = -1;
    std::string name;
    std::string adapterName;

    int         saturationDefault = 100;
    int         saturationMin     = 0;
    int         saturationMax     = 200;
    bool        saturationKnown   = false;
};

struct ColorValue {
    int  current  = kAmdValueDefault;
    int  minimum  = kAmdValueMin;
    int  maximum  = kAmdValueMax;
    int  fallback = kAmdValueDefault;
    bool settable = false;
};

class AdlBackend {
public:
    static AdlBackend& Get();

    bool Init();
    void Shutdown();
    bool Ready() const;

    const std::string& Error() const { return error_; }

    std::vector<DisplayTarget> Displays() const;

    bool GetColor(const DisplayTarget& target, int flag, ColorValue& out) const;
    bool SetColor(const DisplayTarget& target, int flag, int value) const;

    static int MapSaturation(const DisplayTarget& target, int appValue);

    bool SetSaturationOnAll(int appValue) const;

    bool RestoreSaturation() const;

    std::string BackendName() const;

private:
    AdlBackend() = default;

    mutable std::mutex mutex_;
    void*              module_ = nullptr;
    bool               ready_ = false;
    bool               attempted_ = false;
    std::string        error_;

    std::vector<DisplayTarget> displays_;

    int (*create_)(void* (*)(int), int) = nullptr;
    int (*destroy_)() = nullptr;
    int (*adapterCount_)(int*) = nullptr;
    int (*adapterInfo_)(void*, int) = nullptr;
    int (*adapterActive_)(int, int*) = nullptr;
    int (*displayInfo_)(int, int*, void**, int) = nullptr;
    int (*colorSet_)(int, int, int, int) = nullptr;
    int (*colorGet_)(int, int, int, int*, int*, int*, int*, int*) = nullptr;
};

}
