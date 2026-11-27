#pragma once
// Included after the shared palette and painters, at the same scope as the
// other native dialogs. All standard library headers are in main.cpp.
class NightLightLocationDialog {
    enum {SearchEdit=1,SearchButton=2,Results=3,CancelButton=4,ApplyButton=5,ZoomIn=6,ZoomOut=7};
    HINSTANCE instance_;HWND owner_,window_=nullptr,query_=nullptr,results_=nullptr,apply_=nullptr;
    int language_;
    bool closing_=false,searchFailed_=false,mapFailed_=false;
    UINT dpi_=96,frameDpi_=96;
    HFONT font_=nullptr;
    struct MapImage {nightlight::CityTile tile;IStream* stream=nullptr;std::unique_ptr<Gdiplus::Image> image;
        ~MapImage(){image.reset();if(stream)stream->Release();}};
    std::vector<std::unique_ptr<MapImage>> images_;
    static constexpr int mapWidth_=452,mapHeight_=424;
    int zoom_=10,requestedZoom_=0;nightlight::MapPoint center_{};
    bool mapDirty_=false,dragging_=false;POINT dragLast_{};
    int zoomWheelRemainder_=0;
    std::function<Palette()> palette_;std::function<bool()> dark_,hc_;
    nightlight::PaintSurface surface_;
    std::vector<nightlight::CityLocation> cities_;int selected_=-1;
    nightlight::LocationSearchState searchState_;
    bool initializingQuery_=false;
    unsigned mapSelectionEpoch_=0,requestedMapEpoch_=0;
    std::shared_ptr<nightlight::LocationRequest<std::vector<nightlight::CityLocation>>> search_;
    std::shared_ptr<nightlight::LocationRequest<std::vector<nightlight::CityTile>>> map_;
    std::optional<nightlight::CityLocation> answer_;
    static constexpr wchar_t cls_[]=L"NightLight.Location.Native";
    int px(int n)const{return MulDiv(n,static_cast<int>(dpi_),96);}
    const wchar_t* tr(const wchar_t* en,const wchar_t* pt)const{return nightlight::translate(language_,en,pt);}
    void text(HDC dc,const std::wstring& label,RECT r,COLORREF color,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE){
        auto old=SelectObject(dc,font_?font_:GetStockObject(DEFAULT_GUI_FONT));SetBkMode(dc,TRANSPARENT);SetTextColor(dc,color);
        DrawTextW(dc,label.c_str(),-1,&r,flags|DT_NOPREFIX|DT_END_ELLIPSIS);if(old&&old!=HGDI_ERROR)SelectObject(dc,old);
    }
    void repaint(){if(window_)InvalidateRect(window_,nullptr,FALSE);}
    HWND child(int id,const wchar_t* cls,const wchar_t* caption,DWORD style,int x,int y,int w,int h){
        HWND control=CreateWindowExW(0,cls,caption,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|style,px(x),px(y),px(w),px(h),window_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance_,nullptr);
        if(control)SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(font_),FALSE);return control;
    }
    RECT mapRect()const{return {px(249),px(57),px(249+mapWidth_),px(57+mapHeight_)};}
    void releaseImage(){images_.clear();}
    void loadImages(std::vector<nightlight::CityTile> tiles){
        std::vector<std::unique_ptr<MapImage>> next;
        for(auto& tile:tiles){if(tile.png.empty())continue;
            auto item=std::make_unique<MapImage>();item->tile=std::move(tile);
            HGLOBAL data=GlobalAlloc(GMEM_MOVEABLE,item->tile.png.size());if(!data)continue;
            void* bytes=GlobalLock(data);if(!bytes){GlobalFree(data);continue;}
            std::memcpy(bytes,item->tile.png.data(),item->tile.png.size());GlobalUnlock(data);
            if(FAILED(CreateStreamOnHGlobal(data,TRUE,&item->stream))){GlobalFree(data);continue;}
            item->image.reset(Gdiplus::Image::FromStream(item->stream));
            if(!item->image||item->image->GetLastStatus()!=Gdiplus::Ok||item->image->GetWidth()!=256||item->image->GetHeight()!=256)continue;
            // The owned IStream already holds the compressed PNG.
            // clear() retains its allocation; release that duplicate buffer.
            std::string{}.swap(item->tile.png);next.push_back(std::move(item));
        }
        if(!next.empty())images_=std::move(next);mapFailed_=images_.empty();
    }
    void constrainCenter(){const double n=nightlight::mapWorldSize(zoom_);center_.x=std::fmod(center_.x,n);if(center_.x<0)center_.x+=n;center_.y=std::clamp(center_.y,0.0,n);}
    void requestMap(){
        if(selected_<0)return;mapDirty_=true;if(map_||dragging_)return;
        mapDirty_=false;requestedZoom_=zoom_;requestedMapEpoch_=mapSelectionEpoch_;mapFailed_=false;
        map_=nightlight::beginLocationRequest<std::vector<nightlight::CityTile>>([z=zoom_,c=center_]{return nightlight::visibleCityTiles(z,c,mapWidth_,mapHeight_);});
        if(!map_)mapFailed_=true;SetTimer(window_,1,150,nullptr);repaint();
    }
    void zoomBy(int delta){if(selected_<0)return;const int next=std::clamp(zoom_+delta,2,18);if(next==zoom_)return;
        const double ratio=nightlight::mapWorldSize(next)/nightlight::mapWorldSize(zoom_);center_.x*=ratio;center_.y*=ratio;zoom_=next;constrainCenter();requestMap();repaint();}
    bool zoomWheel(WPARAM wp,LPARAM lp){
        if(!(GET_KEYSTATE_WPARAM(wp)&MK_CONTROL)){zoomWheelRemainder_=0;return false;}
        POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(window_,&pt);
        const auto viewport=mapRect();
        if(!PtInRect(&viewport,pt)){zoomWheelRemainder_=0;return false;}
        zoomWheelRemainder_+=GET_WHEEL_DELTA_WPARAM(wp);
        const int steps=zoomWheelRemainder_/WHEEL_DELTA;
        zoomWheelRemainder_%=WHEEL_DELTA;
        if(steps)zoomBy(steps);
        return true;
    }
    void select(int index){
        if(index<0||static_cast<size_t>(index)>=cities_.size())return;
        if(selected_==index)return;selected_=index;++mapSelectionEpoch_;SendMessageW(results_,LB_SETCURSEL,index,0);EnableWindow(apply_,TRUE);
        releaseImage();mapFailed_=false;zoom_=10;center_=nightlight::projectCity(cities_[index],zoom_);constrainCenter();requestMap();
    }
    void listCities(bool selectFirst){
        SendMessageW(results_,WM_SETREDRAW,FALSE,0);
        SendMessageW(results_,LB_RESETCONTENT,0,0);
        for(const auto& city:cities_)SendMessageW(results_,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(city.name.c_str()));
        SendMessageW(results_,WM_SETREDRAW,TRUE,0);InvalidateRect(results_,nullptr,FALSE);
        if(selectFirst&&!cities_.empty())select(0);else EnableWindow(apply_,FALSE);
    }
    std::wstring queryText(){wchar_t text[160]{};GetWindowTextW(query_,text,160);
        std::wstring q=text;const auto first=q.find_first_not_of(L" \t\r\n");
        if(first==std::wstring::npos)return {};return q.substr(first,q.find_last_not_of(L" \t\r\n")-first+1);}
    void queryChanged(){
        if(initializingQuery_||!results_||!apply_)return;
        const auto q=queryText();if(q==searchState_.query)return;
        searchState_.changed(q,GetTickCount64());searchFailed_=false;mapFailed_=false;
        selected_=-1;++mapSelectionEpoch_;cities_.clear();mapDirty_=false;releaseImage();
        SendMessageW(results_,LB_RESETCONTENT,0,0);EnableWindow(apply_,FALSE);
        if(searchState_.busy()||map_)SetTimer(window_,1,150,nullptr);else KillTimer(window_,1);
        repaint();
    }
    void launchSearch(){
        if(search_||!searchState_.ready(GetTickCount64()))return;
        searchState_.started();searchFailed_=false;
        search_=nightlight::beginLocationRequest<std::vector<nightlight::CityLocation>>([q=searchState_.inFlight,language=language_]{return nightlight::searchCities(q,language);});
        if(!search_){searchState_.finished();searchState_.explicitSelection=false;searchFailed_=true;}
        SetTimer(window_,1,150,nullptr);repaint();
    }
    void beginSearch(){
        queryChanged();if(searchState_.query.size()<2)return;
        if(!cities_.empty()&&searchState_.completed==searchState_.query){
            if(selected_<0)select(0);SetFocus(results_);return;
        }
        searchState_.manual(GetTickCount64());launchSearch();SetTimer(window_,1,150,nullptr);repaint();
    }
    void tick(){
        if(search_&&search_->done.load(std::memory_order_acquire)){
            const bool current=searchState_.finished();
            if(current){
                searchFailed_=!search_->ok;cities_=std::move(search_->result);
                const bool choose=searchState_.explicitSelection;searchState_.explicitSelection=false;
                listCities(choose);
            }
            search_.reset();repaint();
        }
        launchSearch();
        if(map_&&map_->done.load(std::memory_order_acquire)){
            auto tiles=std::move(map_->result);map_.reset();
            if(selected_>=0){if(requestedZoom_==zoom_&&requestedMapEpoch_==mapSelectionEpoch_)loadImages(std::move(tiles));if(mapDirty_)requestMap();repaint();}
        }
        if(!searchState_.busy()&&!map_)KillTimer(window_,1);
    }
    void paint(HDC output){
        RECT r{};GetClientRect(window_,&r);HDC dc=surface_.ensure(output,r.right,r.bottom);if(!dc)dc=output;
        auto p=palette_();fill(dc,r,p.page);
        rounded(dc,RECT{px(14),px(12),px(602),px(42)},px(8),p.chip,GetFocus()==query_?p.accent:p.border);
        rounded(dc,RECT{px(14),px(52),px(234),px(506)},px(10),p.card,p.border);
        rounded(dc,RECT{px(244),px(52),px(706),px(506)},px(10),p.card,p.border);
        const RECT viewport=mapRect();
        if(selected_>=0){
            const float scale=static_cast<float>(dpi_)/96.0f;
            const double n=nightlight::mapWorldSize(zoom_);
            const double cx=(viewport.left+viewport.right)/2.0,cy=(viewport.top+viewport.bottom)/2.0;
            Gdiplus::Graphics g(dc);g.SetClip(Gdiplus::Rect(viewport.left,viewport.top,viewport.right-viewport.left,viewport.bottom-viewport.top));
            g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
            auto wrap=[n](double x){return nightlight::wrappedMapDelta(x,n);};
            for(const auto& item:images_){
                const double ratio=n/nightlight::mapWorldSize(item->tile.zoom);
                const double span=256*ratio;
                const double dx=wrap((item->tile.x*256.0+128)*ratio-center_.x)-span/2;
                const double dy=item->tile.y*256.0*ratio-center_.y;
                g.DrawImage(item->image.get(),Gdiplus::RectF(static_cast<float>(cx+dx*scale),static_cast<float>(cy+dy*scale),static_cast<float>(span*scale+0.5),static_cast<float>(span*scale+0.5)));
            }
            const auto city=nightlight::projectCity(cities_[selected_],zoom_);
            const float x=static_cast<float>(cx+wrap(city.x-center_.x)*scale),y=static_cast<float>(cy+(city.y-center_.y)*scale);
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::SolidBrush white(Gdiplus::Color(255,255,255,255)),orange(drawingColor(p.accent));
            g.FillEllipse(&white,x-px(7),y-px(7),static_cast<float>(px(14)),static_cast<float>(px(14)));
            g.FillEllipse(&orange,x-px(5),y-px(5),static_cast<float>(px(10)),static_cast<float>(px(10)));
        }
        if(images_.empty()){
            const auto msg=map_?tr(L"Loading map...",L"A carregar mapa..."):mapFailed_?tr(L"Map unavailable",L"Mapa indisponível"):tr(L"Select a city",L"Selecione uma cidade");
            text(dc,msg,RECT{viewport.left,viewport.top,viewport.right,viewport.top+px(26)},p.muted,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        }
        text(dc,L"© OpenStreetMap contributors",RECT{px(251),px(482),px(699),px(503)},p.muted,DT_RIGHT|DT_VCENTER|DT_SINGLELINE);
        std::wstring status=(searchState_.pending||(!searchState_.inFlight.empty()&&searchState_.inFlight==searchState_.query))?tr(L"Searching...",L"A procurar..."):searchFailed_?tr(L"Search failed. Try again.",L"Falha na pesquisa. Tente novamente."):selected_>=0?cities_[selected_].name:!cities_.empty()?tr(L"Select a city",L"Selecione uma cidade"):!searchState_.completed.empty()?tr(L"No matching cities.",L"Nenhuma cidade encontrada."):tr(L"Search for a city.",L"Pesquise uma cidade.");
        text(dc,status,RECT{px(14),px(510),px(706),px(532)},p.text);
        text(dc,tr(L"Location search: Open-Meteo / GeoNames",L"Pesquisa: Open-Meteo / GeoNames"),RECT{px(14),px(541),px(490),px(562)},p.muted);
        if(dc!=output)BitBlt(output,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
    }
    void draw(const DRAWITEMSTRUCT& item){
        auto p=palette_();RECT r=item.rcItem;
        if(item.CtlType==ODT_LISTBOX){fill(item.hDC,r,p.card);if(item.itemID>=cities_.size())return;
            if(item.itemState&ODS_SELECTED){RECT bg=r;InflateRect(&bg,-px(2),-1);rounded(item.hDC,bg,px(7),mix(p.chip,p.accent,.13),p.border);}
            r.left+=px(7);r.right-=px(4);text(item.hDC,cities_[item.itemID].name,r,p.text);return;}
        HDC dc=nullptr;auto bp=BeginBufferedPaint(item.hDC,&r,BPBF_TOPDOWNDIB,nullptr,&dc);if(!bp)dc=item.hDC;
        fill(dc,r,p.page);const bool primary=item.CtlID==ApplyButton;
        auto bg=primary?mix(p.chip,p.accent,.12):p.chip;
        if(item.itemState&ODS_SELECTED)bg=mix(bg,p.text,.08);
        rounded(dc,r,px(8),bg,primary||(item.itemState&ODS_FOCUS)?p.accent:p.border);
        wchar_t label[64]{};GetWindowTextW(item.hwndItem,label,64);text(dc,label,r,item.itemState&ODS_DISABLED?p.muted:p.text,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        if(bp)EndBufferedPaint(bp,TRUE);
    }
    void close(){if(window_&&!closing_){closing_=true;DestroyWindow(window_);}}
    LRESULT dispatch(UINT msg,WPARAM wp,LPARAM lp){
        switch(msg){
        case WM_CREATE:{
            font_=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_SWISS,nightlight::uiFontFace(language_));
            query_=child(SearchEdit,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,22,18,572,20);
            SendMessageW(query_,EM_SETLIMITTEXT,150,0);SendMessageW(query_,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(tr(L"Search city...",L"Pesquisar cidade...")));
            child(SearchButton,L"BUTTON",tr(L"Search",L"Pesquisar"),WS_TABSTOP|BS_OWNERDRAW,610,12,96,30);
            results_=child(Results,L"LISTBOX",L"",WS_TABSTOP|WS_VSCROLL|LBS_NOTIFY|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOINTEGRALHEIGHT,19,57,210,444);
            child(CancelButton,L"BUTTON",tr(L"Cancel",L"Cancelar"),WS_TABSTOP|BS_OWNERDRAW,506,537,96,30);
            apply_=child(ApplyButton,L"BUTTON",tr(L"Apply",L"Aplicar"),WS_TABSTOP|BS_OWNERDRAW,610,537,96,30);
            child(ZoomIn,L"BUTTON",L"+",WS_TABSTOP|BS_OWNERDRAW,666,64,28,28);
            child(ZoomOut,L"BUTTON",L"−",WS_TABSTOP|BS_OWNERDRAW,666,98,28,28);
            if(!query_||!results_||!apply_)return -1;SendMessageW(results_,LB_SETITEMHEIGHT,0,px(30));EnableWindow(apply_,FALSE);
            applyNightLightFrame(window_,palette_(),dark_(),hc_());return 0;}
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(window_,&ps);paint(dc);EndPaint(window_,&ps);return 0;}
        case WM_MEASUREITEM:if(lp){reinterpret_cast<MEASUREITEMSTRUCT*>(lp)->itemHeight=px(30);return TRUE;}break;
        case WM_DRAWITEM:if(lp){draw(*reinterpret_cast<DRAWITEMSTRUCT*>(lp));return TRUE;}break;
        case WM_CTLCOLOREDIT:case WM_CTLCOLORLISTBOX:case WM_CTLCOLORSTATIC:{const auto p=palette_();auto dc=reinterpret_cast<HDC>(wp);const auto bg=reinterpret_cast<HWND>(lp)==query_?p.chip:p.card;SetTextColor(dc,p.text);SetBkColor(dc,bg);SetDCBrushColor(dc,bg);return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));}
        case WM_MOUSEWHEEL:if(zoomWheel(wp,lp))return 0;break;
        case WM_LBUTTONDOWN:{POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};auto r=mapRect();if(selected_>=0&&PtInRect(&r,pt)){dragging_=true;dragLast_=pt;SetCapture(window_);SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));return 0;}break;}
        case WM_MOUSEMOVE:if(dragging_&&GetCapture()==window_){POINT pt{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};center_.x-=(pt.x-dragLast_.x)*96.0/dpi_;center_.y-=(pt.y-dragLast_.y)*96.0/dpi_;dragLast_=pt;constrainCenter();repaint();return 0;}break;
        case WM_LBUTTONUP:if(dragging_){dragging_=false;if(GetCapture()==window_)ReleaseCapture();requestMap();return 0;}break;
        case WM_CAPTURECHANGED:if(dragging_){dragging_=false;requestMap();}return 0;
        case WM_SETCURSOR:{POINT pt{};GetCursorPos(&pt);ScreenToClient(window_,&pt);auto r=mapRect();if(selected_>=0&&LOWORD(lp)==HTCLIENT&&PtInRect(&r,pt)){SetCursor(LoadCursorW(nullptr,IDC_SIZEALL));return TRUE;}break;}
        case WM_TIMER:if(wp==1){tick();return 0;}break;
        case WM_COMMAND:{const int id=LOWORD(wp),code=HIWORD(wp);
            if(id==SearchButton&&code==BN_CLICKED)beginSearch();
            else if(id==ZoomIn&&code==BN_CLICKED)zoomBy(1);
            else if(id==ZoomOut&&code==BN_CLICKED)zoomBy(-1);
            else if(id==SearchEdit&&code==EN_CHANGE)queryChanged();
            else if(id==SearchEdit&&(code==EN_SETFOCUS||code==EN_KILLFOCUS))repaint();
            else if(id==Results&&code==LBN_SELCHANGE)select(static_cast<int>(SendMessageW(results_,LB_GETCURSEL,0,0)));
            else if(id==Results&&code==LBN_DBLCLK){const int index=static_cast<int>(SendMessageW(results_,LB_GETCURSEL,0,0));if(index>=0&&index!=selected_)select(index);SetFocus(apply_);}
            else if(id==CancelButton&&code==BN_CLICKED)close();
            else if(id==ApplyButton&&code==BN_CLICKED&&selected_>=0){answer_=cities_[selected_];close();}return 0;}
        case WM_CLOSE:close();return 0;
        case WM_NCDESTROY:{HWND old=window_;SetWindowLongPtrW(old,GWLP_USERDATA,0);KillTimer(old,1);window_=nullptr;return DefWindowProcW(old,msg,wp,lp);}
        }return DefWindowProcW(window_,msg,wp,lp);
    }
    static LRESULT CALLBACK proc(HWND h,UINT msg,WPARAM wp,LPARAM lp){auto* self=reinterpret_cast<NightLightLocationDialog*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(msg==WM_NCCREATE){self=static_cast<NightLightLocationDialog*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);self->window_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}return self?self->dispatch(msg,wp,lp):DefWindowProcW(h,msg,wp,lp);}
