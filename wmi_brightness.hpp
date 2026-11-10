#pragma once
#include <memory>
#include <string>

namespace nightlight {
std::wstring normalizeMonitorInstance(std::wstring instance);
struct WmiBrightnessPanel {
    std::wstring path;
    std::wstring instance;
    bool readable = false;
    int percent = 50;
    explicit operator bool() const { return !path.empty(); }
};
// Native equivalent of upstream's WmiMonitorBrightnessMethods fallback.
// Instance matching keeps a brightness write on the selected logical display.
class WmiBrightness {
public:
    WmiBrightness();
    ~WmiBrightness();
    WmiBrightnessPanel find(const std::wstring& device);
    bool set(const WmiBrightnessPanel& panel,int percent);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
