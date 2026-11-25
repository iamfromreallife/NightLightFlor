#include "core.hpp"
#include "ddc.hpp"
#include "wmi_brightness.hpp"
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>
#include <algorithm>
#include <chrono>
#include <vector>

namespace nightlight {
void applyDdcObservation(DdcValue& current,const DdcValue& observed,
                         bool protectUserValue) {
    // Read and write events share the single I/O worker, but a read queued
    // before a slider drag may finish after the user has chosen a new target.
    // Never let that stale read move the live thumb backwards.
    current.supported=observed.supported;
    current.readable=observed.readable;
    current.backend=observed.backend;
    if (!protectUserValue && observed.readable && observed.hasValue) {
        current.percent=std::clamp(observed.percent,0,100);
        current.hasValue=true;
        current.managed=true; // persist the actual hardware value as fallback
        current.failed=false;
    }
    // If hardware cannot be read, retain the last user value or persisted
    // fallback. pending, generation and write-grace belong to local writes
    // and must not be modified by this observation.
}
DWORD ddcRawValue(DWORD lo,DWORD hi,int percent) {
    if (hi<=lo) return lo;
    return lo+static_cast<DWORD>((std::uint64_t(hi-lo)*std::clamp(percent,0,100)+50)/100);
}
int ddcPercentage(DWORD lo,DWORD hi,DWORD value) {
    if (hi<=lo) return 0;
    return static_cast<int>((std::uint64_t(std::clamp(value,lo,hi)-lo)*100+(hi-lo)/2)/(hi-lo));
}
bool writeDdcValue(bool readable,DWORD lo,DWORD hi,int percent,const DdcWrite& highLevel,const DdcWrite& vcp) {
    // Upstream sends 0..100 directly when getters are unavailable. A failed
    // read must never prevent SetMonitorBrightness/SetMonitorContrast.
    const DWORD value=readable?ddcRawValue(lo,hi,percent):static_cast<DWORD>(std::clamp(percent,0,100));
    if (highLevel && highLevel(value)) return true;
    return vcp && vcp(value);
}
namespace {
struct Api {
    HMODULE library=LoadLibraryExW(L"dxva2.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&GetNumberOfPhysicalMonitorsFromHMONITOR) count=nullptr;
    decltype(&GetPhysicalMonitorsFromHMONITOR) monitors=nullptr;
    decltype(&DestroyPhysicalMonitors) destroy=nullptr;
    decltype(&GetMonitorBrightness) getBrightness=nullptr;
    decltype(&SetMonitorBrightness) setBrightness=nullptr;
    decltype(&GetMonitorContrast) getContrast=nullptr;
    decltype(&SetMonitorContrast) setContrast=nullptr;
    decltype(&GetVCPFeatureAndVCPFeatureReply) getVcp=nullptr;
    decltype(&SetVCPFeature) setVcp=nullptr;
    Api() {
        if (!library) return;
#define LOAD(member,name) member=reinterpret_cast<decltype(member)>(GetProcAddress(library,name))
        LOAD(count,"GetNumberOfPhysicalMonitorsFromHMONITOR");
        LOAD(monitors,"GetPhysicalMonitorsFromHMONITOR"); LOAD(destroy,"DestroyPhysicalMonitors");
        LOAD(getBrightness,"GetMonitorBrightness"); LOAD(setBrightness,"SetMonitorBrightness");
        LOAD(getContrast,"GetMonitorContrast"); LOAD(setContrast,"SetMonitorContrast");
        LOAD(getVcp,"GetVCPFeatureAndVCPFeatureReply"); LOAD(setVcp,"SetVCPFeature");
#undef LOAD
    }
    ~Api() { if (library) FreeLibrary(library); }
};
struct FindContext { std::wstring device; HMONITOR monitor=nullptr; };
BOOL CALLBACK findMonitor(HMONITOR monitor,HDC,LPRECT,LPARAM param) {
    auto& context=*reinterpret_cast<FindContext*>(param);
    MONITORINFOEXW info{}; info.cbSize=sizeof(info);
    if (GetMonitorInfoW(monitor,&info) && context.device==info.szDevice) {
        context.monitor=monitor; return FALSE;
    }
    return TRUE;
}
struct Physical {
    Api& api;
    std::vector<PHYSICAL_MONITOR> handles;
    Physical(Api& a,const std::wstring& device):api(a) {
        if (!api.count || !api.monitors || !api.destroy) return;
        FindContext context{device};
        EnumDisplayMonitors(nullptr,nullptr,findMonitor,reinterpret_cast<LPARAM>(&context));
        DWORD count=0;
        // Match upstream: retain every physical handle behind this selected
        // logical display. Do not broadcast to other Windows displays.
        if (!context.monitor || !api.count(context.monitor,&count) || count==0 || count>128) return;
        handles.resize(count);
        if (!api.monitors(context.monitor,count,handles.data())) handles.clear();
    }
    ~Physical() { if (!handles.empty()) api.destroy(static_cast<DWORD>(handles.size()),handles.data()); }
    HANDLE handle() const { return handles.empty()?nullptr:handles.front().hPhysicalMonitor; }
};
struct Reading { bool supported=false; bool vcp=false; DWORD lo=0,value=0,hi=0; };
Reading read(Api& api,HANDLE handle,int feature) {
    Reading r;
    if (!handle) return r;
    const auto get=feature==0?api.getBrightness:api.getContrast;
    if (get && get(handle,&r.lo,&r.value,&r.hi) && r.hi>r.lo) { r.supported=true; return r; }
    r.lo=0;
    if (api.getVcp && api.getVcp(handle,feature==0?0x10:0x12,nullptr,&r.value,&r.hi) && r.hi>0) {
        r.supported=true; r.vcp=true;
    }
    return r;
}
DdcValue observe(Api& api,Physical& physical,WmiBrightness& wmi,const std::wstring& device,int feature) {
    DdcValue value;
    const bool setter=(feature==0?api.setBrightness!=nullptr:api.setContrast!=nullptr) || api.setVcp;
    if (!physical.handles.empty() && setter) {
        value.supported=true; value.backend=DdcBackend::DdcCi;
        for (const auto& monitor:physical.handles) {
            const auto reading=read(api,monitor.hPhysicalMonitor,feature);
            if (reading.supported) {
                value.readable=true; value.hasValue=true;
                value.percent=ddcPercentage(reading.lo,reading.hi,reading.value); break;
            }
        }
    }
    if (feature==0) {
        const auto panel=wmi.find(device);
        if (panel && (panel.readable || !value.readable)) {
            value.supported=true; value.backend=DdcBackend::WindowsWmi;
            value.readable=panel.readable; value.hasValue=panel.readable; value.percent=panel.percent;
        }
    }
    return value;
}
bool writePhysical(Api& api,Physical& physical,int feature,int percent) {
    if (physical.handles.empty()) return false;
    bool allSucceeded=true;
    for (const auto& monitor:physical.handles) {
        const auto reading=read(api,monitor.hPhysicalMonitor,feature);
        if (reading.supported && ddcPercentage(reading.lo,reading.hi,reading.value)==percent) continue;
        const auto setter=feature==0?api.setBrightness:api.setContrast;
        const bool ok=writeDdcValue(reading.supported,reading.lo,reading.hi,percent,
            [&](DWORD raw){return setter && setter(monitor.hPhysicalMonitor,raw);},
            [&](DWORD raw){return api.setVcp && api.setVcp(monitor.hPhysicalMonitor,feature==0?0x10:0x12,raw);});
        allSucceeded=allSucceeded && ok;
    }
    return allSucceeded;
}
}
DdcController::DdcController():worker_(&DdcController::run,this) {}
DdcController::~DdcController() { stop(); }
void DdcController::setWindow(HWND window) { std::lock_guard<std::mutex> lock(mutex_); window_=window; }
void DdcController::seed(const std::wstring& device,int brightness,int contrast,bool savedBrightness,bool savedContrast) {
    if (device.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    const auto inserted=states_.try_emplace(device);
    if (!inserted.second) return;
    auto& state=inserted.first->second;
    state.brightness.percent=std::clamp(brightness,0,100);
    state.contrast.percent=std::clamp(contrast,0,100);
    state.brightness.hasValue=state.contrast.hasValue=true;
    state.brightness.managed=savedBrightness; state.contrast.managed=savedContrast;
}
void DdcController::restore(const std::wstring& device,int brightness,int contrast,bool savedBrightness,bool savedContrast) {
    if (device.empty() || (!savedBrightness && !savedContrast)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    auto& state=states_[device];
    auto restoreValue=[&](DdcValue& value,int feature,int percent,bool saved) {
        if (!saved) return; // Never send a UI placeholder to hardware.
        percent=std::clamp(percent,0,100);
        if (value.pending && value.percent==percent) return;
        value.percent=percent; value.hasValue=true; value.managed=true;
        value.pending=true; value.failed=false; ++value.generation;
        jobs_[{device,feature}]={percent,value.generation};
    };
    restoreValue(state.brightness,0,brightness,savedBrightness);
    restoreValue(state.contrast,1,contrast,savedContrast);
    jobs_[{device,-1}]={};
    wake_.notify_one();
}
void DdcController::invalidateTopology() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++topologyEpoch_;
    jobs_.clear();
    states_.clear();
}
void DdcController::refresh(const std::wstring& device) {
    if (device.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    states_[device].loading=true; jobs_[{device,-1}]={}; wake_.notify_one();
}
void DdcController::poll(const std::wstring& device) {
    if (device.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found=states_.find(device);
    if (stopping_ || found==states_.end()) return;
    const DdcState& state=found->second;
    // Never disable sliders or pile up read jobs. A drag should not wait
    // behind an unnecessary full-hardware poll, nor be reset by one.
    if (state.loading || state.brightness.pending || state.contrast.pending ||
        jobs_.find({device,-1})!=jobs_.end()) return;
    jobs_[{device,-1}]={};
    wake_.notify_one();
}
bool DdcController::set(const std::wstring& device,DdcFeature feature,int percent) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found=states_.find(device);
    if (stopping_ || found==states_.end() || found->second.loading) return false;
    auto& value=feature==DdcFeature::Brightness?found->second.brightness:found->second.contrast;
    if (!value.supported) return false;
    percent=std::clamp(percent,0,100);
    if (value.managed && value.hasValue && value.percent==percent && !value.failed) return true;
    value.percent=percent; value.hasValue=true; value.managed=true; value.pending=true; value.failed=false; ++value.generation;
    jobs_[{device,static_cast<int>(feature)}]={percent,value.generation}; wake_.notify_one();
    return true;
}
DdcState DdcController::state(const std::wstring& device) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found=states_.find(device); return found==states_.end()?DdcState{}:found->second;
}
void DdcController::stop() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_=true; window_=nullptr; jobs_.clear(); }
    wake_.notify_one(); if (worker_.joinable()) worker_.join();
}
void DdcController::run() {
    Api api;
    WmiBrightness wmi;
    std::pair<std::wstring,int> previous;
    bool first=true;
    for (;;) {
        std::pair<std::wstring,int> key; Job job;
        std::uint64_t epoch=0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock,[&]{ return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            const auto next=nextMonitorJob(jobs_,previous,first);
            key=next->first; job=next->second; jobs_.erase(next);
            epoch=topologyEpoch_;
            previous=key; first=false;
        }
        std::unique_lock<std::mutex> physicalLock(physicalDdcMutex());
        Physical monitor(api,key.first);
        if (key.second==-1) {
            const DdcValue brightness=observe(api,monitor,wmi,key.first,0), contrast=observe(api,monitor,wmi,key.first,1);
            std::lock_guard<std::mutex> lock(mutex_);
            // A display sleep/re-enumeration may have reassigned this GDI
            // output while the slow DDC read was in progress.
            if (epoch!=topologyEpoch_ || stopping_) continue;
            auto& state=states_[key.first]; state.loading=false;
            const auto now=GetTickCount64();
            auto publish=[now](DdcValue& value,const DdcValue& observed) {
                const bool protect=value.pending || now<value.protectUntilTickMs;
                applyDdcObservation(value,observed,protect);
            };
            publish(state.brightness,brightness);
            publish(state.contrast,contrast);
            if (window_ && !stopping_) PostMessageW(window_,WM_DDC_COMPLETE,0,0);
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto& state=states_[key.first];
                const auto& latest=key.second==0?state.brightness:state.contrast;
                if (stopping_) return;
                if (epoch!=topologyEpoch_ ||
                    latest.generation!=job.generation) continue;
            }
            bool usedWmi=false;
            bool ok=writePhysical(api,monitor,key.second,job.percent);
            // Upstream also attempts WMI after DDC/CI. Match that behavior for
            // the selected WMI instance, including drivers that return success
            // from SetMonitorBrightness without changing an internal panel.
            if (key.second==0) {
                const auto panel=wmi.find(key.first);
                if (panel && ((panel.readable && panel.percent==job.percent) || wmi.set(panel,job.percent))) { ok=true; usedWmi=true; }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (epoch!=topologyEpoch_ || stopping_) continue;
            auto& state=states_[key.first];
            auto& value=key.second==0?state.brightness:state.contrast;
            if (value.generation==job.generation) {
                value.pending=false; value.failed=!ok;
                if (usedWmi) value.backend=DdcBackend::WindowsWmi;
                // The next poll must not read a monitor's old value while
                // its firmware is still applying our last local write.
                value.protectUntilTickMs=ok?
                    GetTickCount64()+kDdcPostWriteGraceMs:0;
                value.hasValue=true;
                value.percent=job.percent;
            }
            if (window_ && !stopping_) PostMessageW(window_,WM_DDC_COMPLETE,0,0);
        }
        physicalLock.unlock();
        // Bound DDC/CI traffic; pointer events overwrite pending writes.
        { std::unique_lock<std::mutex> lock(mutex_);
          wake_.wait_for(lock,std::chrono::milliseconds(75),[&]{return stopping_;});
          if (stopping_) return; }
    }
}
}
