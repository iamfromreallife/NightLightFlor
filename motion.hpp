#pragma once
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <vector>

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
inline constexpr unsigned kDisplayReorderMs=180;
// Stable insertion order: the captured display keeps its identity while the
// other display chips slide into their new positions.
inline std::vector<std::size_t> displayInsertionOrder(std::size_t count,std::size_t from,std::size_t to) {
    std::vector<std::size_t> order;order.reserve(count);
    for(std::size_t i=0;i<count;++i)order.push_back(i);
    if(from<count && to<count && from!=to){const auto item=order[from];order.erase(order.begin()+from);order.insert(order.begin()+to,item);}
    return order;
}
struct DisplayChipSlot {int x,y;};
inline std::vector<DisplayChipSlot> settingsDisplaySlots(const std::vector<int>& widths,const std::vector<std::size_t>& order) {
    std::vector<DisplayChipSlot> slots(widths.size());int x=26,y=86;
    for(const auto i:order){if(i>=widths.size())continue;const int w=std::clamp(widths[i],110,508);
        if(x+w>534){x=26;y+=38;}slots[i]={x,y};x+=w+6;}
    return slots;
}
inline constexpr unsigned kRefreshSpinMs=460;
inline void startRefreshSpin(Motion& motion,std::uint64_t now,bool animate) {
    // Rebase at a complete revolution when idle. Repeated clicks continue
    // from the visible angle, rather than snapping to zero.
    if(!motion.active(now))motion.snap(0);
    const double current=motion.value(now);
    motion.to(360.0*(std::ceil(current/360.0)+1.0),now,kRefreshSpinMs,animate);
}
}
