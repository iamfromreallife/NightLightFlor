#pragma once
#include "core.hpp"
#include <commctrl.h>
#include <functional>
#include <vector>
#include <string>
#include <optional>
#include <cwchar>
#include <cerrno>

namespace nightlight::inactive_dialog {
constexpr int Enable=101,Amount=102,Percent=103,Test=104,All=105,Displays=106;
constexpr int Preset=110,Custom=116,Minutes=117;
constexpr UINT_PTR TestTimer=1;
struct State {
    Preferences draft;
    const std::vector<Monitor>& monitors;
    bool pt;
    std::function<bool(const Preferences*)> test;
    bool testing=false;
    const wchar_t* tr(const wchar_t* en,const wchar_t* portugal) const {return pt?portugal:en;}
};
inline void updateAmount(HWND hwnd) {
    const auto value=SendDlgItemMessageW(hwnd,Amount,TBM_GETPOS,0,0);
    SetDlgItemTextW(hwnd,Percent,(std::to_wstring(value)+L"%").c_str());
}
inline void readSelection(HWND hwnd,State& state) {
    state.draft.dimInactive=IsDlgButtonChecked(hwnd,Enable)==BST_CHECKED;
    state.draft.dimAmount=static_cast<int>(SendDlgItemMessageW(hwnd,Amount,TBM_GETPOS,0,0));
    const bool all=IsDlgButtonChecked(hwnd,All)==BST_CHECKED;
    if(all) {
        state.draft.dimAll=true;state.draft.dimDisplays.clear();
    } else {
        state.draft.dimAll=false;
        for(size_t i=0;i<state.monitors.size();++i) {
            const auto key=monitorProfileKey(state.monitors[i]);
            if(SendDlgItemMessageW(hwnd,Displays,LB_GETSEL,i,0)>0) state.draft.dimDisplays.insert(key);
            else state.draft.dimDisplays.erase(key);
        }
    }
}
inline void stopTest(HWND hwnd,State& state) {
    KillTimer(hwnd,TestTimer);
    if(state.testing) {state.testing=false;state.test(nullptr);}
    SetDlgItemTextW(hwnd,Test,state.tr(L"Test",L"Testar"));
}
inline bool readTime(HWND hwnd,State& state) {
    for(size_t i=0;i<kDimMinutePresets.size();++i)
        if(IsDlgButtonChecked(hwnd,Preset+static_cast<int>(i))==BST_CHECKED) {
            state.draft.dimMinutes=kDimMinutePresets[i];return true;
        }
    BOOL ok=FALSE;
    const UINT value=GetDlgItemInt(hwnd,Minutes,&ok,FALSE);
    if(!ok || value<1 || value>1440) {
        MessageBoxW(hwnd,state.tr(L"Enter an inactivity time from 1 to 1440 minutes.",
            L"Introduza um tempo de inatividade entre 1 e 1440 minutos."),L"NightLight",MB_OK|MB_ICONWARNING);
        SetFocus(GetDlgItem(hwnd,Minutes));return false;
    }
    state.draft.dimMinutes=static_cast<int>(value);return true;
}
inline INT_PTR CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    auto* state=reinterpret_cast<State*>(GetWindowLongPtrW(hwnd,DWLP_USER));
    if(msg==WM_INITDIALOG) {
        state=reinterpret_cast<State*>(lp);
        SetWindowLongPtrW(hwnd,DWLP_USER,reinterpret_cast<LONG_PTR>(state));
        CheckDlgButton(hwnd,Enable,state->draft.dimInactive?BST_CHECKED:BST_UNCHECKED);
        SendDlgItemMessageW(hwnd,Amount,TBM_SETRANGE,TRUE,MAKELPARAM(1,99));
        SendDlgItemMessageW(hwnd,Amount,TBM_SETPOS,TRUE,state->draft.dimAmount);
        SendDlgItemMessageW(hwnd,Amount,TBM_SETPAGESIZE,0,10);
        updateAmount(hwnd);
        int selected=Custom;
        for(size_t i=0;i<kDimMinutePresets.size();++i)
            if(state->draft.dimMinutes==kDimMinutePresets[i]) selected=Preset+static_cast<int>(i);
        CheckRadioButton(hwnd,Preset,Custom,selected);
        SetDlgItemInt(hwnd,Minutes,state->draft.dimMinutes,FALSE);
        SendDlgItemMessageW(hwnd,Minutes,EM_SETLIMITTEXT,4,0);
        EnableWindow(GetDlgItem(hwnd,Minutes),selected==Custom);
        CheckDlgButton(hwnd,All,state->draft.dimAll?BST_CHECKED:BST_UNCHECKED);
        for(size_t i=0;i<state->monitors.size();++i) {
            const auto& mon=state->monitors[i];
            const auto name=mon.displayName();
            const auto index=SendDlgItemMessageW(hwnd,Displays,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
            if(index>=0 && state->draft.dims(mon))
                SendDlgItemMessageW(hwnd,Displays,LB_SETSEL,TRUE,index);
        }
        EnableWindow(GetDlgItem(hwnd,Displays),!state->draft.dimAll);
        return TRUE;
    }
    if(!state) return FALSE;
    if(msg==WM_HSCROLL && reinterpret_cast<HWND>(lp)==GetDlgItem(hwnd,Amount)) {
        updateAmount(hwnd);return TRUE;
    }
    if(msg==WM_TIMER && wp==TestTimer) {stopTest(hwnd,*state);return TRUE;}
    if(msg==WM_DISPLAYCHANGE || msg==WM_POWERBROADCAST) {
        stopTest(hwnd,*state);
        return FALSE;
    }
    if(msg==WM_COMMAND) {
        const int id=LOWORD(wp);
        if(id>=Preset && id<=Custom) {
            CheckRadioButton(hwnd,Preset,Custom,id);
            EnableWindow(GetDlgItem(hwnd,Minutes),id==Custom);
            if(id==Custom) SetFocus(GetDlgItem(hwnd,Minutes));
            return TRUE;
        }
        if(id==All && HIWORD(wp)==BN_CLICKED) {
            const bool all=IsDlgButtonChecked(hwnd,All)==BST_CHECKED;
            if(all) SendDlgItemMessageW(hwnd,Displays,LB_SETSEL,TRUE,-1);
            EnableWindow(GetDlgItem(hwnd,Displays),!all);
            return TRUE;
        }
        if(id==Test && HIWORD(wp)==BN_CLICKED) {
            if(state->testing) {stopTest(hwnd,*state);return TRUE;}
            readSelection(hwnd,*state);
            // Arm the automatic release before showing any overlay.
            if(SetTimer(hwnd,TestTimer,3000,nullptr)) {
                state->testing=true;
                if(state->test(&state->draft)) {
                    SetDlgItemTextW(hwnd,Test,state->tr(L"Stop",L"Parar"));
                } else {
                    stopTest(hwnd,*state);
                    MessageBoxW(hwnd,state->tr(L"No overlay could be shown. Select a connected display.",
                        L"Não foi possível mostrar a camada. Selecione um ecrã ligado."),L"NightLight",MB_OK|MB_ICONINFORMATION);
                }
            }
            return TRUE;
        }
        if(id==IDOK) {
            stopTest(hwnd,*state);
            readSelection(hwnd,*state);
            if(!readTime(hwnd,*state)) return TRUE;
            EndDialog(hwnd,IDOK);return TRUE;
        }
        if(id==IDCANCEL) {stopTest(hwnd,*state);EndDialog(hwnd,IDCANCEL);return TRUE;}
    }
    if(msg==WM_CLOSE) {stopTest(hwnd,*state);EndDialog(hwnd,IDCANCEL);return TRUE;}
    if(msg==WM_DESTROY) stopTest(hwnd,*state);
    return FALSE;
}
inline std::optional<Preferences> settings(HINSTANCE instance,HWND owner,
    const Preferences& current,const std::vector<Monitor>& monitors,bool pt,
    std::function<bool(const Preferences*)> test) {
    State state{current,monitors,pt,std::move(test)};
    std::vector<WORD> words;
    auto word=[&](WORD v){words.push_back(v);};
    auto dword=[&](DWORD v){word(static_cast<WORD>(v));word(static_cast<WORD>(v>>16));};
    auto string=[&](const wchar_t* s){while(*s)word(static_cast<WORD>(*s++));word(0);};
    const int listHeight=std::clamp(static_cast<int>(monitors.size())*13+4,30,78);
    const int bottom=143+listHeight;
    dword(WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME|DS_CENTER|DS_SETFONT);
    dword(WS_EX_TOOLWINDOW);word(0);word(0);word(0);word(254);word(static_cast<WORD>(bottom+30));
    word(0);word(0);string(state.tr(L"Dim when inactive",L"Reduzir brilho quando inativo"));
    word(9);string(L"Segoe UI");
    WORD count=0;
    auto item=[&](DWORD style,short x,short y,short width,short height,WORD id,WORD cls,const wchar_t* label,const wchar_t* namedClass=nullptr) {
        if(words.size()%2)word(0);
        dword(WS_CHILD|WS_VISIBLE|style);dword(0);
        word(x);word(y);word(width);word(height);word(id);
        if(namedClass) string(namedClass);else {word(0xffff);word(cls);}
        string(label);word(0);++count;
    };
    item(WS_TABSTOP|BS_AUTOCHECKBOX,10,8,234,13,Enable,0x80,state.tr(L"Enable inactivity dimming",L"Ativar escurecimento por inatividade"));
    item(SS_LEFT,10,29,170,11,200,0x82,state.tr(L"Dim amount",L"Escurecimento"));
    item(WS_TABSTOP|TBS_HORZ|TBS_NOTICKS,8,42,158,20,Amount,0,L"",TRACKBAR_CLASSW);
    item(SS_RIGHT,167,45,29,13,Percent,0x82,L"90%");
    item(WS_TABSTOP|BS_PUSHBUTTON,202,41,42,19,Test,0x80,state.tr(L"Test",L"Testar"));
    item(SS_LEFT,10,67,234,12,201,0x82,state.tr(L"Inactivity time",L"Tempo de inatividade"));
    for(size_t i=0;i<kDimMinutePresets.size();++i) {
        const auto text=kDimMinutePresets[i]==60?std::wstring(state.tr(L"1 hour",L"1 hora")):
            std::to_wstring(kDimMinutePresets[i])+L" min";
        item(BS_AUTORADIOBUTTON|(i==0?WS_GROUP|WS_TABSTOP:0),
            static_cast<short>(10+(i%3)*78),static_cast<short>(82+(i/3)*14),75,13,
            static_cast<WORD>(Preset+i),0x80,text.c_str());
    }
    item(BS_AUTORADIOBUTTON,10,110,88,13,Custom,0x80,state.tr(L"Custom (min)",L"Personalizado (min)"));
    item(WS_GROUP|WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,105,109,45,14,Minutes,0x81,L"");
    item(SS_LEFT,10,127,80,11,202,0x82,state.tr(L"Displays",L"Ecrãs"));
    item(WS_TABSTOP|BS_AUTOCHECKBOX,100,126,144,13,All,0x80,state.tr(L"All displays",L"Todos os ecrãs"));
    item(WS_TABSTOP|WS_BORDER|WS_VSCROLL|LBS_MULTIPLESEL|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT,
        10,141,234,static_cast<short>(listHeight),Displays,0x83,L"");
    item(WS_TABSTOP|BS_DEFPUSHBUTTON,140,static_cast<short>(bottom+7),48,16,IDOK,0x80,L"OK");
    item(WS_TABSTOP|BS_PUSHBUTTON,196,static_cast<short>(bottom+7),48,16,IDCANCEL,0x80,state.tr(L"Cancel",L"Cancelar"));
    words[4]=count;
    const auto result=DialogBoxIndirectParamW(instance,
        reinterpret_cast<const DLGTEMPLATE*>(words.data()),owner,procedure,reinterpret_cast<LPARAM>(&state));
    if(state.testing) state.test(nullptr);
    return result==IDOK?std::optional<Preferences>(std::move(state.draft)):std::nullopt;
}
}
