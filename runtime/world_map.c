// Reveal native collectible map icons without changing game progression. Checked icons use native save queries.
#include "host.h"
#include "cfg.h"
#include "platform/platform.h"
#include "world_map.h"
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#define MAIN 0x7000000000ull
static int art_ready;
static uint64_t night_ids[1000];
static unsigned night_count;
static int night_ready;
void world_map_night_loaded(const char *directory,int ready) {
    __atomic_store_n(&night_ready,0,__ATOMIC_RELEASE);night_count=0;
    if(!ready)return;
    char path[2400],magic[8];unsigned count=0;
    if(snprintf(path,sizeof path,"%s/skull-night.bin",directory)>=(int)sizeof path)return;
    FILE *file=fopen(path,"rb");if(!file)return;
    int ok=fread(magic,1,8,file)==8 && !memcmp(magic,"NXWNT001",8) && fread(&count,4,1,file)==1 &&
        count>0 && count<=1000 && fread(night_ids,8,count,file)==count && fgetc(file)==EOF;
    fclose(file);
    if(!ok)return;
    for(unsigned i=0;i<count;i++)if(!night_ids[i] || (i && night_ids[i]<=night_ids[i-1]))return;
    night_count=count;__atomic_store_n(&night_ready,1,__ATOMIC_RELEASE);
    rt_log("world map: night-only hints loaded for %u Skulltula locations",count);
}
static int nocturnal(uint64_t id) {
    if(!__atomic_load_n(&night_ready,__ATOMIC_ACQUIRE))return 0;
    unsigned lo=0,hi=night_count;
    while(lo<hi) { unsigned mid=lo+(hi-lo)/2;if(night_ids[mid]<id)lo=mid+1;else hi=mid; }
    return lo<night_count && night_ids[lo]==id;
}
void world_map_art_loaded(int ready) { __atomic_store_n(&art_ready,ready,__ATOMIC_RELEASE); }
int world_map_ready(void) { return __atomic_load_n(&art_ready,__ATOMIC_ACQUIRE); }
static uint64_t ptr(uint64_t at) { uint64_t v=0;return at && plat_mem_read(at,&v,8)?v:0; }
static unsigned word(uint64_t at) { unsigned v=0;return at && plat_mem_read(at,&v,4)?v:0; }
static unsigned byte(uint64_t at) { unsigned char v=0;return at && plat_mem_read(at,&v,1)?v:0; }
static int context(Cpu *c) { return ptr(c->x[19])==MAIN+0x4be19c8; }
static int enabled(unsigned kind) {
    return world_map_ready() && (kind==6?cfg_bool("NX2NAVI_WORLD_HEART_PIECES"):kind==7?cfg_bool("NX2NAVI_WORLD_SKULLTULAS"):0);
}
void world_map_master_host(Cpu *c) {
    static int noted;
    if(!noted && cfg_get("NX2NAVI_WORLD_MAP_DEBUG")) {
        rt_log("world map: context=%#llx type=%#llx records=%u art=%d",(unsigned long long)c->x[19],
            (unsigned long long)ptr(c->x[19]),word(c->x[19]+0x368),world_map_ready());noted=1;
    }
    if(context(c) && (enabled(6)||enabled(7))) c->x[8]=1;
}
void world_map_visibility_host(Cpu *c) {
    if(!context(c))return;
    unsigned kind=(unsigned)c->x[9];
    if(enabled(kind)) c->x[9]=8; // bypass the collected-only filter; the record's real type is retained
    else if(!byte(c->x[19]+0x45c)) { c->x[9]=6;c->x[8]=1; } // the other toggle must not enable this category
}
static int skull_collected(uint64_t id) {
    uint64_t manager=ptr(MAIN+0x4e842a8),table=ptr(manager+0xc60);
    unsigned count=word(manager+0xc58);
    // Bound corrupted counts without assuming a Skulltula-only registry.
    if(!id || !table || !count || count>1000000)return 0;
    unsigned lo=0,hi=count;
    while(lo<hi) { unsigned mid=lo+(hi-lo)/2;uint64_t key=ptr(table+(uint64_t)mid*16);
        if(key<id)lo=mid+1;else hi=mid;
    }
    return lo<count && ptr(table+(uint64_t)lo*16)==id && (byte(table+(uint64_t)lo*16+12)&1);
}
static void style(Cpu *c) {
    if(!context(c))return;
    unsigned kind=(unsigned)c->x[9];
    if(!enabled(kind))return;
    Cpu saved=*c;
    int collected=kind==6?(rt_call(c,MAIN+0x206290,1,c->sp)&1):skull_collected(ptr(c->sp));
    *c=saved;
    float frame=kind==6?(collected?0.f:2.f):(collected?1.f:3.f);
    if(kind==7 && nocturnal(ptr(c->sp)))frame=collected?5.f:4.f;
    static unsigned noted;
    unsigned bit=1u<<(kind*2+!!collected);
    if(!(noted&bit) && cfg_get("NX2NAVI_WORLD_MAP_DEBUG")) {
        rt_log("world map: kind=%u collected=%d frame=%.0f",kind,collected,(double)frame);
        noted|=bit;
    }
    memcpy(&c->v[0],&frame,4);
}
void world_map_style_host(Cpu *c) {
    style(c);
    // This callback replaces the native animation call, after its frame has been selected.
    rt_call(c,MAIN+0x2d2d90,1,(uint64_t)c->x[0]);
}
