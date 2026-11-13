#pragma once
#include "core.hpp"
#include <map>
#include <vector>

namespace nightlight {
struct DisplayTarget {
    std::wstring physicalId;
    std::wstring friendlyName;
};
struct DisplayTargetSnapshot {
    bool complete=false;
    std::map<std::wstring,std::vector<DisplayTarget>> bySource;
};
std::wstring normalizedDisplayId(std::wstring id);
DisplayTargetSnapshot queryPhysicalDisplayTargets();
}
