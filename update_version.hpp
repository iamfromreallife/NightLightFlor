#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
namespace nightlight {
#ifndef NIGHTLIGHT_VERSION
#define NIGHTLIGHT_VERSION "1.0.21"
#endif
#define NIGHTLIGHT_WIDE_IMPL(x) L##x
#define NIGHTLIGHT_WIDE(x) NIGHTLIGHT_WIDE_IMPL(x)
inline constexpr char kAppVersion[]=NIGHTLIGHT_VERSION;
inline constexpr wchar_t kAppVersionWide[]=NIGHTLIGHT_WIDE(NIGHTLIGHT_VERSION);
#undef NIGHTLIGHT_WIDE
#undef NIGHTLIGHT_WIDE_IMPL
using ReleaseVersion=std::array<unsigned,3>;
inline std::optional<ReleaseVersion> parseReleaseVersion(std::string_view tag) {
    if(!tag.empty() && (tag.front()=='v' || tag.front()=='V')) tag.remove_prefix(1);
    ReleaseVersion result{};
    for(size_t i=0;i<result.size();++i) {
        unsigned digits=0;
        while(!tag.empty() && tag.front()>='0' && tag.front()<='9') {
            if(++digits>5) return {};
            result[i]=result[i]*10+static_cast<unsigned>(tag.front()-'0');
            tag.remove_prefix(1);
        }
        if(!digits) return {};
        if(i+1<result.size()) {
            if(tag.empty() || tag.front()!='.') return {};
            tag.remove_prefix(1);
        }
    }
    if(!tag.empty()) return {};
    return result;
}
inline bool newerThan(std::string_view tag,std::string_view current,std::string_view ignored={}) {
    const auto candidate=parseReleaseVersion(tag), installed=parseReleaseVersion(current);
    const auto skipped=parseReleaseVersion(ignored);
    return candidate && installed && *candidate>*installed && (!skipped || *candidate!=*skipped);
}
inline bool newerRelease(std::string_view tag,std::string_view ignored={}) {
    return newerThan(tag,kAppVersion,ignored);
}
inline bool updateDue(int now,int last) {
    return now>0 && (last<=0 || now<last || now-last>=1440);
}
}
