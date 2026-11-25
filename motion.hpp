#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>

namespace nightlight {
// UI-only interpolation. Hardware targets never pass through this animation.
class Motion {
public:
    Motion()=default;
    explicit Motion(double value):from_(value),target_(value),initialized_(true) {}
    double value(std::uint64_t now) const {
        if (!duration_ || now>=began_+duration_) return target_;
        const double t=std::clamp(double(now-began_)/duration_,0.0,1.0);
        const double remaining=1.0-t;
        const double eased=1.0-remaining*remaining*remaining;
        return from_+(target_-from_)*eased;
    }
    void to(double target,std::uint64_t now,unsigned duration,bool animate) {
        if (!animate) { snap(target); return; }
        if (initialized_ && target==target_) return;
        const double current=value(now);
        from_=initialized_ && animate?current:target;
        target_=target; began_=now; duration_=animate && initialized_?duration:0;
        initialized_=true;
    }
    void snap(double target) { from_=target_=target; duration_=0; initialized_=true; }
    void finish() { snap(target_); }
    bool active(std::uint64_t now) const { return duration_ && now<began_+duration_ && from_!=target_; }
private:
    double from_=0,target_=0;
    std::uint64_t began_=0;
    unsigned duration_=0;
    bool initialized_=false;
};
}
