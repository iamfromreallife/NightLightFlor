#include "inactive_media.hpp"
#include "media_policy.hpp"
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propkey.h>
#include <propvarutil.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#if defined(_MSC_VER) && __has_include(<winrt/Windows.Media.Control.h>)
#define NIGHTLIGHT_MEDIA_WINRT 1
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.h>
#include <winrt/Windows.Media.Control.h>
#endif

namespace nightlight {
namespace {
template<class T> struct Com {
    T* p=nullptr;
    ~Com(){if(p)p->Release();}
    T** out(){return &p;}
    T* operator->() const{return p;}
};
std::wstring lower(std::wstring text) {
    std::transform(text.begin(),text.end(),text.begin(),[](wchar_t c){return std::towlower(c);});
    return text;
}
struct ProcessIdentity { std::wstring path,appId; };
ProcessIdentity identity(DWORD pid) {
    ProcessIdentity id;
    if(!pid) return id;
    const HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(!process) return id;
    // Most executable paths fit in this small buffer. Keep support for
    // long Win32 paths without allocating and clearing 64 KiB per process.
    wchar_t path[512];
    DWORD length=static_cast<DWORD>(sizeof(path)/sizeof(path[0]));
    if(QueryFullProcessImageNameW(process,0,path,&length)) {
        id.path=lower(std::wstring(path,length));
    } else if(GetLastError()==ERROR_INSUFFICIENT_BUFFER) {
        std::vector<wchar_t> longPath(32768);
        length=static_cast<DWORD>(longPath.size());
        if(QueryFullProcessImageNameW(process,0,longPath.data(),&length))
            id.path=lower(std::wstring(longPath.data(),length));
    }
    UINT32 count=0;
    if(GetApplicationUserModelId(process,&count,nullptr)==ERROR_INSUFFICIENT_BUFFER && count>0 && count<=32768) {
        std::vector<wchar_t> name(count);
        if(GetApplicationUserModelId(process,&count,name.data())==ERROR_SUCCESS) id.appId=lower(name.data());
    }
    CloseHandle(process);
    return id;
}
std::wstring windowAppId(HWND window) {
    Com<IPropertyStore> store;
    if(FAILED(SHGetPropertyStoreForWindow(window,IID_IPropertyStore,reinterpret_cast<void**>(store.out())))) return {};
    PROPVARIANT value{};
    std::wstring result;
    constexpr PROPERTYKEY appIdKey={{0x9f4c2855,0x9f79,0x4b39,{0xa8,0xd0,0xe1,0xd4,0x2d,0xe1,0xd5,0xf3}},5};
    if(SUCCEEDED(store->GetValue(appIdKey,&value)) && value.vt==VT_LPWSTR && value.pwszVal)
        result=lower(value.pwszVal);
    PropVariantClear(&value);
    return result;
}
bool audioPlaying(DWORD foreground,const ProcessIdentity& app) {
    Com<IMMDeviceEnumerator> devices;
    if(FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_INPROC_SERVER,
        __uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(devices.out())))) return false;
    Com<IMMDeviceCollection> collection;
    if(FAILED(devices->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,collection.out()))) return false;
    UINT count=0;
    if(FAILED(collection->GetCount(&count))) return false;
    // A process may have many sessions/endpoints. Resolve it once per
    // sample only; never cache across samples or PID reuse.
    std::map<DWORD,bool> matchingProcesses;
    for(UINT i=0;i<count;++i) {
        Com<IMMDevice> device;
        Com<IAudioSessionManager2> manager;
        Com<IAudioSessionEnumerator> sessions;
        if(FAILED(collection->Item(i,device.out())) ||
           FAILED(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_INPROC_SERVER,nullptr,
                reinterpret_cast<void**>(manager.out()))) ||
           FAILED(manager->GetSessionEnumerator(sessions.out()))) continue;
        int n=0;
        if(FAILED(sessions->GetCount(&n))) continue;
        for(int j=0;j<n;++j) {
            Com<IAudioSessionControl> control;
            Com<IAudioSessionControl2> session;
            AudioSessionState state=AudioSessionStateInactive;
            if(FAILED(sessions->GetSession(j,control.out())) ||
               FAILED(control->GetState(&state)) || state!=AudioSessionStateActive ||
               FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),reinterpret_cast<void**>(session.out())))) continue;
            DWORD pid=0;
            if(FAILED(session->GetProcessId(&pid)) || !pid) continue;
            bool matches=pid==foreground;
            if(!matches) {
                if(app.path.empty() && app.appId.empty())continue;
                auto entry=matchingProcesses.try_emplace(pid,false);
                if(entry.second) {
                    const auto other=identity(pid);
                    entry.first->second=(!app.path.empty() && app.path==other.path) ||
                        (!app.appId.empty() && app.appId==other.appId);
                }
                matches=entry.first->second;
            }
            if(!matches) continue;
            // A running audio stream is not proof of actual media playback:
            // browsers can leave a silent stream active indefinitely.
            Com<IAudioMeterInformation> meter;
            float peak=0.0f;
            if(SUCCEEDED(control->QueryInterface(__uuidof(IAudioMeterInformation),
                   reinterpret_cast<void**>(meter.out()))) &&
               SUCCEEDED(meter->GetPeakValue(&peak)) && peak>0.0001f) return true;
        }
    }
    return false;
}
#ifdef NIGHTLIGHT_MEDIA_WINRT
using namespace winrt::Windows::Media::Control;
bool sourceMatches(std::wstring source,const ProcessIdentity& app,const std::wstring& windowId) {
    source=lower(std::move(source));
    if(source.empty()) return false;
    if(source==app.appId || source==windowId || source==app.path) return true;
    if(app.path.empty()) return false;
    const std::filesystem::path executable(app.path);
    const std::filesystem::path sourcePath(source);
    // Desktop media session IDs may be a path, executable name or AUMID.
    return source==executable.stem().wstring() ||
        source==executable.filename().wstring() ||
        (sourcePath.has_extension() && sourcePath.filename()==executable.filename());
}
struct SessionPlayback {bool foreground=false,video=false;};
SessionPlayback systemMediaPlaying(GlobalSystemMediaTransportControlsSessionManager& manager,
                        const ProcessIdentity& app,const std::wstring& windowId) {
    SessionPlayback result;
    try {
        if(!manager) {
            auto request=GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
            if(request.wait_for(std::chrono::milliseconds(750))!=winrt::Windows::Foundation::AsyncStatus::Completed) {
                request.Cancel(); return {};
            }
            manager=request.GetResults();
        }
        for(const auto& session:manager.GetSessions()) {
            // One disappearing session must not invalidate other sessions.
            try {
                const auto playback=session.GetPlaybackInfo();
                if(!playback || playback.PlaybackStatus()!=GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)continue;
                auto kind=ReportedMediaKind::Unknown;
                const auto type=playback.PlaybackType();
                if(type) {
                    using winrt::Windows::Media::MediaPlaybackType;
                    switch(type.Value()) {
                    case MediaPlaybackType::Video:kind=ReportedMediaKind::Video;break;
                    case MediaPlaybackType::Music:kind=ReportedMediaKind::Music;break;
                    case MediaPlaybackType::Image:kind=ReportedMediaKind::Image;break;
                    default:break;
                    }
                }
                if(videoMayBePlaying(true,kind))result.video=true;
                if(result.video)break; // global exemption already covers every selected display
                const auto source=session.SourceAppUserModelId();
                if(sourceMatches(std::wstring(source.c_str()),app,windowId))result.foreground=true;
            } catch(...) {continue;}
        }
    } catch(...) { manager=nullptr; }
    return result;
}
#endif
}
ForegroundMedia::~ForegroundMedia() {
    {std::lock_guard<std::mutex> lock(mutex_); stopping_=true; pending_=false;}
    wake_.notify_one();
    if(worker_.joinable()) worker_.join();
}
void ForegroundMedia::request(HWND window,DWORD pid) {
    // Global media sessions are meaningful even without a foreground window.
    std::lock_guard<std::mutex> lock(mutex_);
    if(stopping_) return;
    if(!worker_.joinable()) {
        try { worker_=std::thread(&ForegroundMedia::run,this); }
        catch(...) { return; }
    }
    if(wanted_!=window || pid_!=pid) ++generation_;
    wanted_=window; pid_=pid; pending_=true;
    wake_.notify_one();
}
ForegroundMediaResult ForegroundMedia::result() {
    std::lock_guard<std::mutex> lock(mutex_); return result_;
}
void ForegroundMedia::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_; pending_=false; wanted_=nullptr; pid_=0; result_={};
    wake_.notify_one();
}
void ForegroundMedia::run() {
    // COM/media enumeration stays away from the UI and colour/DDC workers.
#ifdef NIGHTLIGHT_MEDIA_WINRT
    bool initialized=false;
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); initialized=true; }
    catch(...) {}
    GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
