// SPDX-License-Identifier: MIT
// Original native tower field. No firmware assets or external renderer code.
#pragma once

#include "imgui.h"
#include <algorithm>
#include <array>
#include <cmath>

namespace ps2_background_detail {
namespace {

inline float noise(unsigned int seed) {
    seed ^= seed >> 16;
    seed *= 0x7feb352du;
    seed ^= seed >> 15;
    seed *= 0x846ca68bu;
    seed ^= seed >> 16;
    return static_cast<float>(seed & 0xffffu) / 65535.0f;
}

struct Point { float x, y, z; };
struct Column { float x, z, width, length, height; unsigned int seed; };
struct Camera {
    ImVec2 origin, size;
    float sine, cosine, distance, vertical, horizontal, focal, aim;
    float depth(Point p) const {
        return distance-horizontal*(sine*p.x+cosine*p.z)-vertical*(p.y-aim);
    }
    ImVec2 project(Point p) const {
        const float scale=focal/std::max(2.0f,depth(p));
        return ImVec2(origin.x+size.x*.5f+(cosine*p.x-sine*p.z)*scale,
                      origin.y+size.y*.5f-(horizontal*(p.y-aim)-vertical*(sine*p.x+cosine*p.z))*scale);
    }
};

inline ImU32 shade(float r,float g,float b,int alpha=255) {
    return IM_COL32(static_cast<int>(std::clamp(r,0.0f,255.0f)),
                    static_cast<int>(std::clamp(g,0.0f,255.0f)),
                    static_cast<int>(std::clamp(b,0.0f,255.0f)),alpha);
}
inline void quad(ImDrawList* draw,const std::array<ImVec2,4>& p,ImU32 color) {
    const float winding=(p[1].x-p[0].x)*(p[2].y-p[0].y)-(p[1].y-p[0].y)*(p[2].x-p[0].x);
    if(winding<0) draw->AddQuadFilled(p[0],p[3],p[2],p[1],color);
    else          draw->AddQuadFilled(p[0],p[1],p[2],p[3],color);
}

inline const std::array<Column,90>& columns() {
    static const auto field=[] {
        struct Cell { unsigned int index;float rank; };
        std::array<Cell,120> cells{};
        for(unsigned int i=0;i<cells.size();++i) cells[i]={i,noise(i*31u+117u)};
        std::sort(cells.begin(),cells.end(),[](Cell a,Cell b){
            return a.rank<b.rank||(a.rank==b.rank&&a.index<b.index);
        });
        std::array<Column,90> result{};
        for(unsigned int i=0;i<result.size();++i) {
            const unsigned int cell=cells[i].index,seed=cell*47u+809u;
            result[i]={(static_cast<float>(cell%12)-5.5f)*2.25f,
                       (static_cast<float>(cell/12)-4.5f)*1.72f,
                       1.58f,1.36f,3.3f+noise(seed)*8.6f,seed};
        }
        return result;
    }();
    return field;
}

struct Face {
    std::array<Point,4> points;
    ImU32 color;
    float depth;
    int column; // Only roof faces own the local grain/shadow work.
    bool gradient;
};

inline void cap_detail(ImDrawList* draw,const Camera& camera,const Column& c) {
    const float left=c.x-c.width*.5f,back=c.z-c.length*.5f;
    // Grain stays attached to the stone, without flicker or image storage.
    for(unsigned int i=0;i<24;++i) {
        const unsigned int seed=c.seed+i*173u;
        const float x=left+(.04f+.88f*noise(seed+1u))*c.width;
        const float z=back+(.04f+.88f*noise(seed+2u))*c.length;
        const float w=c.width*(.018f+.032f*noise(seed+3u));
        const float h=c.length*(.018f+.032f*noise(seed+4u));
        const std::array<ImVec2,4> p{{camera.project({x,c.height,z}),
            camera.project({x+w,c.height,z}),camera.project({x+w,c.height,z+h}),
            camera.project({x,c.height,z+h})}};
        quad(draw,p,i&1u?IM_COL32(235,237,244,18):IM_COL32(9,12,20,22));
    }
    // Parallel key-light shadows intersect this receiving roof only. No shadow
    // texture, framebuffer readback or retained graphics history is needed.
    int shadows=0;
    for(const auto& other:columns()) {
        if(other.height<=c.height||other.seed==c.seed) continue;
        const float delta=other.height-c.height;
        const float x=other.x+delta*.31f,z=other.z-delta*.17f;
        const float x0=std::max(left,x-other.width*.5f);
        const float x1=std::min(left+c.width,x+other.width*.5f);
        const float z0=std::max(back,z-other.length*.5f);
        const float z1=std::min(back+c.length,z+other.length*.5f);
        if(x0>=x1||z0>=z1) continue;
        quad(draw,{{camera.project({x0,c.height,z0}),camera.project({x1,c.height,z0}),
                    camera.project({x1,c.height,z1}),camera.project({x0,c.height,z1})}},
             IM_COL32(8,12,25,92));
        if(++shadows==4) break;
    }
}

inline Point rotate(Point p,float ax,float ay,float az) {
    const float cx=std::cos(ax),sx=std::sin(ax),cy=std::cos(ay),sy=std::sin(ay);
    const float cz=std::cos(az),sz=std::sin(az);
    const Point x{p.x,p.y*cx-p.z*sx,p.y*sx+p.z*cx};
    const Point y{x.x*cy+x.z*sy,x.y,-x.x*sy+x.z*cy};
    return {y.x*cz-y.y*sz,y.x*sz+y.y*cz,y.z};
}

} // namespace
} // namespace ps2_background_detail

