#pragma once
// Included after the same shared palette and Win32 painters as the dim window.
class UpdateWindow {
    HINSTANCE instance_;
    HWND owner_,window_=nullptr,notes_=nullptr;
    std::array<HWND,3> buttons_{};
    HFONT font_=nullptr,bold_=nullptr;
    UINT dpi_=96;
    bool pt_;
    std::wstring version_,notesText_;
    std::function<Palette()> palette_;
    std::function<bool()> dark_,hc_;
    std::function<void(int)> action_;
    nightlight::PaintSurface surface_;
    int px(int n) const {return MulDiv(n,static_cast<int>(dpi_),96);}
    const wchar_t* tr(const wchar_t* en,const wchar_t* pt) const {return pt_?pt:en;}
    void fonts() {
        HFONT f=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        HFONT b=CreateFontW(-px(18),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        const auto use=f?f:static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        if(notes_) SendMessageW(notes_,WM_SETFONT,reinterpret_cast<WPARAM>(use),TRUE);
        for(HWND h:buttons_) if(h) SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(use),TRUE);
        if(font_) DeleteObject(font_);
        if(bold_) DeleteObject(bold_);
        font_=f;bold_=b;
    }
    void layout() {
        if(!window_ || !notes_ || IsIconic(window_)) return;
        RECT rc{};GetClientRect(window_,&rc);
        const int w=rc.right,h=rc.bottom,pad=px(14),gap=px(8);
        SetWindowPos(notes_,nullptr,pad+px(12),px(112),std::max(1,w-2*(pad+px(12))),
            std::max(1,h-px(176)),SWP_NOZORDER|SWP_NOACTIVATE);
        const int widths[]={px(94),px(144),px(178)};
        int x=w-pad-widths[0]-widths[1]-widths[2]-2*gap;
        for(size_t i=0;i<buttons_.size();++i) {
            SetWindowPos(buttons_[i],nullptr,x,h-px(44),widths[i],px(32),SWP_NOZORDER|SWP_NOACTIVATE);
            x+=widths[i]+gap;
        }
        InvalidateRect(window_,nullptr,FALSE);
    }
    void text(HDC dc,const std::wstring& value,RECT r,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE) {
        auto old=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);
        DrawTextW(dc,value.c_str(),-1,&r,flags|DT_NOPREFIX|DT_END_ELLIPSIS);
        if(old && old!=HGDI_ERROR)SelectObject(dc,old);
    }
    void paint(HDC output) {
        if(!output || IsIconic(window_)) return;
        RECT r{};GetClientRect(window_,&r);
        if(r.right<=0 || r.bottom<=0) return;
        HDC dc=surface_.ensure(output,r.right,r.bottom);if(!dc)dc=output;
        const auto p=palette_();fill(dc,r,p.page);
        rounded(dc,RECT{px(14),px(14),r.right-px(14),px(81)},px(12),p.card,p.border);
        rounded(dc,RECT{px(14),px(92),r.right-px(14),r.bottom-px(54)},px(12),p.card,p.border);
        text(dc,tr(L"Update available",L"Atualização disponível"),RECT{px(26),px(21),r.right-px(26),px(48)},bold_,p.accent);
        const auto info=std::wstring(tr(L"Installed: ",L"Instalada: "))+nightlight::kAppVersionWide+
            tr(L"    Latest: ",L"    Disponível: ")+version_;
        text(dc,info,RECT{px(26),px(48),r.right-px(26),px(73)},font_,p.text);
        text(dc,tr(L"Release notes",L"Notas da versão"),RECT{px(26),px(94),r.right-px(26),px(112)},font_,p.text);
        if(dc!=output)BitBlt(output,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
    }
    void drawButton(const DRAWITEMSTRUCT& item) {
        RECT r=item.rcItem; HDC dc=nullptr;
        auto bp=BeginBufferedPaint(item.hDC,&r,BPBF_TOPDOWNDIB,nullptr,&dc);
        if(!bp)dc=item.hDC;
        const auto p=palette_();fill(dc,r,p.page);
        const bool primary=item.CtlID==3, pressed=(item.itemState&ODS_SELECTED)!=0;
        auto bg=primary?mix(p.chip,p.accent,0.12):p.chip;
        if(pressed)bg=mix(bg,p.text,0.08);
        rounded(dc,r,px(8),bg,(primary || (item.itemState&ODS_FOCUS))?p.accent:p.border);
        wchar_t label[128]{};GetWindowTextW(item.hwndItem,label,128);
        text(dc,label,r,font_,p.text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(bp)EndBufferedPaint(bp,TRUE);
    }
    LRESULT dispatch(UINT msg,WPARAM wp,LPARAM lp) {
        switch(msg) {
        case WM_CREATE: {
            dpi_=GetDpiForWindow(window_);if(!dpi_)dpi_=96;
            notes_=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|
                ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_NOHIDESEL,
                0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(10)),instance_,nullptr);
            const wchar_t* labels[]={tr(L"Cancel",L"Cancelar"),tr(L"Ignore version",L"Ignorar versão"),
                tr(L"Go to download page",L"Ir para a página de descarga")};
            for(int i=0;i<3;++i)buttons_[i]=CreateWindowExW(0,L"BUTTON",labels[i],
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,window_,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(i+1)),instance_,nullptr);
            if(!notes_ || !buttons_[0] || !buttons_[1] || !buttons_[2])return -1;
            SendMessageW(notes_,EM_SETLIMITTEXT,static_cast<WPARAM>(notesText_.size()+1),0);
            SetWindowTextW(notes_,notesText_.c_str());
            SendMessageW(notes_,EM_SETSEL,0,0);
            fonts();refreshTheme();layout();return 0;
        }
        case WM_SIZE:if(wp==SIZE_MINIMIZED)surface_.reset();else layout();return 0;
        case WM_GETMINMAXINFO:{
            RECT r{0,0,px(470),px(300)};AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,dpi_);
            reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={r.right-r.left,r.bottom-r.top};return 0;
        }
        case WM_DPICHANGED:{
            dpi_=HIWORD(wp);if(!dpi_)dpi_=96;fonts();surface_.reset();
            const auto* r=reinterpret_cast<const RECT*>(lp);
            SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
            layout();return 0;
        }
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
        case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(wp));return 0;
        case WM_DRAWITEM:if(lp){drawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));return TRUE;}break;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:{
            auto p=palette_();auto dc=reinterpret_cast<HDC>(wp);
            SetTextColor(dc,p.text);SetBkColor(dc,p.card);SetDCBrushColor(dc,p.card);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_COMMAND:if(HIWORD(wp)==BN_CLICKED && LOWORD(wp)>=1 && LOWORD(wp)<=3){
            const int id=LOWORD(wp);close();action_(id);return 0;
        }break;
        case WM_THEMECHANGED:case WM_SETTINGCHANGE:refreshTheme();break;
        case WM_CLOSE:close();return 0;
        case WM_NCDESTROY:{
            HWND old=window_;SetWindowLongPtrW(old,GWLP_USERDATA,0);
            window_=nullptr;notes_=nullptr;buttons_={};surface_.reset();
            return DefWindowProcW(old,msg,wp,lp);
        }}
        return DefWindowProcW(window_,msg,wp,lp);
    }
    static LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<UpdateWindow*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<UpdateWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        return self?self->dispatch(msg,wp,lp):DefWindowProcW(h,msg,wp,lp);
    }
