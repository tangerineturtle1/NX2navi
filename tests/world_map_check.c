// Read-only collection queries, independent live toggles and transparent native map hooks.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include "../runtime/world_map.c"
static unsigned char area[0x500], manager[0xd00], table[16*12000], record[0xc8];
static int hearts,skulls,heart_got,queries,animations;
static float animation;
const char *cfg_get(const char *key) { (void)key;return NULL; }
void rt_log(const char *fmt,...) { (void)fmt; }
static uint64_t address(void *p) { return (uint64_t)(uintptr_t)p; }
static void put64(void *p,uint64_t v) { memcpy(p,&v,8); }
static void put32(void *p,unsigned v) { memcpy(p,&v,4); }
int cfg_bool(const char *key) {
    if(!strcmp(key,"NX2NAVI_WORLD_HEART_PIECES"))return hearts;
    assert(!strcmp(key,"NX2NAVI_WORLD_SKULLTULAS"));return skulls;
}
int plat_mem_read(uint64_t at,void *out,size_t n) {
    if(at==MAIN+0x4e842a8) { uint64_t v=address(manager);assert(n==8);memcpy(out,&v,n);return 1; }
    const void *regions[]={area,manager,table,record};size_t sizes[]={sizeof area,sizeof manager,sizeof table,sizeof record};
    for(unsigned i=0;i<4;i++)if(at>=address((void *)regions[i]) && n<=sizes[i] && at-address((void *)regions[i])<=sizes[i]-n) {
        memcpy(out,(const void *)(uintptr_t)at,n);return 1;
    }
    return 0;
}
uint64_t rt_call(Cpu *c,uint64_t fn,int n,...) {
    va_list ap;va_start(ap,n);uint64_t arg=va_arg(ap,uint64_t);va_end(ap);assert(n==1);
    if(fn==MAIN+0x206290) {
        assert(arg==address(record));queries++;
        memset(c,0xa5,sizeof *c);return heart_got;
    }
    assert(fn==MAIN+0x2d2d90);assert(arg==0x1234);animations++;animation=arg_s(c,0);return 0;
}
int main(void) {
    put64(area,MAIN+0x4be19c8);put64(manager+0xc60,address(table));put32(manager+0xc58,3);
    put64(table,10);table[12]=1;put64(table+16,20);put64(table+32,30);table[44]=1;
    Cpu c={0};c.x[19]=address(area);c.sp=address(record);c.x[0]=0x1234;
    Cpu saved;
    world_map_art_loaded(0);hearts=skulls=1;saved=c;world_map_master_host(&c);assert(!memcmp(&saved,&c,sizeof c));
    world_map_art_loaded(1);world_map_master_host(&c);assert(c.x[8]==1);
    for(int kind=6;kind<=7;kind++)for(hearts=0;hearts<2;hearts++)for(skulls=0;skulls<2;skulls++) {
        c.x[9]=kind;c.x[8]=0;area[0x45c]=0;world_map_visibility_host(&c);
        int on=kind==6?hearts:skulls;assert(c.x[9]==(on?8:6));assert(c.x[8]==(on?0:1));
        c.x[9]=kind;c.x[8]=0;area[0x45c]=1;world_map_visibility_host(&c);assert(c.x[9]==(on?8:(unsigned)kind));
    }
    hearts=skulls=1;
    for(int kind=6;kind<=7;kind++)for(int got=0;got<2;got++) {
        c.x[9]=kind;ret_s(&c,kind==6?0:1);heart_got=got;put64(record,got?10:20);saved=c;
        style(&c);float expected=got?(kind==6?0:1):(kind==6?2:3);assert(arg_s(&c,0)==expected);
        memcpy(&c.v[0],&saved.v[0],sizeof c.v[0]);assert(!memcmp(&saved,&c,sizeof c));
        world_map_style_host(&c);assert(animation==expected);
    }
    assert(queries==4 && animations==4);
    night_ids[0]=10;night_ids[1]=20;night_count=2;night_ready=1;c.x[9]=7;
    put64(record,20);style(&c);assert(arg_s(&c,0)==4.f);
    put64(record,10);style(&c);assert(arg_s(&c,0)==5.f);
    put64(record,30);style(&c);assert(arg_s(&c,0)==1.f);
    night_ready=0;put64(record,20);style(&c);assert(arg_s(&c,0)==3.f);
    hearts=skulls=0;c.x[8]=0;saved=c;world_map_master_host(&c);assert(!memcmp(&saved,&c,sizeof c));
    world_map_style_host(&c);assert(queries==4 && animations==5);
    assert(skull_collected(10) && !skull_collected(20) && skull_collected(30) && !skull_collected(15));
    put64(area,0);saved=c;world_map_visibility_host(&c);assert(!memcmp(&saved,&c,sizeof c));
    for(unsigned i=0;i<12000;i++) { put64(table+i*16,i+1);table[i*16+12]=i==11999; }
    put32(manager+0xc58,12000);assert(skull_collected(12000) && !skull_collected(11999));
    puts("world_map_check: passed (independent toggles, native collected flags, CPU preservation, no progression writes)");
}
