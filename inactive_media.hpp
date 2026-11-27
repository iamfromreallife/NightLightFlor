#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace nightlight {
struct ForegroundMediaResult {
    HWND window=nullptr;
    DWORD pid=0;
    ULONGLONG sampled=0;
    bool playing=false;
    bool videoPlaying=false;
};
class ForegroundMedia {
public:
    ~ForegroundMedia();
    void request(HWND window,DWORD pid);
    ForegroundMediaResult result();
    void clear();
private:
    void run();
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread worker_;
    HWND wanted_=nullptr;
    DWORD pid_=0;
    bool pending_=false,stopping_=false;
    std::uint64_t generation_=0;
    ForegroundMediaResult result_;
};
}
