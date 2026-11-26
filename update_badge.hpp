#pragma once
// Render an antialiased orange dot on a copy of the actual DPI/theme tray icon.
inline HICON withUpdateBadge(HICON base,int w,int h) {
    if(!base || w<=0 || h<=0 || w>256 || h>256)return nullptr;
    BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=w;info.bmiHeader.biHeight=-h;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr;HBITMAP color=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    HDC dc=CreateCompatibleDC(nullptr);
    if(!color || !bits || !dc){if(color)DeleteObject(color);if(dc)DeleteDC(dc);return nullptr;}
    auto old=SelectObject(dc,color);
    if(!old || old==HGDI_ERROR){DeleteDC(dc);DeleteObject(color);return nullptr;}
    auto* p=static_cast<unsigned char*>(bits);
    std::fill_n(p,static_cast<size_t>(w)*h*4,0);
    const BOOL drawn=DrawIconEx(dc,0,0,base,w,h,0,nullptr,DI_NORMAL);
    GdiFlush();
    if(!drawn){SelectObject(dc,old);DeleteDC(dc);DeleteObject(color);return nullptr;}
    bool hasAlpha=false;
    for(int i=0;i<w*h;++i)if(p[i*4+3]){hasAlpha=true;break;}
    if(!hasAlpha){
        std::vector<unsigned char> original(p,p+static_cast<size_t>(w)*h*4);
        std::fill_n(p,static_cast<size_t>(w)*h*4,0);
        DrawIconEx(dc,0,0,base,w,h,0,nullptr,DI_MASK);GdiFlush();
        for(int i=0;i<w*h;++i){
            const int k=i*4;const bool opaque=p[k]<128;
            for(int c=0;c<3;++c)p[k+c]=opaque?original[k+c]:0;
            p[k+3]=opaque?255:0;
        }
    }
    const double r=std::max(2.0,std::min(w,h)*0.23),cx=w-r,cy=r;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        int coverage=0;
        for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
            const double dx=x+(sx+0.5)/4-cx,dy=y+(sy+0.5)/4-cy;
            if(dx*dx+dy*dy<=r*r)++coverage;
        }
        if(!coverage)continue;
        const int a=(coverage*255+8)/16,k=(y*w+x)*4;
        const int colors[]={28,125,255};
        for(int c=0;c<3;++c)p[k+c]=static_cast<unsigned char>((colors[c]*a+p[k+c]*(255-a)+127)/255);
        p[k+3]=static_cast<unsigned char>(a+(p[k+3]*(255-a)+127)/255);
    }
    SelectObject(dc,old);DeleteDC(dc);
    const int stride=((w+15)/16)*2;
    std::vector<unsigned char> mask(static_cast<size_t>(stride)*h,0);
    HBITMAP mono=CreateBitmap(w,h,1,1,mask.data());HICON result=nullptr;
    if(mono){ICONINFO ii{};ii.fIcon=TRUE;ii.hbmColor=color;ii.hbmMask=mono;
        result=CreateIconIndirect(&ii);DeleteObject(mono);}
    DeleteObject(color);return result;
}
