#pragma once
// Included after the shared NightLight palette, rounded painters and frame
// helpers in main.cpp. A regular non-modal top-level native window, not a
// dialog template. All UI resources belong to this frame's lifetime.
class InactiveSettingsWindow {
    using Test=std::function<bool(const Preferences*)>;
    using Closed=std::function<void(const std::optional<Preferences>&)>;
    enum : int { Enable=101,Amount=102,Percent=103,TestButton=104,All=105,
        Displays=106,Preset=110,Custom=116,Minutes=117 };
    static constexpr UINT_PTR TestTimer=1;
    static constexpr wchar_t ClassName[]=L"NightLight.InactiveSettings";
    HINSTANCE instance_;
    HWND owner_,window_=nullptr;
    Preferences draft_;
    std::vector<Monitor> monitors_;
    bool portuguese_,testing_=false,accepted_=false,ready_=false,notified_=false;
    UINT dpi_=96;
    HFONT font_=nullptr,bold_=nullptr;
    std::map<int,HWND> controls_;
    std::set<HWND> hovering_;
    int chosenTime_=Custom;
    bool sliderDragging_=false;
    RECT amountCard_{},timeCard_{},displaysCard_{};
    Test test_;
    Closed closed_;
    std::function<Palette()> palette_;
    std::function<bool()> dark_,highContrast_;
    nightlight::PaintSurface surface_,sliderSurface_;
    const wchar_t* tr(const wchar_t* en,const wchar_t* pt) const {return portuguese_?pt:en;}
    int px(int n) const {return MulDiv(n,static_cast<int>(dpi_),96);}
    HWND control(int id) const {
        const auto it=controls_.find(id);
        return it==controls_.end()?nullptr:it->second;
    }
    void text(HDC dc,const std::wstring& label,RECT rect,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE) {
        const auto old=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
        DrawTextW(dc,label.c_str(),static_cast<int>(label.size()),&rect,flags|DT_NOPREFIX|DT_END_ELLIPSIS);
        if(old) SelectObject(dc,old);
    }
    static LRESULT CALLBACK hoverProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
        auto* self=reinterpret_cast<InactiveSettingsWindow*>(data);
        if(msg==WM_MOUSEMOVE && self->hovering_.insert(hwnd).second) {
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
            InvalidateRect(hwnd,nullptr,FALSE);
        } else if(msg==WM_MOUSELEAVE || msg==WM_CAPTURECHANGED) {
            self->hovering_.erase(hwnd);InvalidateRect(hwnd,nullptr,FALSE);
        } else if(msg==WM_NCDESTROY) {
            self->hovering_.erase(hwnd);
            RemoveWindowSubclass(hwnd,hoverProc,id);
        }
        return DefSubclassProc(hwnd,msg,wp,lp);
    }
    RECT sliderChannel(HWND hwnd) const {
        RECT client{},channel{};
        GetClientRect(hwnd,&client);
        SendMessageW(hwnd,TBM_GETCHANNELRECT,0,reinterpret_cast<LPARAM>(&channel));
        if(channel.right<=channel.left) {
            channel.left=px(10);
            channel.right=std::max(channel.left+1,client.right-px(10));
        }
        return channel;
    }
    void moveSlider(HWND hwnd,int x) {
        const RECT channel=sliderChannel(hwnd);
        const int position=1+MulDiv(std::clamp(x,static_cast<int>(channel.left),
            static_cast<int>(channel.right))-static_cast<int>(channel.left),98,
            static_cast<int>(channel.right-channel.left));
        if(SendMessageW(hwnd,TBM_GETPOS,0,0)!=position) {
            // Keep the native value/keyboard semantics, but never let the
            // common control paint an intermediate native thumb.
            SendMessageW(hwnd,TBM_SETPOS,FALSE,position);
            updateAmount();
        }
        InvalidateRect(hwnd,nullptr,FALSE);
    }
    static LRESULT CALLBACK sliderProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
        auto* self=reinterpret_cast<InactiveSettingsWindow*>(data);
        switch(msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            // The common trackbar normally invalidates only its native
            // thumb bounds, which are smaller than our circular thumb.
            // Expand the update region before BeginPaint and paint every
            // pixel, including the previous thumb and its shadow.
            InvalidateRect(hwnd,nullptr,FALSE);
            PAINTSTRUCT ps{};
            HDC dc=BeginPaint(hwnd,&ps);
            if(dc) {
                NMCUSTOMDRAW draw{};
                draw.hdr.hwndFrom=hwnd;draw.dwDrawStage=CDDS_PREPAINT;draw.hdc=dc;
                self->paintSlider(draw);
            }
            EndPaint(hwnd,&ps);
            return 0;
        }
        case WM_PRINTCLIENT: {
            NMCUSTOMDRAW draw{};
            draw.hdr.hwndFrom=hwnd;draw.dwDrawStage=CDDS_PREPAINT;
            draw.hdc=reinterpret_cast<HDC>(wp);
            if(draw.hdc) self->paintSlider(draw);
            return 0;
        }
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:
            if(IsWindowEnabled(hwnd)) {
                SetFocus(hwnd);SetCapture(hwnd);
                self->sliderDragging_=GetCapture()==hwnd;
                self->moveSlider(hwnd,GET_X_LPARAM(lp));
            }
            return 0;
        case WM_MOUSEMOVE:
            if(self->hovering_.insert(hwnd).second) {
                TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,hwnd,0};TrackMouseEvent(&track);
                InvalidateRect(hwnd,nullptr,FALSE);
            }
            if(self->sliderDragging_ && GetCapture()==hwnd)
                self->moveSlider(hwnd,GET_X_LPARAM(lp));
            // No native mouse painting, even when just hovering.
            return 0;
        case WM_MOUSELEAVE:
            self->hovering_.erase(hwnd);InvalidateRect(hwnd,nullptr,FALSE);return 0;
        case WM_LBUTTONUP:
            if(self->sliderDragging_) {
                self->moveSlider(hwnd,GET_X_LPARAM(lp));
                self->sliderDragging_=false;
                if(GetCapture()==hwnd) ReleaseCapture();
                InvalidateRect(hwnd,nullptr,FALSE);
            }
            return 0;
        case WM_CANCELMODE:
            if(self->sliderDragging_ && GetCapture()==hwnd) ReleaseCapture();
            self->sliderDragging_=false;InvalidateRect(hwnd,nullptr,FALSE);
            break;
        case WM_CAPTURECHANGED:
            self->sliderDragging_=false;InvalidateRect(hwnd,nullptr,FALSE);break;
        case WM_NCDESTROY:
            self->sliderDragging_=false;
            self->hovering_.erase(hwnd);
            RemoveWindowSubclass(hwnd,sliderProc,id);
            break;
        }
        const auto result=DefSubclassProc(hwnd,msg,wp,lp);
        // Preserve native keyboard input, range messages and accessibility;
        // never rely on the native thumb's narrow invalidation rectangle.
        if(msg==WM_KEYDOWN || msg==WM_KEYUP || msg==WM_MOUSEWHEEL ||
           msg==TBM_SETPOS || msg==TBM_SETRANGE || msg==WM_SIZE ||
           msg==WM_ENABLE || msg==WM_SETFOCUS || msg==WM_KILLFOCUS)
            InvalidateRect(hwnd,nullptr,FALSE);
        return result;
    }
    HWND child(int id,const wchar_t* cls,const wchar_t* label,DWORD style,DWORD ex=0) {
        HWND hwnd=CreateWindowExW(ex,cls,label,WS_CHILD|WS_VISIBLE|style,
            0,0,1,1,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
        if(hwnd) controls_[id]=hwnd;
        return hwnd;
    }
    void button(int id,const wchar_t* label) {
        const HWND hwnd=child(id,L"BUTTON",label,WS_TABSTOP|BS_OWNERDRAW);
        if(hwnd) SetWindowSubclass(hwnd,hoverProc,1,reinterpret_cast<DWORD_PTR>(this));
    }
    void fonts() {
        HFONT normal=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
            DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        HFONT semibold=CreateFontW(-px(14),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,
            DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,
            DEFAULT_PITCH|FF_SWISS,L"Segoe UI Semibold");
        for(const auto& item:controls_) SendMessageW(item.second,WM_SETFONT,
            reinterpret_cast<WPARAM>(normal?normal:GetStockObject(DEFAULT_GUI_FONT)),FALSE);
        if(font_) DeleteObject(font_);
        if(bold_) DeleteObject(bold_);
        font_=normal;bold_=semibold;
        if(control(Displays)) SendMessageW(control(Displays),LB_SETITEMHEIGHT,0,px(30));
    }
    void updateAmount() {
        draft_.dimAmount=static_cast<int>(SendMessageW(control(Amount),TBM_GETPOS,0,0));
        SetWindowTextW(control(Percent),(std::to_wstring(draft_.dimAmount)+L" %").c_str());
    }
    void setTime(int id) {
        chosenTime_=id;
        EnableWindow(control(Minutes),id==Custom);
        for(int i=Preset;i<=Custom;++i) InvalidateRect(control(i),nullptr,FALSE);
    }
    void createControls() {
        button(Enable,tr(L"Enable inactivity dimming",L"Ativar escurecimento por inatividade"));
        HWND slider=child(Amount,TRACKBAR_CLASSW,L"",WS_TABSTOP|TBS_HORZ|TBS_NOTICKS);
        if(slider) SetWindowSubclass(slider,sliderProc,1,reinterpret_cast<DWORD_PTR>(this));
        child(Percent,L"STATIC",L"",SS_RIGHT|SS_CENTERIMAGE);
        button(TestButton,tr(L"Test",L"Testar"));
        for(size_t i=0;i<nightlight::kDimMinutePresets.size();++i) {
            const int min=nightlight::kDimMinutePresets[i];
            const auto label=min==60?std::wstring(tr(L"1 hour",L"1 hora")):std::to_wstring(min)+L" min";
            button(Preset+static_cast<int>(i),label.c_str());
            if(min==draft_.dimMinutes) chosenTime_=Preset+static_cast<int>(i);
        }
        button(Custom,tr(L"Custom (min)",L"Personalizado (min)"));
        child(Minutes,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL);
        button(All,tr(L"All displays",L"Todos os ecrãs"));
        child(Displays,L"LISTBOX",L"",WS_TABSTOP|WS_VSCROLL|LBS_MULTIPLESEL|
            LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS);
        button(IDOK,L"OK");button(IDCANCEL,tr(L"Cancel",L"Cancelar"));
        fonts();
        SendMessageW(slider,TBM_SETRANGE,TRUE,MAKELPARAM(1,99));
        SendMessageW(slider,TBM_SETPOS,TRUE,draft_.dimAmount);
        SendMessageW(slider,TBM_SETPAGESIZE,0,10);
        updateAmount();
        SetWindowTextW(control(Minutes),std::to_wstring(draft_.dimMinutes).c_str());
        SendMessageW(control(Minutes),EM_SETLIMITTEXT,4,0);
        setTime(chosenTime_);
        for(const auto& mon:monitors_) {
            const auto label=mon.displayName();
            const auto index=SendMessageW(control(Displays),LB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(index>=0 && draft_.dims(mon)) SendMessageW(control(Displays),LB_SETSEL,TRUE,index);
        }
        EnableWindow(control(Displays),!draft_.dimAll);
        ready_=control(IDOK) && control(Displays) && slider && control(Minutes) &&
            controls_.size()==16;
        // Count is validated below rather than letting an incomplete
        // configuration frame accept or test settings.
    }
    void layout() {
        if(!window_ || !ready_ || IsIconic(window_)) return;
        RECT rc{};GetClientRect(window_,&rc);
        const int w=static_cast<int>(rc.right),h=static_cast<int>(rc.bottom);
        const int pad=px(12),left=pad+px(14),right=w-pad-px(14);
        const int content=std::max(px(100),right-left);
        amountCard_={pad,pad,w-pad,px(128)};
        timeCard_={pad,px(140),w-pad,px(278)};
        displaysCard_={pad,px(290),w-pad,std::max(px(373),h-px(56))};
        auto place=[&](int id,int x,int y,int width,int height) {
            if(HWND hwnd=control(id)) SetWindowPos(hwnd,nullptr,x,y,std::max(1,width),std::max(1,height),
                SWP_NOZORDER|SWP_NOACTIVATE);
        };
        place(Enable,left,px(24),content,px(30));
        const int buttonW=px(88),percentW=px(54);
        place(TestButton,right-buttonW,px(84),buttonW,px(30));
        place(Percent,right-buttonW-percentW-px(8),px(84),percentW,px(30));
        place(Amount,left,px(84),content-buttonW-percentW-px(16),px(30));
        const int gap=px(8),cell=(content-gap*2)/3;
        for(int i=0;i<6;++i) place(Preset+i,left+(i%3)*(cell+gap),
            px(171+(i/3)*31),cell,px(26));
        place(Custom,left,px(236),px(180),px(28));
        place(Minutes,left+px(188),px(237),px(66),px(26));
        place(All,right-px(194),px(301),px(194),px(28));
        place(Displays,left,px(335),content,std::max(px(30),
            static_cast<int>(displaysCard_.bottom)-px(347)));
        place(IDOK,right-px(184),h-px(44),px(88),px(32));
        place(IDCANCEL,right-px(88),h-px(44),px(88),px(32));
        InvalidateRect(window_,nullptr,FALSE);
    }
    void paint(HDC output) {
        RECT rc{};GetClientRect(window_,&rc);
        HDC dc=surface_.ensure(output,rc.right,rc.bottom);
        if(!dc) dc=output;
        const auto p=palette_();
        fill(dc,rc,p.page);
        for(RECT card:{amountCard_,timeCard_,displaysCard_})
            rounded(dc,card,px(2*nightlight::kCardCornerRadiusDip),p.card,p.border);
        const int left=px(26),right=rc.right-px(26);
        text(dc,tr(L"Dim amount",L"Escurecimento"),RECT{left,px(63),right,px(82)},bold_,p.text);
        text(dc,tr(L"Inactivity time",L"Tempo de inatividade"),RECT{left,px(148),right,px(170)},bold_,p.text);
        text(dc,tr(L"Displays",L"Ecrãs"),RECT{left,px(301),right-px(202),px(329)},bold_,p.text);
        if(dc!=output) BitBlt(output,0,0,rc.right,rc.bottom,dc,0,0,SRCCOPY);
    }
    void paintButton(const DRAWITEMSTRUCT& item) {
        RECT rc=item.rcItem; HDC dc=nullptr;
        const auto paint=BeginBufferedPaint(item.hDC,&rc,BPBF_TOPDOWNDIB,nullptr,&dc);
        if(!paint) dc=item.hDC;
        const auto p=palette_();
        const int id=static_cast<int>(item.CtlID);
        const bool footer=id==IDOK || id==IDCANCEL;
        fill(dc,rc,footer?p.page:p.card);
        const bool push=(item.itemState&ODS_SELECTED)!=0;
        const bool hover=hovering_.count(item.hwndItem)!=0;
        const bool selected=(id>=Preset && id<=Custom && id==chosenTime_);
        const bool toggle=id==Enable || id==All;
        const bool on=id==Enable?draft_.dimInactive:draft_.dimAll;
        const bool focused=(item.itemState&ODS_FOCUS)!=0;
        COLORREF background=selected?(!highContrast_()?mix(p.selected,RGB(0,0,0),0.07):p.selected):p.chip;
        COLORREF border=selected?mix(p.border,p.accent,0.65):p.border;
        if(focused) border=mix(p.border,p.accent,0.55);
        background=mix(background,p.text,(hover?0.035:0.0)+(push?0.055:0.0));
        RECT face=rc;if(push) InflateRect(&face,-px(1),-px(1));
        rounded(dc,face,px(8),background,border);
        wchar_t label[256]{};GetWindowTextW(item.hwndItem,label,256);
        RECT labelRect=rc;
        if(toggle) {
            RECT track{rc.right-px(43),(rc.top+rc.bottom-px(18))/2,
                rc.right-px(11),(rc.top+rc.bottom+px(18))/2};
            rounded(dc,track,px(18),on?p.accent:p.card,on?p.accent:p.muted);
            smoothCircle(dc,static_cast<float>(track.left+px(on?23:9)),
                static_cast<float>((track.top+track.bottom)/2),static_cast<float>(px(6)),
                on?RGB(255,255,255):p.subtle);
            labelRect.left+=px(10);labelRect.right=track.left-px(6);
        }
        text(dc,label,labelRect,selected?bold_:font_,
            (item.itemState&ODS_DISABLED)?p.muted:p.text,
            (toggle?DT_LEFT:DT_CENTER)|DT_VCENTER|DT_SINGLELINE);
        if(paint) EndBufferedPaint(paint,TRUE);
    }
    void paintMonitor(const DRAWITEMSTRUCT& item) {
        const auto p=palette_();
        fill(item.hDC,item.rcItem,p.card);
        if(item.itemID==static_cast<UINT>(-1) || item.itemID>=monitors_.size()) return;
        const bool selected=(item.itemState&ODS_SELECTED)!=0;
        const bool disabled=draft_.dimAll || (item.itemState&ODS_DISABLED);
        RECT rc=item.rcItem;InflateRect(&rc,-px(2),-px(2));
        rounded(item.hDC,rc,px(8),selected?p.selected:p.chip,
            (item.itemState&ODS_FOCUS)?mix(p.border,p.accent,0.55):p.border);
        RECT box{rc.left+px(9),(rc.top+rc.bottom-px(14))/2,
            rc.left+px(23),(rc.top+rc.bottom+px(14))/2};
        rounded(item.hDC,box,px(4),selected?p.accent:p.card,selected?p.accent:p.muted);
        if(selected) {
            Gdiplus::Graphics g(item.hDC);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pen(drawingColor(RGB(255,255,255)),static_cast<float>(px(2)));
            g.DrawLine(&pen,static_cast<INT>(box.left+px(3)),static_cast<INT>(box.top+px(7)),
                static_cast<INT>(box.left+px(6)),static_cast<INT>(box.top+px(10)));
            g.DrawLine(&pen,static_cast<INT>(box.left+px(6)),static_cast<INT>(box.top+px(10)),
                static_cast<INT>(box.left+px(11)),static_cast<INT>(box.top+px(4)));
        }
        rc.left=box.right+px(9);rc.right-=px(8);
        text(item.hDC,monitors_[item.itemID].displayName(),rc,selected?bold_:font_,disabled?p.muted:p.text);
    }
    LRESULT paintSlider(NMCUSTOMDRAW& draw) {
        if(draw.dwDrawStage!=CDDS_PREPAINT) return CDRF_DODEFAULT;
        HWND slider=draw.hdr.hwndFrom;
        RECT rc{};GetClientRect(slider,&rc);
        if(rc.right<=0 || rc.bottom<=0) return CDRF_SKIPDEFAULT;
        const RECT channel=sliderChannel(slider);
        HDC dc=sliderSurface_.ensure(draw.hdc,rc.right,rc.bottom);
        if(!dc) dc=draw.hdc;
        const auto p=palette_();fill(dc,rc,p.card);
        const int cy=rc.bottom/2;
        RECT track{channel.left,cy-px(2),channel.right,cy+px(2)};
        const int position=std::clamp(static_cast<int>(SendMessageW(slider,TBM_GETPOS,0,0)),1,99);
        // Use the same geometry for pointer input and the painted thumb.
        const int cx=static_cast<int>(track.left)+MulDiv(position-1,
            static_cast<int>(track.right-track.left),98);
        rounded(dc,track,px(4),p.border,p.border);
        RECT active=track;active.right=cx;
        if(active.right>active.left) rounded(dc,active,px(4),p.accent,p.accent);
        paintNightLightThumb(dc,static_cast<float>(cx),static_cast<float>(cy),
            hovering_.count(slider)?1.0:0.0,GetCapture()==slider?1.0:0.0,true,p,dpi_,dark_(),highContrast_());
        if(dc!=draw.hdc) BitBlt(draw.hdc,0,0,rc.right,rc.bottom,dc,0,0,SRCCOPY);
        return CDRF_SKIPDEFAULT;
    }
    void readSelection() {
        updateAmount();
        if(draft_.dimAll) draft_.dimDisplays.clear();
        else for(size_t i=0;i<monitors_.size();++i) {
            const auto key=nightlight::monitorProfileKey(monitors_[i]);
            if(SendMessageW(control(Displays),LB_GETSEL,i,0)>0) draft_.dimDisplays.insert(key);
            else draft_.dimDisplays.erase(key);
        }
    }
    bool readTime() {
        if(chosenTime_>=Preset && chosenTime_<Custom) {
            draft_.dimMinutes=nightlight::kDimMinutePresets[chosenTime_-Preset];return true;
        }
        BOOL valid=FALSE;
        const UINT minutes=GetDlgItemInt(window_,Minutes,&valid,FALSE);
        if(!valid || minutes<1 || minutes>1440) {
            MessageBoxW(window_,tr(L"Enter an inactivity time from 1 to 1440 minutes.",
                L"Introduza um tempo de inatividade entre 1 e 1440 minutos."),L"NightLight",MB_OK|MB_ICONWARNING);
            SetFocus(control(Minutes));return false;
        }
        draft_.dimMinutes=static_cast<int>(minutes);return true;
    }
    void command(int id) {
        if(id==Enable) {
            draft_.dimInactive=!draft_.dimInactive;InvalidateRect(control(id),nullptr,FALSE);
        } else if(id==All) {
            draft_.dimAll=!draft_.dimAll;
            if(draft_.dimAll) SendMessageW(control(Displays),LB_SETSEL,TRUE,-1);
            EnableWindow(control(Displays),!draft_.dimAll);
            InvalidateRect(control(All),nullptr,FALSE);
            InvalidateRect(control(Displays),nullptr,FALSE);
        } else if(id>=Preset && id<=Custom) {
            setTime(id);
            if(id==Custom) SetFocus(control(Minutes));
        } else if(id==TestButton) {
            if(testing_) {cancelTest();return;}
            readSelection();
            if(SetTimer(window_,TestTimer,3000,nullptr)) {
                testing_=true;
                if(test_(&draft_)) SetWindowTextW(control(TestButton),tr(L"Stop",L"Parar"));
                else {
                    cancelTest();
                    MessageBoxW(window_,tr(L"No overlay could be shown. Select a connected display.",
                        L"Não foi possível mostrar a camada. Selecione um ecrã ligado."),L"NightLight",MB_OK|MB_ICONINFORMATION);
                }
            }
        } else if(id==IDOK) {
            cancelTest();readSelection();
            if(readTime()) {accepted_=true;close();}
        } else if(id==IDCANCEL) close();
    }
    LRESULT dispatch(UINT msg,WPARAM wp,LPARAM lp) {
        switch(msg) {
        case WM_CREATE:
            dpi_=GetDpiForWindow(window_);if(!dpi_) dpi_=96;
            createControls();
            if(!ready_) return -1;
            refreshTheme();layout();return 0;
        case WM_SIZE:
            if(wp==SIZE_MINIMIZED) {cancelTest();surface_.reset();sliderSurface_.reset();}
            else layout();
            return 0;
        case WM_GETMINMAXINFO: {
            RECT minimum{0,0,px(428),px(444)};
            AdjustWindowRectExForDpi(&minimum,WS_OVERLAPPEDWINDOW,FALSE,0,dpi_);
            auto* info=reinterpret_cast<MINMAXINFO*>(lp);
            info->ptMinTrackSize={minimum.right-minimum.left,minimum.bottom-minimum.top};
            return 0;
        }
        case WM_DPICHANGED: {
            dpi_=HIWORD(wp);if(!dpi_)dpi_=96;
            fonts();surface_.reset();sliderSurface_.reset();
            const auto* r=reinterpret_cast<const RECT*>(lp);
            SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
            layout();return 0;
        }
        case WM_ERASEBKGND:return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);
            if(dc && !IsIconic(window_)) paint(dc);
            EndPaint(window_,&ps);return 0;
        }
        case WM_PRINTCLIENT:paint(reinterpret_cast<HDC>(wp));return 0;
        case WM_DRAWITEM: {
            const auto* item=reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if(item && item->CtlType==ODT_BUTTON) {paintButton(*item);return TRUE;}
            if(item && item->CtlType==ODT_LISTBOX) {paintMonitor(*item);return TRUE;}
            break;
        }
        case WM_MEASUREITEM: {
            auto* item=reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if(item && item->CtlID==Displays) {item->itemHeight=px(30);return TRUE;}
            break;
        }
        case WM_NOTIFY: {
            auto* hdr=reinterpret_cast<NMHDR*>(lp);
            if(hdr && hdr->hwndFrom==control(Amount) && hdr->code==NM_CUSTOMDRAW)
                return paintSlider(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
            break;
        }
        case WM_HSCROLL:
            if(reinterpret_cast<HWND>(lp)==control(Amount)) {
                updateAmount();InvalidateRect(control(Amount),nullptr,FALSE);return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:case WM_CTLCOLORBTN: {
            const auto p=palette_();HDC dc=reinterpret_cast<HDC>(wp);
            SetTextColor(dc,p.text);SetBkColor(dc,p.card);SetDCBrushColor(dc,p.card);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_COMMAND:
            if(HIWORD(wp)==BN_CLICKED) command(LOWORD(wp));
            return 0;
        case WM_TIMER:
            if(wp==TestTimer) {cancelTest();return 0;}
            break;
        case WM_DISPLAYCHANGE:case WM_POWERBROADCAST:cancelTest();break;
        case WM_THEMECHANGED:case WM_SETTINGCHANGE:refreshTheme();break;
        case WM_CLOSE:close();return 0;
        case WM_DESTROY:
            cancelTest();ready_=false;surface_.reset();sliderSurface_.reset();return 0;
        case WM_NCDESTROY: {
            const HWND old=window_;
            const LRESULT result=DefWindowProcW(old,msg,wp,lp);
            SetWindowLongPtrW(old,GWLP_USERDATA,0);
            window_=nullptr;controls_.clear();hovering_.clear();
            if(!notified_) {notified_=true;closed_(accepted_?std::optional<Preferences>(draft_):std::nullopt);}
            return result;
        }
        }
        return DefWindowProcW(window_,msg,wp,lp);
    }
    static LRESULT CALLBACK proc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<InactiveSettingsWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {
            self=static_cast<InactiveSettingsWindow*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window_=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        return self?self->dispatch(msg,wp,lp):DefWindowProcW(hwnd,msg,wp,lp);
    }
public:
    InactiveSettingsWindow(HINSTANCE instance,HWND owner,const Preferences& current,
        std::vector<Monitor> monitors,bool pt,Test test,Closed closed,
        std::function<Palette()> palette,std::function<bool()> dark,std::function<bool()> hc)
        :instance_(instance),owner_(owner),draft_(current),monitors_(std::move(monitors)),
        portuguese_(pt),test_(std::move(test)),closed_(std::move(closed)),
        palette_(std::move(palette)),dark_(std::move(dark)),highContrast_(std::move(hc)) {}
    ~InactiveSettingsWindow() {close();if(font_) DeleteObject(font_);if(bold_) DeleteObject(bold_);}
    HWND hwnd() const {return window_;}
    void close() {if(window_) DestroyWindow(window_);}
    void activate() {if(window_) {if(IsIconic(window_)) ShowWindow(window_,SW_RESTORE);SetForegroundWindow(window_);}}
    void cancelTest() {
        if(window_) KillTimer(window_,TestTimer);
        if(testing_) {testing_=false;test_(nullptr);}
        if(HWND button=control(TestButton)) SetWindowTextW(button,tr(L"Test",L"Testar"));
    }
    void refreshTheme() {
        if(!window_) return;
        applyNightLightFrame(window_,palette_(),dark_(),highContrast_());
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN);
    }
    bool pretranslate(MSG& msg) {
        if(!window_ || (msg.hwnd!=window_ && !IsChild(window_,msg.hwnd))) return false;
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE) {close();return true;}
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_RETURN && msg.hwnd==control(Minutes)) {command(IDOK);return true;}
        return IsDialogMessageW(window_,&msg)!=FALSE;
    }
    bool open() {
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance_;
        wc.lpfnWndProc=proc;wc.lpszClassName=ClassName;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;
        if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
        POINT cursor{};GetCursorPos(&cursor);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTOPRIMARY),&mi);
        // Get initial DPI from the target monitor through a short-lived
        // ordinary window; the native frame adjusts on WM_DPICHANGED.
        dpi_=GetDpiForSystem();if(!dpi_)dpi_=96;
        RECT r{0,0,px(490),px(std::clamp(420+static_cast<int>(monitors_.size())*30,474,620))};
        AdjustWindowRectExForDpi(&r,WS_OVERLAPPEDWINDOW,FALSE,0,dpi_);
        const int w=std::min(static_cast<int>(r.right-r.left),static_cast<int>(mi.rcWork.right-mi.rcWork.left));
        const int h=std::min(static_cast<int>(r.bottom-r.top),static_cast<int>(mi.rcWork.bottom-mi.rcWork.top));
        const int x=mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-w)/2;
        const int y=mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-h)/2;
        HWND created=CreateWindowExW(0,ClassName,tr(L"Dim when inactive",L"Reduzir brilho quando inativo"),
            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,x,y,w,h,owner_,nullptr,instance_,this);
        if(!created) return false;
        const BOOL disable=FALSE;
        DwmSetWindowAttribute(created,DWMWA_TRANSITIONS_FORCEDISABLED,&disable,sizeof(disable));
        ShowWindow(created,SW_SHOW);
        SetForegroundWindow(created);
        return true;
    }
};
