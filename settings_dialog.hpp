#pragma once
// Native non-modal settings frame. The main window and settings use the same
// buffered painting, palette, antialiased rounded surfaces and DWM caption.
class NightLightSettingsDialog {
public:
    using Read=std::function<Preferences()>;
    using Monitors=std::function<std::vector<Monitor>()>;
    using General=std::function<bool(int,int)>;
    using Backend=std::function<bool(const std::wstring&,int)>;
    using Hotkeys=std::function<bool(const std::wstring&,const std::array<int,4>&)>;
    using Schedule=std::function<void(nightlight::ColorSchedule)>;
private:
    enum : int { GeneralTab=10,BehaviorTab=11,AutomationTab=12,
        Language=20,Theme=21,CloseOutside=22,Startup=23,Ddc=24,
        Scope=30,HotkeyFirst=31,SaveHotkeys=35,ClearHotkeys=36,
        EngineFirst=50,Previous=60,Next=61,
        ScheduleEnabled=70,DayTime=71,NightTime=72,DayKelvin=73,NightKelvin=74,
        Duration=75,ApplySchedule=76,DimSettings=77,CloseButton=90 };
    static constexpr wchar_t cls_[]=L"NightLight.Settings.Native";
    HINSTANCE instance_;
    HWND owner_,window_=nullptr;
    HFONT font_=nullptr,bold_=nullptr;
    UINT dpi_=96;
    int tab_=0,behaviorPage_=0,scope_=0;
    bool building_=false,closing_=false;
    std::vector<std::wstring> scopeKeys_,backendKeys_;
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
    std::function<void()> dim_;
    std::function<bool()> startup_,pt_,dark_,hc_;
    std::function<Palette()> palette_;
    nightlight::PaintSurface surface_;
    int px(int value)const{return MulDiv(value,static_cast<int>(dpi_),96);}
    const wchar_t* tr(const wchar_t* en,const wchar_t* pt)const{return pt_()?pt:en;}
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
        if(m==WM_MOUSEMOVE && self->hovered_.insert(h).second){
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,h,0};TrackMouseEvent(&track);InvalidateRect(h,nullptr,FALSE);
        }else if(m==WM_MOUSELEAVE || m==WM_CAPTURECHANGED){
            self->hovered_.erase(h);InvalidateRect(h,nullptr,FALSE);
        }else if(m==WM_NCDESTROY){
            self->hovered_.erase(h);RemoveWindowSubclass(h,hoverProc,id);
        }
        return DefSubclassProc(h,m,w,l);
    }
    static LRESULT CALLBACK hotkeyProc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data){
        auto* self=reinterpret_cast<NightLightSettingsDialog*>(data);
        if(m==WM_SETFOCUS)self->general_(25,1);
        else if(m==WM_KILLFOCUS || m==WM_NCDESTROY){
            self->general_(25,0);
            if(m==WM_NCDESTROY)RemoveWindowSubclass(h,hotkeyProc,id);
        }
        return DefSubclassProc(h,m,w,l);
    }
    void fonts(){
        HFONT f=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
        HFONT b=CreateFontW(-px(14),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,L"Segoe UI");
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
        add(-1,L"STATIC",value,SS_LEFT|SS_CENTERIMAGE,x,y,w,h);
    }
    void check(int id,const std::wstring& value,int x,int y,int w,bool checked){
        HWND h=add(id,L"BUTTON",value,WS_TABSTOP|BS_AUTOCHECKBOX,x,y,w,26);
        if(h)SendMessageW(h,BM_SETCHECK,checked?BST_CHECKED:BST_UNCHECKED,0);
    }
    bool checked(int id)const{return SendMessageW(ctrl(id),BM_GETCHECK,0,0)==BST_CHECKED;}
    void combo(int id,const std::vector<std::wstring>& values,int selected,int x,int y,int w){
        HWND h=add(id,L"COMBOBOX",L"",WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST|CBS_HASSTRINGS,
            x,y,w,220);
        if(!h)return;
        for(const auto& value:values)SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(value.c_str()));
        SendMessageW(h,CB_SETCURSEL,selected,0);
    }
    void edit(int id,const std::wstring& value,int x,int y,int w,bool numeric=false){
        HWND h=add(id,L"EDIT",value,WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL|(numeric?ES_NUMBER:0),x,y,w,26);
        if(h){SendMessageW(h,EM_SETLIMITTEXT,numeric?5:5,0);SendMessageW(h,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,MAKELPARAM(px(5),px(5)));}
    }
    void section(int top,int bottom,const std::wstring& heading){
        cards_.push_back({14,top,546,bottom});
        headings_.push_back({RECT{26,top+7,534,top+32},heading});
    }
    void clearPage(){
        for(auto it=children_.begin();it!=children_.end();){
            if(it->page){DestroyWindow(it->hwnd);it=children_.erase(it);}else ++it;
        }
        cards_.clear();headings_.clear();backendKeys_.clear();
    }
    void refreshHotkeyFields(){
        if(scope_<0 || static_cast<size_t>(scope_)>=scopeKeys_.size())return;
        const auto prefs=read_();
        std::array<int,4> values{};
        if(auto it=prefs.hotkeys.find(scopeKeys_[scope_]);it!=prefs.hotkeys.end())values=it->second;
        for(int i=0;i<4;++i)SendMessageW(ctrl(HotkeyFirst+i),HKM_SETHOTKEY,values[i],0);
    }
    static std::wstring timeText(int minute){
        wchar_t value[12]{};swprintf_s(value,L"%02d:%02d",minute/60,minute%60);return value;
    }
    std::wstring value(int id)const{
        wchar_t buffer[32]{};GetWindowTextW(ctrl(id),buffer,32);return buffer;
    }
    static int number(const std::wstring& text){
        if(text.empty() || text.size()>5)return -1;
        int n=0;for(wchar_t c:text){if(c<L'0' || c>L'9')return -1;n=n*10+(c-L'0');}return n;
    }
    static int timeValue(const std::wstring& text){
        const auto colon=text.find(L':');
        if(colon==std::wstring::npos || colon<1 || colon>2 || text.size()!=colon+3)return -1;
        const int h=number(text.substr(0,colon)),m=number(text.substr(colon+1));
        return h>=0 && h<24 && m>=0 && m<60?h*60+m:-1;
    }
    void buildGeneral(){
        const auto p=read_();
        section(52,192,tr(L"General",L"Geral"));
        label(tr(L"Language",L"Idioma"),26,86,220);
        combo(Language,{L"English",L"Português (Portugal)"},p.locale==L"pt-PT"?1:0,280,86,254);
        label(tr(L"Theme",L"Tema"),26,116,220);
        combo(Theme,{tr(L"Follow system",L"Seguir sistema"),tr(L"Light",L"Claro"),tr(L"Dark",L"Escuro")},
            p.theme==L"light"?1:p.theme==L"dark"?2:0,280,116,254);
        check(CloseOutside,tr(L"Close when losing focus",L"Fechar ao perder o foco"),26,148,250,p.closeByClickingOutside);
        check(Startup,tr(L"Start with Windows",L"Iniciar com o Windows"),280,148,254,startup_());
        section(202,432,tr(L"Global hotkeys",L"Atalhos de teclado globais"));
        label(tr(L"Apply shortcuts to",L"Aplicar atalhos a"),26,236,230);
        std::vector<std::wstring> names{tr(L"All monitors",L"Todos os ecrãs")};scopeKeys_={L""};
        for(const auto& m:monitors_()){
            names.push_back(m.displayName());scopeKeys_.push_back(nightlight::monitorProfileKey(m));
        }
        // Keep disconnected profiles editable so a conflicting binding can
        // always be removed without reconnecting the original monitor.
        for(const auto& entry:p.hotkeys)if(std::find(scopeKeys_.begin(),scopeKeys_.end(),entry.first)==scopeKeys_.end()){
            names.push_back(std::wstring(tr(L"Disconnected: ",L"Desligado: "))+entry.first);scopeKeys_.push_back(entry.first);
        }
        scope_=std::clamp(scope_,0,static_cast<int>(scopeKeys_.size())-1);
        combo(Scope,names,scope_,280,236,254);
        const wchar_t* labels[]={tr(L"Increase brightness (+5%)",L"Aumentar brilho (+5%)"),
            tr(L"Decrease brightness (-5%)",L"Reduzir brilho (-5%)"),
            tr(L"Increase temperature (+100 K)",L"Aumentar temperatura (+100 K)"),
            tr(L"Decrease temperature (-100 K)",L"Reduzir temperatura (-100 K)")};
        for(int i=0;i<4;++i){
            const int y=267+i*30;label(labels[i],26,y,250);
            if(HWND field=add(HotkeyFirst+i,HOTKEY_CLASSW,L"",WS_TABSTOP|WS_BORDER,280,y,254,26))
                SetWindowSubclass(field,hotkeyProc,1,reinterpret_cast<DWORD_PTR>(this));
        }
        refreshHotkeyFields();
        button(ClearHotkeys,tr(L"Clear",L"Limpar"),280,392,95);
        button(SaveHotkeys,tr(L"Save hotkeys",L"Guardar atalhos"),383,392,151);
    }
    void buildBehavior(){
        const auto p=read_();
        section(52,120,tr(L"Display controls",L"Controlos dos ecrãs"));
        check(Ddc,tr(L"DDC/CI Controls",L"Controlos DDC/CI"),26,86,508,p.ddcControls);
        section(130,432,tr(L"Colour correction engine",L"Motor de correção de cor"));
        const auto monitors=monitors_();
        const int pages=std::max(1,(static_cast<int>(monitors.size())+5)/6);
        behaviorPage_=std::clamp(behaviorPage_,0,pages-1);
        if(monitors.empty())label(tr(L"No connected displays",L"Nenhum ecrã ligado"),26,166,508);
        for(int i=0;i<6;++i){
            const size_t index=static_cast<size_t>(behaviorPage_*6+i);
            if(index>=monitors.size())break;
            const auto& m=monitors[index];backendKeys_.push_back(nightlight::monitorProfileKey(m));
            const int y=168+i*37;
            label(m.displayName(),26,y,245);
            combo(EngineFirst+i,{tr(L"GPU gamma",L"Gama da GPU"),
                tr(L"Monitor hardware (DDC/CI)",L"Hardware do ecrã (DDC/CI)"),
                tr(L"NVIDIA target gamma",L"Gama alvo NVIDIA")},
                m.nvidiaTargetColor?2:m.hardwareColor?1:0,280,y,254);
        }
        if(pages>1){
            label(std::to_wstring(behaviorPage_+1)+L" / "+std::to_wstring(pages),26,393,120);
            button(Previous,tr(L"Previous",L"Anterior"),280,392,123);
            button(Next,tr(L"Next",L"Seguinte"),411,392,123);
            EnableWindow(ctrl(Previous),behaviorPage_>0);
            EnableWindow(ctrl(Next),behaviorPage_+1<pages);
        }
    }
    void buildAutomation(){
        const auto p=read_();const auto s=p.colorSchedule;
        section(52,316,tr(L"Automatic colour temperature",L"Temperatura de cor automática"));
        check(ScheduleEnabled,tr(L"Enable time-based colour temperature",L"Ativar temperatura de cor por horário"),26,84,508,s.enabled);
        label(tr(L"Day starts (HH:MM)",L"Início do dia (HH:MM)"),26,118,246);
        edit(DayTime,timeText(s.dayStart),280,118,112);
        label(tr(L"Night starts (HH:MM)",L"Início da noite (HH:MM)"),26,150,246);
        edit(NightTime,timeText(s.nightStart),280,150,112);
        label(tr(L"Day temperature (K)",L"Temperatura diurna (K)"),26,182,246);
        edit(DayKelvin,std::to_wstring(s.dayKelvin),280,182,112,true);
        label(tr(L"Night temperature (K)",L"Temperatura noturna (K)"),26,214,246);
        edit(NightKelvin,std::to_wstring(s.nightKelvin),280,214,112,true);
        label(tr(L"Transition duration (minutes)",L"Duração da transição (minutos)"),26,246,250);
        edit(Duration,std::to_wstring(s.transitionMinutes),280,246,112,true);
        button(ApplySchedule,tr(L"Apply schedule",L"Aplicar horário"),383,278,151);
        section(326,432,tr(L"Dim when inactive",L"Reduzir brilho quando inativo"));
        label(p.dimInactive?tr(L"Enabled",L"Ativo"):tr(L"Disabled",L"Inativo"),26,364,255);
        button(DimSettings,tr(L"Settings...",L"Definições..."),383,360,151);
        label(tr(L"Uses Windows local time and time zone.",L"Utiliza a hora e o fuso horário do Windows."),26,396,508);
    }
    void layout(){
        if(!window_)return;
        for(const auto& item:children_)SetWindowPos(item.hwnd,nullptr,px(item.x),px(item.y),px(item.w),px(item.h),
            SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW);
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_NOERASE);
    }
    void rebuild(){
        if(!window_ || building_)return;
        building_=true;
        clearPage();
        SetWindowTextW(ctrl(GeneralTab),tr(L"General",L"Geral"));
        SetWindowTextW(ctrl(BehaviorTab),tr(L"Behavior",L"Comportamento"));
        SetWindowTextW(ctrl(AutomationTab),tr(L"Automation",L"Automatização"));
        SetWindowTextW(ctrl(CloseButton),tr(L"Close",L"Fechar"));
        SetWindowTextW(window_,tr(L"NightLight settings",L"Definições do NightLight"));
        if(tab_==0)buildGeneral();else if(tab_==1)buildBehavior();else buildAutomation();
        refreshTheme();
        for(const auto& child:children_)if(child.page)ShowWindow(child.hwnd,SW_SHOWNA);
        layout();building_=false;
    }
    void paint(HDC output){
        RECT r{};GetClientRect(window_,&r);if(!output || r.right<=0 || r.bottom<=0)return;
        HDC dc=surface_.ensure(output,r.right,r.bottom);if(!dc)dc=output;
        const auto p=palette_();fill(dc,r,p.page);
        for(auto card:cards_)rounded(dc,RECT{px(card.left),px(card.top),px(card.right),px(card.bottom)},px(12),p.card,p.border);
        for(const auto& h:headings_)text(dc,h.second,RECT{px(h.first.left),px(h.first.top),px(h.first.right),px(h.first.bottom)},bold_,p.text);
        if(dc!=output)BitBlt(output,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
    }
    void drawButton(const DRAWITEMSTRUCT& item){
        RECT r=item.rcItem;HDC dc=nullptr;
        auto bp=BeginBufferedPaint(item.hDC,&r,BPBF_TOPDOWNDIB,nullptr,&dc);if(!bp)dc=item.hDC;
        const auto p=palette_();const int id=static_cast<int>(item.CtlID);
        const bool tab=id>=GeneralTab && id<=AutomationTab,selected=tab && id-GeneralTab==tab_;
        fill(dc,r,tab || id==CloseButton?p.page:p.card);
        const bool hover=hovered_.count(item.hwndItem)!=0,pressed=(item.itemState&ODS_SELECTED)!=0;
        COLORREF bg=selected?mix(p.chip,p.accent,0.12):p.chip;
        if(hover || pressed)bg=mix(bg,p.text,pressed?0.08:0.035);
        rounded(dc,r,px(8),bg,selected || (item.itemState&ODS_FOCUS)?p.accent:p.border);
        wchar_t labelText[128]{};GetWindowTextW(item.hwndItem,labelText,128);
        text(dc,labelText,r,selected?bold_:font_,(item.itemState&ODS_DISABLED)?p.muted:p.text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(bp)EndBufferedPaint(bp,TRUE);
    }
    void showError(const wchar_t* en,const wchar_t* pt){
        MessageBoxW(window_,tr(en,pt),L"NightLight",MB_OK|MB_ICONWARNING);
    }
    void command(int id,int code){
        if(building_ || closing_)return;
        if(id>=GeneralTab && id<=AutomationTab && code==BN_CLICKED){
            tab_=id-GeneralTab;rebuild();return;
        }
        if(id==CloseButton && code==BN_CLICKED){close();return;}
        if((id==Language || id==Theme) && code==CBN_SELCHANGE){
            const int index=static_cast<int>(SendMessageW(ctrl(id),CB_GETCURSEL,0,0));
            if(index>=0)general_(id,index);
            if(id==Language)PostMessageW(window_,WM_APP+1,0,0);else refreshTheme();
            return;
        }
        if((id==CloseOutside || id==Startup || id==Ddc) && code==BN_CLICKED){
            if(!general_(id,checked(id)?1:0))PostMessageW(window_,WM_APP+1,0,0);
            return;
        }
        if(id==Scope && code==CBN_SELCHANGE){
            scope_=static_cast<int>(SendMessageW(ctrl(id),CB_GETCURSEL,0,0));refreshHotkeyFields();return;
        }
        if(id==ClearHotkeys && code==BN_CLICKED){
            for(int i=0;i<4;++i)SendMessageW(ctrl(HotkeyFirst+i),HKM_SETHOTKEY,0,0);
            return;
        }
        if(id==SaveHotkeys && code==BN_CLICKED && scope_>=0 && static_cast<size_t>(scope_)<scopeKeys_.size()){
            std::array<int,4> keys{};
            for(int i=0;i<4;++i)keys[i]=static_cast<int>(SendMessageW(ctrl(HotkeyFirst+i),HKM_GETHOTKEY,0,0))&65535;
            if(hotkeys_(scopeKeys_[scope_],keys))
                MessageBoxW(window_,tr(L"Hotkeys saved.",L"Atalhos guardados."),L"NightLight",MB_OK|MB_ICONINFORMATION);
            else showError(L"A shortcut is invalid, duplicated or already registered by Windows or another app. Previous bindings were retained.",
                L"Um atalho é inválido, duplicado ou já está registado pelo Windows ou por outra aplicação. Os atalhos anteriores foram mantidos.");
            return;
        }
        if(id>=EngineFirst && id<EngineFirst+6 && code==CBN_SELCHANGE){
            const size_t index=static_cast<size_t>(id-EngineFirst);
            const int selection=static_cast<int>(SendMessageW(ctrl(id),CB_GETCURSEL,0,0));
            if(index<backendKeys_.size() && selection>=0 && !backend_(backendKeys_[index],selection))
                PostMessageW(window_,WM_APP+1,0,0);
            return;
        }
        if((id==Previous || id==Next) && code==BN_CLICKED){
            behaviorPage_+=id==Next?1:-1;rebuild();return;
        }
        if(id==ApplySchedule && code==BN_CLICKED){
            const int day=timeValue(value(DayTime)),night=timeValue(value(NightTime));
            const int warm=number(value(DayKelvin)),cool=number(value(NightKelvin)),minutes=number(value(Duration));
            if(day<0 || night<0 || day==night || warm<1000 || warm>6500 || cool<1000 || cool>6500 || minutes<1 || minutes>120){
                showError(L"Enter different valid start times, temperatures from 1000 to 6500 K and a duration from 1 to 120 minutes.",
                    L"Introduza horas válidas diferentes, temperaturas de 1000 a 6500 K e uma duração de 1 a 120 minutos.");return;
            }
            nightlight::ColorSchedule s{checked(ScheduleEnabled),day,night,warm,cool,minutes};
            if(nightlight::normalizedSchedule(s).transitionMinutes!=minutes){
                showError(L"The transition must fit between both start times.",L"A transição tem de caber entre as duas horas de início.");return;
            }
            schedule_(s);return;
        }
        if(id==DimSettings && code==BN_CLICKED){dim_();return;}
    }
    LRESULT dispatch(UINT msg,WPARAM wp,LPARAM lp){
        switch(msg){
        case WM_CREATE:
            dpi_=GetDpiForWindow(window_);if(!dpi_)dpi_=96;fonts();
            button(GeneralTab,tr(L"General",L"Geral"),14,12,172,30,false);
            button(BehaviorTab,tr(L"Behavior",L"Comportamento"),194,12,172,30,false);
            button(AutomationTab,tr(L"Automation",L"Automatização"),374,12,172,30,false);
            button(CloseButton,tr(L"Close",L"Fechar"),426,443,120,28,false);
            rebuild();
            for(const auto& item:children_)if(!item.page)ShowWindow(item.hwnd,SW_SHOWNA);
            return 0;
        case WM_APP+1:rebuild();return 0;
        case WM_SIZE:if(wp!=SIZE_MINIMIZED)layout();else surface_.reset();return 0;
        case WM_DPICHANGED:{
            dpi_=HIWORD(wp);if(!dpi_)dpi_=96;fonts();surface_.reset();
            const auto* r=reinterpret_cast<const RECT*>(lp);
            SetWindowPos(window_,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);
            layout();return 0;
        }
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
        case WM_DRAWITEM:if(lp){drawButton(*reinterpret_cast<const DRAWITEMSTRUCT*>(lp));return TRUE;}break;
        case WM_CTLCOLORSTATIC:case WM_CTLCOLORBTN:case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:{
            const auto p=palette_();const auto dc=reinterpret_cast<HDC>(wp);
            SetTextColor(dc,p.text);SetBkColor(dc,p.card);SetDCBrushColor(dc,p.card);
            return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_COMMAND:command(LOWORD(wp),HIWORD(wp));return 0;
        case WM_THEMECHANGED:case WM_SETTINGCHANGE:refreshTheme();break;
        case WM_ACTIVATE:
            if(LOWORD(wp)!=WA_INACTIVE && tab_==2 && !building_ && !closing_){
                // Only update the dim status; never discard an unsaved schedule.
                const auto p=read_();
                for(const auto& child:children_)if(child.page && child.id==-1 && child.y==364)
                    SetWindowTextW(child.hwnd,p.dimInactive?tr(L"Enabled",L"Ativo"):tr(L"Disabled",L"Inativo"));
            }
            break;
        case WM_CLOSE:close();return 0;
        case WM_NCDESTROY:{
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
        Hotkeys hotkeys,Schedule schedule,std::function<void()> dim,std::function<bool()> startup,
        std::function<bool()> pt,std::function<Palette()> palette,std::function<bool()> dark,std::function<bool()> hc)
        :instance_(instance),owner_(owner),read_(std::move(read)),monitors_(std::move(monitors)),general_(std::move(general)),
        backend_(std::move(backend)),hotkeys_(std::move(hotkeys)),schedule_(std::move(schedule)),dim_(std::move(dim)),
        startup_(std::move(startup)),pt_(std::move(pt)),dark_(std::move(dark)),hc_(std::move(hc)),palette_(std::move(palette)){}
    ~NightLightSettingsDialog(){close();if(font_)DeleteObject(font_);if(bold_)DeleteObject(bold_);}
    HWND hwnd()const{return window_;}
    void close(){closing_=true;if(window_)DestroyWindow(window_);}
    void activate(){if(window_){if(IsIconic(window_))ShowWindow(window_,SW_RESTORE);SetForegroundWindow(window_);}}
    void refreshTheme(){
        if(!window_)return;
        applyNightLightFrame(window_,palette_(),dark_(),hc_());
        for(const auto& child:children_)SetWindowTheme(child.hwnd,dark_() && !hc_()?L"DarkMode_Explorer":L"Explorer",nullptr);
        RedrawWindow(window_,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME|RDW_ALLCHILDREN|RDW_NOERASE);
    }
    bool pretranslate(MSG& m){
        if(!window_ || (m.hwnd!=window_ && !IsChild(window_,m.hwnd)))return false;
        // The native hotkey control must receive navigation keys itself.
        for(int i=0;i<4;++i)if(m.hwnd==ctrl(HotkeyFirst+i) && m.message==WM_KEYDOWN && m.wParam!=VK_TAB)return false;
        if(m.message==WM_KEYDOWN && m.wParam==VK_ESCAPE){close();return true;}
        return IsDialogMessageW(window_,&m)!=FALSE;
    }
    bool open(){
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance_;wc.lpfnWndProc=proc;wc.lpszClassName=cls_;
        wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);wc.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;
        if(!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        POINT cursor{};GetCursorPos(&cursor);
        MONITORINFO mi{sizeof(mi)};
        if(!GetMonitorInfoW(MonitorFromPoint(cursor,MONITOR_DEFAULTTONEAREST),&mi))return false;
        constexpr DWORD style=WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX|WS_CLIPCHILDREN;
        dpi_=GetDpiForSystem();if(!dpi_)dpi_=96;
        RECT r{0,0,px(560),px(482)};AdjustWindowRectExForDpi(&r,style,FALSE,0,dpi_);
        const int w=r.right-r.left,h=r.bottom-r.top;
        HWND created=CreateWindowExW(0,cls_,tr(L"NightLight settings",L"Definições do NightLight"),style,
            mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-w)/2,mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-h)/2,
            w,h,owner_,nullptr,instance_,this);
        if(!created)return false;
        RECT actual{0,0,px(560),px(482)};AdjustWindowRectExForDpi(&actual,style,FALSE,0,dpi_);
        const int dw=actual.right-actual.left,dh=actual.bottom-actual.top;
        SetWindowPos(created,nullptr,mi.rcWork.left+std::max(0L,(mi.rcWork.right-mi.rcWork.left-dw)/2),
            mi.rcWork.top+std::max(0L,(mi.rcWork.bottom-mi.rcWork.top-dh)/2),dw,dh,SWP_NOZORDER|SWP_NOACTIVATE);
        ShowWindow(created,SW_SHOW);SetFocus(ctrl(GeneralTab));SetForegroundWindow(created);return true;
    }
};
