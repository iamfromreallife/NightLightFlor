#pragma once
#include <windows.h>

namespace nightlight {
// Modeless frames retain normal title bars and stay off the taskbar, but
// never share an owner with one another or with topmost tray/overlay popups.
// A private, hidden, nonactivating owner is stable across frame recreation.
class NormalFrameOwner {
    HWND handle_=nullptr;
public:
    NormalFrameOwner()=default;
    NormalFrameOwner(const NormalFrameOwner&)=delete;
    NormalFrameOwner& operator=(const NormalFrameOwner&)=delete;
    ~NormalFrameOwner(){reset();}
    HWND get() const{return handle_;}
    HWND ensure(HINSTANCE instance){
        if(handle_ && IsWindow(handle_))return handle_;
        constexpr wchar_t name[]=L"NightLight.NormalFrameOwner.Win32";
        WNDCLASSW wc{};wc.hInstance=instance;wc.lpfnWndProc=DefWindowProcW;wc.lpszClassName=name;
        if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return nullptr;
        handle_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,name,L"",WS_POPUP,
            0,0,0,0,nullptr,nullptr,instance,nullptr);
        return handle_;
    }
    void reset(){if(handle_ && IsWindow(handle_))DestroyWindow(handle_);handle_=nullptr;}
};
inline bool isTopmostFrame(HWND h){
    return h && IsWindow(h) && (GetWindowLongPtrW(h,GWL_EXSTYLE)&WS_EX_TOPMOST)!=0;
}
inline constexpr UINT kNormalFrameOrderFlags=SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER;
// No timer, no reordering of already-normal windows, and no activation.
// Correct a topmost owner first, since Win32 can propagate its state.
inline void normalizeFrameZOrder(HWND frame){
    if(!frame || !IsWindow(frame))return;
    const HWND owner=GetWindow(frame,GW_OWNER);
    if(isTopmostFrame(owner))SetWindowPos(owner,HWND_NOTOPMOST,0,0,0,0,kNormalFrameOrderFlags);
    if(isTopmostFrame(frame))SetWindowPos(frame,HWND_NOTOPMOST,0,0,0,0,kNormalFrameOrderFlags);
}
inline void activateNormalFrame(HWND frame){
    if(!frame || !IsWindow(frame))return;
    normalizeFrameZOrder(frame);
    // Raise exactly the requested frame in the ordinary band, not its owner.
    SetWindowPos(frame,HWND_TOP,0,0,0,0,kNormalFrameOrderFlags);
    SetForegroundWindow(frame);
}
inline bool frameReceivesMessage(HWND frame,HWND target){
    return frame && target && (frame==target || IsChild(frame,target));
}
inline void focusActiveFrameControl(HWND frame,HWND control){
    if(GetForegroundWindow()==frame && frameReceivesMessage(frame,control))SetFocus(control);
}
} // namespace nightlight