#else
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    const bool initialized=SUCCEEDED(com);
#endif
    for(;;) {
        HWND window=nullptr; DWORD pid=0; std::uint64_t generation=0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if(!wake_.wait_for(lock,std::chrono::seconds(15),[&]{return stopping_ || pending_;})) {
#ifdef NIGHTLIGHT_MEDIA_WINRT
                manager=nullptr;
#endif
                wake_.wait(lock,[&]{return stopping_ || pending_;});
            }
            if(stopping_) break;
            window=wanted_; pid=pid_; generation=generation_; pending_=false;
        }
        bool playing=false,videoPlaying=false;
        if(initialized) {
            try {
                const auto app=identity(pid);
#ifdef NIGHTLIGHT_MEDIA_WINRT
                const auto sessions=systemMediaPlaying(manager,app,window?windowAppId(window):std::wstring{});
                videoPlaying=sessions.video;playing=sessions.foreground;
#endif
                // Existing audible foreground fallback for players that don't
                // publish a Windows media session. No audio capture is used.
                if(!videoPlaying && !playing && pid)playing=audioPlaying(pid,app);
            } catch(...) { playing=false; }
        }
        const auto now=GetTickCount64();
        std::lock_guard<std::mutex> lock(mutex_);
        if(!stopping_ && generation==generation_ && window==wanted_ && pid==pid_)
            result_={window,pid,now,playing,videoPlaying};
    }
#ifdef NIGHTLIGHT_MEDIA_WINRT
    manager=nullptr;
    if(initialized) winrt::uninit_apartment();
#else
    if(initialized) CoUninitialize();
#endif
}
}
