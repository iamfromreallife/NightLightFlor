#pragma once
#include "core.hpp"
#include "localization.hpp"
#include "solar_time.hpp"
#include "map_view.hpp"
#include "update_version.hpp"
#include <winhttp.h>
#include <atomic>
#include <memory>
#include <thread>
#include <future>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <iterator>
#include <stdexcept>
namespace nightlight {
struct CityLocation {std::wstring name;double latitude=0,longitude=0;};
inline std::array<std::optional<int>,3> localSolarTimes(double lat,double lon){
    SYSTEMTIME date{};GetLocalTime(&date);
    const auto utc=solarUtcMinutes(dayOfYear(date.wYear,date.wMonth,date.wDay),leapYear(date.wYear)?366:365,lat,lon);
    date.wHour=date.wMinute=date.wSecond=date.wMilliseconds=0;
    FILETIME base{};std::array<std::optional<int>,3> out{};
    if(!SystemTimeToFileTime(&date,&base))return out;
    ULARGE_INTEGER b{};b.LowPart=base.dwLowDateTime;b.HighPart=base.dwHighDateTime;
    for(size_t i=0;i<3;++i)if(utc[i]){
        const auto ticks=static_cast<long long>(b.QuadPart)+static_cast<long long>(std::llround(*utc[i]))*600000000LL;
        if(ticks<0)continue;ULARGE_INTEGER value{};value.QuadPart=static_cast<ULONGLONG>(ticks);
        FILETIME ft{value.LowPart,value.HighPart};SYSTEMTIME u{},local{};
        if(FileTimeToSystemTime(&ft,&u) && SystemTimeToTzSpecificLocalTimeEx(nullptr,&u,&local))out[i]=local.wHour*60+local.wMinute;
    }return out;
}
inline bool refreshSolarPoints(ColorSchedule& s){
    if(s.locationName.empty())return false;
    const auto times=localSolarTimes(s.latitude,s.longitude);
    s=effectiveSolarPoints(std::move(s),times);return true;
}
inline std::string locationGet(const wchar_t* host,const std::wstring& path,size_t limit){
    struct Handle {HINTERNET h=nullptr;~Handle(){if(h)WinHttpCloseHandle(h);}};
    const auto agent=std::wstring(L"NightLight/")+kAppVersionWide+L" (+https://github.com/iamfromreallife/NightLightFlor)";
    Handle session{WinHttpOpen(agent.c_str(),WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0)};
    if(!session.h)return {};WinHttpSetTimeouts(session.h,5000,5000,5000,5000);
    Handle conn{WinHttpConnect(session.h,host,INTERNET_DEFAULT_HTTPS_PORT,0)};if(!conn.h)return {};
    Handle req{WinHttpOpenRequest(conn.h,L"GET",path.c_str(),nullptr,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE)};if(!req.h)return {};
    DWORD redirects=WINHTTP_DISABLE_REDIRECTS;WinHttpSetOption(req.h,WINHTTP_OPTION_DISABLE_FEATURE,&redirects,sizeof(redirects));
    if(!WinHttpSendRequest(req.h,WINHTTP_NO_ADDITIONAL_HEADERS,0,WINHTTP_NO_REQUEST_DATA,0,0,0)||!WinHttpReceiveResponse(req.h,nullptr))return {};
    DWORD status=0,size=sizeof(status);if(!WinHttpQueryHeaders(req.h,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,WINHTTP_HEADER_NAME_BY_INDEX,&status,&size,WINHTTP_NO_HEADER_INDEX)||status!=200)return {};
    std::string result;char buf[8192];const auto started=GetTickCount64();
    for(;;){DWORD got=0;if(GetTickCount64()-started>20000||!WinHttpReadData(req.h,buf,sizeof(buf),&got))return {};if(!got)break;if(result.size()+got>limit)return {};result.append(buf,got);}return result;
}
inline std::wstring queryEncode(const std::wstring& text){
    const auto utf8=toUtf8(text);std::wstring out;constexpr wchar_t hex[]=L"0123456789ABCDEF";
    for(unsigned char c:utf8){if((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.')out+=static_cast<wchar_t>(c);else{out+=L'%';out+=hex[c>>4];out+=hex[c&15];}}return out;
}
inline std::vector<CityLocation> searchCities(const std::wstring& text,int language){
    const auto body=locationGet(L"geocoding-api.open-meteo.com",L"/v1/search?count=8&language="+std::wstring(languageAt(language).search)+L"&name="+queryEncode(text),512*1024);
    if(body.empty())throw std::runtime_error("Location request failed");
    const auto json=portable::Json::parse(body);std::vector<CityLocation> out;
    if(json.get("error").boolean())throw std::runtime_error("Location request failed");
    if(const auto* arr=std::get_if<portable::Json::Array>(&json.get("results").value))for(const auto& item:*arr){
        const auto* lat=std::get_if<double>(&item.get("latitude").value);const auto* lon=std::get_if<double>(&item.get("longitude").value);
        if(!lat||!lon||!std::isfinite(*lat)||!std::isfinite(*lon)||std::abs(*lat)>90||std::abs(*lon)>180)continue;
        auto name=item.get("name").str();if(name.empty())continue;
        for(const char* key:{"admin1","country"}){auto part=item.get(key).str();if(!part.empty()&&name.find(part)==std::string::npos)name+=", "+part;}
        out.push_back({fromUtf8(name),*lat,*lon});if(out.size()>=8)break;
    }return out;
}
// Slippy-map world pixels at the selected zoom; x wraps, y is clamped.
inline MapPoint projectCity(const CityLocation& city,int zoom){return projectMapPoint(city.latitude,city.longitude,zoom);}
struct CityTile {std::string png;int x=0,y=0,zoom=0;};
inline CityTile cityTile(int zoom,int x,int y){
    const int n=1<<zoom;x=((x%n)+n)%n;
    CityTile tile{{},x,y,zoom};if(y<0||y>=n)return tile;
    // Visible tiles only; cache for seven days, no speculative prefetch.
    std::filesystem::path cache;try{wchar_t path[32768]{};const DWORD len=GetEnvironmentVariableW(L"LOCALAPPDATA",path,32768);
        if(len && len<32768){cache=std::filesystem::path(path)/L"NightLight"/L"MapCache";std::filesystem::create_directories(cache);cache/=std::to_wstring(zoom)+L"-"+std::to_wstring(x)+L"-"+std::to_wstring(y)+L".png";
            if(std::filesystem::exists(cache)&&std::filesystem::file_size(cache)<1024*1024&&std::filesystem::file_time_type::clock::now()-std::filesystem::last_write_time(cache)<std::chrono::hours(24*7)){
                std::ifstream f(cache,std::ios::binary);tile.png.assign(std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>());if(tile.png.size()>8&&tile.png.compare(0,8,"\x89PNG\r\n\x1a\n",8)==0)return tile;}}}catch(...){cache.clear();}
    tile.png=locationGet(L"tile.openstreetmap.org",L"/"+std::to_wstring(zoom)+L"/"+std::to_wstring(x)+L"/"+std::to_wstring(y)+L".png",1024*1024);
    if(tile.png.size()<8||tile.png.compare(0,8,"\x89PNG\r\n\x1a\n",8)!=0){tile.png.clear();return tile;}
    if(!cache.empty())try{std::ofstream f(cache,std::ios::binary|std::ios::trunc);f.write(tile.png.data(),static_cast<std::streamsize>(tile.png.size()));}catch(...){}return tile;
}
inline std::vector<CityTile> visibleCityTiles(int zoom,MapPoint center,int width,int height){
    std::vector<CityTile> tiles;
    const auto keys=visibleMapTiles(zoom,center,width,height);
    tiles.reserve(keys.size());
    for(const auto& key:keys)tiles.push_back({{},key.x,key.y,zoom});
    for(size_t start=0;start<tiles.size();start+=4){
        std::vector<std::future<CityTile>> jobs;
        jobs.reserve(std::min<size_t>(4,tiles.size()-start));
        for(size_t i=start;i<std::min(start+4,tiles.size());++i){const auto tile=tiles[i];jobs.push_back(std::async(std::launch::async,[tile]{return cityTile(tile.zoom,tile.x,tile.y);}));}
        for(size_t i=0;i<jobs.size();++i)try{tiles[start+i]=jobs[i].get();}catch(...){}
    }return tiles;
}
template<class T> struct LocationRequest {std::atomic<bool> done{false};bool ok=false;T result;};
template<class T,class F> inline std::shared_ptr<LocationRequest<T>> beginLocationRequest(F f){
    try{auto p=std::make_shared<LocationRequest<T>>();std::thread([p,f=std::move(f)]{try{p->result=f();p->ok=true;}catch(...){}p->done.store(true,std::memory_order_release);}).detach();return p;}catch(...){return {};}
}
}
