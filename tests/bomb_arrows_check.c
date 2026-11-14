// Exercise projectile ownership, one-shot consumption, native request arguments and transparent CPU hooks.
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
#include "../runtime/bomb_arrows.c"

static unsigned char heap[65536],page[4096],stack[4096];
static size_t used;
static uint64_t player,manager,projectile,component,bomb,chemical,request,enum_field;
static int enabled=1,stock=3,explosions,creates,cancels;
static uint64_t clock_ns=1000000000;
static uint64_t alloc(size_t n) { uint64_t a=(uint64_t)(uintptr_t)(heap+used);used+=(n+15)&~15u;assert(used<sizeof heap);return a; }
static void put64(uint64_t a,uint64_t v) { memcpy((void *)(uintptr_t)a,&v,8); }
static void put32(uint64_t a,uint32_t v) { memcpy((void *)(uintptr_t)a,&v,4); }
static int inside(uint64_t a,size_t n,const void *p,size_t size) {
    uint64_t b=(uint64_t)(uintptr_t)p;return a>=b && n<=size && a-b<=size-n;
}
int plat_mem_read(uint64_t a,void *out,size_t n) {
    uint64_t v=0;
    if(a==MAIN+0x4e371d0) v=(uint64_t)(uintptr_t)(heap+0x100);
    else if(a==MAIN+0x4c7f3e0) v=(uint64_t)(uintptr_t)(heap+0x180);
    else if(!inside(a,n,heap,sizeof heap) && !inside(a,n,page,sizeof page) && !inside(a,n,stack,sizeof stack)) return 0;
    else { memcpy(out,(void *)(uintptr_t)a,n);return 1; }
    assert(n==8);memcpy(out,&v,8);return 1;
}
uint32_t mem_query(uint64_t a,RtMemoryInfo *out) {
    if(!inside(a,1,stack,sizeof stack)) return 1;
    *out=(RtMemoryInfo){.base=(uint64_t)(uintptr_t)stack,.size=sizeof stack,.perm=3};return 0;
}
int plat_mem_state(uint64_t a,uint64_t limit,uint64_t *end) {
    *end=limit;return inside(a,(size_t)(limit-a),stack,sizeof stack)?PLAT_PROT_RW:PLAT_PROT_NONE;
}
void *rt_patch_page(void) { return page; }
uint64_t plat_now_ns(void) { return clock_ns; }
const char *cfg_get(const char *key) { (void)key;return NULL; }
int cfg_bool(const char *key) { assert(!strcmp(key,"NX2NAVI_BOMB_ARROWS"));return enabled; }
int cheats_bomb_arrow_ammo(int consume) { if(stock<=0)return 0;if(consume)stock--;return 1; }
void rt_log(const char *fmt,...) { (void)fmt; }
void rt_fatal(const char *fmt,...) { (void)fmt;abort(); }
uint64_t rt_call(Cpu *cpu,uint64_t fn,int n,...) {
    uint64_t args[8]={0};va_list ap;va_start(ap,n);
    for(int i=0;i<n;i++)args[i]=va_arg(ap,uint64_t);va_end(ap);
    uint64_t result=0;
    if(fn==MAIN+0xc729b0) {
        assert(n==2 && pointer(args[1])==MAIN+0x3dcd120);result=enum_field;
    } else if(fn==MAIN+0x372a70) {
        assert(n==8 && args[0]==manager && pointer(args[1])==MAIN+0x3cef69b && args[4]==1);
        for(int i=5;i<8;i++)assert(!args[i]);
        assert(!pointer(cpu->sp) && !pointer(cpu->sp+8) && !pointer(cpu->sp+16));
        float *t=(float *)(uintptr_t)args[2];
        assert(t[0]==10 && t[1]==20 && t[2]==30);
        assert(t[3]==1 && t[7]==1 && t[11]==1 && t[12]==1 && t[13]==1 && t[14]==1);
        assert(pointer(args[2]+0x40)==MAIN+0x3cef6a4);
        assert(*(uint8_t *)(uintptr_t)(args[2]+0x98)==3);
        put32(request,creates?3:2);put64(args[3],request);creates++;clock_ns+=100;
    } else if(fn==MAIN+0x3aaefd0) {
        assert(n==3 && args[1]==1 && !args[2]);put64(args[0],0);result=bomb;
    } else if(fn==MAIN+0x2ddf620) {
        assert(n==5 && args[0]==chemical && args[1]==2 && args[2]==(uint64_t)-1 && args[3]==0x7fc00000 && !args[4]);explosions++;
    } else if(fn==MAIN+0xb2fa30) { cancels++; }
    else assert(!"unexpected native call");
    // Guest calls may clobber caller-saved registers and SIMD values; hooks must restore them.
    memset(cpu->x,0x5a,sizeof cpu->x);memset(cpu->v,0xa5,sizeof cpu->v);cpu->nzcv=0xffffffff;
    cpu->x[0]=result;return result;
}
static void name_actor(uint64_t actor,const char *name) {
    uint64_t str=alloc(64);strcpy((char *)(uintptr_t)str,name);put64(actor+0x20,str);
}
static Cpu cpu_for(uint64_t object) {
    Cpu c;memset(&c,0x31,sizeof c);c.x[0]=object;c.x[16]=0x12345678;c.x[30]=0x7002eabc34;
    c.sp=((uint64_t)(uintptr_t)(stack+sizeof stack))&~15ull;return c;
}
static void tick(unsigned state) {
    put32(component+0x24,state);Cpu c=cpu_for(component),expected=c;expected.x[30]=c.x[16];
    bomb_arrows_tick_host(&c);assert(!memcmp(&c,&expected,sizeof c));
}
static void frame(void) {
    Cpu c=cpu_for(0),expected=c;expected.x[30]=c.x[16];unsigned char before[sizeof stack];memcpy(before,stack,sizeof stack);
    bomb_arrows_frame_host(&c);assert(!memcmp(&c,&expected,sizeof c));assert(!memcmp(before,stack,sizeof stack));
}
static void fresh_arrow(void) {
    static unsigned gen=10;put32(projectile+0x18,++gen);
}
int main(void) {
    used=0x1000;memset(stack,0x91,sizeof stack);
    player=alloc(0x500);manager=alloc(0x200);projectile=alloc(0x500);component=alloc(0x800);
    bomb=alloc(0x500);chemical=alloc(0x200);request=alloc(0x40);enum_field=alloc(8);
    name_actor(player,"Player");name_actor(projectile,"Item_Arrow_Normal");name_actor(bomb,"Item_Bomb");
    put64((uint64_t)(uintptr_t)(heap+0x100)+0x480,player);put64((uint64_t)(uintptr_t)(heap+0x180),manager);
    uint64_t components=alloc(0x500),logic=alloc(0x40),bb=alloc(0x200),entry=alloc(0x80),bc=alloc(0x500);
    put64(projectile+0x270,components);put64(components,logic);put64(logic+0x10,bb);
    put64(component+0x18,projectile);put32(component+0xd8,1);
    put64(component+0x78,entry);put32(component+0x80,123);put32(entry+0x48,123);put64(entry+0x40,player);
    put64(bomb+0x270,bc);put64(bc+0x198,chemical);
    float pos[3]={10,20,30};memcpy((void *)(uintptr_t)(projectile+0x31c),pos,12);
    fresh_arrow();tick(1);frame();BombArrowVisual visual[32];assert(bomb_arrows_visuals(visual,32)==1 && stock==3);
    tick(1);assert(stock==3 && !creates);tick(2);tick(2);assert(stock==2);tick(0);tick(0);frame();
    assert(creates==1 && !explosions && !cancels);put32(request,3);frame();assert(explosions==1 && stock==2);
    fresh_arrow();tick(1);tick(6);frame();assert(stock==2 && creates==1);
    fresh_arrow();put32(enum_field,1);tick(1);tick(2);frame();assert(stock==2 && creates==1);put32(enum_field,0);
    fresh_arrow();put32(entry+0x48,124);tick(1);tick(2);frame();assert(stock==2 && creates==1);put32(entry+0x48,123);
    fresh_arrow();enabled=0;tick(1);tick(2);frame();assert(stock==2 && creates==1);enabled=1;
    fresh_arrow();stock=0;tick(0);tick(1);tick(2);frame();assert(!stock && creates==1);
    fresh_arrow();stock=2;tick(1);tick(2);enabled=0;tick(0);frame();assert(stock==1 && creates==2 && explosions==2 && !cancels);
    puts("bomb_arrows_check: passed (ownership, generation, elemental isolation, cancel, one-shot cost, native arguments, CPU/stack preservation)");
}
