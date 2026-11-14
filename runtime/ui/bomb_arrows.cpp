// Small alpha-blended charcoal particles identify bomb arrows without changing elemental arrow materials.
#include <math.h>
#include "imgui.h"
extern "C" {
#include "bomb_arrows.h"
#include "platform/platform.h"
}

extern "C" void bomb_arrows_draw(float width,float height) {
    BombArrowVisual arrows[32];int n=bomb_arrows_visuals(arrows,32);
    if(!n) return;
    BombArrowCamera camera;
    if(!bomb_arrows_camera(&camera) || !isfinite(camera.fov_y) || camera.fov_y<=0 || camera.aspect<=0) return;
    float half=tanf(camera.fov_y*.5f);
    if(!isfinite(half) || half<.01f) return;
    float vh=height,vw=vh*camera.aspect;
    if(vw>width) { vw=width;vh=vw/camera.aspect; }
    float left=(width-vw)*.5f,top=(height-vh)*.5f;
    ImDrawList *draw=ImGui::GetBackgroundDrawList();
    uint64_t now=plat_now_ns();float t=(float)((now/1000000)%60000)*.001f;
    for(int i=0;i<n;i++) {
        if(now-arrows[i].seen_ns>150000000ull) continue;
        float x=0,y=0,z=0;
        for(int k=0;k<3;k++) {
            float delta=arrows[i].pos[k]-camera.pos[k];
            x+=delta*camera.right[k];y+=delta*camera.up[k];z+=delta*camera.fwd[k];
        }
        if(!isfinite(z) || z<.15f) continue;
        float sx=left+vw*(.5f+x/(2*z*half*camera.aspect));
        float sy=top+vh*(.5f-y/(2*z*half));
        if(!isfinite(sx) || !isfinite(sy) || sx<left || sx>left+vw || sy<top || sy>top+vh) continue;
        float radius=vh*.075f/(2*z*half);
        radius=radius<4?4:radius>32?32:radius;
        draw->PushClipRect(ImVec2(left,top),ImVec2(left+vw,top+vh),true);
        // A dark alpha cloud remains visible; an additive black glow would disappear entirely.
        for(int layer=9;layer>=0;layer--) {
            float r=radius*(.45f+layer*.1f);
            draw->AddCircleFilled(ImVec2(sx,sy),r,IM_COL32(4,4,6,10),24);
        }
        for(int p=0;p<6;p++) {
            float a=t*2+p*1.04719755f;
            ImVec2 point(sx+cosf(a)*radius*.5f,sy+sinf(a)*radius*.5f);
            draw->AddCircleFilled(point,radius*.36f,IM_COL32(0,0,0,65),16);
        }
        draw->AddCircle(ImVec2(sx,sy),radius*.82f,IM_COL32(75,75,82,65),24,1);
        draw->PopClipRect();
    }
}
