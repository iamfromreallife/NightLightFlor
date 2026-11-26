#pragma once
#include "core.hpp"
#include <map>
#include <vector>

namespace nightlight {
struct DisplayTarget {
    std::wstring physicalId;
    std::wstring friendlyName;
    LUID adapterId{};
    UINT32 targetId=0;
};
struct DisplayTargetSnapshot {
    bool complete=false;
    std::map<std::wstring,std::vector<DisplayTarget>> bySource;
};
std::wstring normalizedDisplayId(std::wstring id);
DisplayTargetSnapshot queryPhysicalDisplayTargets();
// Borrowed pointer is valid only for the lifetime of the fresh snapshot.
const DisplayTarget* matchPhysicalTarget(const DisplayTargetSnapshot& snapshot,
                                         const std::wstring& device,
                                         const std::wstring& physicalId);
}
