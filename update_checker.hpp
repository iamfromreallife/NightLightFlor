#pragma once
#include "core.hpp"
#include "update_version.hpp"
#include <winhttp.h>
#include <atomic>
#include <memory>
#include <chrono>
#include <thread>
namespace nightlight {
struct ReleaseResult {
    bool ok=false;
    std::string tag,notes;
};
inline int updateMinute() {
    const auto n=std::chrono::duration_cast<std::chrono::minutes>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return n>0 && n<=2147483647?static_cast<int>(n):0;
}
inline ReleaseResult fetchLatestRelease() {
    struct Http {
        HINTERNET value=nullptr;
        ~Http(){if(value)WinHttpCloseHandle(value);}
    };
    Http session{WinHttpOpen(L"NightLight/" L"1.0.19",WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.value) return {};
    WinHttpSetTimeouts(session.value,5000,5000,5000,5000);
    Http connection{WinHttpConnect(session.value,L"api.github.com",INTERNET_DEFAULT_HTTPS_PORT,0)};
    if(!connection.value) return {};
    Http request{WinHttpOpenRequest(connection.value,L"GET",
        L"/repos/iamfromreallife/NightLightFlor/releases/latest",nullptr,
        WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};
    if(!request.value) return {};
    DWORD redirects=WINHTTP_DISABLE_REDIRECTS;
    if(!WinHttpSetOption(request.value,WINHTTP_OPTION_DISABLE_FEATURE,&redirects,sizeof(redirects))) return {};
    constexpr wchar_t headers[]=L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    if(!WinHttpSendRequest(request.value,headers,static_cast<DWORD>(-1L),WINHTTP_NO_REQUEST_DATA,0,0,0) ||
       !WinHttpReceiveResponse(request.value,nullptr)) return {};
    DWORD status=0,size=sizeof(status);
    if(!WinHttpQueryHeaders(request.value,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)) return {};
    if(status==404) return {true,{},{}};
    if(status!=200) return {};
    std::string body;
    char buffer[8192];
    const auto start=GetTickCount64();
    for(;;) {
        DWORD read=0;
        if(GetTickCount64()-start>20000 || !WinHttpReadData(request.value,buffer,sizeof(buffer),&read)) return {};
        if(!read) break;
        if(body.size()+read>2*1024*1024) return {};
        body.append(buffer,read);
    }
    try {
        const auto json=portable::Json::parse(body);
        if(!json.isObject() || json.get("draft").boolean(true) || json.get("prerelease").boolean(true)) return {};
        auto tag=json.get("tag_name").str();
        if(!parseReleaseVersion(tag)) return {};
        auto notes=json.get("body").str();
        if(notes.size()>262144) notes.resize(262144);
        return {true,std::move(tag),std::move(notes)};
    } catch(...) {return {};}
}
// The request owns only its shared result, never HWNDs or application state.
// No UI-thread network I/O, no resident worker, no shutdown wait.
struct UpdateRequest {
    std::atomic<bool> done{false};
    ReleaseResult result;
};
inline std::shared_ptr<UpdateRequest> beginUpdateRequest() {
    try {
        auto request=std::make_shared<UpdateRequest>();
        std::thread([request]{
            try {request->result=fetchLatestRelease();} catch(...) {}
            request->done.store(true,std::memory_order_release);
        }).detach();
        return request;
    } catch(...) {return {};}
}
}