public:
    UpdateWindow(HINSTANCE instance,HWND owner,bool pt,std::wstring version,std::wstring notes,
        std::function<Palette()> p,std::function<bool()> dark,std::function<bool()> hc,std::function<void(int)> action)
        :instance_(instance),owner_(owner),pt_(pt),version_(std::move(version)),
        palette_(std::move(p)),dark_(std::move(dark)),hc_(std::move(hc)),action_(std::move(action)) {
        for(size_t i=0;i<notes.size();++i){
            if(notes[i]==L'\n' && (i==0 || notes[i-1]!=L'\r')) notesText_+=L'\r';
            if(notes[i]!=L'\0')notesText_+=notes[i];
        }
        if(notesText_.empty())notesText_=tr(L"No release notes provided.",L"Não foram fornecidas notas da versão.");
    }
    ~UpdateWindow(){close();if(font_)DeleteObject(font_);if(bold_)DeleteObject(bold_);}
    HWND hwnd()const{return window_;}
    void close(){if(window_)DestroyWindow(window_);}
    void activate(){if(window_){if(IsIconic(window_))ShowWindow(window_,SW_RESTORE);SetForegroundWindow(window_);}}
    void refreshTheme(){if(window_){applyNightLightFrame(window_,palette_(),dark_(),hc_());
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN|RDW_NOERASE);}}
    bool pretranslate(MSG& m){if(!window_ || (m.hwnd!=window_ && !IsChild(window_,m.hwnd)))return false;
        if(m.message==WM_KEYDOWN && m.wParam==VK_ESCAPE){close();return true;}
        return IsDialogMessageW(window_,&m)!=FALSE;}
    bool open(){
        constexpr wchar_t cls[]=L"NightLight.Update";
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance_;wc.lpfnWndProc=proc;wc.lpszClassName=cls;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;
        if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        POINT cursor{};GetCursorPos(&cursor);MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY),&mi))return false;
        dpi_=GetDpiForSystem();if(!dpi_)dpi_=96;
        RECT r{0,0,px(560),px(440)};AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,dpi_);
        const int w=std::min(static_cast<int>(r.right-r.left),static_cast<int>(mi.rcWork.right-mi.rcWork.left));
        const int h=std::min(static_cast<int>(r.bottom-r.top),static_cast<int>(mi.rcWork.bottom-mi.rcWork.top));
        HWND created=CreateWindowExW(0,cls,tr(L"NightLight update",L"Atualização do NightLight"),
            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-w)/2,
            mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-h)/2,w,h,owner_,nullptr,instance_,this);
        if(!created)return false;
        ShowWindow(created,SW_SHOW);SetFocus(buttons_[0]);SetForegroundWindow(created);return true;
    }
};
