#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <vector>
#include <optional>
#include <string>
#include <array>
namespace nightlight {
// Preserve natural chip widths unless all chips and the link switch can fit
// on one row by shortening monitor labels. Never shrink the link's label or
// the existing minimum monitor width; narrow windows retain normal wrapping.
inline void fitLinkedDisplayChips(std::vector<int>& widths,int available,int minimum,int gap) {
    if(widths.size()<3)return;
    const auto monitors=static_cast<std::int64_t>(widths.size()-1);
    const auto room=static_cast<std::int64_t>(available)-widths.back()-static_cast<std::int64_t>(gap)*monitors;
    if(room<static_cast<std::int64_t>(minimum)*monitors)return;
    std::int64_t total=0;for(std::size_t i=0;i<static_cast<std::size_t>(monitors);++i)total+=widths[i];
    if(total<=room)return;
    const auto spare=total-static_cast<std::int64_t>(minimum)*monitors;
    const auto needed=room-static_cast<std::int64_t>(minimum)*monitors;
    for(std::size_t i=0;i<static_cast<std::size_t>(monitors);++i){
        const auto extra=widths[i]-minimum;
        widths[i]=minimum+static_cast<int>(extra*needed/spare);
    }
}
// Settings uses a 508-DIP content row, with an 8-DIP gap. Keep the
// usual dropdown width when possible, but give longer translations room.
inline constexpr int trayWheelDropdownWidth(int checkboxNeeded,int choiceNeeded) {
    return std::clamp(500-checkboxNeeded,std::clamp(choiceNeeded,120,254),254);
}
// Preserve any user-selected extra space above the minimum when changing
// the number of visible hardware rows. Anchor the outer frame at its bottom.
inline constexpr int linkedWindowHeight(int oldHeight,int oldMinimum,int newMinimum) {
    return std::max(newMinimum,oldHeight+newMinimum-oldMinimum);
}
inline constexpr int bottomAnchoredWindowTop(int bottom,int height) {
    return bottom-height;
}
inline constexpr int bottomAnchoredWindowTop(int bottom,int height,int workTop,int workBottom) {
    return std::clamp(bottomAnchoredWindowTop(bottom,height),workTop,
                      std::max(workTop,workBottom-height));
}
inline int accumulateTrayWheel(int& remainder,int delta) {
    const auto total=static_cast<std::int64_t>(remainder)+delta;
    remainder=static_cast<int>(total%120);
    return static_cast<int>(total/120);
}
inline int steppedDisplayValue(int current,int steps,int up,int down,int minimum,int maximum) {
    const auto target=static_cast<std::int64_t>(current)+static_cast<std::int64_t>(steps)*(steps>0?up:down);
    return static_cast<int>(std::clamp(target,static_cast<std::int64_t>(minimum),static_cast<std::int64_t>(maximum)));
}
// Wake recovery may continue long after the first stable topology sample.
// A known capability can be used without waiting for all verification passes.
inline bool hardwareInputReady(bool awake,bool recovering,unsigned stablePasses,bool guarded) {
    return awake && !guarded && (!recovering || stablePasses>0);
}
inline int hardwareWheelPosition(int position,int steps) {
    return steppedDisplayValue(position,steps,100,100,0,10000);
}
struct TrayDisplayValues {
    std::wstring name;
    std::array<std::optional<int>,3> values;
};
inline std::wstring formatTrayValue(std::optional<int> value,const wchar_t* unit) {
    return value?std::to_wstring(*value)+unit:L"—";
}
// Prefer exact per-display values if they differ; the Shell's standard
// NOTIFYICONDATA tip has 127 usable UTF-16 code units.
inline std::wstring displayTrayTooltip(const std::vector<TrayDisplayValues>& displays,
        const std::array<std::wstring,3>& labels,std::size_t limit=127) {
    const std::wstring title=L"NightLight";
    if(displays.empty())return title;
    const wchar_t* units[]{L" K",L"%",L"%"};
    bool identical=true;
    for(const auto& d:displays)if(d.values!=displays.front().values)identical=false;
    if(!identical) {
        std::wstring per=title+L" (K / B% / C%)";
        for(const auto& d:displays) {
            auto name=d.name;
            for(auto& c:name)if(c==L'\r'||c==L'\n'||c==L'\t')c=L' ';
            per+=L"\n"+name+L": ";
            for(std::size_t i=0;i<3;++i){if(i)per+=L" / ";per+=formatTrayValue(d.values[i],units[i]);}
        }
        if(per.size()<=limit)return per;
    }
    std::array<std::wstring,3> values;
    for(std::size_t i=0;i<3;++i){
        std::optional<int> lo,hi;bool missing=false;
        for(const auto& d:displays){if(!d.values[i]){missing=true;continue;}const int v=*d.values[i];lo=lo?std::min(*lo,v):v;hi=hi?std::max(*hi,v):v;}
        values[i]=lo?std::to_wstring(*lo):L"—";
        if(lo){if(*lo!=*hi)values[i]+=L"–"+std::to_wstring(*hi);values[i]+=units[i];if(missing)values[i]+=L" / —";}
    }
    std::wstring result=title;
    for(std::size_t i=0;i<3;++i)result+=L"\n"+labels[i]+L": "+values[i];
    if(result.size()<=limit)return result;
    result=title+L"\nT: "+values[0]+L"\nB: "+values[1]+L"\nC: "+values[2];
    return result.size()<=limit?result:title;
}

}
