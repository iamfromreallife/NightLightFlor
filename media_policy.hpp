#pragma once
#include <cstdint>
namespace nightlight {
enum class ReportedMediaKind { Unknown, Music, Video, Image };
// Unknown is conservative: browser sessions often omit the content type.
inline constexpr bool videoMayBePlaying(bool playing,ReportedMediaKind kind) {
    return playing && (kind==ReportedMediaKind::Video || kind==ReportedMediaKind::Unknown);
}
inline constexpr bool confirmedForegroundMedia(bool playing,std::uint64_t sampled,std::uint64_t now) {
    return playing && sampled!=0 && now>=sampled && now-sampled<=8000;
}
inline constexpr bool mediaExemptsDisplay(bool globalVideo,bool foregroundMedia,bool foregroundDisplay) {
    return globalVideo || (foregroundMedia && foregroundDisplay);
}
}
