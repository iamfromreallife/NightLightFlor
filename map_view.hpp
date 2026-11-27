#pragma once
#include <algorithm>
#include <cmath>
#include <vector>
namespace nightlight {
struct MapPoint {double x=0,y=0;};
struct MapTileKey {int x,y;};
inline double mapWorldSize(int zoom){return 256.0*(1<<zoom);}
inline MapPoint projectMapPoint(double latitude,double longitude,int zoom){
    constexpr double pi=3.14159265358979323846;
    const double n=mapWorldSize(zoom),lat=std::clamp(latitude,-85.051128,85.051128)*pi/180;
    return {(longitude+180)/360*n,(1-std::asinh(std::tan(lat))/pi)*.5*n};
}
inline std::vector<MapTileKey> visibleMapTiles(int zoom,MapPoint center,int width,int height){
    std::vector<MapTileKey> out;const int n=1<<zoom;
    const int left=static_cast<int>(std::floor((center.x-width/2.0)/256)),right=static_cast<int>(std::floor((center.x+width/2.0)/256));
    const int top=std::max(0,static_cast<int>(std::floor((center.y-height/2.0)/256))),bottom=std::min(n-1,static_cast<int>(std::floor((center.y+height/2.0)/256)));
    for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x)out.push_back({((x%n)+n)%n,y});
    return out;
}
inline double wrappedMapDelta(double delta,double worldSize){return delta-std::round(delta/worldSize)*worldSize;}
}
