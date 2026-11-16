#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace apex::color {

struct NvidiaDvcInfo {
    uint32_t version      = 0x10010;
    int32_t  currentLevel = 0;
    int32_t  minLevel     = 0;
    int32_t  maxLevel     = 63;
};

static_assert(sizeof(NvidiaDvcInfo) == 16, "NVAPI DVC info layout");

struct NvidiaDisplayTarget {
    void* handle        = nullptr;
    int   originalLevel = 0;
    int   minLevel      = 0;
    int   maxLevel      = 63;
    bool  known         = false;
};

class NvidiaBackend {
public:
    static NvidiaBackend& Get();

    bool Init();
    void Shutdown();
    bool Ready() const;

    std::string Error() const;
    int DisplayCount() const;

    static int MapSaturation(int appValue, int minLevel, int maxLevel);

    // onlyDisplay is an index into the DVC display list; -1 targets every
    // display.
    bool MatchesSaturation(int appValue, int onlyDisplay = -1) const;
    bool SetSaturationOnAll(int appValue, int onlyDisplay = -1) const;
    bool RestoreSaturation() const;

    std::string BackendName() const;

private:
    NvidiaBackend() = default;

    using QueryInterfaceFn = void* (__cdecl*)(uint32_t);
    using InitializeFn     = int (__cdecl*)();
    using UnloadFn         = int (__cdecl*)();
    using EnumDisplayFn    = int (__cdecl*)(uint32_t, void**);
    using GetDvcInfoFn     = int (__cdecl*)(void*, uint32_t, NvidiaDvcInfo*);
    using SetDvcLevelFn    = int (__cdecl*)(void*, uint32_t, int);
    using GetPhysicalGpusFn = int (__cdecl*)(void*, void**, uint32_t*);
    using GetSystemTypeFn  = int (__cdecl*)(void*, int*);

    mutable std::mutex mutex_;
    void*              module_      = nullptr;
    bool               ready_       = false;
    bool               attempted_   = false;
    bool               initialized_ = false;
    std::string        error_;

    std::vector<NvidiaDisplayTarget> displays_;

    QueryInterfaceFn  query_          = nullptr;
    InitializeFn      initialize_     = nullptr;
    UnloadFn          unload_         = nullptr;
    EnumDisplayFn     enumDisplay_    = nullptr;
    GetDvcInfoFn      getDvcInfo_     = nullptr;
    SetDvcLevelFn     setDvcLevel_    = nullptr;
    GetPhysicalGpusFn getPhysicalGpus_ = nullptr;
    GetSystemTypeFn   getSystemType_  = nullptr;
};

}
