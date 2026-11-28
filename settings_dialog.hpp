#pragma once
// Standard-library headers must be included at file scope in main.cpp.
// Native non-modal settings frame. The main window and settings use the same
// buffered painting, palette, antialiased rounded surfaces and DWM caption.
class NightLightSettingsDialog {
public:
    // Callbacks borrow the owning model only on the UI thread. Builders
    // still take snapshots where needed; individual chip paints do not.
    using Read=std::function<const Preferences&()>;
    using Monitors=std::function<const std::vector<Monitor>&()>;
    using General=std::function<bool(int,int)>;
    using Backend=std::function<bool(const std::wstring&,int)>;
    using Hotkeys=std::function<bool(const std::wstring&,const std::array<int,4>&,const std::array<int,4>&)>;
    using Schedule=std::function<void(nightlight::ColorSchedule)>;
    using DisplayAction=std::function<bool(int,const std::wstring&,const std::wstring&)>;
private:
    enum : int { GeneralTab=10,BehaviorTab=11,AutomationTab=12,AboutTab=13,
        Language=20,Theme=21,CloseOutside=22,Startup=23,Ddc=24,TrayWheel=28,TrayWheelMode=29,CaptureLogs=96,
        Scope=30,HotkeyFirst=31,SaveHotkeys=35,ClearHotkeys=36,HotkeyStep=40,
        EngineFirst=50,Previous=60,Next=61,
        ScheduleEnabled=70,Transition=71,Duration=75,ApplySchedule=76,DimSettings=77,
        PointAdd=78,PointTimeField=100,PointKelvin=140,PointRemove=180,
        DimStatus=87,LocalTimeNote=88,SetLocation=89,CloseButton=90,CheckUpdates=91,AboutRepository=92,LocationHint=93,AboutLicense=94,StartupHint=95,
        DisplayRefresh=3000,DisplayRename=3002,DisplayHint1=3003,DisplayHint2=3004,DisplayFirst=3100 };
    static constexpr wchar_t cls_[]=L"NightLight.Settings.Native";
    HINSTANCE instance_;
    HWND owner_,window_=nullptr,refreshTooltip_=nullptr;
    std::optional<POINT> savedPosition_;
    std::function<void(POINT)> rememberPosition_;
    bool placementReady_=false;
    HFONT font_=nullptr,bold_=nullptr;
    int fontLanguage_=-1;
    UINT dpi_=96,frameDpi_=96;
    static constexpr DWORD frameStyle_=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
    int tab_=0,behaviorPage_=0,scope_=0;
    bool draftLoaded_=false;
    nightlight::ColorSchedule draft_;
    struct Choice {std::vector<std::wstring> values;int selected=0;};
    struct Slider {int low,high,step,value,kind;size_t point;};
    std::map<int,Choice> choices_;
    std::map<int,bool> checks_;
    std::map<int,Slider> sliders_;
    std::map<int,size_t> timeFields_;
    int desiredClientHeight_=550,scrollY_=0;
    std::map<int,size_t> solarLabels_;
    bool locationOpen_=false;
    std::array<int,4> hotkeyValues_{};
    int menuChoice_=-1;
    HWND choicePopup_=nullptr;
    int choiceHighlight_=-1,choiceFirst_=0,choiceRows_=0,choiceWheel_=0;
    bool choicePressed_=false,choiceScrollDrag_=false;
    nightlight::PaintSurface choiceSurface_;
    nightlight::Motion displayRefreshTurn_{0};
    std::unique_ptr<Gdiplus::GraphicsPath> refreshGlyph_;
    bool manualRefreshQueued_=false;
    std::function<bool()> animations_;
    int activeSlider_=-1;
    static constexpr UINT_PTR PreviewTimer=3,SolarUiTimer=4,DisplaySpinTimer=5,DisplayDragTimer=6;
    int solarUiDate_=0,solarUiBias_=INT_MIN;
    bool hotkeyEditing_=false;
    bool building_=false,closing_=false;
    std::vector<std::wstring> scopeKeys_,backendKeys_;
    std::vector<std::wstring> displayKeys_;
    HWND renameField_=nullptr,dragChip_=nullptr,dropChip_=nullptr;
    std::wstring renameKey_;
    POINT dragStart_{};
    struct ChipMotion {nightlight::Motion x,y;};
    std::vector<ChipMotion> chipMotion_;
    std::vector<RECT> dragSlots_;
    std::vector<int> chipWidths_;
    size_t dragFrom_=0,dragTarget_=0;
    bool dragSettling_=false;
    int displayRowsBottom_=86;
    bool dragMoved_=false,dragSwitch_=false,renameCancel_=false;
    bool displayRefreshPending_=false,displayRefreshDeferred_=false;
    DisplayAction displayAction_;
    std::function<std::wstring()> selectedDisplay_;
    std::set<HWND> hovered_;
    struct Child {int id,x,y,w,h;HWND hwnd;bool page;};
    std::vector<Child> children_;
    std::vector<RECT> cards_;
    std::vector<std::pair<RECT,std::wstring>> headings_;
    Read read_;
    Monitors monitors_;
    General general_;
    Backend backend_;
    Hotkeys hotkeys_;
    Schedule schedule_;
    std::function<void(std::optional<int>)> preview_;
    std::function<void()> dim_;
    std::function<bool()> startup_,dark_,hc_;
    std::function<int()> language_;
    std::function<Palette()> palette_;
    nightlight::PaintSurface surface_;
    int px(int value)const{return MulDiv(value,static_cast<int>(dpi_),96);}
    const wchar_t* tr(const wchar_t* en,const wchar_t* pt)const{return nightlight::translate(language_(),en,pt);}
    void fitToMonitor(HMONITOR monitor,UINT dpi){
        frameDpi_=dpi?dpi:96;dpi_=frameDpi_;
        MONITORINFO mi{sizeof(mi)};
        if(GetMonitorInfoW(monitor,&mi)){
            RECT borders{};AdjustWindowRectExForDpi(&borders,frameStyle_,FALSE,0,frameDpi_);
            const int w=static_cast<int>(mi.rcWork.right-mi.rcWork.left-borders.right+borders.left)-GetSystemMetricsForDpi(SM_CXVSCROLL,frameDpi_);
            const int h=static_cast<int>(mi.rcWork.bottom-mi.rcWork.top-borders.bottom+borders.top);
            dpi_=static_cast<UINT>(std::max(48,std::min(static_cast<int>(frameDpi_),w*96/560)));
            (void)h;
        }
    }
    int availableHeight(HMONITOR monitor)const{
        MONITORINFO mi{sizeof(mi)};RECT border{};AdjustWindowRectExForDpi(&border,frameStyle_,FALSE,0,frameDpi_);
        if(!GetMonitorInfoW(monitor,&mi))return px(desiredClientHeight_);
        return std::max(px(100),static_cast<int>(mi.rcWork.bottom-mi.rcWork.top-border.bottom+border.top));
    }
    bool needsScroll(HMONITOR monitor)const{return px(desiredClientHeight_)>availableHeight(monitor);}
    SIZE frameSize(HMONITOR monitor=nullptr)const{
        if(!monitor)monitor=window_?MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST):MonitorFromPoint(POINT{0,0},MONITOR_DEFAULTTOPRIMARY);
        const bool scroll=needsScroll(monitor);
        RECT r{0,0,px(560),std::min(px(desiredClientHeight_),availableHeight(monitor))};
        AdjustWindowRectExForDpi(&r,frameStyle_|(scroll?WS_VSCROLL:0),FALSE,0,frameDpi_);
        // AdjustWindowRectEx does not account for a standard scrollbar.
        if(scroll)r.right+=GetSystemMetricsForDpi(SM_CXVSCROLL,frameDpi_);
        return {r.right-r.left,r.bottom-r.top};
    }
    void configureScroll(HMONITOR monitor){
        const bool need=needsScroll(monitor);const LONG_PTR old=GetWindowLongPtrW(window_,GWL_STYLE);
        const LONG_PTR next=need?old|WS_VSCROLL:old&~static_cast<LONG_PTR>(WS_VSCROLL);
        if(next!=old){SetWindowLongPtrW(window_,GWL_STYLE,next);SetWindowPos(window_,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);}
        if(!need)scrollY_=0;
    }
    void resizeForContent(){
        if(!window_)return;
        HMONITOR monitor=MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST);configureScroll(monitor);
        const auto size=frameSize(monitor);RECT old{};GetWindowRect(window_,&old);MONITORINFO mi{sizeof(mi)};
        int x=old.left,y=old.top;
        if(GetMonitorInfoW(monitor,&mi)){x=std::clamp(x,static_cast<int>(mi.rcWork.left),static_cast<int>(std::max(mi.rcWork.left,mi.rcWork.right-size.cx)));y=std::clamp(y,static_cast<int>(mi.rcWork.top),static_cast<int>(std::max(mi.rcWork.top,mi.rcWork.bottom-size.cy)));}
        if(old.right-old.left!=size.cx||old.bottom-old.top!=size.cy||x!=old.left||y!=old.top)SetWindowPos(window_,nullptr,x,y,size.cx,size.cy,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    HWND ctrl(int id)const{
        for(const auto& item:children_)if(item.id==id)return item.hwnd;
        return nullptr;
    }
    void text(HDC dc,const std::wstring& label,RECT r,HFONT font,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE){
        const auto old=SelectObject(dc,font?font:GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);
        DrawTextW(dc,label.c_str(),-1,&r,flags|DT_NOPREFIX|DT_END_ELLIPSIS);
        if(old && old!=HGDI_ERROR)SelectObject(dc,old);
    }
    static LRESULT CALLBACK hoverProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
        auto* self=reinterpret_cast<NightLightSettingsDialog*>(data);
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_MOUSEMOVE && self->hovered_.insert(h).second){
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);InvalidateRect(h,nullptr,FALSE);
        }else if(m==WM_MOUSELEAVE || m==WM_CAPTURECHANGED){
            self->hovered_.erase(h);InvalidateRect(h,nullptr,FALSE);
        }else if(m==WM_NCDESTROY){
            self->hovered_.erase(h);RemoveWindowSubclass(h,hoverProc,id);
        }
        return DefSubclassProc(h,m,w,l);
    }
    static LRESULT CALLBACK fieldProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR subclass,DWORD_PTR data){
        auto* self=reinterpret_cast<NightLightSettingsDialog*>(data);
        const int id=GetDlgCtrlID(h);
        const bool hot=id>=HotkeyFirst && id<HotkeyFirst+4;
        if(self->isEditField(id) || self->solarLabels_.count(id)){
            if((m==WM_CONTEXTMENU || (m==WM_RBUTTONUP && self->solarLabels_.count(id))) && (self->timeFields_.count(id)||self->solarLabels_.count(id))){
                const auto index=self->timeFields_.count(id)?self->timeFields_.at(id):self->solarLabels_.at(id);
                if(index<self->draft_.points.size() && self->draft_.points[index].kind!=nightlight::SchedulePointKind::Custom){LPARAM where=l;if(m==WM_RBUTTONUP){POINT point{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ClientToScreen(h,&point);where=MAKELPARAM(point.x,point.y);}self->solarMenu(index,where);return 0;}
            }
            if(m==WM_SETFOCUS||m==WM_KILLFOCUS){
                if(m==WM_KILLFOCUS && !self->building_ && !self->closing_ && self->timeFields_.count(id)){
                    if(auto minute=parseTimeText(self->fieldValue(id)))SetWindowTextW(h,timeText(*minute).c_str());
                }
                const auto result=DefSubclassProc(h,m,w,l);InvalidateRect(self->window_,nullptr,FALSE);return result;}
            if(m==WM_NCDESTROY)RemoveWindowSubclass(h,fieldProc,subclass);
            return DefSubclassProc(h,m,w,l);
        }

        if(hot){
            if(m==WM_SETFOCUS){self->hotkeyEditing_=true;self->general_(25,1);InvalidateRect(h,nullptr,FALSE);}
            if(m==WM_KILLFOCUS || m==WM_NCDESTROY){
                if(self->hotkeyEditing_){self->hotkeyEditing_=false;self->general_(25,0);}
                InvalidateRect(h,nullptr,FALSE);
            }
            if(m==WM_GETDLGCODE){
                if(w==VK_TAB)return DLGC_WANTCHARS;
                return DLGC_WANTALLKEYS;
            }
            if(m==WM_KEYDOWN || m==WM_SYSKEYDOWN){
                if(w==VK_TAB)return DefSubclassProc(h,m,w,l);
                if(w==VK_ESCAPE || w==VK_BACK || w==VK_DELETE){
                    if(!(GetKeyState(VK_CONTROL)&0x8000) && !(GetKeyState(VK_MENU)&0x8000) && !(GetKeyState(VK_SHIFT)&0x8000)){
                        self->hotkeyValues_[id-HotkeyFirst]=0;InvalidateRect(h,nullptr,FALSE);return 0;
                    }
                }
                if(w==VK_SHIFT || w==VK_CONTROL || w==VK_MENU || w==VK_LWIN || w==VK_RWIN || w==VK_LSHIFT || w==VK_RSHIFT || w==VK_LCONTROL || w==VK_RCONTROL || w==VK_LMENU || w==VK_RMENU)return 0;
                int flags=((GetKeyState(VK_SHIFT)&0x8000)?HOTKEYF_SHIFT:0)|((GetKeyState(VK_CONTROL)&0x8000)?HOTKEYF_CONTROL:0)|((GetKeyState(VK_MENU)&0x8000)?HOTKEYF_ALT:0);
                if(l&(1L<<24))flags|=HOTKEYF_EXT;
                const int binding=static_cast<int>(w&255)|(flags<<8);
                if(nightlight::validHotkey(binding)){self->hotkeyValues_[id-HotkeyFirst]=binding;InvalidateRect(h,nullptr,FALSE);}
                return 0;
            }
            if(m==WM_SYSCHAR || m==WM_CHAR)return 0;
        }else if(self->sliders_.count(id)){
            if(m==WM_GETDLGCODE)return DLGC_WANTARROWS|DLGC_WANTCHARS;
            if(m==WM_LBUTTONDOWN){
                SetFocus(h);SetCapture(h);self->activeSlider_=id;
                if(self->sliders_.at(id).kind==1)SetTimer(self->window_,PreviewTimer,100,nullptr);
                self->slideAt(h,GET_X_LPARAM(l));return 0;
            }
            if(m==WM_MOUSEMOVE && GetCapture()==h){self->slideAt(h,GET_X_LPARAM(l));return 0;}
            if(m==WM_LBUTTONUP && GetCapture()==h){self->slideAt(h,GET_X_LPARAM(l));ReleaseCapture();return 0;}
            if(m==WM_CAPTURECHANGED || m==WM_CANCELMODE){
                const bool temperature=self->activeSlider_==id && self->sliders_.at(id).kind==1;
                self->activeSlider_=-1;
                if(m==WM_CANCELMODE && GetCapture()==h)ReleaseCapture();
                if(temperature){KillTimer(self->window_,PreviewTimer);self->preview_(std::nullopt);}
                InvalidateRect(h,nullptr,FALSE);
            }
            if(m==WM_KEYDOWN){
                auto& v=self->sliders_.at(id);int next=v.value;
                if(w==VK_LEFT || w==VK_DOWN)next-=v.step;
                else if(w==VK_RIGHT || w==VK_UP)next+=v.step;
                else if(w==VK_PRIOR)next+=v.step*10;
                else if(w==VK_NEXT)next-=v.step*10;
                else if(w==VK_HOME)next=v.low;
                else if(w==VK_END)next=v.high;
                else return DefSubclassProc(h,m,w,l);
                self->setSlider(h,next);return 0;
            }
            if(m==WM_SETFOCUS || m==WM_KILLFOCUS)InvalidateRect(h,nullptr,FALSE);
        }
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_NCDESTROY)RemoveWindowSubclass(h,fieldProc,subclass);
        return DefSubclassProc(h,m,w,l);
    }
    std::wstring displayKey(HWND h)const {
        const int index=GetDlgCtrlID(h)-DisplayFirst;
        return index>=0 && static_cast<size_t>(index)<displayKeys_.size()?displayKeys_[index]:std::wstring{};
    }
    void finishRename(bool save){
        if(!renameField_)return;
        const HWND field=renameField_;renameField_=nullptr;
        wchar_t value[65]{};GetWindowTextW(field,value,65);
        const auto key=renameKey_;renameKey_.clear();
        DestroyWindow(field);
        if(save && !renameCancel_)displayAction_(5,key,value);
        renameCancel_=false;
    }
    static LRESULT CALLBACK renameProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
        auto* self=reinterpret_cast<NightLightSettingsDialog*>(data);
        if(m==WM_GETDLGCODE)return DLGC_WANTALLKEYS;
        if(m==WM_KEYDOWN && (w==VK_RETURN || w==VK_ESCAPE)){
            self->renameCancel_=w==VK_ESCAPE;PostMessageW(self->window_,WM_APP+3,0,reinterpret_cast<LPARAM>(h));return 0;
        }
        if(m==WM_KILLFOCUS)PostMessageW(self->window_,WM_APP+3,0,reinterpret_cast<LPARAM>(h));
        if(m==WM_NCDESTROY)RemoveWindowSubclass(h,renameProc,id);
        return DefSubclassProc(h,m,w,l);
    }
    void beginRename(HWND chip){
        finishRename(true);renameKey_=displayKey(chip);if(renameKey_.empty())return;
        RECT r{};GetWindowRect(chip,&r);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&r),2);
        r.left+=px(31);r.right-=px(50);r.top+=px(4);r.bottom-=px(4);
        if(r.right<=r.left)return;
        wchar_t name[128]{};GetWindowTextW(chip,name,128);
        renameField_=CreateWindowExW(0,L"EDIT",name,WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_AUTOHSCROLL|WS_CLIPSIBLINGS,
            r.left,r.top,r.right-r.left,r.bottom-r.top,window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(DisplayRename)),instance_,nullptr);
        if(!renameField_)return;
        SendMessageW(renameField_,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);
        SendMessageW(renameField_,EM_SETLIMITTEXT,64,0);
        SetWindowSubclass(renameField_,renameProc,1,reinterpret_cast<DWORD_PTR>(this));
        SetWindowPos(renameField_,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        SetFocus(renameField_);SendMessageW(renameField_,EM_SETSEL,0,-1);
    }
    bool chipMotionActive(ULONGLONG now)const {
        for(const auto& m:chipMotion_)if(m.x.active(now)||m.y.active(now))return true;
        return false;
    }
    void finishChipMotion(){
        KillTimer(window_,DisplayDragTimer);
        for(auto& m:chipMotion_){m.x.finish();m.y.finish();}
        dragSettling_=false;
    }
    void prepareDisplayDrag(HWND h){
        dragFrom_=static_cast<size_t>(GetDlgCtrlID(h)-DisplayFirst);dragTarget_=dragFrom_;
        dragSlots_.clear();chipWidths_.clear();chipMotion_.clear();
        for(size_t i=0;i<displayKeys_.size();++i){
            const HWND chip=ctrl(DisplayFirst+static_cast<int>(i));
            auto item=std::find_if(children_.begin(),children_.end(),[&](const Child& c){return c.hwnd==chip;});
            if(item==children_.end()){dragSlots_.clear();return;}
            dragSlots_.push_back({px(item->x),px(item->y),px(item->x+item->w),px(item->y+item->h)});
            chipWidths_.push_back(item->w);chipMotion_.push_back({nightlight::Motion(item->x),nightlight::Motion(item->y)});
        }
    }
    void previewDisplayOrder(size_t target){
        if(target>=displayKeys_.size()||dragFrom_>=displayKeys_.size()||chipWidths_.size()!=displayKeys_.size())return;
        dragTarget_=target;
        const auto slots=nightlight::settingsDisplaySlots(chipWidths_,nightlight::displayInsertionOrder(chipWidths_.size(),dragFrom_,target));
        int bottom=86;const auto now=uiTime();const bool animate=animations_()&&IsWindowVisible(window_)&&!IsIconic(window_);
        for(size_t i=0;i<slots.size();++i){
            auto item=std::find_if(children_.begin(),children_.end(),[&](const Child& c){return c.id==DisplayFirst+static_cast<int>(i);});
            if(item==children_.end())continue;
            item->x=slots[i].x;item->y=slots[i].y;bottom=std::max(bottom,slots[i].y);
            chipMotion_[i].x.to(item->x,now,nightlight::kDisplayReorderMs,animate);
            chipMotion_[i].y.to(item->y,now,nightlight::kDisplayReorderMs,animate);
        }
        // If a different packing order changes the number of rows, keep the
        // instructions, following cards and footer clear of the moving chips.
        const int delta=std::max(0,bottom-displayRowsBottom_);
        if(delta){
            const int below=displayRowsBottom_+40;
            for(auto& item:children_)if(item.id<DisplayFirst && item.y>=below)item.y+=delta;
            if(!cards_.empty())cards_[0].bottom+=delta;
            for(size_t i=1;i<cards_.size();++i)OffsetRect(&cards_[i],0,delta);
            for(size_t i=1;i<headings_.size();++i)OffsetRect(&headings_[i].first,0,delta);
            desiredClientHeight_+=delta;displayRowsBottom_=bottom;displayRefreshDeferred_=true;resizeForContent();
        }
        layout();
        if(chipMotionActive(now) && !SetTimer(window_,DisplayDragTimer,10,nullptr)){finishChipMotion();layout();}
    }
    void endDisplayDrag(bool commit){
        const auto source=dragFrom_,target=dragTarget_;
        const bool changed=dragMoved_&&source!=target&&source<displayKeys_.size()&&target<displayKeys_.size();
        HWND captured=dragChip_;dragChip_=nullptr;dropChip_=nullptr;
        if(captured&&GetCapture()==captured)ReleaseCapture();
        if(changed){
            if(commit){const auto key=displayKeys_[source],destination=displayKeys_[target];
                // The model commits once on release; preview never writes config.
                displayRefreshDeferred_=true;displayAction_(2,key,destination);
            }else previewDisplayOrder(source);
        }
        dragSlots_.clear();dragMoved_=false;dragSwitch_=false;
        dragSettling_=chipMotionActive(uiTime());
        if(!dragSettling_ || !SetTimer(window_,DisplayDragTimer,10,nullptr)){finishChipMotion();layout();if(displayRefreshDeferred_)displaysChanged();}
        repaintDisplays();
    }
    static LRESULT CALLBACK displayProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
        auto* self=reinterpret_cast<NightLightSettingsDialog*>(data);
        if(m==WM_SETCURSOR){SetCursor(LoadCursorW(nullptr,IDC_HAND));return TRUE;}
        if(m==WM_LBUTTONDOWN || m==WM_LBUTTONDBLCLK){
            if(self->dragSettling_ || self->displayRefreshPending_)return 0;
            RECT r{};GetClientRect(h,&r);const bool onSwitch=GET_X_LPARAM(l)>=r.right-self->px(48);
            if(m==WM_LBUTTONDBLCLK && !onSwitch){self->endDisplayDrag(false);self->beginRename(h);return 0;}
            self->finishRename(true);self->dragChip_=h;self->dropChip_=nullptr;self->dragMoved_=false;self->dragSwitch_=onSwitch;
            self->prepareDisplayDrag(h);GetCursorPos(&self->dragStart_);SetFocus(h);SetCapture(h);return 0;
        }
        if(m==WM_MOUSEMOVE && self->dragChip_==h && GetCapture()==h && !self->dragSwitch_){
            POINT pt{};GetCursorPos(&pt);
            if(std::abs(pt.x-self->dragStart_.x)>=GetSystemMetrics(SM_CXDRAG) || std::abs(pt.y-self->dragStart_.y)>=GetSystemMetrics(SM_CYDRAG))self->dragMoved_=true;
            if(self->dragMoved_){
                ScreenToClient(self->window_,&pt);pt.y+=self->scrollY_;
                for(size_t i=0;i<self->dragSlots_.size();++i)if(PtInRect(&self->dragSlots_[i],pt)){
                    if(i!=self->dragTarget_)self->previewDisplayOrder(i);break;}
            }
            return 0;
        }
        if(m==WM_LBUTTONUP && self->dragChip_==h){
            const bool moved=self->dragMoved_,toggle=self->dragSwitch_;
            const auto key=self->displayKey(h);POINT pt{};GetCursorPos(&pt);
            self->endDisplayDrag(true);
            if(!moved){RECT r{};GetWindowRect(h,&r);
                if(PtInRect(&r,pt) && (!toggle || pt.x>=r.right-self->px(48)))self->displayAction_(toggle?1:0,key,L"");}
            return 0;
        }
        if((m==WM_CAPTURECHANGED || m==WM_CANCELMODE || m==WM_NCDESTROY) && self->dragChip_==h){
            if(m==WM_NCDESTROY){self->dragChip_=nullptr;self->dropChip_=nullptr;}
            else self->endDisplayDrag(false);
        }
        if(m==WM_NCDESTROY)RemoveWindowSubclass(h,displayProc,id);
        return DefSubclassProc(h,m,w,l);
    }
    void fonts(){
        fontLanguage_=language_();
        HFONT f=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,nightlight::uiFontFace(language_()));
        HFONT b=CreateFontW(-px(14),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,nightlight::uiFontFace(language_(),true));
        for(const auto& item:children_)SendMessageW(item.hwnd,WM_SETFONT,
            reinterpret_cast<WPARAM>(f?f:GetStockObject(DEFAULT_GUI_FONT)),FALSE);
        if(font_)DeleteObject(font_);if(bold_)DeleteObject(bold_);
        font_=f;bold_=b;
    }
    HWND add(int id,const wchar_t* cls,const std::wstring& label,DWORD style,int x,int y,int w,int h,bool page=true,DWORD ex=0){
        HWND control=CreateWindowExW(ex,cls,label.c_str(),WS_CHILD|WS_CLIPSIBLINGS|style,
            px(x),px(y),px(w),px(h),window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
        if(!control)return nullptr;
        children_.push_back({id,x,y,w,h,control,page});
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_?font_:GetStockObject(DEFAULT_GUI_FONT)),FALSE);
        if((style&BS_TYPEMASK)==BS_OWNERDRAW && wcscmp(cls,L"BUTTON")==0)
            SetWindowSubclass(control,hoverProc,1,reinterpret_cast<DWORD_PTR>(this));
        return control;
    }
    void button(int id,const std::wstring& label,int x,int y,int w,int h=28,bool page=true){
        add(id,L"BUTTON",label,WS_TABSTOP|BS_OWNERDRAW,x,y,w,h,page);
    }
    void label(const std::wstring& value,int x,int y,int w,int h=26){
        add(-1,L"STATIC",value,SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,x,y,w,h);
    }
    HWND editField(int id,const std::wstring& value,int x,int y,int w,bool numeric=false){
        HWND h=add(id,L"EDIT",value,WS_TABSTOP|ES_AUTOHSCROLL|(numeric?ES_NUMBER:0),x+6,y+5,w-12,18);
        if(h){SendMessageW(h,EM_SETLIMITTEXT,numeric?4:24,0);SendMessageW(h,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);SetWindowSubclass(h,fieldProc,2,reinterpret_cast<DWORD_PTR>(this));}
        return h;
    }
    bool isEditField(int id)const{return timeFields_.count(id)!=0||(id>=HotkeyStep&&id<HotkeyStep+4);}
    void check(int id,const std::wstring& value,int x,int y,int w,bool isChecked){
        checks_[id]=isChecked;button(id,value,x,y,w,26);
    }
    bool checked(int id)const{auto it=checks_.find(id);return it!=checks_.end() && it->second;}
    void combo(int id,const std::vector<std::wstring>& values,int selected,int x,int y,int w){
        if(values.empty())return;
        selected=std::clamp(selected,0,static_cast<int>(values.size())-1);
        choices_[id]={values,selected};button(id,values[selected],x,y,w,26);
    }
    int selection(int id)const{auto it=choices_.find(id);return it==choices_.end()?-1:it->second.selected;}
    int choiceCount()const{
        const auto it=choices_.find(menuChoice_);
        return it==choices_.end()?0:static_cast<int>(it->second.values.size());
    }
    int choiceRowHeight()const{return std::max(1,px(29));}
    void paintChoice(HWND h,HDC output){
        RECT r{};GetClientRect(h,&r);
        if(!output||r.right<=0||r.bottom<=0)return;
        HDC dc=choiceSurface_.ensure(output,r.right,r.bottom);if(!dc)dc=output;
        const auto p=palette_();fill(dc,r,p.chip);
        const auto it=choices_.find(menuChoice_);
        if(it!=choices_.end()){
            const int count=choiceCount();const bool scroll=count>choiceRows_;
            const int rowHeight=choiceRowHeight(),right=r.right-(scroll?px(12):0);
            for(int row=0;row<choiceRows_ && choiceFirst_+row<count;++row){
                const int index=choiceFirst_+row;
                RECT line{1,1+row*rowHeight,right-1,1+(row+1)*rowHeight};
                const bool active=index==choiceHighlight_;
                if(active){RECT bg=line;InflateRect(&bg,-px(2),-1);
                    const auto highlight=hc_()?GetSysColor(COLOR_HIGHLIGHT):mix(p.chip,p.text,.07);
                    rounded(dc,bg,px(8),highlight,highlight);}
                if(index==it->second.selected){RECT mark{line.left+px(5),line.top+px(6),line.left+px(8),line.bottom-px(6)};
                    const auto color=active&&hc_()?GetSysColor(COLOR_HIGHLIGHTTEXT):p.accent;
                    rounded(dc,mark,px(3),color,color);}
                line.left+=px(16);line.right-=px(6);
                text(dc,it->second.values[index],line,font_,active&&hc_()?GetSysColor(COLOR_HIGHLIGHTTEXT):p.text);
            }
            // Slim scroll indicator for long per-monitor lists. Wheel,
            // keyboard and dragging the indicator all share the viewport.
            if(scroll){const int track=std::max(1,static_cast<int>(r.bottom)-px(6));
                const int thumb=std::min(track,std::max(px(12),MulDiv(track,choiceRows_,count)));
                const int y=px(3)+MulDiv(track-thumb,choiceFirst_,count-choiceRows_);
                RECT bar{r.right-px(7),y,r.right-px(3),y+thumb};rounded(dc,bar,px(4),p.muted,p.muted);}
        }
        fill(dc,RECT{0,0,r.right,1},p.border);fill(dc,RECT{0,r.bottom-1,r.right,r.bottom},p.border);
        fill(dc,RECT{0,0,1,r.bottom},p.border);fill(dc,RECT{r.right-1,0,r.right,r.bottom},p.border);
        if(dc!=output)BitBlt(output,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
    }
    int choiceHit(int x,int y)const{
        if(!choicePopup_)return -1;RECT r{};GetClientRect(choicePopup_,&r);
        if(x<1||y<1||y>=r.bottom-1||x>=r.right-(choiceCount()>choiceRows_?px(12):1))return -1;
        const int row=(y-1)/choiceRowHeight(),index=choiceFirst_+row;
        return row<choiceRows_&&index<choiceCount()?index:-1;
    }
    void highlightChoice(int index,bool reveal){
        if(!choicePopup_)return;const int oldFirst=choiceFirst_,old=choiceHighlight_;
        choiceHighlight_=index;
        if(reveal&&index>=0){if(index<choiceFirst_)choiceFirst_=index;
            else if(index>=choiceFirst_+choiceRows_)choiceFirst_=index-choiceRows_+1;}
        choiceFirst_=std::clamp(choiceFirst_,0,std::max(0,choiceCount()-choiceRows_));
        if(old!=choiceHighlight_||oldFirst!=choiceFirst_)InvalidateRect(choicePopup_,nullptr,FALSE);
    }
    void scrollChoice(int first){
        const int next=std::clamp(first,0,std::max(0,choiceCount()-choiceRows_));
        if(next==choiceFirst_)return;choiceFirst_=next;
        choiceHighlight_=-1;InvalidateRect(choicePopup_,nullptr,FALSE);
    }
    void closeChoice(int picked=-1){
        const int id=menuChoice_;const HWND popup=choicePopup_;
        if(!popup&&id<0)return;
        // Clear ownership before ReleaseCapture/DestroyWindow can reenter us.
        choicePopup_=nullptr;menuChoice_=-1;choicePressed_=false;choiceScrollDrag_=false;choiceWheel_=0;
        if(popup){if(GetCapture()==popup)ReleaseCapture();DestroyWindow(popup);}
        choiceSurface_.reset();
        if(!window_||closing_)return;
        auto it=choices_.find(id);
        if(it!=choices_.end()&&picked>=0&&picked<static_cast<int>(it->second.values.size())&&picked!=it->second.selected){
            it->second.selected=picked;if(HWND h=ctrl(id)){SetWindowTextW(h,it->second.values[picked].c_str());InvalidateRect(h,nullptr,FALSE);}
            command(id,CBN_SELCHANGE);
        }
        if(displayRefreshDeferred_)displaysChanged();
    }
    bool choiceKey(WPARAM key){
        const auto it=choices_.find(menuChoice_);if(it==choices_.end())return false;
        const int count=choiceCount(),current=choiceHighlight_>=0?choiceHighlight_:it->second.selected;
        if(!count)return false;
        switch(key){
        case VK_ESCAPE:case VK_F4:case VK_MENU:closeChoice();return true;
        case VK_RETURN:case VK_SPACE:closeChoice(current);return true;
        case VK_UP:highlightChoice(std::max(0,current-1),true);return true;
        case VK_DOWN:highlightChoice(std::min(count-1,current+1),true);return true;
        case VK_HOME:highlightChoice(0,true);return true;
        case VK_END:highlightChoice(count-1,true);return true;
        case VK_PRIOR:highlightChoice(std::max(0,current-choiceRows_),true);return true;
        case VK_NEXT:highlightChoice(std::min(count-1,current+choiceRows_),true);return true;
        default:return false;
        }
    }
    void choiceChar(wchar_t c){
        const auto it=choices_.find(menuChoice_);if(it==choices_.end()||c<32)return;
        const int count=choiceCount();if(!count)return;
        const int current=choiceHighlight_>=0?choiceHighlight_:it->second.selected;
        for(int i=1;i<=count;++i){const int index=(current+i)%count;const auto& value=it->second.values[index];
            if(!value.empty()&&CompareStringOrdinal(&value[0],1,&c,1,TRUE)==CSTR_EQUAL){highlightChoice(index,true);return;}}
    }
    void choiceWheel(WPARAM w){
        choiceWheel_+=GET_WHEEL_DELTA_WPARAM(w);const int steps=choiceWheel_/WHEEL_DELTA;
        choiceWheel_%=WHEEL_DELTA;if(steps)scrollChoice(choiceFirst_-steps*3);
    }
    static LRESULT CALLBACK choiceProc(HWND h,UINT m,WPARAM w,LPARAM l){
        auto* self=reinterpret_cast<NightLightSettingsDialog*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<NightLightSettingsDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        if(!self)return DefWindowProcW(h,m,w,l);
        switch(m){
        case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);self->paintChoice(h,dc);EndPaint(h,&ps);return 0;}
        case WM_MOUSEMOVE:{const int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);const int hit=self->choiceHit(x,y);
            if(self->choiceScrollDrag_&&self->choiceCount()>self->choiceRows_){RECT r{};GetClientRect(h,&r);
                self->scrollChoice(MulDiv(std::clamp(y,0,static_cast<int>(r.bottom)-1),self->choiceCount()-self->choiceRows_,std::max(1L,r.bottom-1)));return 0;}
            self->highlightChoice(hit,false);return 0;}
        case WM_LBUTTONDOWN:{RECT r{};GetClientRect(h,&r);const POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if(!PtInRect(&r,pt)){self->closeChoice();return 0;}
            self->choicePressed_=true;const int hit=self->choiceHit(pt.x,pt.y);
            if(hit<0&&self->choiceCount()>self->choiceRows_&&pt.x>=r.right-self->px(12)){self->choiceScrollDrag_=true;self->scrollChoice(MulDiv(pt.y,self->choiceCount()-self->choiceRows_,std::max(1L,r.bottom-1)));}
            self->highlightChoice(hit,false);return 0;}
        case WM_LBUTTONUP:{const int hit=self->choiceHit(GET_X_LPARAM(l),GET_Y_LPARAM(l));
            const bool pressed=self->choicePressed_&&!self->choiceScrollDrag_;self->choicePressed_=false;self->choiceScrollDrag_=false;
            if(pressed&&hit>=0)self->closeChoice(hit);return 0;}
        case WM_RBUTTONDOWN:case WM_MBUTTONDOWN:case WM_CANCELMODE:self->closeChoice();return 0;
        case WM_CAPTURECHANGED:if(self->choicePopup_==h)self->closeChoice();return 0;
        case WM_MOUSEWHEEL:self->choiceWheel(w);return 0;
        case WM_NCDESTROY:
            SetWindowLongPtrW(h,GWLP_USERDATA,0);
            if(self->choicePopup_==h){self->choicePopup_=nullptr;self->menuChoice_=-1;self->choiceSurface_.reset();}
            break;
        }
        return DefWindowProcW(h,m,w,l);
    }
    void choose(int id){
        if(choicePopup_){closeChoice();return;}
        auto it=choices_.find(id);if(it==choices_.end()||it->second.values.empty())return;
        HWND control=ctrl(id);if(!control)return;
        constexpr wchar_t name[]=L"NightLight.Settings.Choice";
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance_;wc.lpfnWndProc=choiceProc;wc.lpszClassName=name;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        RECT anchor{};GetWindowRect(control,&anchor);MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromWindow(control,MONITOR_DEFAULTTONEAREST),&mi))return;
        const int below=std::max(0L,mi.rcWork.bottom-anchor.bottom),above=std::max(0L,anchor.top-mi.rcWork.top);
        const int count=static_cast<int>(it->second.values.size());const int rowHeight=choiceRowHeight();
        const bool upward=count*rowHeight+2>below&&above>below;
        const int space=upward?above:below;
        choiceRows_=std::min(count,std::max(1,(space-2)/rowHeight));
        int width=std::max(px(180),static_cast<int>(anchor.right-anchor.left));
        if(HDC dc=GetDC(control)){auto old=SelectObject(dc,font_?font_:GetStockObject(DEFAULT_GUI_FONT));
            for(const auto& value:it->second.values){SIZE size{};GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&size);width=std::max(width,static_cast<int>(size.cx)+px(34));}
            if(old&&old!=HGDI_ERROR)SelectObject(dc,old);ReleaseDC(control,dc);}
        if(choiceRows_<count)width+=px(12);width=std::min(width,static_cast<int>(mi.rcWork.right-mi.rcWork.left));
        const int height=choiceRows_*rowHeight+2;
        const int x=std::clamp(static_cast<int>(anchor.left),static_cast<int>(mi.rcWork.left),static_cast<int>(mi.rcWork.right)-width);
        const int y=std::clamp(static_cast<int>(upward?anchor.top-height:anchor.bottom),static_cast<int>(mi.rcWork.top),std::max(static_cast<int>(mi.rcWork.top),static_cast<int>(mi.rcWork.bottom)-height));
        menuChoice_=id;choiceHighlight_=it->second.selected;choiceFirst_=std::clamp(choiceHighlight_-choiceRows_+1,0,std::max(0,count-choiceRows_));
        choicePressed_=false;choiceScrollDrag_=false;choiceWheel_=0;
        choicePopup_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,name,L"",WS_POPUP,x,y,width,height,window_,nullptr,instance_,this);
        if(!choicePopup_){menuChoice_=-1;return;}
        SetWindowPos(choicePopup_,HWND_TOP,x,y,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        SetCapture(choicePopup_);
        if(GetCapture()!=choicePopup_)closeChoice();
    }
    void slider(int id,int low,int high,int step,int current,int kind,size_t point,int x,int y,int w){
        sliders_[id]={low,high,step,std::clamp(current,low,high),kind,point};
        button(id,L"",x,y,w,33);
        if(HWND h=ctrl(id))SetWindowSubclass(h,fieldProc,2,reinterpret_cast<DWORD_PTR>(this));
    }
    void setSlider(HWND h,int value){
        const int id=GetDlgCtrlID(h);auto it=sliders_.find(id);if(it==sliders_.end())return;
        auto& v=it->second;value=std::clamp(value,v.low,v.high);
        if(value==v.value)return;
        v.value=value;
        if(v.kind==2)draft_.transitionMinutes=value;
        else if(v.point<draft_.points.size()){
            if(v.kind==0)draft_.points[v.point].minute=value;
            else draft_.points[v.point].kelvin=value;
        }
        InvalidateRect(h,nullptr,FALSE);
        // Full-control repaint, never native partial-track redraws or stale thumbs.
        ::UpdateWindow(h);
    }
    void slideAt(HWND h,int x){
        const auto it=sliders_.find(GetDlgCtrlID(h));if(it==sliders_.end())return;
        const auto& v=it->second;RECT r{};GetClientRect(h,&r);
        const int left=px(12),right=std::max(left+1,static_cast<int>(r.right)-px(74));
        const int position=std::clamp(x,left,right);
        const int raw=v.low+MulDiv(position-left,v.high-v.low,right-left);
        const int next=position==right?v.high:position==left?v.low:v.low+((raw-v.low+v.step/2)/v.step)*v.step;
        setSlider(h,next);
        if(v.kind==1 && activeSlider_==GetDlgCtrlID(h))preview_(v.value);
    }
    void section(int top,int bottom,const std::wstring& heading){
        cards_.push_back({14,top,546,bottom});
        headings_.push_back({RECT{26,top+7,534,top+32},heading});
    }
    void destroyRefreshTooltip(){
        if(refreshTooltip_ && IsWindow(refreshTooltip_))DestroyWindow(refreshTooltip_);
        refreshTooltip_=nullptr;
    }
    void createRefreshTooltip(){
        destroyRefreshTooltip();
        const HWND button=ctrl(DisplayRefresh);if(!button)return;
        refreshTooltip_=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,
            WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,CW_USEDEFAULT,CW_USEDEFAULT,
            CW_USEDEFAULT,CW_USEDEFAULT,window_,nullptr,instance_,nullptr);
        if(!refreshTooltip_)return;
        TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
        tool.hwnd=window_;tool.uId=reinterpret_cast<UINT_PTR>(button);
        tool.lpszText=const_cast<wchar_t*>(nightlight::translate(language_(),L"Detect currently attached displays"));
        SendMessageW(refreshTooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));
        SendMessageW(refreshTooltip_,TTM_SETMAXTIPWIDTH,0,px(430));
    }
    void clearPage(){
        destroyRefreshTooltip();
        finishChipMotion();chipMotion_.clear();chipWidths_.clear();dragSlots_.clear();
        finishRename(true);displayKeys_.clear();dragChip_=nullptr;dropChip_=nullptr;displayRefreshDeferred_=false;
        for(auto it=children_.begin();it!=children_.end();){
            if(it->page){DestroyWindow(it->hwnd);it=children_.erase(it);}else ++it;
        }
        cards_.clear();headings_.clear();backendKeys_.clear();choices_.clear();checks_.clear();sliders_.clear();timeFields_.clear();solarLabels_.clear();activeSlider_=-1;KillTimer(window_,PreviewTimer);preview_(std::nullopt);
    }
    void refreshHotkeyFields(){
        if(scope_<0 || static_cast<size_t>(scope_)>=scopeKeys_.size())return;
        const auto prefs=read_();
        std::array<int,4> values{};
        if(auto it=prefs.hotkeys.find(scopeKeys_[scope_]);it!=prefs.hotkeys.end())values=it->second;
        hotkeyValues_=values;
        const auto steps=nightlight::hotkeyStepsFor(prefs.hotkeySteps,scopeKeys_[scope_]);
        for(int i=0;i<4;++i){InvalidateRect(ctrl(HotkeyFirst+i),nullptr,FALSE);if(HWND h=ctrl(HotkeyStep+i))SetWindowTextW(h,std::to_wstring(steps[i]).c_str());}
    }
    static std::wstring timeText(int minute){
        SYSTEMTIME time{};time.wHour=static_cast<WORD>(minute/60);time.wMinute=static_cast<WORD>(minute%60);
        wchar_t value[128]{};
        if(GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT,TIME_NOSECONDS,&time,nullptr,value,128))return value;
        swprintf_s(value,L"%02d:%02d",minute/60,minute%60);return value;
    }
    static std::optional<int> parseTimeText(std::wstring text){
        for(auto& c:text)if(c==0x00a0||c==0x202f)c=L' ';
        if(auto parsed=nightlight::parseScheduleTime(text))return parsed;
        // Accept the user's localized AM/PM designators and separator as
        // well as the language-independent formats supported by the parser.
        auto locale=[](LCTYPE type){wchar_t out[128]{};GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT,type,out,128);return std::wstring(out);};
        auto lower=[](std::wstring v){for(auto& c:v)c=static_cast<wchar_t>(std::towlower(c));return v;};
        text=lower(std::move(text));
        for(const auto& item:std::array<std::pair<LCTYPE,const wchar_t*>,2>{{{LOCALE_S1159,L"am"},{LOCALE_S2359,L"pm"}}}){
            auto marker=lower(locale(item.first));if(marker.empty())continue;
            const auto at=text.find(marker);if(at!=std::wstring::npos){text.erase(at,marker.size());text+=L" ";text+=item.second;break;}
        }
        auto separator=locale(LOCALE_STIME);if(!separator.empty()&&separator!=L":"){
            if(auto at=text.find(separator);at!=std::wstring::npos)text.replace(at,separator.size(),L":");
        }
        return nightlight::parseScheduleTime(std::move(text));
    }
    std::wstring fieldValue(int id)const{wchar_t buffer[64]{}; if(HWND h=ctrl(id)) GetWindowTextW(h,buffer,64); return buffer;}
    void buildGeneral(){
        const auto p=read_();
        section(52,207,tr(L"General",L"Geral"));
        label(tr(L"Language",L"Idioma"),26,86,220);
        combo(Language,nightlight::languageNames(),nightlight::languageIndex(p.locale),280,86,254);
        label(tr(L"Theme",L"Tema"),26,116,220);
        combo(Theme,{tr(L"Follow system",L"Seguir sistema"),tr(L"Light",L"Claro"),tr(L"Dark",L"Escuro")},
            p.theme==L"light"?1:p.theme==L"dark"?2:0,280,116,254);
        check(CloseOutside,tr(L"Close the app to tray when losing focus",L"Fechar a aplicação para a área de notificação ao perder o foco"),26,148,508,p.closeByClickingOutside);
        const auto startupText=tr(L"Start with Windows",L"Iniciar com o Windows");
        const auto startupHint=nightlight::translate(language_(),L"If changing the exe location, disable and enable this again for the task to update paths");
        int startupWidth=210,hintHeight=42;
        if(HDC dc=GetDC(window_)){
            auto old=SelectObject(dc,font_?font_:GetStockObject(DEFAULT_GUI_FONT));SIZE measured{};
            GetTextExtentPoint32W(dc,startupText,static_cast<int>(wcslen(startupText)),&measured);
            startupWidth=std::clamp(MulDiv(measured.cx,96,static_cast<int>(dpi_))+30,150,250);
            RECT r{0,0,px(508-startupWidth-8),0};DrawTextW(dc,startupHint,-1,&r,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
            hintHeight=std::max(26,MulDiv(r.bottom-r.top,96,static_cast<int>(dpi_))+6);
            if(old)SelectObject(dc,old);ReleaseDC(window_,dc);
        }
        check(Startup,startupText,26,174,startupWidth,startup_());
        add(StartupHint,L"STATIC",startupHint,SS_LEFT|SS_NOPREFIX,26+startupWidth+8,177,508-startupWidth-8,hintHeight);
        const int startupExtra=std::max(0,hintHeight-24);
        const std::wstring wheelLabel=tr(L"Tray icon mousewheel control",L"Controlo pela roda do rato no ícone");
        const std::vector<std::wstring> wheelChoices{tr(L"Color temperature",L"Temperatura de cor"),tr(L"Brightness",L"Brilho"),tr(L"Contrast",L"Contraste")};
        int checkboxNeeded=280,choiceNeeded=180;
        if(HDC dc=GetDC(window_)){
            auto old=SelectObject(dc,font_?font_:GetStockObject(DEFAULT_GUI_FONT));
            auto measure=[&](const std::wstring& value,int padding){
                SIZE size{};GetTextExtentPoint32W(dc,value.c_str(),static_cast<int>(value.size()),&size);
                return (static_cast<int>(size.cx)*96+static_cast<int>(dpi_)-1)/static_cast<int>(dpi_)+padding;
            };
            checkboxNeeded=measure(wheelLabel,28);choiceNeeded=0;
            for(const auto& choice:wheelChoices)choiceNeeded=std::max(choiceNeeded,measure(choice,41));
            if(old && old!=HGDI_ERROR)SelectObject(dc,old);ReleaseDC(window_,dc);
        }
        const int wheelWidth=nightlight::trayWheelDropdownWidth(checkboxNeeded,choiceNeeded);
        const int wheelY=207+startupExtra;
        check(TrayWheel,wheelLabel,26,wheelY,500-wheelWidth,p.trayWheelEnabled);
        combo(TrayWheelMode,wheelChoices,p.trayWheelMode,534-wheelWidth,wheelY,wheelWidth);
        EnableWindow(ctrl(TrayWheelMode),p.trayWheelEnabled);
        const int extra=startupExtra+38;cards_.back().bottom+=extra;
        section(217+extra,452+extra,tr(L"Global hotkeys",L"Atalhos de teclado globais"));
        const auto connected=monitors_();
        std::vector<std::wstring> names{tr(L"All monitors",L"Todos os ecrãs")};scopeKeys_={L""};
        for(const auto& m:connected){
            names.push_back(m.displayName());scopeKeys_.push_back(nightlight::monitorProfileKey(m));
        }
        for(const auto& entry:p.hotkeys)if(std::find(scopeKeys_.begin(),scopeKeys_.end(),entry.first)==scopeKeys_.end()){
            names.push_back(std::wstring(tr(L"Disconnected: ",L"Desligado: "))+entry.first);scopeKeys_.push_back(entry.first);
        }
        const bool showScope=connected.size()>1;
        scope_=showScope?std::clamp(scope_,0,static_cast<int>(scopeKeys_.size())-1):0;
        if(showScope){
            label(tr(L"Apply shortcuts to",L"Aplicar atalhos a"),26,251+extra,230);
            combo(Scope,names,scope_,280,251+extra,254);
        }
        const int hotkeyTop=(showScope?282:251)+extra;
        const wchar_t* labels[]={tr(L"Increase brightness",L"Aumentar brilho"),
            tr(L"Decrease brightness",L"Reduzir brilho"),
            tr(L"Increase temperature",L"Aumentar temperatura"),
            tr(L"Decrease temperature",L"Reduzir temperatura")};
        for(int i=0;i<4;++i){
            const int y=hotkeyTop+i*32;label(labels[i],26,y,160);
            label(i%2==0?L"+":L"−",188,y,13);
            editField(HotkeyStep+i,L"",201,y,54,true);
            label(i<2?L"%":L"K",261,y,21);
            button(HotkeyFirst+i,L"",286,y,248,26);
            if(HWND field=ctrl(HotkeyFirst+i))SetWindowSubclass(field,fieldProc,2,reinterpret_cast<DWORD_PTR>(this));
        }
        refreshHotkeyFields();
        const int buttonsY=hotkeyTop+133;cards_.back().bottom=buttonsY+39;desiredClientHeight_=cards_.back().bottom+52;
        button(ClearHotkeys,tr(L"Clear",L"Limpar"),280,buttonsY,95);
        button(SaveHotkeys,tr(L"Save hotkeys",L"Guardar atalhos"),383,buttonsY,151);
    }
    void buildAbout(){
        section(52,236,tr(L"About NightLight",L"Acerca do NightLight"));
        label(tr(L"Version",L"Versão"),26,86,116);
        label(nightlight::kAppVersionWide,150,86,384);
        label(tr(L"Author",L"Autor"),26,120,116);
        label(L"iamfromreallife",150,120,384);
        label(tr(L"GitHub link",L"Ligação GitHub"),26,154,116);
        button(AboutRepository,L"https://github.com/iamfromreallife/NightLightFlor",150,154,384,26);
        label(tr(L"License",L"Licença"),26,188,116);
        button(AboutLicense,nightlight::translate(language_(),L"MIT License"),150,188,384,26);
        desiredClientHeight_=288;
    }
    void buildBehavior(){
        const auto p=read_();const auto monitors=monitors_();
        // Use the same physical display profiles as the main window. The
        // chips wrap naturally; their switch is separate from drag/rename.
        int x=26,y=86;const int right=534;
        section(52,140,tr(L"Displays",L"Ecrãs"));
        button(DisplayRefresh,nightlight::translate(language_(),L"Detect currently attached displays"),502,56,32,28);
        createRefreshTooltip();
        for(size_t i=0;i<monitors.size();++i){
            const auto& m=monitors[i];const auto key=nightlight::monitorProfileKey(m);
            displayKeys_.push_back(key);
            HDC dc=GetDC(window_);SIZE measured{};
            if(dc){auto old=SelectObject(dc,bold_?bold_:GetStockObject(DEFAULT_GUI_FONT));
                const auto name=m.displayName();GetTextExtentPoint32W(dc,name.c_str(),static_cast<int>(name.size()),&measured);
                if(old)SelectObject(dc,old);ReleaseDC(window_,dc);}
            const int w=std::clamp(MulDiv(measured.cx,96,static_cast<int>(dpi_))+88,110,508);
            if(x+w>right){x=26;y+=38;}
            button(DisplayFirst+static_cast<int>(i),m.displayName(),x,y,w,32);
            if(HWND chip=ctrl(DisplayFirst+static_cast<int>(i)))SetWindowSubclass(chip,displayProc,3,reinterpret_cast<DWORD_PTR>(this));
            x+=w+6;
        }
        if(monitors.empty())label(tr(L"No connected displays",L"Nenhum ecrã ligado"),26,y,508);
        displayRowsBottom_=y;
        y+=40;
        add(DisplayHint1,L"STATIC",tr(L"Rearrange displays by dragging",L"Reordene os ecrãs arrastando-os"),SS_LEFT|SS_CENTERIMAGE,26,y,508,20);
        add(DisplayHint2,L"STATIC",tr(L"Rename displays by double clicking",L"Mude o nome dos ecrãs com um duplo clique"),SS_LEFT|SS_CENTERIMAGE,26,y+20,508,20);
        y+=48;cards_.back().bottom=y;
        section(y+10,y+78,tr(L"Display controls",L"Controlos dos ecrãs"));
        check(Ddc,tr(L"DDC/CI Controls",L"Controlos DDC/CI"),26,y+44,508,p.ddcControls);
        const int top=y+88;
        const int pages=std::max(1,(static_cast<int>(monitors.size())+5)/6);
        behaviorPage_=std::clamp(behaviorPage_,0,pages-1);
        const int count=std::min(6,std::max(0,static_cast<int>(monitors.size())-behaviorPage_*6));
        const int bottom=top+44+std::max(1,count)*37+(pages>1?34:0);
        section(top,bottom,tr(L"Colour correction engine",L"Motor de correção de cor"));
        if(monitors.empty())label(tr(L"No connected displays",L"Nenhum ecrã ligado"),26,top+36,508);
        for(int i=0;i<count;++i){
            const size_t index=static_cast<size_t>(behaviorPage_*6+i);
            const auto& m=monitors[index];backendKeys_.push_back(nightlight::monitorProfileKey(m));
            const int row=top+38+i*37;
            label(m.displayName(),26,row,245);
            combo(EngineFirst+i,{tr(L"GPU gamma",L"Gama da GPU"),
                tr(L"Monitor hardware (DDC/CI)",L"Hardware do ecrã (DDC/CI)"),
                tr(L"NVIDIA target gamma",L"Gama alvo NVIDIA")},
                m.nvidiaTargetColor?2:m.hardwareColor?1:0,280,row,254);
        }
        if(pages>1){
            const int row=bottom-35;
            label(std::to_wstring(behaviorPage_+1)+L" / "+std::to_wstring(pages),26,row,120);
            button(Previous,tr(L"Previous",L"Anterior"),280,row,123);
            button(Next,tr(L"Next",L"Seguinte"),411,row,123);
            EnableWindow(ctrl(Previous),behaviorPage_>0);EnableWindow(ctrl(Next),behaviorPage_+1<pages);
        }
        desiredClientHeight_=std::max(400,bottom+52);
    }
    std::wstring solarCaption(nightlight::SchedulePointKind kind)const{
        if(kind==nightlight::SchedulePointKind::Sunrise)return tr(L"Sunrise",L"Nascer do sol");
        if(kind==nightlight::SchedulePointKind::Noon)return tr(L"Noon",L"Meio-dia");
        return tr(L"Sunset",L"Pôr do sol");
    }
    bool collectTimes(bool show=true){
        auto points=draft_.points;
        for(const auto& entry:timeFields_){auto value=parseTimeText(fieldValue(entry.first));
            if(!value){if(show){showError(L"Enter a valid time, e.g. 7pm, 7h00 or 7.30.",L"Introduza uma hora válida, por exemplo 7pm, 7h00 ou 7.30.");SetFocus(ctrl(entry.first));}return false;}
            if(entry.second<points.size()){auto& p=points[entry.second];if(p.minute!=*value && p.kind!=nightlight::SchedulePointKind::Custom)p.customTime=true;p.minute=*value;}
        }
        if(!nightlight::uniqueScheduleTimes(points)){if(show)showError(L"Each schedule point must have a different time.",L"Cada ponto do horário tem de ter uma hora diferente.");return false;}
        draft_.points=std::move(points);return true;
    }
    void solarMenu(size_t index,LPARAM position){
        if(index>=draft_.points.size())return;
        const auto kind=draft_.points[index].kind;
        HMENU menu=CreatePopupMenu();if(!menu)return;
        std::wstring custom=kind==nightlight::SchedulePointKind::Sunrise?tr(L"Set custom sunrise time",L"Definir hora personalizada do nascer do sol"):kind==nightlight::SchedulePointKind::Noon?tr(L"Set custom noon time",L"Definir hora personalizada do meio-dia"):tr(L"Set custom sunset time",L"Definir hora personalizada do pôr do sol");
        AppendMenuW(menu,MF_STRING,1,custom.c_str());
        AppendMenuW(menu,MF_STRING|(draft_.locationName.empty()?MF_GRAYED:0),2,tr(L"Reset to current location's time",L"Repor hora da localização atual"));
        POINT pt{GET_X_LPARAM(position),GET_Y_LPARAM(position)};if(position==-1){RECT r{};GetWindowRect(ctrl(PointTimeField+static_cast<int>(index)),&r);pt={r.left,r.bottom};}
        const UINT selected=TrackPopupMenuEx(menu,TPM_RETURNCMD|TPM_NONOTIFY,pt.x,pt.y,window_,nullptr);DestroyMenu(menu);
        if(!window_||!selected||index>=draft_.points.size())return;
        if(selected==1){draft_.points[index].customTime=true;HWND h=ctrl(PointTimeField+static_cast<int>(index));SetFocus(h);SendMessageW(h,EM_SETSEL,0,-1);return;}
        const auto times=nightlight::localSolarTimes(draft_.latitude,draft_.longitude);const int n=static_cast<int>(kind)-1;
        if(n<0||n>=3||!times[n]){showError(L"This solar event does not occur today at the selected location.",L"Este evento solar não ocorre hoje na localização selecionada.");return;}
        auto& point=draft_.points[index];point.customTime=false;point.minute=*times[n];SetWindowTextW(ctrl(PointTimeField+static_cast<int>(index)),timeText(point.minute).c_str());
    }
    void openLocation(){
        if(locationOpen_||!collectTimes())return;
        locationOpen_=true;general_(25,1);NightLightLocationDialog dialog(instance_,window_,language_(),palette_,dark_,hc_);
        const auto selected=dialog.run(draft_);locationOpen_=false;general_(25,0);
        if(!selected||!window_||closing_)return;
        const auto times=nightlight::localSolarTimes(selected->latitude,selected->longitude);
        if(!times[0]||!times[1]||!times[2]){showError(L"This location has no sunrise or sunset today. Choose a different location or keep custom times.",L"Esta localização não tem nascer ou pôr do sol hoje. Escolha outra localização ou mantenha as horas personalizadas.");return;}
        auto next=draft_;const bool first=next.locationName.empty();
        next.locationName=selected->name;next.latitude=selected->latitude;next.longitude=selected->longitude;
        // Migrate the original two anonymous daily points without losing
        // their temperatures. More complex schedules retain custom points.
        if(first&&next.points.size()==2&&next.points[0].kind==nightlight::SchedulePointKind::Custom&&next.points[1].kind==nightlight::SchedulePointKind::Custom){
            std::sort(next.points.begin(),next.points.end(),[](auto a,auto b){return a.minute<b.minute;});
            next.points[0].kind=nightlight::SchedulePointKind::Sunrise;next.points[0].customTime=false;
            next.points[1].kind=nightlight::SchedulePointKind::Sunset;next.points[1].customTime=false;
        }
        const int defaults[]={6500,5000,3500};
        for(int i=0;i<3;++i){const auto kind=static_cast<nightlight::SchedulePointKind>(i+1);
            auto found=std::find_if(next.points.begin(),next.points.end(),[&](const auto& p){return p.kind==kind;});
            if(found==next.points.end()){if(next.points.size()>=nightlight::kMaxSchedulePoints){showError(L"Remove a custom time before adding solar points.",L"Remova uma hora personalizada antes de adicionar pontos solares.");return;}next.points.push_back({*times[i],defaults[i],kind,false});}
            else if(!found->customTime){found->minute=*times[i];found->customTime=false;}
        }
        if(!nightlight::uniqueScheduleTimes(next.points)){showError(L"A solar time overlaps an existing custom time. Change the custom time and try again.",L"Uma hora solar coincide com uma hora personalizada. Altere a hora personalizada e tente novamente.");return;}
        draft_=nightlight::normalizedSchedule(std::move(next));schedule_(draft_);rebuild();
    }
    void refreshEditorSolarTimes(bool force=false){
        if(!draftLoaded_||draft_.locationName.empty())return;
        SYSTEMTIME now{};GetLocalTime(&now);TIME_ZONE_INFORMATION tz{};const DWORD state=GetTimeZoneInformation(&tz);
        const int date=now.wYear*10000+now.wMonth*100+now.wDay;
        const int bias=tz.Bias+(state==TIME_ZONE_ID_DAYLIGHT?tz.DaylightBias:state==TIME_ZONE_ID_STANDARD?tz.StandardBias:0);
        if(!force&&date==solarUiDate_&&bias==solarUiBias_)return;
        const auto times=nightlight::localSolarTimes(draft_.latitude,draft_.longitude);
        bool deferred=false;
        for(size_t i=0;i<draft_.points.size();++i){auto& point=draft_.points[i];const int n=static_cast<int>(point.kind)-1;
            if(point.customTime||n<0||n>=3||!times[n])continue;
            HWND field=ctrl(PointTimeField+static_cast<int>(i));
            if(field){
                if(GetFocus()==field){deferred=true;continue;}
                auto typed=parseTimeText(fieldValue(PointTimeField+static_cast<int>(i)));
                if(!typed||*typed!=point.minute)continue; // preserve uncommitted input
                SetWindowTextW(field,timeText(*times[n]).c_str());
            }
            point.minute=*times[n];
        }
        if(!deferred){solarUiDate_=date;solarUiBias_=bias;}
    }
    void buildAutomation(){
        if(!draftLoaded_){draft_=nightlight::normalizedSchedule(read_().colorSchedule);draftLoaded_=true;}
        refreshEditorSolarTimes(true);
        std::stable_sort(draft_.points.begin(),draft_.points.end(),[](auto a,auto b){return a.minute<b.minute;});
        int y=84;
        section(52,0,tr(L"Automatic colour temperature",L"Temperatura de cor automática"));
        check(ScheduleEnabled,tr(L"Enable time-based colour temperature",L"Ativar temperatura de cor por horário"),26,y,354,draft_.enabled);
        button(SetLocation,tr(L"Set location...",L"Definir localização..."),390,y-2,144,28);
        y+=28;
        // One aligned row: selected city on the left, location explanation
        // right-aligned with the location button, with no overlapping HWNDs.
        if(!draft_.locationName.empty())add(LocalTimeNote,L"STATIC",draft_.locationName,SS_LEFT|SS_CENTERIMAGE|SS_ENDELLIPSIS,45,y,258,22);
        add(LocationHint,L"STATIC",tr(L"Determines sunrise and sunset",L"Determina o nascer e o pôr do sol"),SS_RIGHT|SS_CENTERIMAGE,308,y,226,22);
        y+=25;
        y+=8;label(tr(L"Transition",L"Transição"),26,y,120);
        combo(Transition,{tr(L"Automatic",L"Automática"),tr(L"Custom (minutes)",L"Personalizada (minutos)"),tr(L"Instant",L"Instantânea")},static_cast<int>(draft_.mode),280,y,254);y+=34;
        if(draft_.mode==nightlight::ScheduleMode::Custom){label(tr(L"Duration (minutes)",L"Duração (minutos)"),26,y,220);slider(Duration,1,120,1,draft_.transitionMinutes,2,0,280,y-3,254);y+=38;}
        label(tr(L"Time of day",L"Hora do dia"),26,y,160);label(tr(L"Colour temperature",L"Temperatura de cor"),270,y,264);y+=30;
        for(size_t i=0;i<draft_.points.size();++i){
            auto& point=draft_.points[i];const bool solar=point.kind!=nightlight::SchedulePointKind::Custom;
            const std::wstring caption=solar?solarCaption(point.kind):std::wstring(tr(L"Time",L"Hora"))+L" "+std::to_wstring(i+1);
            const int labelId=220+static_cast<int>(i),timeId=PointTimeField+static_cast<int>(i);
            HWND heading=add(labelId,L"STATIC",caption,SS_LEFT|SS_CENTERIMAGE|SS_NOTIFY,26,y,116,26);
            if(solar&&heading){solarLabels_[labelId]=i;SetWindowSubclass(heading,fieldProc,2,reinterpret_cast<DWORD_PTR>(this));}
            timeFields_[timeId]=i;editField(timeId,timeText(point.minute),146,y,112);
            slider(PointKelvin+static_cast<int>(i),1000,6500,10,point.kelvin,1,i,270,y-3,224);
            if(!solar){button(PointRemove+static_cast<int>(i),L"×",501,y,33,26);EnableWindow(ctrl(PointRemove+static_cast<int>(i)),draft_.points.size()>2);}y+=38;
        }
        button(PointAdd,tr(L"+ Add time",L"+ Adicionar hora"),26,y+4,144);EnableWindow(ctrl(PointAdd),draft_.points.size()<nightlight::kMaxSchedulePoints);
        button(ApplySchedule,tr(L"Apply schedule",L"Aplicar horário"),383,y+4,151);
        const int firstBottom=y+44;cards_.back().bottom=firstBottom;
        const int dimTop=firstBottom+10;section(dimTop,dimTop+76,tr(L"Dim screens on inactivity",L"Reduzir o brilho dos ecrãs por inatividade"));
        add(DimStatus,L"STATIC",read_().dimInactive?tr(L"Enabled",L"Ativo"):tr(L"Disabled",L"Inativo"),SS_LEFT|SS_CENTERIMAGE,26,dimTop+38,255,26);
        button(DimSettings,tr(L"Settings...",L"Definições..."),383,dimTop+36,151);
        desiredClientHeight_=dimTop+76+52;
    }
    void addSchedulePoint(){
        if(draft_.points.size()>=nightlight::kMaxSchedulePoints||!collectTimes())return;
        auto sorted=nightlight::normalizedSchedule(draft_);
        int bestGap=0,bestTime=0;
        for(size_t i=0;i<sorted.points.size();++i){
            int a=sorted.points[i].minute,b=sorted.points[(i+1)%sorted.points.size()].minute;
            const int gap=(b-a+1440)%1440;
            if(gap>bestGap){bestGap=gap;bestTime=(a+gap/2)%1440;}
        }
        if(bestGap<2)return;
        const auto target=nightlight::scheduledColor(sorted,bestTime*60);
        draft_.points.push_back({bestTime,target.kelvin,nightlight::SchedulePointKind::Custom,true});
        draft_=nightlight::normalizedSchedule(std::move(draft_));
        const auto inserted=std::find_if(draft_.points.begin(),draft_.points.end(),[&](const auto& p){return p.minute==bestTime;});
        const int index=static_cast<int>(inserted-draft_.points.begin());
        rebuild();if(HWND h=ctrl(PointTimeField+index)){SetFocus(h);ensureVisible(h);SendMessageW(h,EM_SETSEL,0,-1);}
    }
    void layout(){
        if(!window_)return;
        RECT client{};GetClientRect(window_,&client);const int page=std::max(1,static_cast<int>(client.bottom));
        scrollY_=std::clamp(scrollY_,0,std::max(0,px(desiredClientHeight_)-page));
        if(GetWindowLongPtrW(window_,GWL_STYLE)&WS_VSCROLL){SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,std::max(0,px(desiredClientHeight_)-1),static_cast<UINT>(page),scrollY_,0};SetScrollInfo(window_,SB_VERT,&info,TRUE);}
        const auto now=uiTime();
        // Batch moves in a single frame, without intermediate background erases.
        HDWP batch=BeginDeferWindowPos(static_cast<int>(children_.size()));
        for(const auto& item:children_){
            int x=px(item.x),y=px(item.y)-scrollY_;
            if(item.id>=DisplayFirst && static_cast<size_t>(item.id-DisplayFirst)<chipMotion_.size()){
                auto& motion=chipMotion_[item.id-DisplayFirst];
                x=static_cast<int>(std::lround(motion.x.value(now)*dpi_/96.0));
                y=static_cast<int>(std::lround(motion.y.value(now)*dpi_/96.0))-scrollY_;
            }
            if(batch)batch=DeferWindowPos(batch,item.hwnd,nullptr,x,y,px(item.w),px(item.h),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW);
            else SetWindowPos(item.hwnd,nullptr,x,y,px(item.w),px(item.h),SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW);
        }
        if(batch)EndDeferWindowPos(batch);
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_NOERASE);
    }
    void scrollTo(int y){
        RECT client{};GetClientRect(window_,&client);
        const int next=std::clamp(y,0,std::max(0,px(desiredClientHeight_)-std::max(1,static_cast<int>(client.bottom))));
        if(next==scrollY_)return;
        scrollY_=next;layout();
    }
    void ensureVisible(HWND h){if(!h||!(GetWindowLongPtrW(window_,GWL_STYLE)&WS_VSCROLL))return;RECT r{},client{};GetWindowRect(h,&r);MapWindowPoints(nullptr,window_,reinterpret_cast<POINT*>(&r),2);GetClientRect(window_,&client);
        if(r.top<0)scrollTo(scrollY_+r.top-px(8));else if(r.bottom>client.bottom)scrollTo(scrollY_+r.bottom-client.bottom+px(8));}
    void rebuild(){
        if(!window_ || building_ || menuChoice_>=0)return;
        building_=true;
        clearPage();
        if(fontLanguage_!=language_())fonts();
        SetWindowTextW(ctrl(GeneralTab),tr(L"General",L"Geral"));
        SetWindowTextW(ctrl(BehaviorTab),tr(L"Displays",L"Ecrãs"));
        SetWindowTextW(ctrl(AutomationTab),tr(L"Automation",L"Automatização"));
        SetWindowTextW(ctrl(AboutTab),tr(L"About",L"Acerca"));
        SetWindowTextW(ctrl(CloseButton),tr(L"Close",L"Fechar"));
        SetWindowTextW(ctrl(CheckUpdates),tr(L"Check for updates...",L"Procurar atualizações..."));
        SetWindowTextW(ctrl(CaptureLogs),general_(27,-1)?tr(L"Stop capture",L"Parar captura"):tr(L"Capture logs",L"Capturar registos"));
        SetWindowTextW(window_,tr(L"NightLight settings",L"Definições do NightLight"));
        desiredClientHeight_=550;
        if(tab_==0)buildGeneral();else if(tab_==1)buildBehavior();else if(tab_==2)buildAutomation();else buildAbout();
        if(!cards_.empty())cards_.front().top=42;
        for(auto& item:children_)if(item.id==CloseButton || item.id==CheckUpdates || item.id==CaptureLogs)item.y=desiredClientHeight_-40;
        resizeForContent();
        if(tab_==2&&!draft_.locationName.empty())SetTimer(window_,SolarUiTimer,30000,nullptr);else KillTimer(window_,SolarUiTimer);
        refreshTheme();
        for(const auto& child:children_)if(child.page)ShowWindow(child.hwnd,SW_SHOWNA);
        layout();building_=false;
    }
    void paint(HDC output){
        RECT r{};GetClientRect(window_,&r);if(!output || r.right<=0 || r.bottom<=0)return;
        HDC dc=surface_.ensure(output,r.right,r.bottom);if(!dc)dc=output;
        const auto p=palette_();fill(dc,r,p.page);
        for(size_t i=0;i<cards_.size();++i){const auto& card=cards_[i];
            RECT bounds{px(card.left),px(card.top)-scrollY_,px(card.right),px(card.bottom)-scrollY_};
            rounded(dc,bounds,px(12),p.card,p.border);
            if(i==0){
                // The card meets the browser-style tabs on a straight edge.
                // Do not leave a rounded corner or a gap beneath the tab.
                fill(dc,RECT{bounds.left,bounds.top,bounds.right,bounds.top+px(7)},p.card);
                fill(dc,RECT{bounds.left,bounds.top,bounds.right,bounds.top+1},p.border);
                fill(dc,RECT{bounds.left,bounds.top,bounds.left+1,bounds.top+px(7)},p.border);
                fill(dc,RECT{bounds.right-1,bounds.top,bounds.right,bounds.top+px(7)},p.border);
            }
        }
        for(const auto& h:headings_)text(dc,h.second,RECT{px(h.first.left),px(h.first.top)-scrollY_,px(h.first.right),px(h.first.bottom)-scrollY_},bold_,p.text);
        for(const auto& child:children_)if(isEditField(child.id)){
            RECT frame{px(child.x-6),px(child.y-5)-scrollY_,px(child.x+child.w+6),px(child.y+child.h+3)-scrollY_};
            rounded(dc,frame,px(8),p.chip,GetFocus()==child.hwnd?p.accent:p.border);
        }
        if(dc!=output)BitBlt(output,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
    }
    std::wstring hotkeyText(int binding)const{
        if(!binding)return tr(L"None",L"Nenhum");
        const int flags=(binding>>8)&255,key=binding&255;std::wstring name;
        if(flags&HOTKEYF_CONTROL)name+=nightlight::translate(language_(),L"Ctrl + ");
        if(flags&HOTKEYF_ALT)name+=nightlight::translate(language_(),L"Alt + ");
        if(flags&HOTKEYF_SHIFT)name+=nightlight::translate(language_(),L"Shift + ");
        if(key==VK_ADD)name+=tr(L"Num +",L"Num +");
        else if(key==VK_SUBTRACT)name+=tr(L"Num −",L"Num −");
        else if(key==VK_LEFT)name+=tr(L"Left",L"Esquerda");
        else if(key==VK_RIGHT)name+=tr(L"Right",L"Direita");
        else if(key==VK_UP)name+=tr(L"Up",L"Cima");
        else if(key==VK_DOWN)name+=tr(L"Down",L"Baixo");
        else if(key>=VK_F1 && key<=VK_F24)name+=L"F"+std::to_wstring(key-VK_F1+1);
        else if((key>='A' && key<='Z') || (key>='0' && key<='9'))name+=static_cast<wchar_t>(key);
        else {wchar_t keyName[80]{};UINT scan=MapVirtualKeyW(key,MAPVK_VK_TO_VSC);
            const LONG param=static_cast<LONG>((scan<<16)|((flags&HOTKEYF_EXT)?(1<<24):0));
            if(GetKeyNameTextW(param,keyName,80)>0)name+=keyName;else name+=std::to_wstring(key);}
        return name;
    }
    COLORREF temperatureTint(int kelvin)const{
        const COLORREF stops[]={RGB(237,103,76),RGB(248,167,99),RGB(255,224,161),RGB(220,235,245),RGB(239,243,246)};
        const double t=std::clamp((kelvin-1000)/5500.0,0.0,1.0)*4;
        const int index=std::min(3,static_cast<int>(t));return mix(stops[index],stops[index+1],t-index);
    }
    void paintSlider(HDC dc,RECT r,const Slider& v,const Palette& p,bool focus,bool engaged){
        const int left=px(12),right=std::max(left+1,static_cast<int>(r.right)-px(74)),cy=(r.top+r.bottom)/2;
        const int center=left+MulDiv(v.value-v.low,right-left,v.high-v.low);
        const int half=std::max(1,px(2));
        if(v.kind==1 && !hc_()){
            { Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
            Gdiplus::Color gradient[]={drawingColor(temperatureTint(1000)),drawingColor(temperatureTint(2375)),drawingColor(temperatureTint(3750)),drawingColor(temperatureTint(5125)),drawingColor(temperatureTint(6500))};
            const float pos[]={0,.25f,.5f,.75f,1};
            Gdiplus::LinearGradientBrush brush(Gdiplus::PointF(static_cast<float>(left),static_cast<float>(cy)),Gdiplus::PointF(static_cast<float>(right),static_cast<float>(cy)),gradient[0],gradient[4]);
            brush.SetInterpolationColors(gradient,pos,5);
            g.FillRectangle(&brush,left,cy-half,right-left,half*2); }
            smoothCircle(dc,static_cast<float>(left),static_cast<float>(cy),static_cast<float>(half),temperatureTint(1000));
            smoothCircle(dc,static_cast<float>(right),static_cast<float>(cy),static_cast<float>(half),temperatureTint(6500));
        }else{
            rounded(dc,RECT{left,cy-half,right,cy+half},half*2,p.border,p.border);
            if(center>left+1)rounded(dc,RECT{left,cy-half,center,cy+half},half*2,p.accent,p.accent);
        }
        const float radius=static_cast<float>(px(engaged?9:8));
        smoothCircle(dc,static_cast<float>(center),static_cast<float>(cy),radius+1,p.border);
        smoothCircle(dc,static_cast<float>(center),static_cast<float>(cy),radius,p.card);
        smoothCircle(dc,static_cast<float>(center),static_cast<float>(cy),std::max(2.0f,radius-3),v.kind==1 && !hc_()?temperatureTint(v.value):p.accent);
        const std::wstring value=v.kind==0?timeText(v.value):std::to_wstring(v.value)+(v.kind==1?L" K":nightlight::translate(language_(),L" min"));
        text(dc,value,RECT{right+px(12),r.top,r.right-px(4),r.bottom},font_,p.text,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        if(focus){smoothCircle(dc,static_cast<float>(center),static_cast<float>(cy),radius+2,p.accent);
            smoothCircle(dc,static_cast<float>(center),static_cast<float>(cy),radius,p.card);
            smoothCircle(dc,static_cast<float>(center),static_cast<float>(cy),std::max(2.0f,radius-3),v.kind==1 && !hc_()?temperatureTint(v.value):p.accent);}
    }
    void drawButton(const DRAWITEMSTRUCT& item){
        RECT r=item.rcItem;HDC dc=nullptr;
        auto bp=BeginBufferedPaint(item.hDC,&r,BPBF_TOPDOWNDIB,nullptr,&dc);if(!bp)dc=item.hDC;
        const auto p=palette_();const int id=static_cast<int>(item.CtlID);
        const bool tab=id>=GeneralTab && id<=AboutTab,selected=tab && id-GeneralTab==tab_;
        fill(dc,r,tab || id==CloseButton || id==CheckUpdates || id==CaptureLogs?p.page:p.card);
        const bool hover=hovered_.count(item.hwndItem)!=0,pressed=(item.itemState&ODS_SELECTED)!=0;
        const bool focus=(item.itemState&ODS_FOCUS)!=0;
        const bool recording=id==CaptureLogs && general_(27,-1);
        COLORREF bg=(selected || recording)?mix(p.chip,p.accent,0.12):p.chip;
        if(hover || pressed)bg=mix(bg,p.text,pressed?0.08:0.035);
        const auto fg=(item.itemState&ODS_DISABLED)?p.muted:p.text;
        wchar_t labelText[512]{};GetWindowTextW(item.hwndItem,labelText,512);
        if(tab){
            RECT tabRect=r;tabRect.bottom+=px(8);
            const auto background=selected?p.card:bg;
            rounded(dc,tabRect,px(10),background,p.border);
            if(selected){fill(dc,RECT{r.left+1,r.bottom-px(5),r.right-1,r.bottom},p.card);
                rounded(dc,RECT{r.left+px(10),r.top+px(2),r.right-px(10),r.top+px(4)},px(2),p.accent,p.accent);}
            else fill(dc,RECT{r.left,r.bottom-1,r.right,r.bottom},p.border);
            text(dc,labelText,RECT{r.left,r.top+px(4),r.right,r.bottom-px(2)},selected?bold_:font_,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }else if(id==AboutRepository || id==AboutLicense){
            rounded(dc,r,px(8),bg,focus?p.accent:p.border);
            text(dc,labelText,RECT{r.left+px(8),r.top,r.right-px(8),r.bottom},font_,hc_()?p.text:p.accent);
        }else if(id==DisplayRefresh){
            if(!refreshGlyph_)refreshGlyph_=makeCenteredIconPath(L'\uE777',19);
            paintRefreshGlyph(dc,r,refreshGlyph_.get(),dpi_,displayRefreshTurn_.value(uiTime()),p.accent);
        }else if(id>=DisplayFirst && static_cast<size_t>(id-DisplayFirst)<displayKeys_.size()){
            const auto& key=displayKeys_[id-DisplayFirst];const auto& ms=monitors_();
            const auto mon=std::find_if(ms.begin(),ms.end(),[&](const Monitor& m){return nightlight::monitorProfileKey(m)==key;});
            const bool enabled=mon!=ms.end() && mon->enabled;
            const bool active=selectedDisplay_()==key;
            const auto chip=active?(hc_()?p.selected:mix(p.selected,RGB(0,0,0),.07)):bg;
            rounded(dc,r,px(8),chip,item.hwndItem==dropChip_?p.accent:p.border);
            const int cy=(r.top+r.bottom)/2;
            {Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen pen(drawingColor(p.subtle),static_cast<float>(std::max(1,px(1))));
                g.DrawRectangle(&pen,px(10),cy-px(7),px(14),px(10));
                g.DrawLine(&pen,px(17),cy+px(3),px(17),cy+px(7));
                g.DrawLine(&pen,px(13),cy+px(7),px(21),cy+px(7));}
            text(dc,mon!=ms.end()?mon->displayName():std::wstring(labelText),RECT{px(31),r.top,r.right-px(48),r.bottom},bold_,active?p.selectedText:fg);
            RECT track{r.right-px(44),cy-px(9),r.right-px(12),cy+px(9)};
            const auto trackColor=enabled?p.accent:p.card;
            rounded(dc,track,px(18),trackColor,enabled?p.accent:p.muted);
            smoothCircle(dc,static_cast<float>(track.left+px(enabled?23:9)),static_cast<float>(cy),static_cast<float>(px(6)),enabled?RGB(255,255,255):p.subtle);
        }else if(auto it=checks_.find(id);it!=checks_.end()){
            const int size=px(14),top=(r.bottom-size)/2;
            RECT box{px(1),top,px(1)+size,top+size};
            const auto checkedColor=hc_()?p.selected:p.accent;
            rounded(dc,box,px(5),it->second?checkedColor:bg,focus?p.text:(it->second?checkedColor:p.border));
            if(it->second){Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                Gdiplus::Pen pen(drawingColor(hc_()?p.selectedText:RGB(255,255,255)),static_cast<float>(std::max(1,px(2))));
                pen.SetStartCap(Gdiplus::LineCapRound);pen.SetEndCap(Gdiplus::LineCapRound);
                const Gdiplus::Point points[]={{static_cast<int>(box.left)+px(3),static_cast<int>(box.top)+px(7)},{static_cast<int>(box.left)+px(6),static_cast<int>(box.top)+px(10)},{static_cast<int>(box.right)-px(3),static_cast<int>(box.top)+px(4)}};
                g.DrawLines(&pen,points,3);}
            text(dc,labelText,RECT{px(22),r.top,r.right,r.bottom},font_,fg);
        }else if(auto it=sliders_.find(id);it!=sliders_.end()){
            paintSlider(dc,r,it->second,p,focus,activeSlider_==id || hover);
        }else if(choices_.count(id) || (id>=HotkeyFirst && id<HotkeyFirst+4)){
            rounded(dc,r,px(8),bg,focus?p.accent:p.border);
            const bool dropdown=choices_.count(id)!=0;
            const auto caption=dropdown?std::wstring(labelText):hotkeyText(hotkeyValues_[id-HotkeyFirst]);
            text(dc,caption,RECT{px(9),r.top,r.right-px(dropdown?26:8),r.bottom},font_,fg);
            if(dropdown){Gdiplus::Graphics g(dc);g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const float x=static_cast<float>(r.right-px(13)),y=static_cast<float>((r.top+r.bottom)/2);
                Gdiplus::Pen pen(drawingColor(p.muted),1.5f);
                const Gdiplus::PointF points[]={{x-px(4),y-px(2)},{x,y+px(2)},{x+px(4),y-px(2)}};g.DrawLines(&pen,points,3);}
        }else{
            rounded(dc,r,px(8),bg,selected || focus || recording?p.accent:p.border);
            text(dc,labelText,r,selected || recording?bold_:font_,fg,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        if(bp)EndBufferedPaint(bp,TRUE);
    }
    void showError(const wchar_t* en,const wchar_t* pt){
        MessageBoxW(window_,tr(en,pt),L"NightLight",MB_OK|MB_ICONWARNING);
    }
    void command(int id,int code){
        if(building_ || closing_)return;
        if(id>=GeneralTab && id<=AboutTab && code==BN_CLICKED){
            if(tab_==id-GeneralTab)return;if(tab_==2&&!collectTimes())return;tab_=id-GeneralTab;scrollY_=0;rebuild();return;
        }
        if(id==CloseButton && code==BN_CLICKED){close();return;}
        if(id==CheckUpdates && code==BN_CLICKED){general_(26,0);return;}
        if(id==CaptureLogs && code==BN_CLICKED){general_(27,0);SetWindowTextW(ctrl(CaptureLogs),general_(27,-1)?tr(L"Stop capture",L"Parar captura"):tr(L"Capture logs",L"Capturar registos"));InvalidateRect(ctrl(CaptureLogs),nullptr,FALSE);return;}
        if(id==AboutLicense && code==BN_CLICKED){
            if(reinterpret_cast<INT_PTR>(ShellExecuteW(window_,L"open",L"https://github.com/iamfromreallife/NightLightFlor/blob/main/LICENSE",nullptr,nullptr,SW_SHOWNORMAL))<=32)
                showError(L"Could not open the MIT license.",L"Não foi possível abrir a licença MIT.");
            return;
        }
        if(id==AboutRepository && code==BN_CLICKED){
            if(reinterpret_cast<INT_PTR>(ShellExecuteW(window_,L"open",L"https://github.com/iamfromreallife/NightLightFlor",nullptr,nullptr,SW_SHOWNORMAL))<=32)
                showError(L"Could not open the GitHub repository.",L"Não foi possível abrir o repositório GitHub.");
            return;
        }
        if(id==DisplayRefresh && code==BN_CLICKED){
            nightlight::startRefreshSpin(displayRefreshTurn_,uiTime(),animations_());
            if(displayRefreshTurn_.active(uiTime()))SetTimer(window_,DisplaySpinTimer,10,nullptr);
            if(HWND h=ctrl(DisplayRefresh)){InvalidateRect(h,nullptr,FALSE);::UpdateWindow(h);}
            if(!manualRefreshQueued_){manualRefreshQueued_=true;if(!PostMessageW(window_,WM_APP+4,0,0)){manualRefreshQueued_=false;displayAction_(3,L"",L"");}}
            return;
        }
        if(id>=DisplayFirst && static_cast<size_t>(id-DisplayFirst)<displayKeys_.size() && code==BN_CLICKED){displayAction_(0,displayKeys_[id-DisplayFirst],L"");repaintDisplays();return;}
        if(code==BN_CLICKED && choices_.count(id)){choose(id);return;}
        if(code==BN_CLICKED && checks_.count(id)){checks_[id]=!checks_[id];InvalidateRect(ctrl(id),nullptr,FALSE);}
        if(id==ScheduleEnabled && code==BN_CLICKED){draft_.enabled=checked(id);return;}
        if(id==Transition && code==CBN_SELCHANGE){if(!collectTimes()){choices_[id].selected=static_cast<int>(draft_.mode);SetWindowTextW(ctrl(id),choices_[id].values[choices_[id].selected].c_str());return;}draft_.mode=static_cast<nightlight::ScheduleMode>(selection(id));PostMessageW(window_,WM_APP+1,0,0);return;}
        if(id==PointAdd && code==BN_CLICKED){addSchedulePoint();return;}
        if(id==SetLocation && code==BN_CLICKED){openLocation();return;}
        if(id>=PointRemove && id<PointRemove+static_cast<int>(nightlight::kMaxSchedulePoints) && code==BN_CLICKED){
            const size_t index=static_cast<size_t>(id-PointRemove);
            if(draft_.points.size()>2 && index<draft_.points.size() && collectTimes()){draft_.points.erase(draft_.points.begin()+index);rebuild();}return;
        }
        if(id==TrayWheelMode && code==CBN_SELCHANGE){general_(id,selection(id));return;}
        if(id==TrayWheel && code==BN_CLICKED){if(!general_(id,checked(id)?1:0)){checks_[id]=false;InvalidateRect(ctrl(id),nullptr,FALSE);}EnableWindow(ctrl(TrayWheelMode),checked(id));return;}
        if((id==Language || id==Theme) && code==CBN_SELCHANGE){
            const int index=selection(id);
            if(index>=0)general_(id,index);
            if(id==Language)PostMessageW(window_,WM_APP+1,0,0);else refreshTheme();
            return;
        }
        if((id==CloseOutside || id==Startup || id==Ddc) && code==BN_CLICKED){
            if(!general_(id,checked(id)?1:0))PostMessageW(window_,WM_APP+1,0,0);
            return;
        }
        if(id==Scope && code==CBN_SELCHANGE){
            scope_=selection(id);refreshHotkeyFields();return;
        }
        if(id==ClearHotkeys && code==BN_CLICKED){
            hotkeyValues_={};for(int i=0;i<4;++i)InvalidateRect(ctrl(HotkeyFirst+i),nullptr,FALSE);
            return;
        }
        if(id==SaveHotkeys && code==BN_CLICKED && scope_>=0 && static_cast<size_t>(scope_)<scopeKeys_.size()){
            const auto keys=hotkeyValues_;auto steps=nightlight::kDefaultHotkeySteps;
            for(int i=0;i<4;++i){const auto v=fieldValue(HotkeyStep+i);int parsed=0;bool valid=!v.empty()&&v.size()<=4;
                for(wchar_t c:v){if(c<L'0'||c>L'9'){valid=false;break;}parsed=parsed*10+c-L'0';}
                if(!valid||!nightlight::validHotkeyStep(i,parsed)){showError(L"Enter 1–100 for brightness or 1–5500 for temperature.",L"Introduza 1–100 para brilho ou 1–5500 para temperatura.");SetFocus(ctrl(HotkeyStep+i));return;}steps[i]=parsed;}
            if(hotkeys_(scopeKeys_[scope_],keys,steps))
                MessageBoxW(window_,tr(L"Hotkeys saved.",L"Atalhos guardados."),L"NightLight",MB_OK|MB_ICONINFORMATION);
            else showError(L"A shortcut is invalid, duplicated or already registered by Windows or another app. Previous bindings were retained.",
                L"Um atalho é inválido, duplicado ou já está registado pelo Windows ou por outra aplicação. Os atalhos anteriores foram mantidos.");
            return;
        }
        if(id>=EngineFirst && id<EngineFirst+6 && code==CBN_SELCHANGE){
            const size_t index=static_cast<size_t>(id-EngineFirst);
            const int selectedEngine=selection(id);
            if(index<backendKeys_.size() && selectedEngine>=0 && !backend_(backendKeys_[index],selectedEngine))
                PostMessageW(window_,WM_APP+1,0,0);
            return;
        }
        if((id==Previous || id==Next) && code==BN_CLICKED){
            behaviorPage_+=id==Next?1:-1;rebuild();return;
        }
        if(id==ApplySchedule && code==BN_CLICKED){if(!collectTimes())return;draft_=nightlight::normalizedSchedule(draft_);schedule_(draft_);rebuild();return;}
        if(id==DimSettings && code==BN_CLICKED){dim_();return;}
    }
    LRESULT dispatch(UINT msg,WPARAM wp,LPARAM lp){
        switch(msg){
        case WM_CREATE:
            fitToMonitor(MonitorFromWindow(window_,MONITOR_DEFAULTTONEAREST),GetDpiForWindow(window_));fonts();
            button(GeneralTab,tr(L"General",L"Geral"),14,12,127,31,false);
            button(BehaviorTab,tr(L"Displays",L"Ecrãs"),149,12,127,31,false);
            button(AutomationTab,tr(L"Automation",L"Automatização"),284,12,127,31,false);
            button(AboutTab,tr(L"About",L"Acerca"),419,12,127,31,false);
            button(CaptureLogs,tr(L"Capture logs",L"Capturar registos"),54,desiredClientHeight_-40,174,28,false);
            button(CheckUpdates,tr(L"Check for updates...",L"Procurar atualizações..."),236,desiredClientHeight_-40,182,28,false);
            button(CloseButton,tr(L"Close",L"Fechar"),426,desiredClientHeight_-40,120,28,false);
            rebuild();
            for(const auto& item:children_)if(!item.page)ShowWindow(item.hwnd,SW_SHOWNA);
            return 0;
        case WM_APP+1:rebuild();return 0;
        case WM_APP+2:
            displayRefreshPending_=false;
            if(tab_==1){
                if(!renameField_ && !dragChip_ && !dragSettling_ && menuChoice_<0){displayRefreshDeferred_=false;rebuild();}
                else displayRefreshDeferred_=true;
            }
            return 0;
        case WM_APP+3:if(renameField_ && renameField_==reinterpret_cast<HWND>(lp)){finishRename(!renameCancel_);displaysChanged();}return 0;
        case WM_APP+4:manualRefreshQueued_=false;if(!closing_)displayAction_(3,L"",L"");return 0;
        case WM_TIMER:
            if(wp==DisplayDragTimer){
                if(!animations_()||tab_!=1||!IsWindowVisible(window_)||IsIconic(window_))finishChipMotion();
                layout();
                if(!chipMotionActive(uiTime())){finishChipMotion();if(!dragChip_&&displayRefreshDeferred_)displaysChanged();}
                return 0;
            }
            if(wp==DisplaySpinTimer){
                if(!animations_()||tab_!=1||!IsWindowVisible(window_)||IsIconic(window_))displayRefreshTurn_.finish();
                if(!displayRefreshTurn_.active(uiTime()))KillTimer(window_,DisplaySpinTimer);
                if(HWND h=ctrl(DisplayRefresh))InvalidateRect(h,nullptr,FALSE);
                return 0;
            }
            if(wp==SolarUiTimer){if(tab_==2&&!building_&&!locationOpen_&&IsWindowVisible(window_))refreshEditorSolarTimes();return 0;}
            if(wp==PreviewTimer){
                const auto it=sliders_.find(activeSlider_);
                if(it!=sliders_.end() && it->second.kind==1 && GetCapture()==ctrl(activeSlider_))preview_(it->second.value);
                else {KillTimer(window_,PreviewTimer);preview_(std::nullopt);}
                return 0;
            }break;
        case WM_VSCROLL:{SCROLLINFO si{sizeof(si),SIF_ALL};GetScrollInfo(window_,SB_VERT,&si);int next=scrollY_;
            switch(LOWORD(wp)){case SB_LINEUP:next-=px(32);break;case SB_LINEDOWN:next+=px(32);break;case SB_PAGEUP:next-=static_cast<int>(si.nPage);break;case SB_PAGEDOWN:next+=static_cast<int>(si.nPage);break;case SB_THUMBTRACK:case SB_THUMBPOSITION:next=si.nTrackPos;break;case SB_TOP:next=0;break;case SB_BOTTOM:next=px(desiredClientHeight_);break;default:return 0;}scrollTo(next);return 0;}
        case WM_MOUSEWHEEL:if(GetWindowLongPtrW(window_,GWL_STYLE)&WS_VSCROLL){scrollTo(scrollY_-MulDiv(GET_WHEEL_DELTA_WPARAM(wp),px(96),WHEEL_DELTA));return 0;}break;
        case WM_SIZE:if(wp!=SIZE_MINIMIZED)layout();else {closeChoice();surface_.reset();}return 0;
        case WM_DPICHANGED:{
            if(dragChip_)endDisplayDrag(false);
            finishChipMotion();
            closeChoice();
            const auto* r=reinterpret_cast<const RECT*>(lp);
            const HMONITOR monitor=MonitorFromRect(r,MONITOR_DEFAULTTONEAREST);
            fitToMonitor(monitor,HIWORD(wp));fonts();surface_.reset();configureScroll(monitor);
            const auto size=frameSize(monitor);
            MONITORINFO mi{sizeof(mi)};int x=r->left,y=r->top;
            if(GetMonitorInfoW(monitor,&mi)){
                x=std::clamp(x,static_cast<int>(mi.rcWork.left),static_cast<int>(std::max(mi.rcWork.left,mi.rcWork.right-size.cx)));
                y=std::clamp(y,static_cast<int>(mi.rcWork.top),static_cast<int>(std::max(mi.rcWork.top,mi.rcWork.bottom-size.cy)));
            }
            SetWindowPos(window_,nullptr,x,y,size.cx,size.cy,SWP_NOZORDER|SWP_NOACTIVATE);
            layout();return 0;
        }
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
        case WM_DRAWITEM:if(lp){const auto& item=*reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
            if(item.CtlType==ODT_BUTTON){drawButton(item);return TRUE;}}break;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{
            const auto p=palette_();const auto dc=reinterpret_cast<HDC>(wp);
            const int id=GetDlgCtrlID(reinterpret_cast<HWND>(lp));
            SetTextColor(dc,(id==LocalTimeNote || id==LocationHint || id==DisplayHint1 || id==DisplayHint2 || id==StartupHint)?p.muted:p.text);const auto bg=isEditField(id)?p.chip:p.card;SetBkColor(dc,bg);SetDCBrushColor(dc,bg);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_COMMAND:command(LOWORD(wp),HIWORD(wp));return 0;
        case WM_TIMECHANGE:if(tab_==2)refreshEditorSolarTimes(true);break;
        case WM_THEMECHANGED:case WM_SETTINGCHANGE:refreshTheme();break;
        case WM_CANCELMODE:if(choicePopup_)closeChoice();break;
        case WM_ACTIVATE:
            if(LOWORD(wp)==WA_INACTIVE && dragChip_)endDisplayDrag(false);
            if(LOWORD(wp)==WA_INACTIVE && choicePopup_)closeChoice();
            if(LOWORD(wp)==WA_INACTIVE && activeSlider_>=0){
                if(GetCapture()==ctrl(activeSlider_))ReleaseCapture();
                activeSlider_=-1;KillTimer(window_,PreviewTimer);preview_(std::nullopt);
            }
            if(LOWORD(wp)!=WA_INACTIVE && tab_==2 && !building_ && !closing_){
                // Only update the dim status; never discard an unsaved schedule.
                refreshEditorSolarTimes();
                const auto p=read_();
                SetWindowTextW(ctrl(DimStatus),p.dimInactive?tr(L"Enabled",L"Ativo"):tr(L"Disabled",L"Inativo"));
            }
            break;
        case WM_EXITSIZEMOVE:savePlacement();return 0;
        case WM_CLOSE:close();return 0;
        case WM_NCDESTROY:{
            closing_=true;closeChoice();destroyRefreshTooltip();
            HWND old=window_;SetWindowLongPtrW(old,GWLP_USERDATA,0);window_=nullptr;
            children_.clear();hovered_.clear();surface_.reset();
            return DefWindowProcW(old,msg,wp,lp);
        }}
        return DefWindowProcW(window_,msg,wp,lp);
    }
    static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l){
        auto* self=reinterpret_cast<NightLightSettingsDialog*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(m==WM_NCCREATE){self=static_cast<NightLightSettingsDialog*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->window_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        return self?self->dispatch(m,w,l):DefWindowProcW(h,m,w,l);
    }
public:
    NightLightSettingsDialog(HINSTANCE instance,HWND owner,Read read,Monitors monitors,General general,Backend backend,
        Hotkeys hotkeys,Schedule schedule,std::function<void(std::optional<int>)> preview,std::function<void()> dim,std::function<bool()> startup,
        std::function<int()> language,std::function<Palette()> palette,std::function<bool()> dark,std::function<bool()> hc,
        DisplayAction displayAction,std::function<std::wstring()> selectedDisplay,std::function<bool()> animations,
        std::optional<POINT> savedPosition,std::function<void(POINT)> rememberPosition)
        :instance_(instance),owner_(owner),savedPosition_(savedPosition),rememberPosition_(std::move(rememberPosition)),animations_(std::move(animations)),displayAction_(std::move(displayAction)),selectedDisplay_(std::move(selectedDisplay)),
        read_(std::move(read)),monitors_(std::move(monitors)),general_(std::move(general)),
        backend_(std::move(backend)),hotkeys_(std::move(hotkeys)),schedule_(std::move(schedule)),preview_(std::move(preview)),dim_(std::move(dim)),
        startup_(std::move(startup)),language_(std::move(language)),dark_(std::move(dark)),hc_(std::move(hc)),palette_(std::move(palette)){}
    ~NightLightSettingsDialog(){close();if(font_)DeleteObject(font_);if(bold_)DeleteObject(bold_);}
    HWND hwnd()const{return window_;}
    void savePlacement(){
        if(!placementReady_ || !window_ || !IsWindowVisible(window_) || IsIconic(window_) || IsZoomed(window_))return;
        RECT r{};if(!GetWindowRect(window_,&r))return;
        POINT p{r.left,r.top};
        if(savedPosition_ && savedPosition_->x==p.x && savedPosition_->y==p.y)return;
        savedPosition_=p;if(rememberPosition_)rememberPosition_(p);
    }
    void repaintDisplays(){if(window_ && tab_==1)for(size_t i=0;i<displayKeys_.size();++i)if(HWND h=ctrl(DisplayFirst+static_cast<int>(i)))InvalidateRect(h,nullptr,FALSE);}
    void displaysChanged(){if(window_ && tab_==1 && !closing_ && !building_ && !displayRefreshPending_){displayRefreshPending_=true;PostMessageW(window_,WM_APP+2,0,0);}}
    void hideToTray(){if(window_ && IsWindowVisible(window_)){savePlacement();if(refreshTooltip_)SendMessageW(refreshTooltip_,TTM_POP,0,0);closeChoice();KillTimer(window_,DisplaySpinTimer);displayRefreshTurn_.finish();finishRename(true);if(dragChip_ && GetCapture()==dragChip_)ReleaseCapture();if(activeSlider_>=0 && GetCapture()==ctrl(activeSlider_))ReleaseCapture();KillTimer(window_,PreviewTimer);preview_(std::nullopt);ShowWindow(window_,SW_HIDE);}}
    void close(){closing_=true;if(window_){savePlacement();destroyRefreshTooltip();closeChoice();KillTimer(window_,DisplaySpinTimer);finishRename(true);KillTimer(window_,PreviewTimer);preview_(std::nullopt);DestroyWindow(window_);}}
    void activate(){if(window_){if(IsIconic(window_))ShowWindow(window_,SW_RESTORE);else if(!IsWindowVisible(window_))ShowWindow(window_,SW_SHOW);raiseNormalAppWindow(window_);}}
    void refreshTheme(){
        if(!window_)return;
        applyNightLightFrame(window_,palette_(),dark_(),hc_());
        if(choicePopup_)InvalidateRect(choicePopup_,nullptr,FALSE);
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN|RDW_NOERASE);
    }
    bool pretranslate(MSG& m){
        if(!window_ || locationOpen_)return false;
        if(choicePopup_ && (m.hwnd==choicePopup_||m.hwnd==window_||IsChild(window_,m.hwnd))){
            if(m.message==WM_MOUSEWHEEL){choiceWheel(m.wParam);return true;}
            if(m.message==WM_KEYDOWN||m.message==WM_SYSKEYDOWN){
                if(m.wParam==VK_TAB)closeChoice();
                else if(choiceKey(m.wParam))return true;
                else {TranslateMessage(&m);return true;}
            }else if(m.message==WM_CHAR||m.message==WM_SYSCHAR){choiceChar(static_cast<wchar_t>(m.wParam));return true;}
        }
        if(m.hwnd!=window_ && !IsChild(window_,m.hwnd))return false;
        if(m.message==WM_MOUSEWHEEL && (GetWindowLongPtrW(window_,GWL_STYLE)&WS_VSCROLL)){SendMessageW(window_,WM_MOUSEWHEEL,m.wParam,m.lParam);return true;}
        if((m.message==WM_KEYDOWN||m.message==WM_SYSKEYDOWN)&&choices_.count(GetDlgCtrlID(m.hwnd))&&
            (m.wParam==VK_F4||m.wParam==VK_DOWN||m.wParam==VK_UP||m.wParam==VK_RETURN)){choose(GetDlgCtrlID(m.hwnd));return true;}
        // The custom hotkey recorder must receive navigation keys itself.
        for(int i=0;i<4;++i)if(m.hwnd==ctrl(HotkeyFirst+i) && (m.message==WM_KEYDOWN || m.message==WM_SYSKEYDOWN) && m.wParam!=VK_TAB){
            TranslateMessage(&m);DispatchMessageW(&m);return true;
        }
        if(timeFields_.count(GetDlgCtrlID(m.hwnd)) && m.message==WM_KEYDOWN && m.wParam==VK_RETURN){
            if(collectTimes()){const int minute=draft_.points[timeFields_.at(GetDlgCtrlID(m.hwnd))].minute;draft_=nightlight::normalizedSchedule(std::move(draft_));rebuild();
                const auto found=std::find_if(draft_.points.begin(),draft_.points.end(),[&](const auto& p){return p.minute==minute;});
                if(found!=draft_.points.end())if(HWND field=ctrl(PointTimeField+static_cast<int>(found-draft_.points.begin()))){SetFocus(field);ensureVisible(field);SendMessageW(field,EM_SETSEL,0,-1);}}
            return true;
        }
        if(m.hwnd==renameField_ && m.message==WM_KEYDOWN && (m.wParam==VK_RETURN || m.wParam==VK_ESCAPE)){DispatchMessageW(&m);return true;}
        if(m.message==WM_KEYDOWN && m.wParam==VK_ESCAPE){if(dragChip_)endDisplayDrag(false);else close();return true;}
        const bool handled=IsDialogMessageW(window_,&m)!=FALSE;if(handled&&m.message==WM_KEYDOWN&&m.wParam==VK_TAB)ensureVisible(GetFocus());return handled;
    }
    bool open(){
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance_;wc.lpfnWndProc=proc;wc.lpszClassName=cls_;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;
        if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        POINT cursor{};GetCursorPos(&cursor);
        const HMONITOR monitor=MonitorFromPoint(savedPosition_.value_or(cursor),MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(monitor,&mi))return false;
        fitToMonitor(monitor,GetDpiForSystem());
        auto position=[&](SIZE size){
            const POINT requested=savedPosition_.value_or(POINT{
                mi.rcWork.left+std::max(0L,(mi.rcWork.right-mi.rcWork.left-size.cx)/2),
                mi.rcWork.top+std::max(0L,(mi.rcWork.bottom-mi.rcWork.top-size.cy)/2)});
            return POINT{std::clamp(requested.x,mi.rcWork.left,std::max(mi.rcWork.left,mi.rcWork.right-size.cx)),
                         std::clamp(requested.y,mi.rcWork.top,std::max(mi.rcWork.top,mi.rcWork.bottom-size.cy))};
        };
        const auto initial=frameSize(monitor);const auto first=position(initial);
        HWND created=CreateWindowExW(0,cls_,tr(L"NightLight settings",L"Definições do NightLight"),frameStyle_,
            first.x,first.y,initial.cx,initial.cy,owner_,nullptr,instance_,this);
        if(!created)return false;
        const auto actual=frameSize(monitor);const auto restored=position(actual);
        SetWindowPos(created,nullptr,restored.x,restored.y,actual.cx,actual.cy,SWP_NOZORDER|SWP_NOACTIVATE);
        placementReady_=true;
        ShowWindow(created,SW_SHOW);raiseNormalAppWindow(created);nightlight::focusActiveFrameControl(created,ctrl(GeneralTab));return true;
    }
};
