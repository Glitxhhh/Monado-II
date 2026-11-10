// Both authored adapters execute together over a synthetic native resource.
// Reuse the full UI ownership fixture rather than duplicating its resource and
// native-constructor contracts. No game-derived resource or save is present.
#define main stats_ui_contract_main
#define hle_alloc stats_ui_contract_alloc
#define guest_call stats_ui_contract_guest_call
#define QUICKSTEP_STATS_INTEGRATION
#include "test_quickstep_stats_ui.c"
#undef guest_call
#undef hle_alloc
#undef main
#include "hle/quickstep_stats.h"
#include "hle/quickstep_slot.h"
#define SUMMARY_OLD (B+0xe00000)
#define SUMMARY_NEW (B+0xe01000)
#define ITEM_CACHE (B+0xe10000)
#define ITEM_TABLE (B+0xe12000)
#define ITEM_FIELD (B+0xe14000)
#define ITEM_RAW (B+0xe15000)
#define ITEM_MESSAGE (B+0xe16000)
#define ITEM_CACHE_DATA (B+0xe18000)
#define ITEM_LIST (B+0xe20000)
static QuickstepGem integration_gem;
static uint64_t integration_allocation;
void *hle_alloc(uint64_t bytes) {
    if(bytes==0x600)return stats_ui_contract_alloc(bytes);
    if(!integration_allocation)integration_allocation=MEM+0x1000;
    assert(bytes==0x80);uint64_t p=integration_allocation;integration_allocation+=bytes;return(void*)p;
}
bool quickstep_native_picker_active(uint64_t seq) {assert(seq==SEQ);return false;}
bool quickstep_slot_get(Cpu*c,uint64_t context,uint32_t ch,QuickstepGem*out) {
    assert(c&&context==B+0x9000&&ch==1);*out=integration_gem;return true;
}
bool quickstep_slot_reference_matches(uint64_t context,uint32_t ch,const QuickstepGem*item,uint64_t*epoch) {
    if(!g_settings.quickstep_slot||context!=B+0x9000||ch!=1||!item||!epoch
       ||(*epoch&&*epoch!=1)||item->uid!=integration_gem.uid
       ||memcmp(item->fingerprint,integration_gem.fingerprint,32))return false;
    *epoch=1;return true;
}
void guest_call(Cpu*c,uint64_t fn) {
    uint64_t a=c->x[0],b=c->x[1],d=c->x[2],e=c->x[3],f=c->x[4],x0=0,x1=0;
    switch(fn) {
    case 0x70000a8290ull:
        assert(a==B+0x9000&&b==integration_gem.uid&&!d&&e==f+4);
        memset((void*)f,0,0x2c);ST16(f,77);ST16(f+0xa,3);ST16(f+0xe,22);ST16(f+0x16,25);x0=1;break;
    case 0x70000ae8e0ull:assert(a==B+0x9000);x0=ITEM_CACHE;break;
    case 0x700006b270ull:assert(a==ITEM_CACHE&&b==0x14);x0=ITEM_TABLE;break;
    case 0x7000062310ull:assert(a==ITEM_TABLE&&!strcmp((char*)b,"name"));x0=ITEM_FIELD;break;
    case 0x7000069320ull:assert(a==ITEM_CACHE_DATA&&b==2);x0=ITEM_RAW;break;
    case 0x7000510630ull:assert(a==ITEM_RAW&&b==ITEM_FIELD&&d==77&&e==2);x0=229;break;
    case 0x7000062620ull:assert(a==ITEM_TABLE&&b==229);x0=ITEM_MESSAGE;x1=229;break;
    default:stats_ui_contract_guest_call(c,fn);return;
    }
    memset(c,0xbd,sizeof*c);c->x[0]=x0;c->x[1]=x1;
}
int main(void) {
    // Also execute the reused adapter's alias, destructor and OFF controls.
    assert(stats_ui_contract_main()==0);
    initialize();g_settings.quickstep_slot=true;
    memset(&integration_gem,0,sizeof integration_gem);integration_gem.uid=0x30001;
    integration_gem.character=1;integration_gem.rank=6;integration_gem.value=22;
    memset(integration_gem.fingerprint,0x2b,32);
    ST64(SEQ+0x4c10,ITEM_LIST);ST8(ITEM_LIST+0x19,0);
    ST64(ITEM_TABLE+8,ITEM_CACHE_DATA);ST32(ITEM_TABLE+0x10,2);
    memset((void*)SUMMARY_OLD,0,0x178);memset((void*)SUMMARY_NEW,0,0x178);
    for(unsigned i=0;i<8;++i)for(unsigned side=0;side<2;++side) {
        uint64_t r=(side?SUMMARY_NEW:SUMMARY_OLD)+0x30+i*0x28;
        ST16(r,10+i);ST64(r+8,ITEM_MESSAGE);ST16(r+0x10,2);
        ST32(r+0x18,i+1);ST32(r+0x1c,25);ST8(r+0x20,3);ST8(r+0x21,2);
    }
    ST64(SUMMARY_OLD+0x170,8);ST64(SUMMARY_NEW+0x170,8);
    uint8_t original_summaries[2*0x178];memcpy(original_summaries,(void*)SUMMARY_OLD,0x178);
    memcpy(original_summaries+0x178,(void*)SUMMARY_NEW,0x178);
    unsigned prior_appends=appends;
    Cpu c=cpu();c.x[0]=PANEL;c.x[1]=1;c.x[2]=SUMMARY_OLD;c.x[3]=SUMMARY_NEW;c.x[4]=0;
    Cpu before=c;quickstep_stats_begin(&c);assert(!memcmp(&before,&c,sizeof c));
    assert(appends==prior_appends+1&&LD64(PANEL+0x1948)==20&&LD32(ROW(19)+0x5c)==19);
    assert(!memcmp(original_rows,(void*)ROW(0),sizeof original_rows));
    c.sp=STACK-0x2b0;c.x[19]=PANEL;c.x[21]=SUMMARY_NEW;c.x[22]=SUMMARY_OLD;c.x[8]=8;
    quickstep_stats_count(&c);assert(c.x[8]==9);
    for(unsigned i=0;i<9;++i) {
        c.x[20]=i;assert(quickstep_stats_row(&c)==1);
        if(i<8)assert(!memcmp((void*)(c.sp+0x150),original_summaries+0x178+0x30+i*0x28,0x28));
        else {
            assert(LD16(c.sp+0x150)==77&&LD32(c.sp+0x168)==22&&LD32(c.sp+0x16c)==25);
            assert(LD64(c.sp+0x158)==ITEM_MESSAGE&&LD16(c.sp+0x160)==229);
            quickstep_stats_compare(&c);assert(c.x[24]==0&&c.x[26]==0);
        }
    }
    assert(!memcmp(original_summaries,(void*)SUMMARY_OLD,0x178));
    assert(!memcmp(original_summaries+0x178,(void*)SUMMARY_NEW,0x178));
    // The selected-ARM loop probe separately proves the native renderer calls
    // row19. Its native row-update boundary marks this physical row active.
    ST8(ROW(19)+0x34,1);c.x[0]=UI;before=c;quickstep_stats_ui_tick(&c);
    assert(!memcmp(&before,&c,sizeof c)&&LD32(NODE(CLONE,0)+0xb0)&&LD32(NODE(NESTED,20)+0xb0));
    quickstep_stats_end(&c);quickstep_stats_ui_layout(&c,PANEL,false);
    for(unsigned i=0;i<8;++i)assert(lf(NODE(SOURCE,13+i)+0xd4)==0.f);
    g_settings.quickstep_slot=false;close_panel(&c);unchanged();
    assert(!memcmp(original_summaries,(void*)SUMMARY_OLD,0x178));
    assert(!memcmp(original_summaries+0x178,(void*)SUMMARY_NEW,0x178));
    puts("combined native ninth-row and fixed-summary contracts: PASS");return 0;
}
