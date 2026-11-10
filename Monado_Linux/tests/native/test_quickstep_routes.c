// [XCDE] LINUX-044: actual native focus + picker together. Only resource,
// inventory and original guest helper boundaries are authored synthetic spies.
// Reuse the native resource fake rather than duplicate its ownership geometry.
#define QUICKSTEP_ROUTE_FIXTURE
#define main native_geometry_contracts
#include "test_quickstep_native.c"
#undef main
#define ROUTE_LIST (BASE+0xe00000)
#define ROUTE_DETAIL (BASE+0xe02000)
#define ROUTE_TOOLTIP (BASE+0xe03000)
#define ROUTE_ITEM(n) (BASE+0xe10000+(n)*0x2c)
#define ROUTE_EVENT (BASE+0xe20000)
static QuickstepGem route_gems[2];
static unsigned route_enumerations,route_opens,route_sounds,route_sets,route_open_sounds;
static uint32_t route_assigned;
static void route_fp(uint64_t item,uint8_t *out){
 memcpy(out,(void*)(item+0xc),4);memcpy(out+4,(void*)(item+4),4);
 memcpy(out+8,(void*)(item+0x14),24);
}
size_t quickstep_slot_list(Cpu*c,uint64_t ctx,uint32_t ch,QuickstepGem*out,size_t cap){
 assert(c&&ctx==LD64(UI+0x10)&&ch==LD32(SEQ+0x18b8));
 if(out)memcpy(out,route_gems,(cap<2?cap:2)*sizeof*out);return 2;
}
bool quickstep_slot_get(Cpu*c,uint64_t ctx,uint32_t ch,QuickstepGem*out){
 assert(c&&ctx==LD64(UI+0x10)&&ch==LD32(SEQ+0x18b8));
 for(unsigned i=0;i<2;i++)if(route_gems[i].uid==route_assigned){*out=route_gems[i];return true;}
 return false;
}
bool quickstep_slot_set_checked(Cpu*c,uint64_t ctx,uint32_t ch,uint32_t uid,const uint8_t*fp){
 QuickstepGem old;(void)quickstep_slot_get(c,ctx,ch,&old);
 for(unsigned i=0;i<2;i++)if(route_gems[i].uid==uid&&!memcmp(fp,route_gems[i].fingerprint,32)){
  route_assigned=item_uid=uid;++route_sets;return true;
 }
 return false;
}
bool quickstep_slot_clear_checked(Cpu*c,uint64_t ctx,uint32_t ch,uint32_t uid,const uint8_t*fp){
 QuickstepGem old;if(!quickstep_slot_get(c,ctx,ch,&old)||old.uid!=uid||memcmp(fp,old.fingerprint,32))return false;
 route_assigned=item_uid=0;return true;
}
void quickstep_stats_refresh(Cpu*c,uint64_t seq){assert(c&&seq==SEQ);}
static bool quickstep_route_helper(Cpu*c,uint64_t fn){
 uint64_t a=c->x[0],b=c->x[1];
 switch(fn){
 case 0x70002fb9b0ull:{
  assert(a==SEQ&&b==1&&LD32(UI+0x24)<20);++route_enumerations;
  bool gear=LD32(UI+0x24)==5;unsigned count=gear?2:3;
  for(unsigned i=0;i<count;i++)ST64(SEQ+0xe0+8*i,ROUTE_ITEM(i+(gear?3:0)));
  ST64(SEQ+0x1080,count);ST32(SEQ+0x1090,gear?5:3);ST32(SEQ+0x1098,LD32(UI+0x24));
  Cpu saved=*c;c->x[0]=SEQ+0xe0;quickstep_native_picker_filter(c);*c=saved;
  c->x[0]=ROUTE_LIST+0x58;c->x[1]=LD64(SEQ+0x1080);quickstep_native_picker_count(c);
  ST32(ROUTE_LIST+0x78,c->x[1]);*c=saved;return true;}
 case 0x700020af90ull:assert(a==ROUTE_LIST);return true;
 case 0x700020a510ull:assert(a==UI&&b==1);ST8(UI+0x20,1);return true;
 case 0x70002fec10ull:assert(a==SEQ);++route_opens;ST8(ROUTE_LIST+0x19,1);ST32(ROUTE_LIST+0x6c,0);return true;
 case 0x7000240ef0ull:assert(a==ROUTE_LIST+0x58);ST32(ROUTE_LIST+0x6c,b);return true;
 case 0x70002ff870ull:{assert(a==SEQ);Cpu saved=*c;assert(quickstep_native_picker_details(c));*c=saved;return true;}
 case 0x70002fea30ull:assert(a==SEQ);return true;
 case 0x7000240cf0ull:assert(a==ROUTE_LIST+0x58);return true;
 case 0x7000172c40ull:
  if(a<STACK-0x8000||a>=STACK)return false;
  assert(b==LD64(UI+0x10));memset((void*)a,0,0x140);ST64(a+8,b);return true;
 case 0x7000172cf0ull:
  if(a<STACK-0x8000||a>=STACK)return false;
  ST32(a,b);return true;
 case 0x7000237a30ull:assert(a==ROUTE_DETAIL);return true;
 case 0x70002369c0ull:assert(a==ROUTE_DETAIL||a==ROUTE_TOOLTIP);return true;
 case 0x70001b7600ull:
  assert(a==LD64(UI+0x10)&&!b&&quickstep_native_picker_active(SEQ)&&LD8(ROUTE_LIST+0x19));
  ++route_open_sounds;return true;
 case 0x70001b75f0ull:{
  float volume;memcpy(&volume,&c->v[0],4);assert(a==LD64(UI+0x10)&&b==0x14&&volume==1.f);
  assert(route_assigned&&quickstep_native_picker_active(SEQ));++route_sounds;return true;}
 case 0x700020a610ull:assert(a==UI&&!b);ST8(UI+0x20,0);ordinary_focus_live=true;return true;
 case 0x70002ff2e0ull:
  assert(a==SEQ);ST8(ROUTE_LIST+0x19,0);ST8(UI+0x20,0);quickstep_native_picker_closed(c);return true;
 default:return false;
 }
}
static bool route_event(Cpu*c,uint32_t event){
 Cpu saved=*c;c->x[19]=SEQ;c->x[20]=ROUTE_EVENT;ST32(ROUTE_EVENT,event);
 Cpu routed=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
 bool consumed=quickstep_native_input_event(c);
 assert(!memcmp(c,&routed,sizeof*c)&&rt_get_fpcr()==fpcr&&rt_get_fpsr()==fpsr);*c=saved;return consumed;
}
int main(int argc,char**argv){
 assert(argc==2);g_romfs_dir=argv[1];
 assert(mmap((void*)BASE,0x1000000,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
 assert(mmap((void*)0x7000b40000ull,0x50000,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)0x7000b40000ull);
 initialize();item_uid=0;g_settings.quickstep_slot=true;Cpu c=cpu();construct(&c);
 ST64(SEQ+0x4c10,ROUTE_LIST);ST64(SEQ+0x8448,ROUTE_DETAIL);
 ST64(SEQ+0x5da0,ROUTE_TOOLTIP);ST64(SEQ+0x5e20,ROUTE_TOOLTIP);
 for(unsigned i=0;i<5;i++){
  uint64_t at=ROUTE_ITEM(i);ST32(at,0x30000+i);ST16(at+6,3);ST32(at+0xc,100+i);
  ST8(at+0x10,1);ST8(at+0x16,i?2:5);ST8(at+0x1a,1);ST16(at+0x1c,i<2?77:22);
  if(i<2){route_gems[i].uid=0x30000+i;route_gems[i].rank=i?2:5;route_gems[i].availability=QUICKSTEP_FREE;route_fp(at,route_gems[i].fingerprint);}
 }
 uint32_t characters[]={1,2,3,4,5,6,7,8,1,7,14,15};
 for(unsigned ch=0;ch<12;ch++)for(unsigned parent=0;parent<5;parent++)for(unsigned equip=0;equip<2;equip++){
  route_assigned=item_uid=0;
  ST32(SEQ+0x18b8,characters[ch]);ST64(UI+0x10,ch<8?CONTEXT:CONTEXT+0x1000);
  uint32_t selected=parent<3?5:13,pane=parent<3?1:2;
  ST32(UI+0x24,selected);ST32(UI+0x18,pane);ST32(SEQ+0x18c0,pane);ST8(UI+0x328+13,parent<3?0:1);tick(&c);
  // All actual native focus/picker code is linked; no focus flag is mocked.
  unsigned cursor_before=move_sounds,open_before=route_open_sounds;
  assert(route_event(&c,0x208)&&move_sounds==cursor_before+1);
  assert(route_event(&c,0x208)&&move_sounds==cursor_before+1); // held blocked Right
  assert(quickstep_native_bonus_context(SEQ,NULL,NULL,NULL));
  assert(route_event(&c,0x201)&&route_open_sounds==open_before+1);
  assert(quickstep_native_picker_active(SEQ));
  assert(LD64(SEQ+0x1080)==2&&LD64(SEQ+0xe0)==ROUTE_ITEM(0)&&LD64(SEQ+0xe8)==ROUTE_ITEM(1));
  unsigned before_sounds=route_sounds,before_sets=route_sets,before_builds=route_enumerations;
  if(equip)assert(route_event(&c,0x201));else quickstep_native_picker_cancel(&c,SEQ);
  assert(!quickstep_native_picker_active(SEQ));
  assert(LD64(SEQ+0xe0)==ROUTE_ITEM(selected==5?3:0));
  assert(LD64(SEQ+0x1080)==(selected==5?2u:3u)&&route_enumerations==before_builds+1);
  assert(route_sounds==before_sounds+equip&&route_sets==before_sets+equip);
  assert(move_sounds==cursor_before+1&&route_open_sounds==open_before+1);
  tick(&c);assert(quickstep_native_bonus_context(SEQ,NULL,NULL,NULL));
  assert(route_event(&c,0x207)&&move_sounds==cursor_before+2);
  assert(ordinary_focus_live&&!quickstep_native_bonus_context(SEQ,NULL,NULL,NULL));
  assert(LD32(UI+0x24)==selected&&LD32(UI+0x18)==pane&&LD32(SEQ+0x18c0)==pane);
  // The native chooser Confirm301E60 opens the existing cache without a
  // builder. Our real router must decline it and leave complete normal rows.
  assert(!route_event(&c,0x201)&&route_open_sounds==open_before+1);
  unsigned ordinary_before=route_opens;
  c.x[0]=SEQ;guest_call(&c,0x70002fec10ull);c=cpu();
  assert(route_opens==ordinary_before+1&&LD64(SEQ+0x1080)==(selected==5?2u:3u));
  assert(LD64(SEQ+0xe0)==ROUTE_ITEM(selected==5?3:0));
  assert(LD64(SEQ+0xe8)==ROUTE_ITEM(selected==5?4:1));
  assert(LD32(SEQ+0x1090)==(selected==5?5u:3u)&&LD32(SEQ+0x1098)==selected);
  c.x[0]=SEQ;assert(!quickstep_native_picker_preview(&c)&&!quickstep_native_picker_commit(&c));
  c=cpu();c.x[0]=SEQ;guest_call(&c,0x70002ff2e0ull);c=cpu();
 }
 c.x[0]=SEQ;quickstep_native_release_sequence(&c);
 puts("combined native Quick Step cancel/equip/Left/ordinary Confirm routes: PASS");return 0;
}