// Fixed scene: 90 stone columns, four nested glass cubes and seven lights.
// Face/sort/projection storage is bounded; normal ImGui buffers are reused.
// This effect runs only while the launcher/menu is drawing.
static inline void draw_ps2_startup_background(ImDrawList* draw,ImVec2 origin,ImVec2 size,float time) {
    using namespace ps2_background_detail;
    if(!draw||size.x<=0||size.y<=0) return;
    if(!std::isfinite(time)) time=0;
    const float intro=std::clamp(time/6.0f,0.0f,1.0f);
    const float vertical=31.0f-intro*intro*(3.0f-2.0f*intro);
    constexpr float radius=1.65f;
    const float distance=std::sqrt(vertical*vertical+radius*radius);
    const float angle=.025f+std::sin(time*.045f)*.035f;
    const Camera camera{origin,size,std::sin(angle),std::cos(angle),distance,
                        vertical/distance,radius/distance,size.y*.92f,2.6f};
    const Point eye{camera.sine*radius,vertical+camera.aim,camera.cosine*radius};
    draw->PushClipRect(origin,ImVec2(origin.x+size.x,origin.y+size.y),true);
    draw->AddRectFilled(origin,ImVec2(origin.x+size.x,origin.y+size.y),IM_COL32(22,22,24,255));
    const ImVec2 glow=camera.project({0,4,0});
    for(int i=8;i>0;--i)
        draw->AddCircleFilled(glow,size.y*(.12f+.032f*static_cast<float>(i)),IM_COL32(29,45,102,5),32);

    std::array<Face,320> faces{};
    size_t count=0;
    auto add=[&](std::array<Point,4> points,ImU32 color,int column,bool gradient) {
        float depth=0;
        for(Point p:points) depth+=camera.depth(p)*.25f;
        if(count<faces.size()) faces[count++]={points,color,depth,column,gradient};
    };
    for(size_t i=0;i<columns().size();++i) {
        const auto& c=columns()[i];
        const float x0=c.x-c.width*.5f,x1=c.x+c.width*.5f;
        const float z0=c.z-c.length*.5f,z1=c.z+c.length*.5f,h=c.height;
        const float edge=1.0f-.38f*std::clamp((std::hypot(c.x,c.z)-7.0f)/9.0f,0.0f,1.0f);
        const float tone=(.66f+noise(c.seed+5u)*.39f)*edge;
        const float blue=std::max(0.0f,1.0f-std::hypot(c.x,c.z)/14.0f);
        const ImU32 cap=shade(174*tone,180*tone,195*tone);
        const ImU32 dim=shade(48*tone,53*tone,65*tone+blue*14);
        const ImU32 lit=shade(69*tone,74*tone,87*tone+blue*15);
        if(eye.x>x1) add({{{x1,0,z0},{x1,0,z1},{x1,h,z1},{x1,h,z0}}},dim,-1,true);
        else if(eye.x<x0) add({{{x0,0,z1},{x0,0,z0},{x0,h,z0},{x0,h,z1}}},lit,-1,true);
        if(eye.z>z1) add({{{x1,0,z1},{x0,0,z1},{x0,h,z1},{x1,h,z1}}},lit,-1,true);
        else if(eye.z<z0) add({{{x0,0,z0},{x1,0,z0},{x1,h,z0},{x0,h,z0}}},dim,-1,true);
        add({{{x0,h,z0},{x1,h,z0},{x1,h,z1},{x0,h,z1}}},cap,static_cast<int>(i),false);
    }

    constexpr Point locations[4]={{-8.5f,11.0f,-4.7f},{7.6f,11.3f,-4.0f},
                                   {-7.8f,11.4f,6.3f},{-1.0f,12.1f,-.3f}};
    constexpr int corners[6][4]={{0,1,3,2},{4,6,7,5},{0,4,5,1},
                                 {2,3,7,6},{0,2,6,4},{1,5,7,3}};
    constexpr Point normals[6]={{0,0,-1},{0,0,1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0}};
    for(int cube=0;cube<4;++cube) {
        Point center=locations[cube];
        center.y+=std::sin(time*.5f+static_cast<float>(cube)*1.6f)*.25f;
        for(int layer=0;layer<2;++layer) {
            const float phase=static_cast<float>(cube)*1.4f;
            const float ax=phase+time*(layer?-.17f:.20f);
            const float ay=phase+time*(layer?.21f:.29f);
            const float az=phase*.3f+time*.11f;
            const float half=layer?.86f:1.37f;
            std::array<Point,8> points{};
            for(int v=0;v<8;++v) {
                Point p=rotate({v&1?half:-half,v&2?half:-half,v&4?half:-half},ax,ay,az);
                points[v]={center.x+p.x,center.y+p.y,center.z+p.z};
            }
            for(int f=0;f<6;++f) {
                const Point n=rotate(normals[f],ax,ay,az);
                const float facing=n.x*(eye.x-center.x)+n.y*(eye.y-center.y)+n.z*(eye.z-center.z);
                const float light=std::max(0.0f,-n.x*.31f+n.y*.93f+n.z*.17f);
                const int alpha=layer?(facing>0?104:56):(facing>0?63:31);
                add({{points[corners[f][0]],points[corners[f][1]],points[corners[f][2]],points[corners[f][3]]}},
                    shade(61+light*62,71+light*62,97+light*71,alpha),-1,false);
            }
        }
    }
    std::sort(faces.begin(),faces.begin()+count,[](const Face& a,const Face& b){return a.depth>b.depth;});
    for(size_t f=0;f<count;++f) {
        const Face& face=faces[f];
        std::array<ImVec2,4> points{};
        for(int v=0;v<4;++v) points[v]=camera.project(face.points[v]);
        const int first=draw->VtxBuffer.Size;
        quad(draw,points,face.color);
        if(face.gradient) {
            const ImVec2 base((points[0].x+points[1].x)*.5f,(points[0].y+points[1].y)*.5f);
            const ImVec2 top((points[2].x+points[3].x)*.5f,(points[2].y+points[3].y)*.5f);
            const float dx=top.x-base.x,dy=top.y-base.y,len=dx*dx+dy*dy;
            for(int v=first;v<draw->VtxBuffer.Size;++v) {
                auto& vertex=draw->VtxBuffer[v];
                const float t=len>.001f?std::clamp(((vertex.pos.x-base.x)*dx+(vertex.pos.y-base.y)*dy)/len,0.0f,1.0f):1.0f;
                const float k=t*t*(3.0f-2.0f*t);
                const ImU32 rgb=shade(22+(static_cast<float>((face.color>>IM_COL32_R_SHIFT)&255)-22)*k,
                                     22+(static_cast<float>((face.color>>IM_COL32_G_SHIFT)&255)-22)*k,
                                     24+(static_cast<float>((face.color>>IM_COL32_B_SHIFT)&255)-24)*k);
                vertex.col=(rgb&~IM_COL32_A_MASK)|(vertex.col&IM_COL32_A_MASK);
            }
        }
        if(face.column>=0) cap_detail(draw,camera,columns()[static_cast<size_t>(face.column)]);
    }

    for(unsigned int i=0;i<7;++i) {
        const float phase=static_cast<float>(i)*.897f;
        const float a=time*(.40f+static_cast<float>(i)*.033f)+phase;
        const float radius_light=4.0f+static_cast<float>(i)*.38f;
        const ImVec2 p=camera.project({std::cos(a)*radius_light,10.3f+std::sin(a*1.3f)*1.5f,std::sin(a)*radius_light});
        const bool warm=i==1||i==5;
        draw->AddCircleFilled(p,7.0f,warm?IM_COL32(188,80,66,10):IM_COL32(123,157,230,12),12);
        draw->AddCircleFilled(p,3.2f,warm?IM_COL32(220,108,87,35):IM_COL32(161,190,244,45),10);
        draw->AddCircleFilled(p,1.3f,IM_COL32(225,228,242,130),8);
    }
    const ImU32 dark=IM_COL32(0,0,0,155),clear=IM_COL32(0,0,0,0);
    const float x0=origin.x,x1=x0+size.x,y0=origin.y,y1=y0+size.y;
    draw->AddRectFilledMultiColor({x0,y0},{x0+size.x*.18f,y1},dark,clear,clear,dark);
    draw->AddRectFilledMultiColor({x1-size.x*.18f,y0},{x1,y1},clear,dark,dark,clear);
    draw->AddRectFilledMultiColor({x0,y0},{x1,y0+size.y*.16f},dark,dark,clear,clear);
    draw->AddRectFilledMultiColor({x0,y1-size.y*.16f},{x1,y1},clear,clear,dark,dark);
    draw->PopClipRect();
}
