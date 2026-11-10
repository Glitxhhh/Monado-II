// Authored dense native-resource fixture. No game bytes, fonts or renderer.
#include "hle.h"
#include "settings.h"
#include "hle/quickstep_stats_ui.h"
#include "hle/quickstep_ui_memory.h"
#include <assert.h>
#include <math.h>
#include <sys/mman.h>
#define B 0x7200000000ull
#define SEQ (B+0x10000)
#define UI (SEQ+0x18d8)
#define PANEL (B+0x30000)
#define ORIGINAL (B+0x100000)
#define SOURCE (B+0x200000)
#define NESTED (B+0x400000)
#define CLONE NESTED
#define FACTORY (B+0x800000)
#define OLD_ASSET (B+0x900000)
#define NEW_ASSET (B+0x901000)
#define ASSET_CLASS 0x7000b673a8ull
#define ASSET_VIEW (B+0x902000)
#define ASSET_OBJECT (B+0x903000)
#define MEM fixture_memory
#define STACK (B+0xc00000)
#define OTHER(n) (B+0x1000000+(n)*0x1000)
#define NODE(root,i) ((root)+0x10000+(i)*0x400)
#define PART(root,i) ((root)+0x50000+(i)*0x400)
#define RESOURCE(root) ((root)+0x8000)
#define ROW(i) (PANEL+0x148+(i)*0xc0)
Settings g_settings;
int g_rt_unwind;
static uint64_t fixture_memory=B+0x9ff000;
static bool alias_main,alias_nested,alias_text,lose_owner,wrong_resource,wrong_cache,owner=true;
static bool bad_return,bad_count,deny_budget,alias_parent,reenter_release;
static unsigned bad_asset;
static unsigned retire_parent;
static bool retired_lower;
static unsigned deny_budget_at;
static unsigned helpers,mutations,creates,deletes,appends,destructs,allocations;
static unsigned animation_sync,loader_creates,loader_deletes,budget_checks,pending_updates,refreshes;
static uint64_t invalidate_fn;
static uint64_t invalidate_source_fn;
static unsigned invalidate_source;
static uint8_t tail[0xc0],original_rows[19*0xc0];
void rt_fatal_trace(void){abort();}
void rt_unwind_point(void){}
void *hle_alloc(uint64_t n){assert(n==0x600);++allocations;fixture_memory+=0x1000;return(void*)MEM;}
static void sf(uint64_t p,float f){memcpy((void*)p,&f,4);}
static float lf(uint64_t p){float f;memcpy(&f,(void*)p,4);return f;}
bool quickstep_ui_memory_allow(Cpu*c,uint64_t bytes,const char*consumer){
 assert(c&&(bytes==4ull*1024*1024||bytes==1)&&consumer);++budget_checks;return !deny_budget&&budget_checks!=deny_budget_at;
}
#ifndef QUICKSTEP_STATS_INTEGRATION
void quickstep_stats_refresh(Cpu*c,uint64_t seq){assert(c&&seq==SEQ);++refreshes;}
bool quickstep_native_picker_active(uint64_t seq){assert(seq==SEQ);return false;}
#endif
bool quickstep_native_stats_context(uint64_t p,uint32_t ch,uint64_t*s,uint64_t*c,uint64_t*g){
 if(!owner||!g_settings.quickstep_slot||p!=PANEL||ch!=1)return false;
 if(s)*s=SEQ;if(c)*c=B+0x9000;if(g)*g=1;return true;
}
static void make_tree(uint64_t root,bool nested,bool old){
 memset((void*)root,0,0x9000);ST64(root,0x7000b66c18ull);ST32(root+0x18,1);
 ST64(root+0x38,NODE(root,0));ST64(root+0x68,root+0x1000);ST64(root+0x70,nested?193:3);
 ST64(root+0x78,root+0x3000);ST64(root+0x80,nested?480:2);ST64(root+0x30,OLD_ASSET);
 unsigned n=nested?193:3;
 for(unsigned i=0;i<n;++i){
  uint64_t node=NODE(root,i);memset((void*)node,0,0x400);ST64(root+0x1000+i*32,node);
  ST32(root+0x1014+i*32,i==0?UINT32_MAX:nested&&i==21?20:0);
  ST32(root+0x1010+i*32,nested?(i*3<480?i*3:480):(i<2?0:1));
  sf(node+0xdc,1.f);sf(node+0xe0,1.f);ST32(node+0xb0,1);ST32(node+0x3c4,nested?(i<160?3:0):2);
  if(nested&&i>=13&&i<=20)sf(node+0x190,(400.f+(i-13)*24.f)/720.f);
 }
 ST64(NODE(root,0)+0xc0,RESOURCE(root));ST32(RESOURCE(root)+0x3c,0x40);ST32(RESOURCE(root)+0x44,n);
 if(nested){for(unsigned i=0;i<480;++i){uint64_t part=PART(root,i);memset((void*)part,0,0x400);
  ST64(root+0x3000+i*16,part);ST32(root+0x3008+i*16,i/3);ST64(part,0x7000b60000ull);ST64(part+0x38,part+0x40);ST32(part+0x40+0xb0,1);}}
 else{ST64(root+0x3000,old?SOURCE:NESTED);ST32(root+0x3008,1);ST64(root+0x3010,OTHER(old?8:9));ST32(root+0x3018,2);
  ST64(OTHER(old?8:9)+0x38,OTHER(old?8:9)+0x100);ST32(OTHER(old?8:9)+0x100+0xb0,1);}
}
static void initialize(void){
 memset((void*)SEQ,0,0x8a18);memset((void*)PANEL,0,0x1b00);owner=true;retired_lower=false;
 ST64(SEQ+0x1c48,UI);ST64(SEQ+0x8440,PANEL);ST32(SEQ+0x18b8,1);
 ST64(UI,0x7000b460d8ull);ST64(UI+8,ORIGINAL);ST8(UI+0x21,1);ST8(UI+0x29,1);
 ST64(SEQ+0x4c10,OTHER(30));ST8(OTHER(30)+0x19,0);
 ST64(SEQ+0x28,FACTORY);ST64(FACTORY,0x7000b432a8ull);ST64(0x7000b432a8ull+0x20,0x70001b7a30ull);
 ST64(FACTORY+8,OTHER(20));ST64(FACTORY+0x18,OTHER(21));ST64(FACTORY+0x20,OTHER(22));ST64(FACTORY+0x38,OTHER(23));
 ST64(OLD_ASSET,ASSET_CLASS);ST64(NEW_ASSET,bad_asset==1?ASSET_CLASS+0x40:ASSET_CLASS);
 ST64(ASSET_CLASS+0x18,bad_asset==2?0x700058a800ull:0x700058a820ull);
 ST64(ASSET_CLASS+0x20,0x700058a800ull);
 ST64(ASSET_VIEW+8,ASSET_OBJECT);ST64(ASSET_OBJECT+0x250,RESOURCE(SOURCE));
 make_tree(ORIGINAL,false,true);make_tree(SOURCE,true,true);
 ST64(NODE(ORIGINAL,1)+0x280,OTHER(12));ST64(NODE(SOURCE,0)+0x280,OTHER(13));
 ST64(PANEL,0x7000b477f0ull);ST64(PANEL+0x1948,19);ST32(PANEL+0x1950,19);ST32(PANEL+0x20,19);
 for(unsigned i=0;i<19;++i){uint64_t r=ROW(i);memset((void*)r,0xa1,0xc0);ST64(r,0x7000b478a0ull);ST64(r+8,SOURCE);
  ST32(r+0x10,2+i);ST64(r+0x18,PANEL);ST32(r+0x30,i);ST32(r+0x5c,i);}
 memset((void*)ROW(19),0xd3,0xc0);memcpy(tail,(void*)ROW(19),0xc0);memcpy(original_rows,(void*)ROW(0),sizeof original_rows);
}
void guest_call(Cpu*c,uint64_t fn){
 uint64_t a=c->x[0],b=c->x[1],d=c->x[2],ret=0;float x,y;bool fp_result=false;memcpy(&x,&c->v[0],4);memcpy(&y,&c->v[1],4);++helpers;
 switch(fn){
 case 0x7000582fd0ull:
  assert(a==MEM+0x100&&b==MEM+0x300&&!strcmp((char*)LD64(b+0x20),"mnu004_cont19"));
  assert(LD64(b)==OTHER(20)&&LD64(b+8)==OTHER(21)&&LD64(b+0x10)==OTHER(21)&&LD64(b+0x18)==OTHER(22)&&LD64(b+0x28)==OTHER(23));
  ++loader_creates;ST64(a,0x7000b671f0ull);ST64(a+0x28,NEW_ASSET);ST32(a+0x78,0);break;
 case 0x7000584d60ull:
  assert(a==MEM+0x100);if(pending_updates)--pending_updates;else ST32(a+0x78,0x7f);
  if(reenter_release){reenter_release=false;Cpu saved=*c;c->x[0]=PANEL;quickstep_stats_ui_release_panel(c);*c=saved;}
  break;
 case 0x70001b7ab0ull:assert(a==MEM+0x100);ret=LD32(a+0x78)==0x7f;break;
 case 0x700058a820ull:assert(a==NEW_ASSET);ret=bad_asset==3?0:1;break;
 case 0x700058a800ull:assert(a==NEW_ASSET);ST64(ASSET_OBJECT+0x250,RESOURCE(wrong_cache?ORIGINAL:SOURCE));ret=bad_asset==4?0:ASSET_VIEW;break;
 case 0x7000583240ull:assert(!retired_lower&&a==MEM+0x100&&LD64(a)==0x7000b671f0ull);++loader_deletes;ST64(a,0x7000b670b0ull);break;
 case 0x70005859a0ull:
  assert(a==MEM+0x100&&LD64(b)==ORIGINAL&&LD32(b+8)==1&&LD32(b+12)==0);++creates;
  make_tree(NESTED,true,false);ST64(NODE(NESTED,0)+0xc0,RESOURCE(SOURCE));
  if(alias_text)ST64(NESTED+0x3000,LD64(SOURCE+0x3000));
  if(alias_nested)ST64(NESTED+0x1000+30*32,LD64(SOURCE+0x1000+30*32));
  if(alias_parent)ST64(NESTED+0x38,NODE(ORIGINAL,1));
  ST64(c->x[8],alias_main?SOURCE:NESTED);
  if(retire_parent){
   unsigned route=retire_parent;retire_parent=0;Cpu saved=*c;c->x[0]=route==1?UI:SEQ;
   if(route==1)quickstep_stats_ui_release_ui(c);else quickstep_stats_ui_release_sequence(c);*c=saved;
   // Model completed original wrapper teardown and native scene disposal.
   ST64(ORIGINAL,0x7000b66bd0ull);ST64(SEQ+0x1c48,0);
   memset((void*)NODE(NESTED,0),0xd7,0x400);retired_lower=true;
  }
  break;
 case 0x700057a770ull:
  assert(a==ORIGINAL&&!strcmp((char*)b,"InfoStatus_mc/rect_cont19_mc"));ret=wrong_resource?ORIGINAL:SOURCE;break;
 case 0x700057aeb0ull:assert(!strcmp((char*)b,"InfoStatusAnim_mc/StatusEffect_08_mc"));ret=NODE(a,20);break;
 case 0x700074ea30ull:assert(!retired_lower);++mutations;ST32(a+0xb0,b?1:0);break;
 case 0x700074e6a0ull:++mutations;assert(isfinite(x)&&isfinite(y));sf(a+0xd0,x);sf(a+0xd4,y);break;
 case 0x70007499d0ull:assert(LD64(a)==NODE(SOURCE,0));ret=2;break;
 case 0x7000749a10ull:assert(LD64(a)==NODE(SOURCE,0));fp_result=true;break;
 case 0x7000749c00ull:assert(LD64(a)==NODE(NESTED,0)&&b==2);++animation_sync;break;
 case 0x7000749a50ull:assert(LD64(a)==NODE(NESTED,0)&&x==17.5f);break;
 case 0x70001b3f20ull:ST64(a,b);ST32(a+8,(uint32_t)d);break;
 case 0x700023ec20ull:
  assert(a==PANEL&&LD64(a+0x1948)==19&&LD64(b)==NESTED&&LD32(b+8)==20);++appends;
  memset((void*)ROW(19),0,0xc0);ST64(ROW(19),0x7000b478a0ull);ST64(ROW(19)+8,NESTED);ST32(ROW(19)+0x10,20);
  ST64(ROW(19)+0x18,PANEL);ST32(ROW(19)+0x30,19);ST64(PANEL+0x1948,bad_count?21:20);ST32(PANEL+0x1950,20);ST32(PANEL+0x20,20);ret=ROW(19)+(bad_return?8:0);break;
 case 0x700023d6f0ull:assert(a==ROW(19)&&LD64(a+8)==NESTED&&LD64(PANEL+0x1948)==20);++destructs;memset((void*)a,0xdd,0xc0);break;
 case 0x700057a590ull:assert(!retired_lower&&a==NESTED&&LD32(NESTED+0x18)==0);++deletes;break;
 case 0x700023b150ull:assert(a==B+0x9000&&b==1&&c->x[4]==PANEL);++refreshes;break;
 default:fprintf(stderr,"unexpected native stats UI helper %llx\n",(unsigned long long)fn);abort();
 }
 if(lose_owner&&fn==invalidate_fn){lose_owner=false;ST64(SEQ+0x8440,PANEL+0x2000);}
 if(invalidate_source&&fn==invalidate_source_fn){unsigned kind=invalidate_source;invalidate_source=0;
  if(kind==1)ST64(SOURCE+0x38,GUEST_HI-0x10);
  else if(kind==2)ST32(RESOURCE(SOURCE)+0x3c,UINT32_MAX);
  else ST64(ORIGINAL+0x3000,OTHER(40));}
 memset(c,0xb8,sizeof*c);c->x[0]=ret;if(fp_result){float time=17.5f;memcpy(&c->v[0],&time,4);}
}
static Cpu cpu(void){Cpu c;memset(&c,0x5a,sizeof c);c.sp=STACK;return c;}
static bool ensure(Cpu*c){Cpu before=*c;bool r=quickstep_stats_ui_ensure(c,PANEL,1);assert(!memcmp(c,&before,sizeof*c));return r;}
static void close_panel(Cpu*c){c->x[0]=PANEL;Cpu before=*c;quickstep_stats_ui_release_panel(c);assert(!memcmp(c,&before,sizeof*c));}
static void unchanged(void){assert(!memcmp(original_rows,(void*)ROW(0),sizeof original_rows));assert(!memcmp(tail,(void*)ROW(19),sizeof tail));assert(LD64(PANEL+0x1948)==19&&LD32(PANEL+0x1950)==19&&LD32(PANEL+0x20)==19);}
int main(void){
 assert(mmap((void*)B,0x2000000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)B);
 assert(mmap((void*)0x7000b40000ull,0x30000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)0x7000b40000ull);
 initialize();Cpu c=cpu();g_settings.quickstep_slot=false;assert(!ensure(&c)&&!helpers&&!allocations);
 g_settings.quickstep_slot=true;assert(ensure(&c)&&appends==1&&creates==1&&loader_creates==1);
 assert(!memcmp(original_rows,(void*)ROW(0),sizeof original_rows));assert(LD64(PANEL+0x1948)==20&&LD32(ROW(19)+0x5c)==19);
 assert(!LD32(NODE(NESTED,20)+0xb0)&&LD32(NODE(NESTED,21)+0xb0)&&!LD32(NODE(NESTED,22)+0xb0));
 Cpu before=c;quickstep_stats_ui_layout(&c,PANEL,true);assert(!memcmp(&before,&c,sizeof c));
 assert(animation_sync==1&&LD32(NODE(NESTED,0)+0xb0)&&LD32(NODE(NESTED,20)+0xb0));
 for(unsigned refresh=0;refresh<8;++refresh){
  for(unsigned i=0;i<8;++i)sf(NODE(SOURCE,13+i)+0x190,(400.f+i*21.f)/720.f);
  quickstep_stats_ui_layout(&c,PANEL,false);quickstep_stats_ui_layout(&c,PANEL,true);
  for(unsigned i=0;i<8;++i){assert(fabsf(lf(NODE(SOURCE,13+i)+0xd4)+i*3.f)<.001f);assert(lf(NODE(SOURCE,13+i)+0xdc)==1.f);}
 }
 // Real attachment inherits parent transforms; this adapter never sets or
 // copies the original parent scale/matrix. Entry zoom stays native1x locally.
 for(unsigned zoom=0;zoom<3;++zoom){sf(NODE(ORIGINAL,1)+0xdc,.5f+zoom*.25f);quickstep_stats_ui_layout(&c,PANEL,true);assert(lf(NODE(NESTED,0)+0xdc)==1.f);}
 ST32(NODE(SOURCE,0)+0xb0,0);quickstep_stats_ui_layout(&c,PANEL,true);assert(!LD32(NODE(NESTED,0)+0xb0));
 ST32(NODE(SOURCE,0)+0xb0,1);ST32(NODE(ORIGINAL,1)+0xb0,0);quickstep_stats_ui_layout(&c,PANEL,true);assert(!LD32(NODE(NESTED,0)+0xb0));
 ST32(NODE(ORIGINAL,1)+0xb0,1);quickstep_stats_ui_layout(&c,PANEL,true);assert(LD32(NODE(NESTED,0)+0xb0));
 ST8(ROW(19)+0x34,1);c.x[0]=UI;before=c;quickstep_stats_ui_tick(&c);assert(!memcmp(&before,&c,sizeof c));
 ST8(UI+0x29,0);quickstep_stats_ui_tick(&c);assert(!LD32(NODE(NESTED,0)+0xb0));ST8(UI+0x29,1);
 g_settings.quickstep_slot=false;close_panel(&c);unchanged();assert(destructs==1&&deletes==1&&loader_deletes==1);
 close_panel(&c);assert(destructs==1&&deletes==1&&loader_deletes==1);
 // A linked wrapper's asset ownership field is not the selected loader's
 // cache type. Both variants preserve the exact same native raw resource.
 for(unsigned source_asset=0;source_asset<2;++source_asset){initialize();g_settings.quickstep_slot=true;
  ST64(SOURCE+0x30,source_asset?OLD_ASSET:0);ST64(OLD_ASSET,0x7000b66000ull);
  assert(ensure(&c));assert(LD64(PANEL+0x1948)==20);close_panel(&c);unchanged();}
 // Exact new cache class/methods, readiness and payload remain mandatory.
 for(bad_asset=1;bad_asset<=4;++bad_asset){initialize();g_settings.quickstep_slot=true;
  unsigned prior_creates=creates,prior_appends=appends,prior_mutations=mutations;
  assert(!ensure(&c)&&creates==prior_creates&&appends==prior_appends&&mutations==prior_mutations);
  unchanged();close_panel(&c);}
 bad_asset=0;
 for(unsigned scenario=0;scenario<5;++scenario){initialize();g_settings.quickstep_slot=true;
  alias_main=scenario==0;alias_nested=scenario==1;alias_text=scenario==2;wrong_resource=scenario==3;wrong_cache=scenario==4;
  unsigned old_mutations=mutations,old_appends=appends;assert(!ensure(&c)&&appends==old_appends);
  if(scenario==0||scenario>=3)assert(mutations==old_mutations);
  for(unsigned i=0;i<193;++i)assert(LD32(NODE(SOURCE,i)+0xb0)==1);
  unchanged();close_panel(&c);alias_main=alias_nested=alias_text=wrong_resource=wrong_cache=false;
 }
 // Headroom refusal never calls a constructor; it is not reported as success.
 initialize();deny_budget=true;unsigned old_creates=loader_creates;assert(!ensure(&c)&&loader_creates==old_creates);unchanged();close_panel(&c);deny_budget=false;
 for(unsigned step=2;step<=3;++step){initialize();deny_budget_at=budget_checks+step;unsigned old_instances=creates;
  assert(!ensure(&c)&&creates==old_instances);unchanged();close_panel(&c);deny_budget_at=0;}
 initialize();alias_parent=true;unsigned old_mutations=mutations;assert(!ensure(&c)&&mutations==old_mutations);
 unchanged();close_panel(&c);alias_parent=false;
 // Native dependencies advance once per tick, without repeating construction.
 initialize();pending_updates=2;unsigned old_refresh=refreshes;old_creates=loader_creates;assert(!ensure(&c));unchanged();
 c.x[0]=UI;quickstep_stats_ui_tick(&c);unchanged();ST8(OTHER(30)+0x19,1);quickstep_stats_ui_tick(&c);
 assert(LD64(PANEL+0x1948)==20&&loader_creates==old_creates+1&&refreshes==old_refresh);
 quickstep_stats_ui_tick(&c);assert(refreshes==old_refresh);ST8(OTHER(30)+0x19,0);quickstep_stats_ui_tick(&c);
 assert(refreshes==old_refresh+1);close_panel(&c);unchanged();
 initialize();pending_updates=2;assert(!ensure(&c));g_settings.quickstep_slot=false;c.x[0]=UI;quickstep_stats_ui_tick(&c);unchanged();pending_updates=0;
 // A reentrant panel close during a pending update defers its dtor until the
 // helper has returned, then destroys the still-owned loader exactly once.
 initialize();g_settings.quickstep_slot=true;pending_updates=2;assert(!ensure(&c));
 unsigned prior_loader_deletes=loader_deletes;reenter_release=true;c.x[0]=UI;quickstep_stats_ui_tick(&c);
 assert(loader_deletes==prior_loader_deletes+1&&LD64(MEM+0x100)==0x7000b670b0ull);unchanged();close_panel(&c);pending_updates=0;
 // Actual parent lifetime retirement during attachment invalidates lower
 // storage. No visibility call, upper/lower dtor or loader teardown may touch
 // it afterward. A new owner receives fresh scratch, preserving all retained
 // loader/config/name bytes instead of recycling the ambiguous capsule.
 for(unsigned route=1;route<=2;++route){initialize();g_settings.quickstep_slot=true;retire_parent=route;
  unsigned prior_deletes=deletes,prior_loaders=loader_deletes;assert(!ensure(&c));
  assert(deletes==prior_deletes&&loader_deletes==prior_loaders);unchanged();
  for(unsigned i=0;i<0x400;++i)assert(LD8(NODE(NESTED,0)+i)==0xd7);
  uint64_t retained_memory=MEM;uint8_t retained_bytes[0x600];memcpy(retained_bytes,(void*)retained_memory,sizeof retained_bytes);
  close_panel(&c);initialize();unsigned prior_allocations=allocations;assert(ensure(&c));
  assert(allocations==prior_allocations+1&&MEM!=retained_memory);
  assert(!memcmp(retained_bytes,(void*)retained_memory,sizeof retained_bytes));close_panel(&c);unchanged();
 }
 // Lost owner after constructor or attachment must not append to an old panel.
 for(unsigned scenario=0;scenario<2;++scenario){initialize();g_settings.quickstep_slot=true;lose_owner=true;invalidate_fn=scenario?0x70005859a0ull:0x7000582fd0ull;
  unsigned old_appends=appends;assert(!ensure(&c)&&appends==old_appends);unchanged();close_panel(&c);}
 for(unsigned scenario=0;scenario<2;++scenario){initialize();g_settings.quickstep_slot=true;lose_owner=true;
  invalidate_fn=scenario?0x700058a800ull:0x700058a820ull;unsigned prior_creates=creates;
  assert(!ensure(&c)&&creates==prior_creates);unchanged();close_panel(&c);}
 // Every yielding lookup/loader/cache boundary must reject a retired linked
 // source before following its lower/table or constructing an extra instance.
 const uint64_t source_boundaries[]={0x700057a770ull,0x700057aeb0ull,0x7000582fd0ull,
  0x7000584d60ull,0x70001b7ab0ull,0x700058a820ull,0x700058a800ull};
 for(unsigned site=0;site<sizeof source_boundaries/sizeof*source_boundaries;++site)
  for(unsigned kind=1;kind<=3;++kind){initialize();g_settings.quickstep_slot=true;
   invalidate_source_fn=source_boundaries[site];invalidate_source=kind;
   unsigned prior_creates=creates,prior_appends=appends;
   assert(!ensure(&c)&&creates==prior_creates&&appends==prior_appends);
   unchanged();close_panel(&c);}
 initialize();ST64(PANEL+0x1948,20);assert(!ensure(&c)&&LD64(PANEL+0x1948)==20);close_panel(&c);
 initialize();ST32(ROW(10)+0x5c,25);assert(!ensure(&c)&&LD32(ROW(10)+0x5c)==25);close_panel(&c);
 initialize();lose_owner=true;invalidate_fn=0x700023ec20ull;unsigned old_deletes=deletes;
 assert(!ensure(&c)&&LD64(PANEL+0x1948)==20&&deletes==old_deletes);assert(!LD32(NODE(NESTED,0)+0xb0));close_panel(&c);
 initialize();bad_return=true;assert(!ensure(&c));unchanged();bad_return=false;close_panel(&c);
 initialize();bad_count=true;old_deletes=deletes;assert(!ensure(&c)&&LD64(PANEL+0x1948)==21&&deletes==old_deletes);bad_count=false;close_panel(&c);
 for(unsigned kind=0;kind<2;++kind){initialize();g_settings.quickstep_slot=true;assert(ensure(&c));g_settings.quickstep_slot=false;owner=false;
  c.x[0]=kind?UI:SEQ;before=c;if(kind)quickstep_stats_ui_release_ui(&c);else quickstep_stats_ui_release_sequence(&c);assert(!memcmp(&before,&c,sizeof c));unchanged();}
 puts("native stats UI ownership contracts: PASS");return 0;
}
