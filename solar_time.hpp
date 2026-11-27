#pragma once
#include <cmath>
#include <array>
#include <optional>
#include <algorithm>
namespace nightlight {
// NOAA fractional-year approximation. Times are UTC minutes relative to the
// supplied calendar date; the caller performs date-aware local conversion.
inline std::array<std::optional<double>,3> solarUtcMinutes(int dayOfYear,int days,double lat,double lon){
    constexpr double pi=3.14159265358979323846;
    if(!std::isfinite(lat)||!std::isfinite(lon)||std::abs(lat)>90||std::abs(lon)>180)return {};
    const double g=2*pi/days*(dayOfYear-1), r=lat*pi/180;
    const double eq=229.18*(0.000075+0.001868*std::cos(g)-0.032077*std::sin(g)-0.014615*std::cos(2*g)-0.040849*std::sin(2*g));
    const double dec=0.006918-0.399912*std::cos(g)+0.070257*std::sin(g)-0.006758*std::cos(2*g)+0.000907*std::sin(2*g)-0.002697*std::cos(3*g)+0.00148*std::sin(3*g);
    const double noon=720-4*lon-eq;
    const double denominator=std::cos(r)*std::cos(dec);
    if(std::abs(denominator)<1e-10)return {std::nullopt,noon,std::nullopt};
    const double a=(std::cos(90.833*pi/180)-std::sin(r)*std::sin(dec))/denominator;
    if(a < -1 || a > 1)return {std::nullopt,noon,std::nullopt};
    const double minutes=4*std::acos(a)*180/pi;
    return {noon-minutes,noon,noon+minutes};
}
inline bool leapYear(int y){return y%4==0 && (y%100!=0 || y%400==0);}
inline int dayOfYear(int y,int m,int d){constexpr int start[]={0,31,59,90,120,151,181,212,243,273,304,334};return start[std::clamp(m,1,12)-1]+d+(m>2 && leapYear(y)?1:0);}
}