public:
    NightLightLocationDialog(HINSTANCE i,HWND owner,int language,std::function<Palette()> p,std::function<bool()> dark,std::function<bool()> hc):instance_(i),owner_(owner),language_(language),palette_(std::move(p)),dark_(std::move(dark)),hc_(std::move(hc)){}
    ~NightLightLocationDialog(){close();releaseImage();if(font_)DeleteObject(font_);}
    std::optional<nightlight::CityLocation> run(const nightlight::ColorSchedule& current){
        WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.hInstance=instance_;wc.lpfnWndProc=proc;wc.lpszClassName=cls_;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon=LoadIconW(instance_,MAKEINTRESOURCEW(101));wc.hIconSm=wc.hIcon;
        if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return {};
        MONITORINFO mi{sizeof(mi)};if(!GetMonitorInfoW(MonitorFromWindow(owner_,MONITOR_DEFAULTTONEAREST),&mi))return {};
        frameDpi_=GetDpiForWindow(owner_);if(!frameDpi_)frameDpi_=96;dpi_=frameDpi_;
        constexpr DWORD style=WS_POPUP|WS_CAPTION|WS_SYSMENU|WS_CLIPCHILDREN;
        RECT borders{};AdjustWindowRectExForDpi(&borders,style,FALSE,0,frameDpi_);
        dpi_=static_cast<UINT>(std::max(48,std::min({static_cast<int>(frameDpi_),static_cast<int>((mi.rcWork.right-mi.rcWork.left-borders.right+borders.left)*96/720),static_cast<int>((mi.rcWork.bottom-mi.rcWork.top-borders.bottom+borders.top)*96/580)})));
        RECT r{0,0,px(720),px(580)};AdjustWindowRectExForDpi(&r,style,FALSE,0,frameDpi_);const int w=r.right-r.left,h=r.bottom-r.top;
        HWND created=CreateWindowExW(0,cls_,tr(L"Set location",L"Definir localização"),style,mi.rcWork.left+(mi.rcWork.right-mi.rcWork.left-w)/2,mi.rcWork.top+(mi.rcWork.bottom-mi.rcWork.top-h)/2,w,h,owner_,nullptr,instance_,this);
        if(!created)return {};
        if(!current.locationName.empty()){initializingQuery_=true;cities_.push_back({current.locationName,current.latitude,current.longitude});SetWindowTextW(query_,current.locationName.c_str());searchState_.query=queryText();searchState_.completed=searchState_.query;initializingQuery_=false;listCities(true);}
        EnableWindow(owner_,FALSE);ShowWindow(created,SW_SHOW);SetForegroundWindow(created);SetFocus(query_);SendMessageW(query_,EM_SETSEL,0,-1);
        MSG msg{};while(window_){const BOOL n=GetMessageW(&msg,nullptr,0,0);if(n<=0){if(n==0)PostQuitMessage(static_cast<int>(msg.wParam));close();break;}
            // Wheel messages may be sent to the focused search/list control
            // rather than the map HWND. Hit-test their screen coordinates.
            if(msg.message==WM_MOUSEWHEEL&&(msg.hwnd==window_||IsChild(window_,msg.hwnd)) && zoomWheel(msg.wParam,msg.lParam))continue;
            if(msg.message==WM_KEYDOWN&&(msg.hwnd==window_||IsChild(window_,msg.hwnd))){
                if((GetKeyState(VK_CONTROL)&0x8000) && (msg.wParam==VK_ADD||msg.wParam==VK_OEM_PLUS||msg.wParam==VK_SUBTRACT||msg.wParam==VK_OEM_MINUS)){zoomBy(msg.wParam==VK_ADD||msg.wParam==VK_OEM_PLUS?1:-1);continue;}
                if(msg.wParam==VK_ESCAPE){close();continue;}
                if(msg.hwnd==query_&&(msg.wParam==VK_DOWN||msg.wParam==VK_UP)&&!cities_.empty()){
                    const int index=selected_<0?(msg.wParam==VK_DOWN?0:static_cast<int>(cities_.size())-1):std::clamp(selected_+(msg.wParam==VK_DOWN?1:-1),0,static_cast<int>(cities_.size())-1);
                    select(index);SetFocus(results_);continue;}
                if(msg.wParam==VK_RETURN&&msg.hwnd==query_){beginSearch();continue;}
                if(msg.wParam==VK_RETURN&&msg.hwnd==results_){const int index=static_cast<int>(SendMessageW(results_,LB_GETCURSEL,0,0));if(index>=0&&index!=selected_)select(index);SetFocus(apply_);continue;}}
            if(!window_||!IsDialogMessageW(window_,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
        if(IsWindow(owner_)){EnableWindow(owner_,TRUE);SetForegroundWindow(owner_);}
        return answer_;
    }
};
