#include "display_identity.hpp"
#include <algorithm>
#include <cwctype>
#include <set>

namespace nightlight {
std::wstring normalizedDisplayId(std::wstring id) {
    std::transform(id.begin(),id.end(),id.begin(),
        [](wchar_t c){return static_cast<wchar_t>(std::towupper(c));});
    return id;
}
std::wstring monitorProfileKey(const Monitor& monitor) {
    // Keep the legacy GDI key as a fallback on drivers which cannot expose
    // the display target interface path. Never replace the path with a model
    // name or EDID product code: two identical panels may share those.
    return monitor.physicalId.empty()?monitor.device:
        L"physical:"+normalizedDisplayId(monitor.physicalId);
}
bool samePhysicalMonitor(const Monitor& first,const Monitor& second) {
    if (!first.physicalId.empty() && !second.physicalId.empty())
        return normalizedDisplayId(first.physicalId)==
               normalizedDisplayId(second.physicalId);
    // If one side has physical identification, a reused DISPLAYx number is
    // NOT proof that it is the same monitor.
    return first.physicalId.empty() && second.physicalId.empty() &&
        first.device==second.device && first.windowsName==second.windowsName;
}
bool wakeTopologyIsUsable(const std::vector<Monitor>& previous,
                          const std::vector<Monitor>& detected) {
    if (detected.empty() || detected.size()<previous.size()) return false;
    std::set<std::wstring> sources,physical;
    for (const auto& monitor:detected) {
        if (monitor.device.empty() ||
            !sources.insert(normalizedDisplayId(monitor.device)).second ||
            !physical.insert(monitorProfileKey(monitor)).second) return false;
    }
    // A transient one-monitor or partially resolved enumeration must not
    // inherit another screen's name, temperature, brightness or contrast.
    for (const auto& old:previous)
        if (std::none_of(detected.begin(),detected.end(),
                         [&](const Monitor& fresh) {
                             return samePhysicalMonitor(old,fresh);
                         })) return false;
    return true;
}
const DisplayTarget* matchPhysicalTarget(const DisplayTargetSnapshot& snapshot,
                                         const std::wstring& device,
                                         const std::wstring& physicalId) {
    if(!snapshot.complete || physicalId.empty()) return nullptr;
    const auto it=snapshot.bySource.find(normalizedDisplayId(device));
    if(it==snapshot.bySource.end() || it->second.size()!=1) return nullptr;
    const auto& target=it->second.front();
    return normalizedDisplayId(target.physicalId)==normalizedDisplayId(physicalId)?
        &target:nullptr;
}
DisplayTargetSnapshot queryPhysicalDisplayTargets() {
    DisplayTargetSnapshot result;
    constexpr UINT32 flags=QDC_ONLY_ACTIVE_PATHS|QDC_VIRTUAL_MODE_AWARE;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG status=ERROR_INSUFFICIENT_BUFFER;
    // QueryDisplayConfig can race a topology change between its sizing and
    // data queries. Retry with new buffer sizes rather than using partial data.
    for (unsigned attempt=0;attempt<5;++attempt) {
        UINT32 pathCount=0,modeCount=0;
        status=GetDisplayConfigBufferSizes(flags,&pathCount,&modeCount);
        if (status!=ERROR_SUCCESS) return result;
        paths.resize(pathCount);
        modes.resize(modeCount);
        status=QueryDisplayConfig(flags,&pathCount,paths.data(),
                                  &modeCount,modes.data(),nullptr);
        if (status==ERROR_INSUFFICIENT_BUFFER) continue;
        if (status!=ERROR_SUCCESS) return result;
        paths.resize(pathCount);
        break;
    }
    if (status!=ERROR_SUCCESS || paths.empty()) return result;
    result.complete=true;
    for (const auto& path:paths) {
        // Windows can still report an active path during unplug/wake while
        // targetAvailable is false. The snapshot is not reliable in that case.
        if (!(path.flags & DISPLAYCONFIG_PATH_ACTIVE) ||
            !path.targetInfo.targetAvailable) {
            result.complete=false;
            continue;
        }
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size=sizeof(source);
        source.header.adapterId=path.sourceInfo.adapterId;
        source.header.id=path.sourceInfo.id;
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header.type=DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        target.header.size=sizeof(target);
        target.header.adapterId=path.targetInfo.adapterId;
        target.header.id=path.targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header)!=ERROR_SUCCESS ||
            DisplayConfigGetDeviceInfo(&target.header)!=ERROR_SUCCESS ||
            !source.viewGdiDeviceName[0] || !target.monitorDevicePath[0]) {
            result.complete=false;
            continue;
        }
        result.bySource[normalizedDisplayId(source.viewGdiDeviceName)].push_back({
            normalizedDisplayId(target.monitorDevicePath),
            target.monitorFriendlyDeviceName,
            path.targetInfo.adapterId,
            path.targetInfo.id
        });
    }
    return result;
}
}
