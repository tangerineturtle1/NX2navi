// Bomb arrows for game 1.1.1. Native guest-thread hooks own projectile state and actor requests.
#include "host.h"
#include "cfg.h"
#include "cheats.h"
#include "bomb_arrows.h"
#include "platform/platform.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#define MAIN 0x7000000000ull
enum { ARROWS=32, REQUESTS=8 };
typedef struct {
    uint64_t actor, component, shooter, seen_ns;
    uint32_t generation, state;
    int armed, detonated, eligible;
    float pos[3],aura_pos[3];
} Arrow;
static Arrow arrows[ARROWS];
static uint64_t pending[REQUESTS];
static int visual_lock,visual_count;
static int camera_needed;
static int state_lock, queued_count;
static float queued_positions[16][3];
static BombArrowVisual visuals[ARROWS];
static uint64_t pointer(uint64_t a) { uint64_t p=0; if(a) plat_mem_read(a,&p,8); return p; }
static uint32_t number(uint64_t a) { uint32_t n=0; if(a) plat_mem_read(a,&n,4); return n; }
static void write64(uint64_t a,uint64_t p) { memcpy((void *)(uintptr_t)a,&p,8); }
static int actor_named(uint64_t actor,const char *expected) {
    char name[64]={0};
    return actor && plat_mem_read(pointer(actor+0x20),name,63) && !strcmp(name,expected);
}
static uint64_t ref_actor(uint64_t ref) {
    uint64_t entry=pointer(ref+8);uint32_t generation=number(ref+0x10);
    return entry && generation!=0xffffffffu && number(entry+0x48)==generation ? pointer(entry+0x40) : 0;
}
static void lock_visual(void) { while(__atomic_exchange_n(&visual_lock,1,__ATOMIC_ACQUIRE)) {} }
static void unlock_visual(void) { __atomic_store_n(&visual_lock,0,__ATOMIC_RELEASE); }
static void lock_state(void) { while(__atomic_exchange_n(&state_lock,1,__ATOMIC_ACQUIRE)) {} }
static void unlock_state(void) { __atomic_store_n(&state_lock,0,__ATOMIC_RELEASE); }
int bomb_arrows_visuals(BombArrowVisual *out,int capacity) {
    lock_visual();int n=visual_count<capacity?visual_count:capacity;
    memcpy(out,visuals,(size_t)n*sizeof *out);unlock_visual();return n;
}
int bomb_arrows_camera_needed(void) { return __atomic_load_n(&camera_needed,__ATOMIC_RELAXED); }
static uint64_t request_handle(int slot) { return (uint64_t)(uintptr_t)rt_patch_page()+0x600+slot*16; }
static int request_bomb(Cpu *c,const float pos[3]) {
    int slot=-1;
    for(int i=0;i<REQUESTS;i++) if(!pending[i]) { slot=i;break; }
    uint64_t manager=pointer(pointer(MAIN+0x4c7f3e0));
    if(slot<0 || !manager || !rt_patch_page()) { rt_log("bomb-arrow: request unavailable slot %d manager %#llx",slot,(unsigned long long)manager);return 0; }
    // Match ExecuteGenericRequestCreateActor's position, rotation, scale and instance-name defaults.
    uint64_t scratch=(c->sp-0x200)&~15ull;
    uint64_t end;
    // The kernel's initial stack is host-backed; guest QueryMemory can report its old source mapping as free.
    // Check the actual host page protection before touching scratch storage below the current stack pointer.
    for(uint64_t p=scratch;p<scratch+0x200;p=end)
        if(plat_mem_state(p,scratch+0x200,&end)!=PLAT_PROT_RW || end<=p) return 0;
    unsigned char saved[0x200];memcpy(saved,(void *)(uintptr_t)scratch,sizeof saved);
    memset((void *)(uintptr_t)scratch,0,sizeof saved);
    uint64_t name=scratch+0x20,arg=scratch+0x40,handle=request_handle(slot);
    write64(name,MAIN+0x3cef69b);write64(handle,0);
    float *transform=(float *)(uintptr_t)arg;
    memcpy(transform,pos,12);
    transform[3]=transform[7]=transform[11]=1;
    transform[12]=transform[13]=transform[14]=1;
    write64(arg+0x40,MAIN+0x3cef6a4);
    *(uint8_t *)(uintptr_t)(arg+0x98)=3;
    uint64_t old_sp=c->sp;c->sp=scratch;
    rt_call(c,MAIN+0x372a70,8,manager,name,arg,handle,(uint64_t)1,(uint64_t)0,(uint64_t)0,(uint64_t)0);
    c->sp=old_sp;memcpy((void *)(uintptr_t)scratch,saved,sizeof saved);
    if(!pointer(handle)) { rt_log("bomb-arrow: actor request was refused");return 0; }
    pending[slot]=plat_now_ns();
    rt_log("bomb-arrow: explosion requested at %.2f %.2f %.2f",pos[0],pos[1],pos[2]);return 1;
}
void bomb_arrows_frame_host(Cpu *c) {
    c->x[30]=c->x[16];Cpu saved=*c;uint64_t now=plat_now_ns();
    // Worker-side projectile callbacks only queue positions; actor creation stays on the framework thread.
    float positions[16][3];
    lock_state();int count=queued_count;memcpy(positions,queued_positions,(size_t)count*12);queued_count=0;unlock_state();
    for(int i=0;i<count;i++) request_bomb(c,positions[i]);
    for(int i=0;i<REQUESTS;i++) if(pending[i]) {
        uint64_t handle=request_handle(i),request=pointer(handle);
        unsigned state=request?number(request)&0xffff:0;
        if(state==3) {
            uint64_t actor=rt_call(c,MAIN+0x3aaefd0,3,handle,(uint64_t)1,(uint64_t)0);
            uint64_t chemical=pointer(pointer(actor+0x270)+0x198);
            if(actor_named(actor,"Item_Bomb") && chemical) {
                rt_call(c,MAIN+0x2ddf620,5,chemical,(uint64_t)2,(uint64_t)-1,(uint64_t)0x7fc00000,(uint64_t)0);
                rt_log("bomb-arrow: native detonation requested for %#llx",(unsigned long long)actor);
            } else rt_log("bomb-arrow: created actor did not validate as a bomb");
            pending[i]=0;
        } else if(!request || state==4 || (now>=pending[i] && now-pending[i]>5000000000ull)) {
            if(request) rt_call(c,MAIN+0xb2fa30,2,handle,(uint64_t)1);
            write64(handle,0);pending[i]=0;rt_log("bomb-arrow: request failed/timed out (state %u)",state);
        }
    }
    lock_state();lock_visual();visual_count=0;
    for(int i=0;i<ARROWS;i++) {
        Arrow *a=&arrows[i];
        if(a->actor && now-a->seen_ns<150000000ull && !a->detonated && (a->armed || a->eligible)) {
            BombArrowVisual *v=&visuals[visual_count++];memcpy(v->pos,a->aura_pos,12);v->seen_ns=a->seen_ns;v->armed=a->armed;
        }
    }
    __atomic_store_n(&camera_needed,visual_count!=0,__ATOMIC_RELAXED);
    unlock_visual();unlock_state();
    *c=saved;
}
void bomb_arrows_tick_host(Cpu *c) {
    c->x[30]=c->x[16];
    int enabled=cfg_bool("NX2NAVI_BOMB_ARROWS"),debug=cfg_get("NX2NAVI_BOMB_ARROWS_DEBUG")!=NULL;
    uint64_t component=c->x[0],actor=pointer(component+0x18);
    if(!actor) return;
    char name[64]={0};if(!plat_mem_read(pointer(actor+0x20),name,63)) return;
    if(strcmp(name,"Item_Arrow_Normal") && strcmp(name,"NormalArrow")) return;
    uint32_t state=number(component+0x24),type=number(component+0xd8),generation=number(actor+0x18);
    uint64_t now=plat_now_ns();
    Cpu saved=*c;
    uint64_t player=pointer(pointer(MAIN+0x4e371d0)+0x480),shooter=ref_actor(component+0x70);
    int normal=(!strcmp(name,"Item_Arrow_Normal") || !strcmp(name,"NormalArrow")) && actor_named(player,"Player") && shooter==player;
    if(normal) {
        // The projectile's named runtime values live in ActorLogic's blackboard, not the parameter blackboard.
        uint64_t bb=pointer(pointer(pointer(actor+0x270))+0x10),scratch=(c->sp-0x20)&~15ull;
        uint64_t old=pointer(scratch);write64(scratch,MAIN+0x3dcd120);
        uint64_t field=bb?rt_call(c,MAIN+0xc729b0,2,bb,scratch):0;
        write64(scratch,old);normal=field && number(field)==0;
    }
    lock_state();
    Arrow *a=NULL;int oldest=0;
    for(int i=0;i<ARROWS;i++) {
        if(arrows[i].actor==actor && arrows[i].generation==generation) { a=&arrows[i];break; }
        if(arrows[i].seen_ns<arrows[oldest].seen_ns) oldest=i;
    }
    if(!a) { a=&arrows[oldest];memset(a,0,sizeof *a);a->actor=actor;a->component=component;a->generation=generation;a->state=6; }
    if(debug && a->state!=state) rt_log("bomb-arrow: %s actor %#llx type %u state %u -> %u",name,(unsigned long long)actor,type,a->state,state);
    if(plat_mem_read(actor+0x31c,a->pos,12) && isfinite(a->pos[0]) && isfinite(a->pos[1]) && isfinite(a->pos[2])) {
        memcpy(a->aura_pos,a->pos,12);
        // The actor origin is at the fletching; identify the arrow at its tip rather than Link's drawing hand.
        float direction[3]={0};
        for(int k=0;k<3;k++) plat_mem_read(actor+0x330+12*k,&direction[k],4);
        float length=direction[0]*direction[0]+direction[1]*direction[1]+direction[2]*direction[2];
        if(isfinite(length) && length>.9f && length<1.1f)
            for(int k=0;k<3;k++) a->aura_pos[k]+=.65f*direction[k];
        // Native states: 1 is nocked, 2 is flying, 0 is resting after impact.
        a->seen_ns=now;a->eligible=enabled && normal && state==1 && cheats_bomb_arrow_ammo(0);
        if(enabled && normal && state==2 && a->state!=2 && !a->armed && !a->detonated && cheats_bomb_arrow_ammo(1)) {
            a->armed=1;a->shooter=shooter;rt_log("bomb-arrow: armed projectile %#llx",(unsigned long long)actor);
        }
        if(a->armed && !a->detonated && state==0) {
            a->detonated=1;
            if(queued_count<16) memcpy(queued_positions[queued_count++],a->pos,12);
            else rt_log("bomb-arrow: explosion queue full");
        }
    }
    a->state=state;unlock_state();*c=saved;
}
