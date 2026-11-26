#include "startup_task.hpp"
#include <taskschd.h>
#include <sddl.h>
#include <oleauto.h>
#include <filesystem>
#include <string>
#include <vector>

namespace nightlight {
namespace {
template<class T> struct ComPtr {
    T* value=nullptr;
    ComPtr()=default;
    ComPtr(const ComPtr&)=delete;
    ComPtr& operator=(const ComPtr&)=delete;
    ~ComPtr() { if(value) value->Release(); }
    T** out() { return &value; }
    T* operator->() const { return value; }
};
struct ComApartment {
    HRESULT result=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    ~ComApartment() { if(SUCCEEDED(result)) CoUninitialize(); }
    bool usable() const { return SUCCEEDED(result) || result==RPC_E_CHANGED_MODE; }
};
struct Bstr {
    BSTR value=nullptr;
    explicit Bstr(const wchar_t* text):value(SysAllocString(text)) {}
    explicit Bstr(const std::wstring& text):Bstr(text.c_str()) {}
    ~Bstr() { SysFreeString(value); }
    Bstr(const Bstr&)=delete;
    Bstr& operator=(const Bstr&)=delete;
    operator BSTR() const { return value; }
};
HRESULT winError() {
    const DWORD error=GetLastError();
    return HRESULT_FROM_WIN32(error?error:ERROR_GEN_FAILURE);
}
bool missingTask(HRESULT hr) {
    return hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) ||
           hr==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);
}
HRESULT currentUserSid(std::wstring& sid) {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)) return winError();
    DWORD bytes=0;
    GetTokenInformation(token,TokenUser,nullptr,0,&bytes);
    if(!bytes) { const auto hr=winError(); CloseHandle(token); return hr; }
    std::vector<BYTE> buffer(bytes);
    if(!GetTokenInformation(token,TokenUser,buffer.data(),bytes,&bytes)) {
        const auto hr=winError(); CloseHandle(token); return hr;
    }
    CloseHandle(token);
    LPWSTR text=nullptr;
    if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,&text))
        return winError();
    sid=text;
    LocalFree(text);
    return S_OK;
}
HRESULT executablePath(std::wstring& path) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length=GetModuleFileNameW(nullptr,buffer.data(),static_cast<DWORD>(buffer.size()));
    if(!length) return winError();
    if(length>=buffer.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    path.assign(buffer.data(),length);
    return S_OK;
}
struct Scheduler {
    // COM objects are released before the apartment is uninitialized.
    ComApartment apartment;
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> folder;
    std::wstring sid,name;
    HRESULT connect() {
        if(!apartment.usable()) return apartment.result;
        HRESULT hr=currentUserSid(sid);
        if(FAILED(hr)) return hr;
        // Distinct task per account; never modify another user's task.
        name=L"NightLight-"+sid;
        hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,
            IID_ITaskService,reinterpret_cast<void**>(service.out()));
        if(FAILED(hr)) return hr;
        VARIANT empty{};
        hr=service->Connect(empty,empty,empty,empty);
        if(FAILED(hr)) return hr;
        Bstr root(L"\\");
        if(!root.value) return E_OUTOFMEMORY;
        return service->GetFolder(root,folder.out());
    }
    HRESULT query(bool& enabled) {
        enabled=false;
        Bstr taskName(name);
        if(!taskName.value) return E_OUTOFMEMORY;
        ComPtr<IRegisteredTask> task;
        const HRESULT hr=folder->GetTask(taskName,task.out());
        if(missingTask(hr)) return S_OK;
        if(FAILED(hr)) return hr;
        VARIANT_BOOL state=VARIANT_FALSE;
        const HRESULT status=task->get_Enabled(&state);
        if(SUCCEEDED(status)) enabled=state!=VARIANT_FALSE;
        return status;
    }
    HRESULT remove() {
        Bstr taskName(name);
        if(!taskName.value) return E_OUTOFMEMORY;
        const HRESULT hr=folder->DeleteTask(taskName,0);
        return missingTask(hr)?S_OK:hr;
    }
    HRESULT create() {
        std::wstring exe;
        HRESULT hr=executablePath(exe);
        if(FAILED(hr)) return hr;
        const std::wstring directory=std::filesystem::path(exe).parent_path().wstring();
        Bstr user(sid),taskName(name),path(exe),workdir(directory),
            noLimit(L"PT0S"),description(L"Start NightLight in the system tray at sign-in.");
        if(!user.value || !taskName.value || !path.value || !workdir.value ||
           !noLimit.value || !description.value) return E_OUTOFMEMORY;
        ComPtr<ITaskDefinition> definition;
        ComPtr<IRegistrationInfo> registration;
        ComPtr<IPrincipal> principal;
        ComPtr<ITaskSettings> settings;
        ComPtr<ITriggerCollection> triggers;
        ComPtr<ITrigger> trigger;
        ComPtr<ILogonTrigger> logon;
        ComPtr<IActionCollection> actions;
        ComPtr<IAction> action;
        ComPtr<IExecAction> exec;
        ComPtr<IRegisteredTask> registered;
        // Check every call: a partially configured task is never registered.
        if(FAILED(hr=service->NewTask(0,definition.out())) ||
           FAILED(hr=definition->get_RegistrationInfo(registration.out())) ||
           FAILED(hr=registration->put_Description(description)) ||
           FAILED(hr=definition->get_Principal(principal.out())) ||
           FAILED(hr=principal->put_UserId(user)) ||
           FAILED(hr=principal->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN)) ||
           FAILED(hr=principal->put_RunLevel(TASK_RUNLEVEL_LUA)) ||
           FAILED(hr=definition->get_Settings(settings.out())) ||
           FAILED(hr=settings->put_Enabled(VARIANT_TRUE)) ||
           FAILED(hr=settings->put_DisallowStartIfOnBatteries(VARIANT_FALSE)) ||
           FAILED(hr=settings->put_StopIfGoingOnBatteries(VARIANT_FALSE)) ||
           FAILED(hr=settings->put_ExecutionTimeLimit(noLimit)) ||
           FAILED(hr=settings->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW)) ||
           FAILED(hr=definition->get_Triggers(triggers.out())) ||
           FAILED(hr=triggers->Create(TASK_TRIGGER_LOGON,trigger.out())) ||
           FAILED(hr=trigger->QueryInterface(IID_ILogonTrigger,reinterpret_cast<void**>(logon.out()))) ||
           FAILED(hr=logon->put_UserId(user)) ||
           FAILED(hr=logon->put_Enabled(VARIANT_TRUE)) ||
           FAILED(hr=definition->get_Actions(actions.out())) ||
           FAILED(hr=actions->Create(TASK_ACTION_EXEC,action.out())) ||
           FAILED(hr=action->QueryInterface(IID_IExecAction,reinterpret_cast<void**>(exec.out()))) ||
           FAILED(hr=exec->put_Path(path)) ||
           FAILED(hr=exec->put_WorkingDirectory(workdir))) return hr;
        VARIANT account{},empty{};
        account.vt=VT_BSTR;
        account.bstrVal=user.value; // borrowed; Bstr owns the allocation
        // No password, elevation, shell process, or command-line quoting.
        // Ignore registration triggers; the app is already running.
        return folder->RegisterTaskDefinition(taskName,definition.value,
            TASK_CREATE_OR_UPDATE|TASK_IGNORE_REGISTRATION_TRIGGERS,
            account,empty,TASK_LOGON_INTERACTIVE_TOKEN,empty,registered.out());
    }
};
}
HRESULT queryStartupTask(bool& enabled) {
    enabled=false;
    Scheduler scheduler;
    const HRESULT hr=scheduler.connect();
    return FAILED(hr)?hr:scheduler.query(enabled);
}
HRESULT setStartupTask(bool enabled) {
    Scheduler scheduler;
    HRESULT hr=scheduler.connect();
    if(FAILED(hr)) return hr;
    hr=enabled?scheduler.create():scheduler.remove();
    if(FAILED(hr)) return hr;
    bool actual=false;
    hr=scheduler.query(actual);
    if(FAILED(hr)) return hr;
    return actual==enabled?S_OK:E_FAIL;
}
}
