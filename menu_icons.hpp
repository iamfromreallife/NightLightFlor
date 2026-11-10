#pragma once
#include <windows.h>
#include <gdiplus.h>
#include <cstring>

namespace nightlight {
enum class MenuIcon { Language,Theme,Light,Dark,Start,Window,Minimize,Tray,Adjust,Monitor,Disable,Exit };
// Premultiplied transparent bitmaps work with native menu layout, checkmarks,
// keyboard navigation and screen readers; there is no owner-drawn menu text.
inline HBITMAP menuIconBitmap(MenuIcon icon,UINT dpi,COLORREF foreground,bool checked=false) {
    const int size=MulDiv(20,dpi,96),width=MulDiv(36,dpi,96);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-size;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* pixels=nullptr;
    HBITMAP handle=CreateDIBSection(nullptr,&info,DIB_RGB_COLORS,&pixels,nullptr,0);
    if (!handle || !pixels) return handle;
    std::memset(pixels,0,width*size*4);
    {
        Gdiplus::Bitmap bitmap(width,size,width*4,PixelFormat32bppPARGB,static_cast<BYTE*>(pixels));
        Gdiplus::Graphics g(&bitmap); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const float scale=static_cast<float>(size)/20; g.ScaleTransform(scale,scale);
        g.TranslateTransform(static_cast<float>(MulDiv(16,dpi,96)),0,Gdiplus::MatrixOrderAppend);
        Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(foreground),GetGValue(foreground),GetBValue(foreground)),1.25f);
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
        auto line=[&](float x,float y,float u,float v){g.DrawLine(&pen,x,y,u,v);};
        switch (icon) {
        case MenuIcon::Language:
            g.DrawEllipse(&pen,3.0f,3.0f,14.0f,14.0f); g.DrawEllipse(&pen,7.0f,3.0f,6.0f,14.0f);
            line(3,10,17,10); break;
        case MenuIcon::Theme:
            g.DrawEllipse(&pen,3.0f,3.0f,14.0f,14.0f);
            line(10,3,10,17); line(11,5,14,7); line(11,9,16,12); line(11,13,13,15); break;
        case MenuIcon::Light:
            g.DrawEllipse(&pen,6.0f,6.0f,8.0f,8.0f);
            line(10,1,10,3);line(10,17,10,19);line(1,10,3,10);line(17,10,19,10);
            line(3,3,4.5f,4.5f);line(15.5f,15.5f,17,17);line(3,17,4.5f,15.5f);line(15.5f,4.5f,17,3);break;
        case MenuIcon::Dark: {
            Gdiplus::GraphicsPath p;
            p.AddBezier(13.0f,3.0f,6.0f,0.0f,1.0f,7.0f,4.0f,13.0f);
            p.AddBezier(4.0f,13.0f,7.0f,19.0f,15.0f,18.0f,17.0f,11.0f);
            p.AddBezier(17.0f,11.0f,10.0f,14.0f,7.0f,7.0f,13.0f,3.0f);
            g.DrawPath(&pen,&p);break;
        }
        case MenuIcon::Start:
            g.DrawArc(&pen,3.0f,3.0f,14.0f,14.0f,310.0f,280.0f);line(10,1,10,10);break;
        case MenuIcon::Window:
            g.DrawRectangle(&pen,2.5f,3.5f,15.0f,13.0f);line(3,7,17,7);line(5,5,6,5);break;
        case MenuIcon::Minimize:
            line(3,12,17,12);break;
        case MenuIcon::Tray:
            line(3,10,3,16);line(3,16,17,16);line(17,16,17,10);line(3,10,7,10);line(13,10,17,10);
            g.DrawArc(&pen,7.0f,7.0f,6.0f,6.0f,0.0f,180.0f);line(10,2,10,7);line(7,4,10,7);line(10,7,13,4);break;
        case MenuIcon::Adjust:
            line(2,6,6,6);line(10,6,18,6);line(2,14,11,14);line(15,14,18,14);
            g.DrawEllipse(&pen,6.0f,4.0f,4.0f,4.0f);g.DrawEllipse(&pen,11.0f,12.0f,4.0f,4.0f);break;
        case MenuIcon::Monitor:
            g.DrawRectangle(&pen,3.0f,4.0f,14.0f,10.0f);line(10,14,10,17);line(7,17,13,17);break;
        case MenuIcon::Disable:
            g.DrawArc(&pen,2.0f,6.0f,8.0f,8.0f,90.0f,180.0f);
            g.DrawArc(&pen,10.0f,6.0f,8.0f,8.0f,270.0f,180.0f);
            line(6,6,14,6);line(6,14,14,14);
            g.DrawEllipse(&pen,4.5f,8.5f,3.0f,3.0f);break;
        case MenuIcon::Exit:
            line(9,3,3,3);line(3,3,3,17);line(3,17,9,17);line(8,10,18,10);line(14,6,18,10);line(18,10,14,14);break;
        }
        if (checked) {
            g.ResetTransform(); g.ScaleTransform(scale,scale);
            line(2,10,6,14);line(6,14,13,6);
        }
    }
    return handle;
}
}
