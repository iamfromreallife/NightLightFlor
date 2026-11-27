#pragma once
#include <algorithm>
#include <array>
#include <map>
#include <string>
namespace nightlight {
struct ColorSchedule {
    bool enabled=false;
    int dayStart=7*60,nightStart=19*60;
    int dayKelvin=6500,nightKelvin=3500;
    int transitionMinutes=30;
};
inline ColorSchedule normalizedSchedule(ColorSchedule s) {
    s.dayStart=std::clamp(s.dayStart,0,1439);
    s.nightStart=std::clamp(s.nightStart,0,1439);
    if(s.dayStart==s.nightStart)s.nightStart=(s.dayStart+720)%1440;
    s.dayKelvin=std::clamp(s.dayKelvin,1000,6500);
    s.nightKelvin=std::clamp(s.nightKelvin,1000,6500);
    const int daySpan=(s.nightStart-s.dayStart+1440)%1440;
    s.transitionMinutes=std::clamp(s.transitionMinutes,1,std::min({120,daySpan,1440-daySpan}));
    return s;
}
struct ScheduleTarget {int kelvin;bool transitioning;};
inline ScheduleTarget scheduledColor(ColorSchedule s,int secondOfDay) {
    s=normalizedSchedule(s);
    constexpr int day=86400;
    secondOfDay=((secondOfDay%day)+day)%day;
    const int sinceDay=(secondOfDay-s.dayStart*60+day)%day;
    const int sinceNight=(secondOfDay-s.nightStart*60+day)%day;
    const bool daylight=sinceDay<sinceNight;
    const int elapsed=daylight?sinceDay:sinceNight;
    const int from=daylight?s.nightKelvin:s.dayKelvin,to=daylight?s.dayKelvin:s.nightKelvin;
    const int duration=s.transitionMinutes*60;
    if(elapsed>=duration)return {to,false};
    // Smoothstep, using 64-bit intermediates for long transitions.
    const auto t=static_cast<long long>(elapsed),d=static_cast<long long>(duration);
    const auto denominator=d*d*d;
    const auto factor=t*t*(3*d-2*t);
    const auto delta=static_cast<long long>(to-from)*factor;
    const auto rounded=(delta>=0?delta+denominator/2:delta-denominator/2)/denominator;
    return {from+static_cast<int>(rounded),true};
}
// Four independent actions: brightness up/down, temperature up/down.
// Native HOTKEY control encoding (virtual key low byte, modifiers high byte).
using HotkeyBindings=std::map<std::wstring,std::array<int,4>>;
inline bool validHotkey(int value) {
    if(!value)return true;
    if(value<0 || value>65535)return false;
    const int key=value&255,flags=(value>>8)&255;
    return key>0 && key<255 && (flags&7)!=0 && (flags&~15)==0 &&
        key!=0x10 && key!=0x11 && key!=0x12 && key!=0x5b && key!=0x5c;
}
}
