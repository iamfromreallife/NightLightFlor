#pragma once
#include <cstdint>
#include <string>
#include <utility>
namespace nightlight {
// Single-flight search coordination, independent of HWND/network lifetime.
struct LocationSearchState {
    static constexpr std::uint64_t debounceMs=400;
    std::wstring query, completed, inFlight;
    std::uint64_t due=0;
    bool pending=false, explicitSelection=false;
    void changed(std::wstring text,std::uint64_t now){
        query=std::move(text);completed.clear();explicitSelection=false;
        pending=query.size()>=3;due=now+debounceMs;
    }
    void manual(std::uint64_t now){
        if(query.size()<2)return;
        explicitSelection=true;pending=true;due=now;
    }
    bool ready(std::uint64_t now)const{return pending&&inFlight.empty()&&now>=due;}
    void started(){inFlight=query;pending=false;}
    bool finished(){
        const bool current=!inFlight.empty()&&inFlight==query;
        inFlight.clear();
        if(current){completed=query;pending=false;}
        return current;
    }
    bool busy()const{return pending||!inFlight.empty();}
};
}
