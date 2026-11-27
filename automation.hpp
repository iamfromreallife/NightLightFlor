#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include <optional>
#include <cwctype>
#include <cmath>
namespace nightlight {
enum class ScheduleMode { Automatic=0, Custom=1, Instant=2 };
enum class SchedulePointKind { Custom=0, Sunrise=1, Noon=2, Sunset=3 };
struct SchedulePoint {
    int minute=420,kelvin=6500;
    SchedulePointKind kind=SchedulePointKind::Custom;
    bool customTime=false;
};
inline constexpr std::size_t kMaxSchedulePoints=24;
struct ColorSchedule {
    bool enabled=false;
    std::vector<SchedulePoint> points{{420,6500,SchedulePointKind::Sunrise,false},{720,5000,SchedulePointKind::Noon,false},{1140,3500,SchedulePointKind::Sunset,false}};
    ScheduleMode mode=ScheduleMode::Automatic;
    int transitionMinutes=30;
    std::wstring locationName=L"";
    double latitude=0.0,longitude=0.0;
};
inline std::optional<int> parseScheduleTime(std::wstring text){
    auto trim=[](std::wstring& value){while(!value.empty()&&std::iswspace(value.front()))value.erase(value.begin());while(!value.empty()&&std::iswspace(value.back()))value.pop_back();};
    trim(text);for(auto& c:text)c=static_cast<wchar_t>(std::towlower(c));
    bool am=false,pm=false;
    if(text.size()>=2){const auto suffix=text.substr(text.size()-2);am=suffix==L"am";pm=suffix==L"pm";if(am||pm){text.resize(text.size()-2);trim(text);}}
    const auto sep=text.find_first_of(L":h.,");
    auto digits=[](const std::wstring& value)->std::optional<int>{if(value.empty()||value.size()>2)return {};int n=0;for(auto c:value){if(c<L'0'||c>L'9')return {};n=n*10+c-L'0';}return n;};
    const auto hour=digits(text.substr(0,sep));if(!hour)return {};
    int h=*hour,m=0;
    if(sep!=std::wstring::npos){const auto tail=text.substr(sep+1);if(tail.empty()){if(text[sep]!=L'h')return {};}else{const auto minute=digits(tail);if(!minute)return {};m=*minute;}}
    if(am||pm){if(h<1||h>12)return {};h=h%12+(pm?12:0);}
    if(h>23||m>59)return {};
    return h*60+m;
}
inline bool uniqueScheduleTimes(const std::vector<SchedulePoint>& points) {
    if(points.size()<2 || points.size()>kMaxSchedulePoints)return false;
    std::array<bool,1440> seen{};
    for(const auto& p:points){if(p.minute<0 || p.minute>=1440 || seen[p.minute])return false;seen[p.minute]=true;}
    return true;
}
inline ColorSchedule normalizedSchedule(ColorSchedule s) {
    if(s.points.size()>kMaxSchedulePoints)s.points.resize(kMaxSchedulePoints);
    if(s.locationName.size()>512||!std::isfinite(s.latitude)||!std::isfinite(s.longitude)||std::abs(s.latitude)>90||std::abs(s.longitude)>180){s.locationName.clear();s.latitude=s.longitude=0;}
    for(auto& p:s.points){p.minute=std::clamp(p.minute,0,1439);p.kelvin=std::clamp(p.kelvin,1000,6500);if(static_cast<int>(p.kind)<0||static_cast<int>(p.kind)>3)p.kind=SchedulePointKind::Custom;}
    std::stable_sort(s.points.begin(),s.points.end(),[](auto a,auto b){return a.minute<b.minute;});
    s.points.erase(std::unique(s.points.begin(),s.points.end(),[](auto a,auto b){return a.minute==b.minute;}),s.points.end());
    if(s.points.empty())s.points={{420,6500,SchedulePointKind::Sunrise,false},{720,5000,SchedulePointKind::Noon,false},{1140,3500,SchedulePointKind::Sunset,false}};
    if(s.points.size()==1){auto p=s.points.front();s.points.push_back({(p.minute+720)%1440,p.kelvin,SchedulePointKind::Custom,true});
        std::sort(s.points.begin(),s.points.end(),[](auto a,auto b){return a.minute<b.minute;});}
    if(s.mode!=ScheduleMode::Automatic && s.mode!=ScheduleMode::Custom && s.mode!=ScheduleMode::Instant)s.mode=ScheduleMode::Automatic;
    s.transitionMinutes=std::clamp(s.transitionMinutes,1,120);
    return s;
}
// Effective daily schedule only: update all non-overridden solar events.
// A custom point wins if today's solar time coincides with it; the original
// configured points are kept intact so the solar event returns tomorrow.
inline ColorSchedule effectiveSolarPoints(ColorSchedule s,const std::array<std::optional<int>,3>& times){
    for(auto& p:s.points){const int i=static_cast<int>(p.kind)-1;
        if(!p.customTime && i>=0 && i<3 && times[i])p.minute=std::clamp(*times[i],0,1439);}
    std::stable_sort(s.points.begin(),s.points.end(),[](const auto& a,const auto& b){
        if(a.minute!=b.minute)return a.minute<b.minute;
        const bool ac=a.customTime||a.kind==SchedulePointKind::Custom,bc=b.customTime||b.kind==SchedulePointKind::Custom;
        return ac&&!bc;
    });
    return normalizedSchedule(std::move(s));
}
struct ScheduleTarget {int kelvin;bool transitioning;};
inline ScheduleTarget scheduledColor(const ColorSchedule& s,int secondOfDay) {
    // All callers use normalized, sorted points. Invalid input is defensive.
    if(s.points.size()<2)return {s.points.empty()?6500:s.points.front().kelvin,false};
    constexpr int day=86400;
    const int now=((secondOfDay%day)+day)%day;
    auto upper=std::upper_bound(s.points.begin(),s.points.end(),now,[](int second,const SchedulePoint& p){return second<p.minute*60;});
    const auto& next=upper==s.points.end()?s.points.front():*upper;
    const auto& prev=upper==s.points.begin()?s.points.back():*(upper-1);
    const int span=(next.minute*60-prev.minute*60+day)%day;
    const int elapsed=(now-prev.minute*60+day)%day;
    if(!span || s.mode==ScheduleMode::Instant || prev.kelvin==next.kelvin)return {prev.kelvin,false};
    // Automatic covers the entire interval, custom approaches the next point
    // over the specified minutes (bounded by the gap), instant jumps at it.
    const int duration=s.mode==ScheduleMode::Automatic?span:std::min(span,s.transitionMinutes*60);
    const int progress=elapsed-(span-duration);
    if(progress<0)return {prev.kelvin,false};
    const auto delta=static_cast<long long>(next.kelvin-prev.kelvin)*progress;
    const auto rounded=(delta>=0?delta+duration/2:delta-duration/2)/duration;
    return {prev.kelvin+static_cast<int>(rounded),true};
}
// Native HOTKEY-compatible encoding (virtual key low byte, modifiers high byte).
using HotkeyBindings=std::map<std::wstring,std::array<int,4>>;
inline constexpr std::array<int,4> kDefaultHotkeySteps{5,5,100,100};
inline bool validHotkeyStep(int action,int step){return action>=0 && action<4 && step>=1 && step<=(action<2?100:5500);}
inline std::array<int,4> hotkeyStepsFor(const HotkeyBindings& groups,const std::wstring& key){
    auto result=kDefaultHotkeySteps;auto found=groups.find(key);
    if(found!=groups.end())for(int i=0;i<4;++i)if(validHotkeyStep(i,found->second[i]))result[i]=found->second[i];
    return result;
}
inline bool validHotkey(int value) {
    if(!value)return true;
    if(value<0 || value>65535)return false;
    const int key=value&255,flags=(value>>8)&255;
    return key>0 && key<255 && (flags&7)!=0 && (flags&~15)==0 &&
        key!=0x10 && key!=0x11 && key!=0x12 && key!=0x5b && key!=0x5c;
}
}
