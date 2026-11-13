#pragma once
#include <windows.h>
namespace nightlight::system_start {
constexpr wchar_t kTaskName[] = L"NightLightAllUsers";
constexpr wchar_t kServiceName[] = L"NightLightBackground";
bool taskInstalled();
bool serviceInstalled();
// Called only in a UAC-elevated helper process.
DWORD setTaskEnabled(bool enabled);
DWORD setServiceEnabled(bool enabled);
// Runs only as a Windows Service, in Session 0. No user-interface or
// system-wide gamma changes occur in Session 0.
DWORD runService();
}
