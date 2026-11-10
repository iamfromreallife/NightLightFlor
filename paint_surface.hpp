#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>

namespace nightlight {
class PaintSurface {
public:
    PaintSurface()=default;
    PaintSurface(const PaintSurface&)=delete;
    PaintSurface& operator=(const PaintSurface&)=delete;
    ~PaintSurface() { reset(); }
    HDC ensure(HDC reference,int width,int height) {
        if (width<=0 || height<=0) return nullptr;
        const auto area=static_cast<std::int64_t>(width)*height;
        const auto cached=static_cast<std::int64_t>(width_)*height_;
        const bool oversized=cached>std::max<std::int64_t>(area*4,65536);
        if (bitmap_ && width<=width_ && height<=height_ && !oversized) { SetViewportOrgEx(dc_,0,0,nullptr); return dc_; }
        const int roundedW=((width+63)/64)*64,roundedH=((height+63)/64)*64;
        int capacityW=oversized?roundedW:std::max(roundedW,width_);
        int capacityH=oversized?roundedH:std::max(roundedH,height_);
        if (static_cast<std::int64_t>(capacityW)*capacityH>static_cast<std::int64_t>(roundedW)*roundedH*2) {
            capacityW=roundedW;capacityH=roundedH;
        }
        HBITMAP bitmap=CreateCompatibleBitmap(reference,capacityW,capacityH);
        if (!bitmap) return nullptr;
        if (!dc_) dc_=CreateCompatibleDC(reference);
        if (!dc_) { DeleteObject(bitmap); return nullptr; }
        HGDIOBJ previous=SelectObject(dc_,bitmap);
        if (!previous || previous==HGDI_ERROR) { DeleteObject(bitmap); return nullptr; }
        if (!original_) original_=previous;
        if (bitmap_) DeleteObject(bitmap_);
        bitmap_=bitmap; width_=capacityW; height_=capacityH;
        SetViewportOrgEx(dc_,0,0,nullptr);
        return dc_;
    }
    HDC dc() const { return dc_; }
    void reset() {
        if (dc_) { if (original_) SelectObject(dc_,original_); if (bitmap_) DeleteObject(bitmap_); DeleteDC(dc_); }
        dc_=nullptr;bitmap_=nullptr;original_=nullptr;width_=height_=0;
    }
private:
    HDC dc_=nullptr;
    HBITMAP bitmap_=nullptr;
    HGDIOBJ original_=nullptr;
    int width_=0,height_=0;
};
}
