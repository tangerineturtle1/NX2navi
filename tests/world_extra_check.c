// Independent world toggles, conservative completion and transparent native calls.
#include <assert.h>
#include <stdarg.h>
#include "../runtime/world_extra.c"
static unsigned char map[0x500],records[0xc0*5],pool[40],element[0x80],manager[0x40],root[8];
static unsigned char hashes[22584*8],values[11292*24];
static int switches[5],styles,visible;
static float frame;
static uint64_t addr(void *p) { return (uint64_t)(uintptr_t)p; }
static void q(void *p,uint64_t v) { memcpy(p,&v,8); }
static void w(void *p,unsigned v) { memcpy(p,&v,4); }
const char *cfg_get(const char *s) { (void)s;return NULL; }
int cfg_bool(const char *s) {
    const char *keys[]={"NX2NAVI_WORLD_CHESTS","NX2NAVI_WORLD_GROTTOS","NX2NAVI_WORLD_BEAN_PLOTS","NX2NAVI_WORLD_FAIRIES","NX2NAVI_WORLD_UPGRADES"};
    for(int i=0;i<5;i++)if(!strcmp(s,keys[i]))return switches[i];
    assert(0);return 0;
}
void rt_log(const char *s,...) { (void)s; }
int plat_mem_read(uint64_t at,void *out,size_t n) {
    if(at==MAIN+0x4c7e180) { uint64_t p=addr(root);assert(n==8);memcpy(out,&p,8);return 1; }
    const void *regions[]={map,records,pool,element,manager,root,hashes,values};
    size_t sizes[]={sizeof map,sizeof records,sizeof pool,sizeof element,sizeof manager,sizeof root,sizeof hashes,sizeof values};
    for(unsigned i=0;i<8;i++)if(at>=addr((void *)regions[i]) && n<=sizes[i] && at-addr((void *)regions[i])<=sizes[i]-n) {
        memcpy(out,(void *)(uintptr_t)at,n);return 1;
    }
    return 0;
}
uint64_t rt_call(Cpu *c,uint64_t fn,int n,...) {
    va_list ap;va_start(ap,n);uint64_t arg=va_arg(ap,uint64_t);va_end(ap);
    if(fn==MAIN+0x2d2d90) { assert(n==1 && arg==0x123456);styles++;frame=arg_s(c,0);c->x[8]=0x1234;return 0; }
    if(fn==MAIN+0x20a3e0) { assert(n==1);visible=1;c->x[8]=0x1234;return 0; }
    assert(fn==MAIN+0x2095e0 && n==2 && arg==addr(element));
    visible=0;memset(c,0xa5,sizeof *c);return 0;
}
int main(void) {
    q(map,MAIN+0x4be19c8);w(map+0x370,5);w(map+0x1e0,5);q(map+0x1e8,addr(records));
    w(map+0xe0,5);q(map+0xe8,addr(pool));q(map+0x58,0x9988);q(element+0x48,0x123456);
    ready=artwork_ready=1;entry_count=5;
    for(unsigned i=0;i<5;i++) { entries[i]=(WorldExtra){.id=100+i,.category=i<4?i+1:4,.flags=i==4};q(records+i*0xc0,100+i);q(pool+i*8,addr(element)); }
    Cpu c={0};c.x[19]=addr(map);
    for(unsigned i=0;i<4;i++)for(unsigned mask=0;mask<32;mask++) {
        for(unsigned j=0;j<5;j++)switches[j]=(mask>>j)&1;
        c.x[24]=i;c.x[0]=addr(element);c.x[1]=0x9988;Cpu gate=c;gate.x[8]=0x1234;world_facility_position_host(&c);assert(visible==switches[i+1]);assert(!memcmp(&gate,&c,sizeof c));
        c.x[0]=0x123456;ret_s(&c,11.f);Cpu expected=c;ret_s(&expected,39.f+2*i);expected.x[8]=0x1234;
        world_facility_style_host(&c);assert(!memcmp(&c,&expected,sizeof c));assert(frame==39.f+2*i);
    }
    q(records,999);c.x[24]=0;c.x[0]=addr(element);world_facility_position_host(&c);assert(visible);
    c.x[0]=0x123456;ret_s(&c,7.f);world_facility_style_host(&c);assert(frame==7.f);q(records,100);

    w(map+0x358,1);w(map+0x180,1);q(map+0x188,addr(records));
    entries[5]=(WorldExtra){.id=200,.category=4,.flags=2};entries[6]=(WorldExtra){.id=201,.category=4,.flags=3};entry_count=7;
    for(int chest=0;chest<2;chest++)for(int upgrades=0;upgrades<2;upgrades++)for(int kind=0;kind<4;kind++)for(int native=0;native<2;native++) {
        const uint64_t ids[]={100,104,200,201};switches[0]=chest;switches[4]=upgrades;q(records,ids[kind]);c.x[23]=0;c.x[8]=c.x[9]=native;
        world_chests_master_host(&c);
        unsigned expected=chest || (upgrades && (kind&1))?1:kind>=2?0:(unsigned)native;
        assert(c.x[8]==expected && c.x[9]==expected);
    }
    q(root,addr(manager));w(manager+0x10,11292);w(manager+0x2c,22584);q(manager+0x18,addr(values));q(manager+0x20,addr(hashes));
    unsigned hash=12345,index=67;w(hashes+(hash%22584)*8,hash);w(hashes+(hash%22584)*8+4,index);q(values+24*index,MAIN+0x1000);
    WorldExtra reward={.conditions=1,.hash0=hash,.index0=index};
    assert(!completed(&reward));values[24*index+22]=1;assert(completed(&reward));
    values[24*index+22]=3;assert(completed(&reward));
    reward.index0++;assert(!completed(&reward));reward.index0=index;reward.conditions=2;assert(!completed(&reward));
    reward.hash1=hash;reward.index1=index;assert(completed(&reward));w(manager+0x2c,0);assert(!completed(&reward));
    assert(styles==129);
    q(records,100);artwork_ready=0;switches[1]=1;c.x[24]=0;c.x[0]=addr(element);c.x[1]=1;
    world_facility_position_host(&c);assert(!visible);
    c.x[0]=0x123456;ret_s(&c,11.f);world_facility_style_host(&c);assert(frame==11.f);
    puts("world_extra_check: passed (all toggles independent, native call preservation, completion guards, resource conflicts)");
}
