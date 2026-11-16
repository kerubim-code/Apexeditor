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

    bool Apply(float brightness, float contrast, float gamma,
               const std::string& device = std::string());
    bool Reset(const std::string& device = std::string());

    bool Matches(float brightness, float contrast, float gamma,
                 const std::string& device = std::string()) const;

    bool WriteRamp(const GammaRamp& ramp,
                   const std::string& device = std::string());

    std::string DeviceName(int index) const;

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

    Entry*       FindEntry(const std::string& device);
    const Entry* FindEntry(const std::string& device) const;

    std::vector<Entry> displays_;
    std::string        error_;
    bool               ready_ = false;
};

}
