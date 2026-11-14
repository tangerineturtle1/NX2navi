// Optional world markers share native map positioning and never write progression.
#include "host.h"
#include "cfg.h"
#include "world_map.h"
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>
#define MAIN 0x7000000000ull
typedef struct { uint64_t id;uint32_t category,flags,conditions,hash0,index0,hash1,index1,pad; } WorldExtra;
_Static_assert(sizeof(WorldExtra)==40,"world marker metadata layout");
static WorldExtra entries[2048];
static unsigned entry_count;
static int ready;
static int artwork_ready;
static uint64_t ptr(uint64_t at) { uint64_t value=0;return at && plat_mem_read(at,&value,8)?value:0; }
static unsigned word(uint64_t at) { unsigned value=0;return at && plat_mem_read(at,&value,4)?value:0; }
static unsigned byte(uint64_t at) { unsigned char value=0;return at && plat_mem_read(at,&value,1)?value:0; }
static int context(uint64_t at) { return ptr(at)==MAIN+0x4be19c8; }
static int enabled(unsigned category) {
    static const char *keys[]={NULL,"NX2NAVI_WORLD_GROTTOS","NX2NAVI_WORLD_BEAN_PLOTS","NX2NAVI_WORLD_FAIRIES","NX2NAVI_WORLD_UPGRADES"};
    return artwork_ready && category>0 && category<5 && cfg_bool(keys[category]);
}
void world_extra_loaded(const char *directory,int available,int artwork) {
    ready=0;artwork_ready=artwork;entry_count=0;if(!available)return;
    char path[2400],magic[8];unsigned count=0;
    if(snprintf(path,sizeof path,"%s/extra-world.bin",directory)>=(int)sizeof path)return;
    FILE *file=fopen(path,"rb");if(!file)return;
    int ok=fread(magic,1,8,file)==8 && !memcmp(magic,"NXWEX001",8) && fread(&count,4,1,file)==1 &&
        count>0 && count<=2048 && fread(entries,sizeof *entries,count,file)==count && fgetc(file)==EOF;
    fclose(file);if(!ok)return;
    for(unsigned i=0;i<count;i++)if(!entries[i].id || entries[i].category<1 || entries[i].category>4 ||
        entries[i].conditions>2 || entries[i].flags>3 || (i && entries[i].id<=entries[i-1].id))return;
    entry_count=count;ready=1;rt_log("world map: %u additional world records loaded",count);
}
static const WorldExtra *lookup(uint64_t id) {
    if(!ready || !id)return NULL;
    unsigned lo=0,hi=entry_count;
    while(lo<hi) { unsigned mid=lo+(hi-lo)/2;if(entries[mid].id<id)lo=mid+1;else hi=mid; }
    return lo<entry_count && entries[lo].id==id?&entries[lo]:NULL;
}
static const WorldExtra *facility(uint64_t map,uint64_t index) {
    unsigned count=word(map+0x370),capacity=word(map+0x1e0);
    if(index>=count || index>=capacity || count>2048)return NULL;
    const WorldExtra *entry=lookup(ptr(ptr(map+0x1e8)+index*0xc0));
    return entry && !entry->flags?entry:NULL;
}
static int flag(uint32_t hash,uint32_t expected) {
    uint64_t manager=ptr(ptr(MAIN+0x4c7e180));
    if(word(manager+0x10)!=11292 || word(manager+0x2c)!=22584)return 0;
    uint64_t table=ptr(manager+0x20),values=ptr(manager+0x18);
    if(!table || !values || expected>=11292 || !hash)return 0;
    unsigned slot=hash%22584;
    for(unsigned i=0;i<22584;i++,slot=(slot+1)%22584) {
        unsigned key=word(table+(uint64_t)slot*8);if(!key)return 0;
        if(key!=hash)continue;
        if(word(table+(uint64_t)slot*8+4)!=expected)return 0;
        uint64_t entry=values+(uint64_t)expected*24,vtable=ptr(entry);
        return vtable>=MAIN && vtable<MAIN+0x4ea0000 && (byte(entry+22)&1);
    }
    return 0;
}
static int completed(const WorldExtra *entry) {
    return entry->conditions && flag(entry->hash0,entry->index0) &&
        (entry->conditions<2 || flag(entry->hash1,entry->index1));
}
void world_chests_master_host(Cpu *c) {
    uint64_t map=c->x[19],index=c->x[23];if(!context(map))return;
    int reveal=cfg_bool("NX2NAVI_WORLD_CHESTS");
    const WorldExtra *entry=index<word(map+0x358) && index<word(map+0x180)?lookup(ptr(ptr(map+0x188)+index*32)):NULL;
    if(!reveal && enabled(4))reveal=entry && (entry->flags&1);
    if(reveal)c->x[8]=c->x[9]=1;
    // Added outdoor records have no ordinary map marker to restore when both applicable cheats are off.
    else if(entry && (entry->flags&2))c->x[8]=c->x[9]=0;
}
void world_facility_position_host(Cpu *c) {
    uint64_t map=c->x[19],index=c->x[24];
    rt_call(c,MAIN+0x20a3e0,1,(uint64_t)c->x[0]);
    if(!context(map))return;
    const WorldExtra *entry=facility(map,index);
    if(!entry || enabled(entry->category) || index>=word(map+0xe0))return;
    uint64_t element=ptr(ptr(map+0xe8)+index*8),parent=ptr(map+0x58);
    if(!element || !parent)return;
    // Positioning needs the native parent. Detach only after its transform has finished.
    Cpu saved=*c;
    rt_call(c,MAIN+0x2095e0,2,element,parent);
    *c=saved;
}
void world_facility_style_host(Cpu *c) {
    if(context(c->x[19])) {
        const WorldExtra *entry=facility(c->x[19],c->x[24]);
        if(entry && artwork_ready)ret_s(c,39.f+2.f*(entry->category-1)+(completed(entry)?1.f:0.f));
    }
    rt_call(c,MAIN+0x2d2d90,1,(uint64_t)c->x[0]);
}
