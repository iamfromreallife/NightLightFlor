#pragma once
#include <windows.h>
#include <vector>
#include <string>
#include <optional>
#include <cwchar>
#include <cerrno>

namespace nightlight {
namespace inactive_dialog {
struct Value { const wchar_t* prompt; int value,min,max; bool portuguese; };
inline INT_PTR CALLBACK procedure(HWND dialog,UINT msg,WPARAM wp,LPARAM lp) {
    auto* data=reinterpret_cast<Value*>(GetWindowLongPtrW(dialog,DWLP_USER));
    if(msg==WM_INITDIALOG) {
        data=reinterpret_cast<Value*>(lp);
        SetWindowLongPtrW(dialog,DWLP_USER,reinterpret_cast<LONG_PTR>(data));
        SetDlgItemTextW(dialog,101,data->prompt);
        SetDlgItemInt(dialog,102,data->value,FALSE);
        SetDlgItemTextW(dialog,IDCANCEL,data->portuguese?L"Cancelar":L"Cancel");
        SendDlgItemMessageW(dialog,102,EM_SETLIMITTEXT,4,0);
        SetFocus(GetDlgItem(dialog,102));
        SendDlgItemMessageW(dialog,102,EM_SETSEL,0,-1);
        return FALSE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK && data) {
        wchar_t text[16]{};
        GetDlgItemTextW(dialog,102,text,16);
        wchar_t* end=nullptr; errno=0;
        const long value=std::wcstol(text,&end,10);
        if(errno || end==text || *end || value<data->min || value>data->max) {
            std::wstring message=(data->portuguese?L"Introduza um valor entre ":L"Enter a value between ")+
                std::to_wstring(data->min)+(data->portuguese?L" e ":L" and ")+std::to_wstring(data->max)+L".";
            MessageBoxW(dialog,message.c_str(),L"NightLight",MB_OK|MB_ICONWARNING);
            return TRUE;
        }
        data->value=static_cast<int>(value);
        EndDialog(dialog,IDOK); return TRUE;
    }
    if((msg==WM_COMMAND && LOWORD(wp)==IDCANCEL) || msg==WM_CLOSE) {
        EndDialog(dialog,IDCANCEL); return TRUE;
    }
    return FALSE;
}
// Standard dialog template: native controls, no new runtime or taskbar window.
inline std::optional<int> number(HINSTANCE instance,HWND owner,const wchar_t* title,
    const wchar_t* prompt,int current,int min,int max,bool portuguese) {
    std::vector<WORD> words;
    auto word=[&](WORD v){words.push_back(v);};
    auto dword=[&](DWORD v){word(static_cast<WORD>(v));word(static_cast<WORD>(v>>16));};
    auto string=[&](const wchar_t* s){while(*s)word(static_cast<WORD>(*s++));word(0);};
    dword(WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER|DS_SETFONT);
    dword(0);word(4);word(0);word(0);word(208);word(78);
    word(0);word(0);string(title);word(9);string(L"Segoe UI");
    auto item=[&](DWORD style,short x,short y,short width,short height,WORD id,WORD cls,const wchar_t* text) {
        if(words.size()%2)word(0);
        dword(WS_CHILD|WS_VISIBLE|style);dword(0);
        word(x);word(y);word(width);word(height);word(id);
        word(0xffff);word(cls);string(text);word(0);
    };
    item(SS_LEFT,10,8,188,12,101,0x82,L"");
    item(WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,10,25,188,15,102,0x81,L"");
    item(WS_TABSTOP|BS_DEFPUSHBUTTON,96,55,48,14,IDOK,0x80,L"OK");
    item(WS_TABSTOP|BS_PUSHBUTTON,150,55,48,14,IDCANCEL,0x80,L"Cancel");
    Value data{prompt,current,min,max,portuguese};
    // vector allocation is suitably aligned for DLGTEMPLATE.
    const auto result=DialogBoxIndirectParamW(instance,
        reinterpret_cast<const DLGTEMPLATE*>(words.data()),owner,procedure,reinterpret_cast<LPARAM>(&data));
    return result==IDOK?std::optional<int>(data.value):std::nullopt;
}
}
}
