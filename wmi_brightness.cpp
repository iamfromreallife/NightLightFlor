#include "core.hpp"
#include "wmi_brightness.hpp"
#include <wbemidl.h>
#include <algorithm>
#include <cwctype>
#include <vector>

namespace nightlight {
std::wstring normalizeMonitorInstance(std::wstring value) {
    std::transform(value.begin(),value.end(),value.begin(),[](wchar_t c){return std::towupper(c);});
    if (value.rfind(L"\\\\?\\",0)==0) value.erase(0,4);
    std::replace(value.begin(),value.end(),L'#',L'\\');
    const auto guid=value.find(L"\\{"); if (guid!=std::wstring::npos) value.erase(guid);
    const auto suffix=value.rfind(L'_');
    if (suffix!=std::wstring::npos && suffix>value.rfind(L'\\') && suffix+1<value.size() &&
        std::all_of(value.begin()+suffix+1,value.end(),[](wchar_t c){return c>=L'0' && c<=L'9';})) value.erase(suffix);
    if (value.rfind(L"MONITOR\\",0)==0) value.replace(0,8,L"DISPLAY\\");
    return value;
}
namespace {
template<class T> struct Com {
    T* value=nullptr;
    ~Com() { if (value) value->Release(); }
    T** out() { return &value; }
    T* operator->() const { return value; }
    operator T*() const { return value; }
};
struct Bstr {
    BSTR value;
    explicit Bstr(const wchar_t* text):value(SysAllocString(text)) {}
    explicit Bstr(const std::wstring& text):Bstr(text.c_str()) {}
    ~Bstr() { SysFreeString(value); }
    operator BSTR() const { return value; }
};
struct Variant {
    VARIANT value;
    Variant() { VariantInit(&value); }
    ~Variant() { VariantClear(&value); }
};
std::wstring stringProperty(IWbemClassObject* object,const wchar_t* property) {
    Variant v;
    if (SUCCEEDED(object->Get(property,0,&v.value,nullptr,nullptr)) && v.value.vt==VT_BSTR && v.value.bstrVal)
        return std::wstring(v.value.bstrVal,SysStringLen(v.value.bstrVal));
    return {};
}
bool active(IWbemClassObject* object) {
    Variant v;
    return FAILED(object->Get(L"Active",0,&v.value,nullptr,nullptr)) || v.value.vt!=VT_BOOL || v.value.boolVal!=VARIANT_FALSE;
}
bool numberProperty(IWbemClassObject* object,const wchar_t* property,DWORD& number) {
    Variant v,converted;
    if (FAILED(object->Get(property,0,&v.value,nullptr,nullptr)) ||
        FAILED(VariantChangeType(&converted.value,&v.value,0,VT_UI4))) return false;
    number=converted.value.ulVal; return true;
}
struct DisplayContext { std::wstring wanted; bool found=false; unsigned count=0; };
BOOL CALLBACK enumerateDisplay(HMONITOR monitor,HDC,LPRECT,LPARAM param) {
    auto& context=*reinterpret_cast<DisplayContext*>(param);
    MONITORINFOEXW info{}; info.cbSize=sizeof(info);
    if (GetMonitorInfoW(monitor,&info)) { ++context.count; if (context.wanted==info.szDevice) context.found=true; }
    return TRUE;
}
}
struct WmiBrightness::Impl {
    HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Com<IWbemServices> services;
    ~Impl() {
        // Release proxies before leaving COM on this worker thread.
        if (services.value) { services.value->Release(); services.value=nullptr; }
        if (SUCCEEDED(initialized)) CoUninitialize();
    }
    bool connect() {
        if (services.value) return true;
        if (FAILED(initialized)) return false;
        Com<IWbemLocator> locator;
        if (FAILED(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,
            reinterpret_cast<void**>(locator.out())))) return false;
        Bstr name(L"ROOT\\WMI");
        if (FAILED(locator->ConnectServer(name,nullptr,nullptr,nullptr,0,nullptr,nullptr,services.out()))) return false;
        if (FAILED(CoSetProxyBlanket(services,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,
            RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE))) {
            services.value->Release(); services.value=nullptr; return false;
        }
        return true;
    }
    template<class Visitor> void query(const wchar_t* text,Visitor visitor) {
        if (!connect()) return;
        Bstr language(L"WQL"),command(text);
        Com<IEnumWbemClassObject> rows;
        if (FAILED(services->ExecQuery(language,command,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY|WBEM_FLAG_ENSURE_LOCATABLE,nullptr,rows.out()))) return;
        for (unsigned i=0;i<64;++i) {
            Com<IWbemClassObject> object; ULONG count=0;
            const HRESULT result=rows->Next(1500,1,object.out(),&count);
            if (FAILED(result) || !count) return;
            visitor(object.value);
        }
    }
};
WmiBrightness::WmiBrightness():impl_(std::make_unique<Impl>()) {}
WmiBrightness::~WmiBrightness()=default;
WmiBrightnessPanel WmiBrightness::find(const std::wstring& device) {
    DisplayContext display{device};
    EnumDisplayMonitors(nullptr,nullptr,enumerateDisplay,reinterpret_cast<LPARAM>(&display));
    if (!display.found) return {};
    std::vector<std::wstring> identities;
    for (DWORD i=0;i<32;++i) {
        DISPLAY_DEVICEW info{}; info.cb=sizeof(info);
        if (!EnumDisplayDevicesW(device.c_str(),i,&info,EDD_GET_DEVICE_INTERFACE_NAME)) break;
        if (info.DeviceID[0] && (info.StateFlags&DISPLAY_DEVICE_ACTIVE))
            identities.push_back(normalizeMonitorInstance(info.DeviceID));
    }
    std::vector<WmiBrightnessPanel> panels;
    impl_->query(L"SELECT * FROM WmiMonitorBrightnessMethods",[&](IWbemClassObject* object) {
        if (!active(object)) return;
        WmiBrightnessPanel panel;
        panel.instance=stringProperty(object,L"InstanceName"); panel.path=stringProperty(object,L"__PATH");
        if (panel) panels.push_back(std::move(panel));
    });
    std::vector<WmiBrightnessPanel> matches;
    for (auto& panel:panels) {
        const auto identity=normalizeMonitorInstance(panel.instance);
        if (std::find(identities.begin(),identities.end(),identity)!=identities.end()) matches.push_back(panel);
    }
    // Upstream broadcasts to all WMI panels. Only use its singleton fallback
    // when the Windows display and WMI panel mapping is unambiguous.
    if (matches.empty() && display.count==1 && panels.size()==1) matches.push_back(panels.front());
    if (matches.size()!=1) return {};
    auto selected=matches.front();
    impl_->query(L"SELECT * FROM WmiMonitorBrightness",[&](IWbemClassObject* object) {
        if (!active(object) || normalizeMonitorInstance(stringProperty(object,L"InstanceName"))!=normalizeMonitorInstance(selected.instance)) return;
        DWORD value=0;
        if (numberProperty(object,L"CurrentBrightness",value) && value<=100) { selected.readable=true; selected.percent=static_cast<int>(value); }
    });
    return selected;
}
bool WmiBrightness::set(const WmiBrightnessPanel& panel,int percent) {
    if (!panel || !impl_->connect()) return false;
    Bstr className(L"WmiMonitorBrightnessMethods"),method(L"WmiSetBrightness"),path(panel.path);
    Com<IWbemClassObject> klass,inputSignature,input;
    if (FAILED(impl_->services->GetObject(className,0,nullptr,klass.out(),nullptr)) ||
        FAILED(klass->GetMethod(method,0,inputSignature.out(),nullptr)) ||
        FAILED(inputSignature->SpawnInstance(0,input.out()))) return false;
    Variant timeout,brightness;
    timeout.value.vt=VT_UI4; timeout.value.ulVal=1;
    brightness.value.vt=VT_UI1; brightness.value.bVal=static_cast<BYTE>(std::clamp(percent,0,100));
    if (FAILED(input->Put(L"Timeout",0,&timeout.value,0)) || FAILED(input->Put(L"Brightness",0,&brightness.value,0))) return false;
    Com<IWbemCallResult> call;
    if (FAILED(impl_->services->ExecMethod(path,method,WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,input,nullptr,call.out()))) return false;
    LONG status=0;
    if (call->GetCallStatus(2500,&status)!=WBEM_S_NO_ERROR || FAILED(status)) return false;
    Com<IWbemClassObject> output;
    if (FAILED(call->GetResultObject(0,output.out())) || !output.value) return false;
    DWORD result=1;
    return numberProperty(output,L"ReturnValue",result) && result==0;
}
}
