#pragma once
#include <array>
#include <optional>
#include <string>
#include <string_view>
namespace nightlight {
inline constexpr char kAppVersion[]="1.0.19";
inline constexpr wchar_t kAppVersionWide[]=L"1.0.19";
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
inline bool newerRelease(std::string_view tag,std::string_view ignored={}) {
    const auto candidate=parseReleaseVersion(tag), installed=parseReleaseVersion(kAppVersion);
    const auto skipped=parseReleaseVersion(ignored);
    return candidate && installed && *candidate>*installed && (!skipped || *candidate!=*skipped);
}
inline bool updateDue(int now,int last) {
    return now>0 && (last<=0 || now<last || now-last>=1440);
}
}
