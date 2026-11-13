#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "system_start.hpp"
#include <windows.h>
#include <shlobj.h>
#include <taskschd.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <algorithm>

namespace nightlight::system_start {
using Microsoft::WRL::ComPtr;
namespace {
struct Bstr {
    BSTR b=nullptr;
    explicit Bstr(const wchar_t* text):b(SysAllocString(text)) {}
    explicit Bstr(const std::wstring& text):b(SysAllocString(text.c_str())) {}
    ~Bstr() { SysFreeString(b); }
    operator BSTR() const { return b; }
    Bstr(const Bstr&)=delete;
    Bstr& operator=(const Bstr&)=delete;
};
DWORD errorFrom(HRESULT hr) {
    return SUCCEEDED(hr)?ERROR_SUCCESS:
        (HRESULT_FACILITY(hr)==FACILITY_WIN32?HRESULT_CODE(hr):
         static_cast<DWORD>(hr));
}
std::wstring executablePath() {
    std::wstring path(32768,L'\0');
    const DWORD n=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if (!n || n>=path.size()) return {};
    path.resize(n);
    return path;
}
std::wstring installedPath() {
    PWSTR programFiles=nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&programFiles)) ||
        !programFiles) return {};
    std::wstring result=std::wstring(programFiles)+L"\\NightLight\\NightLight.exe";
    CoTaskMemFree(programFiles);
    return result;
}
DWORD ensureInstalledBinary() {
    const std::wstring dst=installedPath();
    const std::wstring src=executablePath();
    if (src.empty() || dst.empty()) return ERROR_PATH_NOT_FOUND;
    if (_wcsicmp(src.c_str(),dst.c_str())==0) return ERROR_SUCCESS;
    const size_t pos=dst.find_last_of(L'\\');
    if (pos==std::wstring::npos) return ERROR_BAD_PATHNAME;
    const std::wstring directory=dst.substr(0,pos);
    if (!CreateDirectoryW(directory.c_str(),nullptr) &&
        GetLastError()!=ERROR_ALREADY_EXISTS) return GetLastError();
    // Program Files inherits admin-only write ACLs, unlike portable folders.
    // Never install a LocalSystem service referencing a user-writable binary.
    if (!CopyFileW(src.c_str(),dst.c_str(),FALSE)) {
        // An existing installed binary may be locked by another session.
        // It is already at the protected destination and remains usable.
        if (GetLastError()==ERROR_SHARING_VIOLATION ||
            GetLastError()==ERROR_ACCESS_DENIED) {
            if (GetFileAttributesW(dst.c_str())!=INVALID_FILE_ATTRIBUTES)
                return ERROR_SUCCESS;
        }
        return GetLastError();
    }
    return ERROR_SUCCESS;
}
class ComSession {
    HRESULT hr_;
public:
    ComSession():hr_(CoInitializeEx(nullptr,COINIT_MULTITHREADED)) {}
    ~ComSession() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr_); }
    DWORD error() const { return errorFrom(hr_); }
};
HRESULT openTaskFolder(ComPtr<ITaskFolder>& folder) {
    ComPtr<ITaskService> service;
    HRESULT hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&service));
    if (FAILED(hr)) return hr;
    VARIANT empty{}; VariantInit(&empty);
    hr=service->Connect(empty,empty,empty,empty);
    if (FAILED(hr)) return hr;
    return service->GetFolder(Bstr(L"\\"),&folder);
}
bool isTaskPresent() {
    ComSession com;
    if (!com.ok()) return false;
    ComPtr<ITaskFolder> folder;
    if (FAILED(openTaskFolder(folder))) return false;
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED(folder->GetTask(Bstr(kTaskName),&task));
}
DWORD changeTask(bool enabled) {
    ComSession com;
    if (!com.ok()) return com.error();
    ComPtr<ITaskFolder> folder;
    HRESULT hr=openTaskFolder(folder);
    if (FAILED(hr)) return errorFrom(hr);
    if (!enabled) {
        hr=folder->DeleteTask(Bstr(kTaskName),0);
        if (hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return ERROR_SUCCESS;
        return errorFrom(hr);
    }
    const DWORD installed=ensureInstalledBinary();
    if (installed!=ERROR_SUCCESS) return installed;
    ComPtr<ITaskService> service;
    hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&service));
    if (FAILED(hr)) return errorFrom(hr);
    VARIANT empty{}; VariantInit(&empty);
    hr=service->Connect(empty,empty,empty,empty);
    if (FAILED(hr)) return errorFrom(hr);
    ComPtr<ITaskDefinition> definition;
    hr=service->NewTask(0,&definition);
    if (FAILED(hr)) return errorFrom(hr);
    ComPtr<IRegistrationInfo> registration;
    hr=definition->get_RegistrationInfo(&registration);
    if (FAILED(hr)) return errorFrom(hr);
    registration->put_Description(Bstr(
        L"Starts NightLight in each signed-in user's interactive desktop."));
    ComPtr<IPrincipal> principal;
    hr=definition->get_Principal(&principal);
    if (FAILED(hr)) return errorFrom(hr);
    hr=principal->put_GroupId(Bstr(L"S-1-5-32-545")); // BUILTIN\Users, locale-independent
    if (FAILED(hr)) return errorFrom(hr);
    hr=principal->put_LogonType(TASK_LOGON_GROUP);
    if (FAILED(hr)) return errorFrom(hr);
    hr=principal->put_RunLevel(TASK_RUNLEVEL_LUA);
    if (FAILED(hr)) return errorFrom(hr);
    ComPtr<ITriggerCollection> triggers;
    hr=definition->get_Triggers(&triggers);
    if (FAILED(hr)) return errorFrom(hr);
    ComPtr<ITrigger> trigger;
    hr=triggers->Create(TASK_TRIGGER_LOGON,&trigger);
    if (FAILED(hr)) return errorFrom(hr);
    // No UserId means every member of the principal's Users group.
    ComPtr<IActionCollection> actions;
    hr=definition->get_Actions(&actions);
    if (FAILED(hr)) return errorFrom(hr);
    ComPtr<IAction> action;
    hr=actions->Create(TASK_ACTION_EXEC,&action);
    if (FAILED(hr)) return errorFrom(hr);
    ComPtr<IExecAction> exec;
    hr=action.As(&exec);
    if (FAILED(hr)) return errorFrom(hr);
    const std::wstring path=installedPath();
    hr=exec->put_Path(Bstr(path));
    if (FAILED(hr)) return errorFrom(hr);
    hr=exec->put_Arguments(Bstr(L"--managed --tray --quiet-duplicate"));
    if (FAILED(hr)) return errorFrom(hr);
    ComPtr<ITaskSettings> settings;
    hr=definition->get_Settings(&settings);
    if (FAILED(hr)) return errorFrom(hr);
    settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
    settings->put_StopIfGoingOnBatteries(VARIANT_FALSE);
    settings->put_StartWhenAvailable(VARIANT_TRUE);
    settings->put_ExecutionTimeLimit(Bstr(L"PT0S"));
    settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
    settings->put_Enabled(VARIANT_TRUE);
    ComPtr<IRegisteredTask> registered;
    hr=folder->RegisterTaskDefinition(Bstr(kTaskName),definition.Get(),
        TASK_CREATE_OR_UPDATE,empty,empty,TASK_LOGON_GROUP,empty,&registered);
    return errorFrom(hr);
}
DWORD removeService() {
    SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    SC_HANDLE svc=OpenServiceW(scm,kServiceName,DELETE|SERVICE_STOP|SERVICE_QUERY_STATUS);
    if (!svc) {
        const DWORD error=GetLastError();
        CloseServiceHandle(scm);
        return error==ERROR_SERVICE_DOES_NOT_EXIST?ERROR_SUCCESS:error;
    }
    SERVICE_STATUS status{};
    ControlService(svc,SERVICE_CONTROL_STOP,&status);
    const BOOL removed=DeleteService(svc);
    const DWORD result=removed?ERROR_SUCCESS:GetLastError();
    CloseServiceHandle(svc); CloseServiceHandle(scm);
    return result==ERROR_SERVICE_MARKED_FOR_DELETE?ERROR_SUCCESS:result;
}
DWORD createService() {
    const DWORD installed=ensureInstalledBinary();
    if (installed!=ERROR_SUCCESS) return installed;
    SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,
        SC_MANAGER_CREATE_SERVICE|SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    SC_HANDLE svc=OpenServiceW(scm,kServiceName,
        SERVICE_QUERY_STATUS|SERVICE_START|SERVICE_CHANGE_CONFIG);
    const std::wstring command=L"\""+installedPath()+L"\" --nightlight-service";
    if (!svc) {
        svc=CreateServiceW(scm,kServiceName,L"NightLight background session service",
            SERVICE_QUERY_STATUS|SERVICE_START|SERVICE_CHANGE_CONFIG,
            SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,
            command.c_str(),nullptr,nullptr,nullptr,nullptr,nullptr);
        if (!svc) {
            const DWORD error=GetLastError();
            CloseServiceHandle(scm);
            return error;
        }
    } else if (!ChangeServiceConfigW(svc,SERVICE_NO_CHANGE,SERVICE_AUTO_START,
               SERVICE_NO_CHANGE,command.c_str(),nullptr,nullptr,nullptr,nullptr,
               nullptr,L"NightLight background session service")) {
        const DWORD error=GetLastError();
        CloseServiceHandle(svc);CloseServiceHandle(scm);return error;
    }
    SERVICE_DESCRIPTIONW description{const_cast<LPWSTR>(
        L"Starts a per-user NightLight agent after interactive sign-in; does not control UAC Secure Desktop.")};
    ChangeServiceConfig2W(svc,SERVICE_CONFIG_DESCRIPTION,&description);
    SERVICE_STATUS_PROCESS status{};
    DWORD bytes=0;
    if (QueryServiceStatusEx(svc,SC_STATUS_PROCESS_INFO,
            reinterpret_cast<LPBYTE>(&status),sizeof(status),&bytes) &&
        status.dwCurrentState==SERVICE_STOPPED) {
        if (!StartServiceW(svc,0,nullptr)) {
            const DWORD error=GetLastError();
            if (error!=ERROR_SERVICE_ALREADY_RUNNING) {
                CloseServiceHandle(svc);CloseServiceHandle(scm);return error;
            }
        }
    }
    CloseServiceHandle(svc);CloseServiceHandle(scm);
    return ERROR_SUCCESS;
}
bool hasRunningAgent(DWORD sessionId) {
    HANDLE snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if (snapshot==INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found=false;
    if (Process32FirstW(snapshot,&entry)) do {
        if (_wcsicmp(entry.szExeFile,L"NightLight.exe")!=0) continue;
        DWORD processSession=0;
        if (!ProcessIdToSessionId(entry.th32ProcessID,&processSession) ||
            processSession!=sessionId) continue;
        HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,entry.th32ProcessID);
        if (!process) continue;
        wchar_t path[32768]{};
        DWORD length=32768;
        if (QueryFullProcessImageNameW(process,0,path,&length) &&
            _wcsicmp(path,installedPath().c_str())==0) found=true;
        CloseHandle(process);
        if (found) break;
    } while (Process32NextW(snapshot,&entry));
    CloseHandle(snapshot);
    return found;
}
void startUserAgent(DWORD sessionId) {
    if (sessionId==0 || hasRunningAgent(sessionId)) return;
    HANDLE token=nullptr;
    if (!WTSQueryUserToken(sessionId,&token)) return;
    LPVOID environment=nullptr;
    CreateEnvironmentBlock(&environment,token,FALSE);
    const std::wstring exe=installedPath();
    std::wstring command=L"\""+exe+L"\" --managed --tray --quiet-duplicate";
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop=const_cast<LPWSTR>(L"winsta0\\default");
    PROCESS_INFORMATION process{};
    const BOOL started=CreateProcessAsUserW(token,exe.c_str(),command.data(),
        nullptr,nullptr,FALSE,
        environment?CREATE_UNICODE_ENVIRONMENT:0,
        environment,nullptr,&startup,&process);
    if (started) {CloseHandle(process.hThread);CloseHandle(process.hProcess);}
    if (environment) DestroyEnvironmentBlock(environment);
    CloseHandle(token);
}
void refreshSessions() {
    WTS_SESSION_INFOW* sessions=nullptr;
    DWORD count=0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE,0,1,&sessions,&count))
        return;
    for (DWORD i=0;i<count;++i)
        if (sessions[i].State==WTSActive)
            startUserAgent(sessions[i].SessionId);
    WTSFreeMemory(sessions);
}
SERVICE_STATUS_HANDLE statusHandle=nullptr;
SERVICE_STATUS serviceStatus{};
HANDLE stopEvent=nullptr;
HANDLE wakeEvent=nullptr;
void reportServiceStatus(DWORD state,DWORD error=ERROR_SUCCESS) {
    serviceStatus.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState=state;
    serviceStatus.dwControlsAccepted=
        state==SERVICE_RUNNING?(SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN|
            SERVICE_ACCEPT_SESSIONCHANGE):0;
    serviceStatus.dwWin32ExitCode=error;
    SetServiceStatus(statusHandle,&serviceStatus);
}
DWORD WINAPI serviceControl(DWORD code,DWORD,void*,void*) {
    if (code==SERVICE_CONTROL_STOP || code==SERVICE_CONTROL_SHUTDOWN) {
        reportServiceStatus(SERVICE_STOP_PENDING);
        if (stopEvent) SetEvent(stopEvent);
    } else if (code==SERVICE_CONTROL_SESSIONCHANGE) {
        if (wakeEvent) SetEvent(wakeEvent);
    }
    return ERROR_SUCCESS;
}
void WINAPI serviceMain(DWORD,LPWSTR*) {
    statusHandle=RegisterServiceCtrlHandlerExW(kServiceName,serviceControl,nullptr);
    if (!statusHandle) return;
    reportServiceStatus(SERVICE_START_PENDING);
    stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    wakeEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if (!stopEvent || !wakeEvent) {
        reportServiceStatus(SERVICE_STOPPED,GetLastError());return;
    }
    reportServiceStatus(SERVICE_RUNNING);
    HANDLE events[]={stopEvent,wakeEvent};
    // Also verify running sessions periodically, covering fast user switches.
    while (true) {
        refreshSessions();
        const DWORD result=WaitForMultipleObjects(2,events,FALSE,15000);
        if (result==WAIT_OBJECT_0 || result==WAIT_FAILED) break;
    }
    CloseHandle(wakeEvent);wakeEvent=nullptr;
    CloseHandle(stopEvent);stopEvent=nullptr;
    reportServiceStatus(SERVICE_STOPPED);
}
} // namespace

bool taskInstalled() {return isTaskPresent();}
bool serviceInstalled() {
    SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc=OpenServiceW(scm,kServiceName,SERVICE_QUERY_STATUS);
    const bool exists=svc!=nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return exists;
}
DWORD setTaskEnabled(bool enabled) {return changeTask(enabled);}
DWORD setServiceEnabled(bool enabled) {return enabled?createService():removeService();}
DWORD runService() {
    SERVICE_TABLE_ENTRYW table[]={
        {const_cast<LPWSTR>(kServiceName),serviceMain},{nullptr,nullptr}};
    if (!StartServiceCtrlDispatcherW(table)) return GetLastError();
    return ERROR_SUCCESS;
}
} // namespace nightlight::system_start
