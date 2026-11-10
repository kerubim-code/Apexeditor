#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace apex::color {

struct GammaRamp {
    uint16_t red[256]   = {};
    uint16_t green[256] = {};
    uint16_t blue[256]  = {};
};

class GammaBackend {
public:
    static GammaBackend& Get();

    bool Init();
    void Shutdown();

    bool Ready() const { return ready_; }
    int  DisplayCount() const { return static_cast<int>(displays_.size()); }
    const std::string& Error() const { return error_; }

    bool Apply(float brightness, float contrast, float gamma);
    bool Reset();

    bool Matches(float brightness, float contrast, float gamma) const;

    bool WriteRamp(const GammaRamp& ramp);

    static void BuildRamp(float brightness, float contrast, float gamma, GammaRamp& out);

private:
    GammaBackend() = default;

    struct Entry {
        std::string device;
        std::string label;
        void*       dc = nullptr;
        GammaRamp   factory;
        bool        hasFactory = false;
    };

    std::vector<Entry> displays_;
    std::string        error_;
    bool               ready_ = false;
};

}
