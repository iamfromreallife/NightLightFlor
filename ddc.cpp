#include "core.hpp"
#include "display_identity.hpp"
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
    if (!protectUserValue && !current.restoring && !current.dimOwned && observed.readable && observed.hasValue) {
        current.percent=std::clamp(observed.percent,0,100);
        current.hasValue=true;
        current.managed=true; // persist the actual hardware value as fallback
        current.failed=false;
    }
    // If hardware cannot be read, retain the last user value or persisted
    // fallback. pending, generation and write-grace belong to local writes
    // and must not be modified by this observation.
}
void verifyDdcRestore(DdcValue& current,const DdcValue& observed) {
    if(!current.restoring || current.dimOwned || current.pending || !observed.readable || !observed.hasValue) return;
    // A one-percentage-point difference can be hardware quantization.
    const int difference=current.percent-observed.percent;
    current.failed=difference>1 || difference< -1;
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
bool writePhysical(Api& api,Physical& physical,int feature,int percent,bool force) {
    if (physical.handles.empty()) return false;
    bool allSucceeded=true;
    for (const auto& monitor:physical.handles) {
        const auto reading=read(api,monitor.hPhysicalMonitor,feature);
        // At boot a cached getter can already claim the desired percentage
        // while the actual panel is still using its power-on brightness.
        if (!force && reading.supported && ddcPercentage(reading.lo,reading.hi,reading.value)==percent) continue;
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
void DdcController::restore(const std::wstring& device,int brightness,int contrast,bool savedBrightness,bool savedContrast,
                            const std::wstring& physicalId) {
    if (device.empty() || (!savedBrightness && !savedContrast)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_) return;
    restoreWindow_=true;
    auto& state=states_[device];
    auto restoreValue=[&](DdcValue& value,int feature,int percent,bool saved) {
        if (!saved) return; // Never send a UI placeholder to hardware.
        percent=std::clamp(percent,0,100);
        value.restoring=true;
        if (value.pending && value.percent==percent) return;
        value.percent=percent; value.hasValue=true; value.managed=true;
        value.pending=true; value.failed=false; ++value.generation;
        jobs_[{device,feature}]={percent,value.generation,true,physicalId};
    };
    restoreValue(state.brightness,0,brightness,savedBrightness);
    restoreValue(state.contrast,1,contrast,savedContrast);
    jobs_[{device,-1}]={};
    wake_.notify_one();
}
void DdcController::finishRestore() {
    std::lock_guard<std::mutex> lock(mutex_);
    restoreWindow_=false;
    for(auto& item:states_) for(auto* value:{&item.second.brightness,&item.second.contrast}) {
        if(value->restoring && !value->pending && !value->failed) {
            value->restoring=false;
            value->protectUntilTickMs=GetTickCount64()+5000;
        }
    }
}
void DdcController::invalidateTopology() {
    std::lock_guard<std::mutex> lock(mutex_);
    ++topologyEpoch_;
    restoreWindow_=false;
    jobs_.clear();
    states_.clear();
    readingDevice_.clear();
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
        readingDevice_==device || jobs_.find({device,-1})!=jobs_.end()) return;
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
    if (value.managed && value.hasValue && value.percent==percent && !value.failed && !value.restoring) return true;
    value.restoring=false;
    value.percent=percent; value.hasValue=true; value.managed=true; value.pending=true; value.failed=false; ++value.generation;
    const bool dimWrite=feature==DdcFeature::Brightness && value.dimOwned;
    if(dimWrite) {
        value.dimAttempts=1;
        value.dimRetryAt=GetTickCount64()+5000;
        jobs_[{device,0}]={dimmedBrightness(percent,value.dimAmount),value.generation,
            false,found->second.dimPhysicalId,true};
    } else jobs_[{device,static_cast<int>(feature)}]={percent,value.generation};
    wake_.notify_one();
    return true;
}
void DdcController::setTemporaryDim(const std::wstring& device,const std::wstring& physicalId,int amount) {
    if(device.empty() || physicalId.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto found=states_.find(device);
    if(stopping_ || restoreWindow_ || found==states_.end()) return;
    auto& state=found->second;
    auto& value=state.brightness;
    amount=std::clamp(amount,0,99);
    if(!value.dimOwned && amount==0) return;
    if(value.dimOwned && state.dimPhysicalId!=physicalId) return;
    if(amount>0 && (!value.supported || !value.managed || !value.hasValue ||
                    value.restoring || (!value.dimOwned && (state.loading || value.pending || value.failed)))) return;
    const auto now=GetTickCount64();
    const bool changed=amount!=value.dimAmount;
    if(!changed && value.dimOwned) {
        if(value.pending || now<value.dimRetryAt) return;
        if(!value.failed) {
            // A successful normal-brightness write still needs a settled
            // readback before releasing the protected baseline.
            if(amount==0 && !state.loading && readingDevice_!=device &&
               jobs_.find({device,-1})==jobs_.end()) {
                jobs_[{device,-1}]={};
                value.dimRetryAt=now+3000;
                wake_.notify_one();
            }
            return;
        }
        if(value.dimAttempts>=3) return;
    }
    if(changed) value.dimAttempts=0;
    value.dimOwned=true;
    value.dimAmount=amount;
    state.dimPhysicalId=physicalId;
    ++value.dimAttempts;
    value.dimRetryAt=now+5000;
    value.pending=true; value.failed=false; ++value.generation;
    jobs_[{device,0}]={dimmedBrightness(value.percent,amount),value.generation,false,physicalId,true};
    wake_.notify_one();
}
bool DdcController::dimRestoreOutstanding() {
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& entry:states_) {
        const auto& v=entry.second.brightness;
        if(v.dimOwned && (v.dimAmount!=0 || v.pending || !v.failed || v.dimAttempts<3)) return true;
    }
    return false;
}
void DdcController::restoreDimmedBeforeStop() {
    // All dim targets have a durable normal-brightness profile. Restore on
    // clean exit before stopping the worker; never write to a recycled ID.
    std::vector<std::pair<std::wstring,std::wstring>> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(stopping_ || restoreWindow_) return;
        for(auto& entry:states_) if(entry.second.brightness.dimOwned) {
            auto& value=entry.second.brightness;
            // Force one fresh retry even if earlier attempts exhausted.
            value.dimRetryAt=0; value.dimAttempts=0;
            targets.emplace_back(entry.first,entry.second.dimPhysicalId);
        }
    }
    for(const auto& target:targets) setTemporaryDim(target.first,target.second,0);
    std::unique_lock<std::mutex> lock(mutex_);
    wake_.wait_for(lock,std::chrono::seconds(12),[&] {
        for(const auto& entry:states_)
            if(entry.second.brightness.dimOwned && entry.second.brightness.pending) return false;
        return true;
    });
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
    std::optional<Api> apiStorage;
    std::optional<WmiBrightness> wmiStorage;
    std::uint64_t lastEpoch=0;
    std::pair<std::wstring,int> previous;
    bool first=true;
    for (;;) {
        std::pair<std::wstring,int> key; Job job;
        std::uint64_t epoch=0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto ready=[&]{return stopping_ || !jobs_.empty();};
            if(wmiStorage || apiStorage) {
                if(!wake_.wait_for(lock,std::chrono::seconds(30),ready)) {
                    lock.unlock();
                    wmiStorage.reset();
                    apiStorage.reset();
                    continue;
                }
            } else wake_.wait(lock,ready);
            if (stopping_) return;
            const auto next=nextMonitorJob(jobs_,previous,first);
            key=next->first; job=next->second; jobs_.erase(next);
            epoch=topologyEpoch_;
            if(key.second==-1) readingDevice_=key.first;
            previous=key; first=false;
        }
        {
        std::unique_lock<std::mutex> physicalLock(physicalDdcMutex());
        { std::lock_guard<std::mutex> lock(mutex_);
          if(stopping_) return;
          if(epoch!=topologyEpoch_) continue; }
        if(!apiStorage) apiStorage.emplace();
        if(!wmiStorage) wmiStorage.emplace();
        if(lastEpoch!=epoch) { wmiStorage->clearCache(); lastEpoch=epoch; }
        auto& api=*apiStorage;
        auto& wmi=*wmiStorage;
        Physical monitor(api,key.first);
        if (key.second==-1) {
            const DdcValue brightness=observe(api,monitor,wmi,key.first,0), contrast=observe(api,monitor,wmi,key.first,1);
            std::lock_guard<std::mutex> lock(mutex_);
            // A display sleep/re-enumeration may have reassigned this GDI
            // output while the slow DDC read was in progress.
            if (epoch!=topologyEpoch_ || stopping_) continue;
            readingDevice_.clear();
            auto& state=states_[key.first];
            const bool wasLoading=state.loading;
            const auto beforeBrightness=state.brightness,beforeContrast=state.contrast;
            state.loading=false;
            const auto now=GetTickCount64();
            auto publish=[this,now](DdcValue& value,const DdcValue& observed) {
                // A failed restoration must not be replaced by a waking
                // panel's defaults. Release protection only on matching read,
                // a successful restore, or an explicit new user adjustment.
                if(!restoreWindow_ && value.restoring && !value.pending &&
                   observed.readable && observed.hasValue && observed.percent==value.percent) {
                    value.restoring=false;
                    value.failed=false;
                }
                const bool protect=value.pending || now<value.protectUntilTickMs;
                if(restoreWindow_ && !protect) verifyDdcRestore(value,observed);
                applyDdcObservation(value,observed,protect);
            };
            auto& dim=state.brightness;
            if(dim.dimOwned && dim.dimAmount==0 && !dim.pending && now>=dim.protectUntilTickMs) {
                if(brightness.readable && brightness.hasValue) {
                    if(std::abs(dim.percent-brightness.percent)<=1) {
                        dim.dimOwned=false; dim.failed=false;
                        // Keep the exact saved target rather than adopt
                        // a one-point quantization difference.
                        dim.protectUntilTickMs=now+5000;
                    } else { dim.failed=true; dim.dimRetryAt=now+1000; }
                } else if(!dim.failed) {
                    // A setter-only display cannot be independently read.
                    dim.dimOwned=false;
                    dim.protectUntilTickMs=now+5000;
                }
            }
            publish(state.brightness,brightness);
            publish(state.contrast,contrast);
            const auto changed=[](const DdcValue& a,const DdcValue& b){
                return a.supported!=b.supported || a.readable!=b.readable ||
                    a.hasValue!=b.hasValue || a.managed!=b.managed || a.failed!=b.failed ||
                    a.percent!=b.percent || a.backend!=b.backend;
            };
            if (window_ && !stopping_ && (wasLoading ||
                changed(beforeBrightness,state.brightness) || changed(beforeContrast,state.contrast)))
                PostMessageW(window_,WM_DDC_COMPLETE,0,0);
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (stopping_) return;
                if (epoch!=topologyEpoch_) continue;
                const auto found=states_.find(key.first);
                if(found==states_.end()) continue;
                const auto& state=found->second;
                const auto& latest=key.second==0?state.brightness:state.contrast;
                if (latest.generation!=job.generation) continue;
            }
            // A saved restoration job must still address the same physical
            // panel at execution time, not merely the old DISPLAYx number.
            bool correctTarget=true;
            if((job.restoring || job.dimWrite) && !job.physicalId.empty()) {
                const auto snapshot=queryPhysicalDisplayTargets();
                correctTarget=matchPhysicalTarget(snapshot,key.first,job.physicalId)!=nullptr;
            }
            bool usedWmi=false;
            bool ok=correctTarget && writePhysical(api,monitor,key.second,job.percent,job.restoring || job.dimWrite);
            // Upstream also attempts WMI after DDC/CI. Match that behavior for
            // the selected WMI instance, including drivers that return success
            // from SetMonitorBrightness without changing an internal panel.
            if (correctTarget && key.second==0) {
                const auto panel=wmi.find(key.first);
                if (panel && ((!job.restoring && !job.dimWrite && panel.readable && panel.percent==job.percent) || wmi.set(panel,job.percent))) { ok=true; usedWmi=true; }
            }
            std::lock_guard<std::mutex> lock(mutex_);
            if (epoch!=topologyEpoch_ || stopping_) continue;
            auto& state=states_[key.first];
            auto& value=key.second==0?state.brightness:state.contrast;
            if (value.generation==job.generation) {
                value.pending=false; value.failed=!ok;
                if(job.restoring && ok && !restoreWindow_) value.restoring=false;
                if (usedWmi) value.backend=DdcBackend::WindowsWmi;
                // The next poll must not read a monitor's old value while
                // its firmware is still applying our last local write.
                value.protectUntilTickMs=ok?
                    GetTickCount64()+((job.restoring || job.dimWrite)?5000:kDdcPostWriteGraceMs):0;
                value.hasValue=true;
                if(!job.dimWrite) value.percent=job.percent;
                else {
                    value.dimRetryAt=GetTickCount64()+5000;
                    if(ok && value.dimAmount==0 && !value.readable) value.dimOwned=false;
                }
            }
            wake_.notify_all(); // clean exit may be waiting for normal brightness
            if (window_ && !stopping_) PostMessageW(window_,WM_DDC_COMPLETE,0,0);
        }
        } // Destroy physical monitor handles before releasing DDC lock.
        // Bound DDC/CI traffic; pointer events overwrite pending writes.
        { std::unique_lock<std::mutex> lock(mutex_);
          wake_.wait_for(lock,std::chrono::milliseconds(75),[&]{return stopping_;});
          if (stopping_) return; }
    }
}
}
