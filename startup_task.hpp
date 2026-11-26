#pragma once
#include <windows.h>

namespace nightlight {
// Queries only the task owned by this application for the current user.
// A failed query is distinct from a missing or disabled task.
HRESULT queryStartupTask(bool& enabled);
HRESULT setStartupTask(bool enabled);
}
