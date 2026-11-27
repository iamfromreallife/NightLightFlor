#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstddef>
#include <limits>
namespace nightlight {
inline constexpr int kMaxDimSeconds=86400;
inline constexpr std::array<int,9> kDimSecondPresets{5,15,30,120,300,600,900,1800,3600};
inline constexpr int normalizeDimSeconds(int seconds) {return std::clamp(seconds,1,kMaxDimSeconds);}
inline constexpr int legacyDimSeconds(int minutes) {return std::clamp(minutes,1,1440)*60;}
inline constexpr int legacyDimMinutes(int seconds) {return (normalizeDimSeconds(seconds)+59)/60;}
// All list rows must fit at the exact rounded per-monitor DPI row height.
// Clamp only to the actual work area (or int range for an unconstrained size).
inline constexpr int dimDialogClientHeight(std::size_t count,int rowHeight,int fixedHeight,
    int minimumHeight,int availableHeight=std::numeric_limits<int>::max()) {
    const int limit=std::max(0,availableHeight);
    const int base=std::clamp(fixedHeight,0,limit);
    const int row=std::max(1,rowHeight);
    const std::size_t rows=std::max<std::size_t>(1,count);
    const int needed=rows>static_cast<std::size_t>((limit-base)/row)?limit:
        base+static_cast<int>(rows)*row;
    return std::min(limit,std::max(minimumHeight,needed));
}
inline constexpr bool displayIdleLongEnough(std::uint64_t now,std::uint64_t last,int seconds) {
    return now>=last && now-last>=static_cast<std::uint64_t>(normalizeDimSeconds(seconds))*1000;
}
}
