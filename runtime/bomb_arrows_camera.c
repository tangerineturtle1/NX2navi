// Camera rows come from the same opaque scene draws that render the arrow, independent of anti-aliasing.
#include "bomb_arrows.h"
#include "platform/platform.h"
#include <math.h>
#include <string.h>
typedef struct { uint64_t address; unsigned votes; BombArrowCamera camera; } View;
static View views[8];
static uint64_t frame_id,seen_ns;
static unsigned count,best;
static int camera_lock;
static void lock(void) { while(__atomic_exchange_n(&camera_lock,1,__ATOMIC_ACQUIRE)) {} }
static void unlock(void) { __atomic_store_n(&camera_lock,0,__ATOMIC_RELEASE); }
void bomb_arrows_camera_observe(uint64_t frame,uint64_t address,const float *v) {
    if(!v || !address || !isfinite(v[28]) || !isfinite(v[33]) || fabsf(v[28])<.01f || fabsf(v[33])<.01f) return;
    // Orthographic UI and shadow projections do not describe the player's perspective camera.
    if(fabsf(v[43])>.001f || fabsf(fabsf(v[42])-1)>.001f) return;
    lock();
    if(frame_id!=frame) { frame_id=frame;count=best=0; }
    unsigned i=0;
    while(i<count && views[i].address!=address) i++;
    if(i==count) {
        if(count==8) { unlock();return; }
        View *view=&views[count++];memset(view,0,sizeof *view);view->address=address;
        BombArrowCamera *c=&view->camera;
        for(int k=0;k<3;k++) {
            c->right[k]=v[k];c->up[k]=v[4+k];c->fwd[k]=-v[8+k];
            c->pos[k]=-(v[k]*v[3]+v[4+k]*v[7]+v[8+k]*v[11]);
        }
        c->fov_y=2*atanf(1/fabsf(v[33]));c->aspect=fabsf(v[33]/v[28]);
    }
    if(++views[i].votes>views[best].votes) best=i;
    seen_ns=plat_now_ns();unlock();
}
int bomb_arrows_camera(BombArrowCamera *out) {
    lock();int valid=count && plat_now_ns()-seen_ns<150000000ull;
    if(valid) *out=views[best].camera;
    unlock();return valid;
}
