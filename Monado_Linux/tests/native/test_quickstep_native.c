// Authored control/geometry/ownership fixture; every guest object is synthetic.
#include "hle.h"
#include "settings.h"
#include "hle/quickstep_native.h"
#include "hle/quickstep_picker.h"
#include "hle/quickstep_slot.h"
#include <assert.h>
#include <math.h>
#include <sys/mman.h>
#define BASE 0x7200000000ull
#define SEQ (BASE+0x10000)
#define UI (SEQ+0x18d8)
#define CONTEXT (BASE+0x20000)
#define ORIGINAL (BASE+0x30000)
#define CLONE (BASE+0x40000)
#define ROOT(n) (BASE+0x100000+(n)*0x1000)
#define ICON (BASE+0x81000)
#define TEXT (BASE+0x82000)
#define OLD_ICON (BASE+0x83000)
#define FACTORY (BASE+0x84000)
#define BADGE (BASE+0x85000)
#define OLD_BADGE (BASE+0x86000)
#define PANEL (BASE+0x8a000)
#define MEMORY (BASE+0x400000)
#define RAW (BASE+0x410000)
#define WINDOW(n) (BASE+0xb00000+(n)*0x1000)
#define CLONE2 (BASE+0x50000)
#define ICON2 (BASE+0xb1000)
#define TEXT2 (BASE+0xb2000)
#define BADGE2 (BASE+0xb5000)
#define FRAMES (BASE+0xd1000)
#define CONFIG (BASE+0xd3000)
#define DIMS (BASE+0xd4000)
#define STACK (BASE+0x600000)
Settings g_settings;
const char *g_romfs_dir;
static bool picker,menu,budget=true,alias_root,alias_icon,alias_badge,alias_under,change_character;
static unsigned calls,creates,deletes,allocations,opens,forgotten,bindings,windows,retired,budgets;
static uint32_t item_uid=0x30001;
static uint64_t now=1000000000ull;
static unsigned picker_confirms,under_plays;
static bool reenter,reenter_binding,replacement;
static uint64_t last_scratch=MEMORY;
static unsigned scratch_allocations;
static float colors[3][4],offsets[3][4],rim_color[4];
static unsigned ordinary_clears,ordinary_restores,ordinary_offs,clone_offs;
static bool ordinary_focus_live=true;
int g_rt_unwind;
void rt_fatal_trace(void){abort();} void rt_unwind_point(void){}
uint64_t plat_now_ns(void){return now;}
static void sf(uint64_t p,float f){memcpy((void*)p,&f,4);}
static float lf(uint64_t p){float f;memcpy(&f,(void*)p,4);return f;}
void *hle_alloc(uint64_t bytes){++allocations;assert(bytes==0xa00||bytes==49152);if(bytes==49152)return(void*)RAW;last_scratch=MEMORY+(scratch_allocations++)*0x1000;return(void*)last_scratch;}
bool quickstep_ui_memory_allow(Cpu*c,uint64_t bytes,const char*consumer){(void)c;assert((bytes==512*1024&&!strcmp(consumer,"quickstep-window"))||(bytes==4*1024*1024&&!strcmp(consumer,"quickstep-gem-control")));++budgets;return budget;}
bool quickstep_native_picker_active(uint64_t seq){assert(seq==SEQ);return picker;}
bool quickstep_native_picker_context_menu_active(uint64_t seq){assert(seq==SEQ);return menu;}
bool quickstep_native_picker_open(Cpu*c,uint64_t seq){(void)c;assert(seq==SEQ);++opens;picker=true;return true;}
bool quickstep_native_picker_input_event(Cpu*c,uint64_t seq,uint32_t event){(void)c;assert(seq==SEQ);if(!picker||event!=0x201)return false;++picker_confirms;return true;}
void quickstep_native_picker_forget(uint64_t seq){assert(seq==SEQ);++forgotten;picker=menu=false;}
void quickstep_native_picker_cancel(Cpu*c,uint64_t seq){(void)c;assert(seq==SEQ);picker=false;}
bool quickstep_slot_get(Cpu*c,uint64_t context,uint32_t character,QuickstepGem*out){
 (void)c;assert((context==CONTEXT||context==CONTEXT+0x1000)&&character>0&&character<16);memset(out,0,sizeof*out);
 out->uid=item_uid;memset(out->fingerprint,0x35,32);return item_uid!=0;
}
static void initialize(void);
static Cpu cpu(void);
static void synthetic_clone(uint64_t clone,unsigned offset){
#define CR(n) ROOT((n)+offset)
 memset((void*)clone,0,0x8000);ST64(clone,0x7000b66c18ull);ST32(clone+0x18,1);
 ST64(clone+0x38,CR(0));ST64(clone+0x68,clone+0x1000);ST64(clone+0x70,273);
 ST64(clone+0x78,clone+0x4000);ST64(clone+0x80,361);
 for(unsigned i=0;i<273;i++){
  memset((void*)CR(i),0,0x1000);ST64(clone+0x1000+i*32,CR(i));
  ST32(clone+0x1014+i*32,i==0?UINT32_MAX:i==3?2:i==42?3:i==43?42:0);
 }
 for(unsigned i=0;i<361;i++){
  uint64_t part=i==0?(offset?ICON2:ICON):i==1?(offset?TEXT2:TEXT):i==2?(offset?BADGE2:BADGE):BASE+0x700000+offset*0x100+i*0x80;
  memset((void*)part,0,0x80);memset((void*)CR(300+i),0,0x1000);
  ST64(part+0x38,CR(300+i));ST64(clone+0x4000+i*16,part);
 }
 ST64(CR(42)+0x280,clone+0x6000);ST64(CR(43)+0x280,clone+0x6100);
 ST32(clone+0x6000+0x10,2);ST32(clone+0x6100+0x10,4);
}
#undef CR

void guest_call(Cpu*c,uint64_t fn){
 uint64_t a=c->x[0],b=c->x[1],d=c->x[2],e=c->x[3],f=c->x[4],ret=0;float x=0,y=0;
 memcpy(&x,&c->v[0],4);memcpy(&y,&c->v[1],4);++calls;
 switch(fn){
 case 0x7000749710ull:{assert(!a&&b==RAW);uint64_t n=WINDOW(windows);++windows;memset((void*)n,0,0x1000);ST64(n,0x7000b72bb8ull);ST64(n+0xc0,RAW);ST32(n+0x2b8,9);ST16(n+0x1a8,32);ST16(n+0x1aa,32);ret=n;break;}
 case 0x7000749f90ull:assert(LD64(a)>=WINDOW(0)&&LD64(a)<WINDOW(128));assert(b==7);ST32(LD64(a)+0xc8,b);break;
 case 0x700074d290ull:assert(a>=WINDOW(0)&&a<WINDOW(128));ST8(a+0x1c,0x22);++retired;break;
 case 0x7000580bc0ull:
  assert(a==FACTORY);assert((LD64(a)==0x7000b432a8ull&&LD64(LD64(a)+0x20)==0x70001b7a30ull)
   ||(LD64(a)==0x7000b671f0ull&&LD64(LD64(a)+0x20)==0x7000585040ull));++creates;
  {uint64_t root=replacement?CLONE2:CLONE;unsigned offset=replacement?700:0;synthetic_clone(root,offset);
   uint64_t result=c->x[8];
   if(reenter){reenter=false;Cpu nested=cpu();nested.x[0]=UI;quickstep_native_release(&nested);initialize();nested=cpu();quickstep_native_construct(&nested);replacement=true;quickstep_native_tick(&nested);replacement=false;}
   ST64(result,alias_root?ORIGINAL:root);
  }break;
 case 0x700057aeb0ull:
  assert(a==CLONE||a==CLONE2||a==ORIGINAL);
  if(!strcmp((char*)b,"Equip_mc/EquipAnim_mc/Jem_08_mc/jem_under_mc"))ret=a!=ORIGINAL&&!alias_under?ROOT((a==CLONE2?700:0)+43):BASE+0xa7000;
  else{assert(!strcmp((char*)b,"Equip_mc/EquipAnim_mc/Jem_08_mc")||!strcmp((char*)b,"Equip_mc/EquipAnim_mc/Weapon_06_mc"));ret=a!=ORIGINAL?ROOT((a==CLONE2?700:0)+42):LD64(ORIGINAL+0x1000+(!strcmp((char*)b,"Equip_mc/EquipAnim_mc/Weapon_06_mc")?66:42)*32);}break;
 case 0x700057a770ull:
  assert(a==ORIGINAL||a==CLONE||a==CLONE2);
  if(!strcmp((char*)b,"Equip_mc/EquipAnim_mc/Jem_08_mc/rect_icon_Jem_mc"))ret=a==ORIGINAL||alias_icon?OLD_ICON:a==CLONE2?ICON2:ICON;
  else{assert(!strcmp((char*)b,"Equip_mc/EquipAnim_mc/Jem_08_mc/rect_item_jem_slot_mc"));ret=a==ORIGINAL||alias_badge?OLD_BADGE:a==CLONE2?BADGE2:BADGE;}break;
 case 0x70001b31c0ull:assert(a==UI+0x30&&!b);++ordinary_clears;ordinary_focus_live=false;break;
 case 0x7000208fe0ull:assert(a==UI&&b==LD32(UI+0x24)&&b<20);++ordinary_restores;ordinary_focus_live=true;break;
 case 0x700074ea30ull:ST32(a+0xb0,b?1:0);break;
 case 0x700074e750ull:assert(x>0&&y>0);sf(a+0xdc,x);sf(a+0xe0,y);break;
 case 0x700074e6a0ull:sf(a+0xd0,x);sf(a+0xd4,y);break;
 case 0x7000749f10ull:case 0x7000749f20ull:{
  uint64_t n=LD64(a);int which=(n==ROOT(300)||n==ROOT(1000))?0:(n==ROOT(302)||n==ROOT(1002))?1:(n==ROOT(43)||n==ROOT(743))?2:-1;
  if(fn==0x7000749f20ull)assert(lf(b+12)==0);
  if(which>=0)memcpy(fn==0x7000749f10ull?colors[which]:offsets[which],(void*)b,16);
  else {assert(n>=WINDOW(0)&&n<WINDOW(128));if(fn==0x7000749f10ull)memcpy(rim_color,(void*)b,16);}break;}
 case 0x70007576c0ull:{
  assert(b>=MEMORY+0x800&&b<=MEMORY+0x8800&&(b-MEMORY)%0x1000==0x800);assert(d==BASE+0xa1000+0x194||d==BASE+0xa2000+0x194);
  float scale=d==BASE+0xa1000+0x194?40.f/1280:24.f/1280;
  float m[6]={0,0,scale,scale,0,0};memcpy((void*)a,m,sizeof m);break;}
 case 0x700074e310ull:{
  assert(a>=MEMORY+0x860&&(a-MEMORY)%0x1000==0x860&&b==a-0x20);
  float px=lf(DIMS+0x5e4)*.5f,py=lf(DIMS+0x5e8)*-.5f;
  float box[4]={(lf(b)+lf(b+8))*px,(lf(b+4)+lf(b+12))*py,lf(b)*px,lf(b+4)*py};memcpy((void*)a,box,sizeof box);break;}
 case 0x7000172c40ull:assert(a>=MEMORY+0x400&&(a-MEMORY)%0x1000==0x400&&(b==CONTEXT||b==CONTEXT+0x1000));memset((void*)a,0x11,0x140);break;
 case 0x7000172cf0ull:assert(a>=MEMORY+0x400&&(a-MEMORY)%0x1000==0x400&&b==item_uid);break;
 case 0x7000209240ull:
  assert(a>=MEMORY&&(a-MEMORY)%0x1000==0&&b==13&&d==a+0x400&&e==1&&!f);++bindings;
  if(reenter_binding){
   reenter_binding=false;unsigned old_deletes=deletes;Cpu nested=cpu();nested.x[0]=UI;quickstep_native_release(&nested);
   assert(deletes==old_deletes&&LD32(CLONE+0x18)==1);initialize();nested=cpu();quickstep_native_construct(&nested);
   replacement=true;quickstep_native_tick(&nested);replacement=false;
   assert(deletes==old_deletes&&LD32(CLONE+0x18)==1);
  }break;
 case 0x7000749c90ull:{
  uint64_t n=LD64(a);bool own_row=n==ROOT(42)||n==ROOT(742),under=n==ROOT(43)||n==ROOT(743);
  bool original_row=n==LD64(ORIGINAL+0x1000+42*32)||n==LD64(ORIGINAL+0x1000+66*32);
  assert(own_row||under||original_row);
  if(own_row){assert(!strcmp((char*)b,"off"));++clone_offs;}
  else if(under){assert(!strcmp((char*)b,"slot"));++under_plays;}
  else{assert(!strcmp((char*)b,"off"));++ordinary_offs;}
  break;}

 case 0x700057a590ull:assert((a==CLONE||a==CLONE2)&&LD32(a+0x18)==0);++deletes;break;
 default:fprintf(stderr,"unexpected helper %llx\n",(unsigned long long)fn);abort();
 }
 if(change_character&&fn==0x7000580bc0ull){change_character=false;ST32(SEQ+0x18b8,2);}
 memset(c,0xad,sizeof*c);c->x[0]=ret;
}
static Cpu cpu(void){Cpu c;memset(&c,0x56,sizeof c);c.sp=STACK;c.x[0]=UI;c.x[19]=SEQ;c.x[20]=UI;return c;}
static void construct(Cpu*c){Cpu before=*c;quickstep_native_construct(c);assert(!memcmp(c,&before,sizeof*c));}
static void tick(Cpu*c){Cpu before=*c;quickstep_native_tick(c);assert(!memcmp(c,&before,sizeof*c));}
static void layout(float ax,float ay,float shift,float px,float py){
 float m[6]={(11*ax+6+shift)/1280,(277*ay+70)/1280,ax,ay,0,0};memcpy((void*)(FRAMES+16*0x6c),m,sizeof m);
 sf(DIMS+0x5e4,2*px);sf(DIMS+0x5e8,-2*py);
}
static void initialize(void){
 ordinary_focus_live=true;memset((void*)SEQ,0,0x8a18);ST64(UI,0x7000b460d8ull);ST64(UI+8,ORIGINAL);ST64(UI+0x10,CONTEXT);
 ST64(SEQ+0x1c48,UI);ST32(SEQ+0x18b8,1);ST32(UI+0x18,2);ST8(UI+0x21,1);ST8(UI+0x29,1);ST32(UI+0x24,13);
 ST64(SEQ+0x28,FACTORY);ST64(FACTORY,0x7000b432a8ull);
 ST64(0x7000b432a8ull+0x20,0x70001b7a30ull);ST64(0x7000b671f0ull+0x20,0x7000585040ull);
 ST64(ORIGINAL,0x7000b66c18ull);ST64(ORIGINAL+0x38,BASE+0xa0000);ST32(BASE+0xa0000+0xc8,7);
 ST64(OLD_ICON+0x38,BASE+0xa1000);ST64(OLD_BADGE+0x38,BASE+0xa2000);ST64(SEQ+0x8440,PANEL);
 ST16(BASE+0xa1000+0x1a8,40);ST16(BASE+0xa1000+0x1aa,40);ST16(BASE+0xa2000+0x1a8,24);ST16(BASE+0xa2000+0x1aa,24);
 ST64(ORIGINAL+0x68,ORIGINAL+0x1000);ST64(ORIGINAL+0x70,273);
 for(unsigned i=0;i<273;i++)ST64(ORIGINAL+0x1000+i*32,BASE+0x900000+i*0x1000);
 ST64(LD64(ORIGINAL+0x1000+43*32)+0x280,BASE+0xd7000);
 ST64(LD64(ORIGINAL+0x1000+42*32)+0x280,BASE+0xd7100);ST64(LD64(ORIGINAL+0x1000+66*32)+0x280,BASE+0xd7200);
 uint64_t row=LD64(ORIGINAL+0x1000+42*32);ST32(row+0x3c4,3);ST64(row+0x3b8,BASE+0xd6000);
 const int16_t rects[2][4]={{-12,-12,12,12},{-8,-8,8,7}};memcpy((void*)(BASE+0xd6000+32+24),rects[0],8);memcpy((void*)(BASE+0xd6000+64+24),rects[1],8);
 uint64_t anim=LD64(ORIGINAL+0x1000+3*32);ST64(anim+0x2c8,BASE+0xd0000);ST64(BASE+0xd0000+0x30,FRAMES);
 ST64(0x7000b8a278ull,BASE+0xd2000);ST64(BASE+0xd2000,CONFIG);ST64(CONFIG+0x48,DIMS);layout(1,1,0,1,1);
}
static void check_filter(Cpu*c,bool own){Cpu before=*c;c->x[19]=ROOT(42);Cpu frame=*c;ST8(ROOT(42)+0x3c8,7);assert(quickstep_native_filter_background(c)==own);assert(!memcmp(c,&frame,sizeof*c));assert(LD8(ROOT(42)+0x3c8)==(own?0:7));c->x[19]=BASE+0xa6000;assert(!quickstep_native_filter_background(c));*c=before;}
int main(int argc,char**argv){
 assert(argc==2);g_romfs_dir=argv[1];assert(mmap((void*)BASE,0x1000000,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)BASE);
 assert(mmap((void*)0x7000b40000ull,0x50000,3,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)==(void*)0x7000b40000ull);
 initialize();Cpu c=cpu();uint64_t context,generation;uint32_t ch;
 g_settings.quickstep_slot=false;construct(&c);tick(&c);assert(!calls&&!allocations);
 g_settings.quickstep_slot=true;construct(&c);uint8_t original[0x370];memcpy(original,(void*)UI,sizeof original);
 tick(&c);assert(creates==1&&windows==2&&bindings==1&&allocations==2&&!memcmp(original,(void*)UI,sizeof original));
 for(unsigned i=0;i<273;i++)assert(LD32(ROOT(i)+0xb0)==(i==0||i==2||i==3||i==42||i==43));
 for(unsigned i=0;i<361;i++)assert(LD32(ROOT(300+i)+0xb0)==(i==0||i==2));
 unsigned initial_under_plays=under_plays;tick(&c);assert(under_plays==initial_under_plays);
 check_filter(&c,true);assert(!quickstep_native_bonus_context(SEQ,NULL,NULL,NULL));
 // Reproducer: native character/context change precedes the next owner tick.
 // The separately owned batch must remain filtered while input becomes stale.
 ST32(SEQ+0x18b8,2);ST64(UI+0x10,CONTEXT+0x1000);check_filter(&c,true);
 ST32(SEQ+0x18b8,1);ST64(UI+0x10,CONTEXT);
 uint64_t owned_lower=LD64(CLONE+0x38);ST64(CLONE+0x38,BASE+0xa0000);check_filter(&c,false);ST64(CLONE+0x38,owned_lower);
 ST64(UI,0);check_filter(&c,false);ST64(UI,0x7000b460d8ull);
 assert(LD32(WINDOW(0)+0xb0)&&rim_color[0]<.5f&&rim_color[1]<.5f);

 assert(quickstep_native_stats_context(PANEL,1,NULL,&context,&generation)&&context==CONTEXT);
 // No computed ordinary glyph bounds are needed, including naked/fixed boots.
 sf(BASE+0xa1000+0x18c,0);assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_RIGHT));
 assert(quickstep_native_bonus_context(SEQ,&context,&ch,&generation)&&ch==1);tick(&c);
 assert(LD32(WINDOW(0)+0xb0)&&rim_color[0]==1&&rim_color[1]==1&&!ordinary_focus_live);
 uint64_t event=BASE+0x65000;c.x[20]=event;ST32(event,0x20d);Cpu before=c;
 assert(!quickstep_native_input_event(&c)&&quickstep_native_bonus_context(SEQ,NULL,NULL,NULL)&&!memcmp(&c,&before,sizeof c));
 menu=true;ST8(UI+0x20,1);c.x[20]=UI;unsigned clears_before=ordinary_clears;ordinary_focus_live=true;tick(&c);
 assert(ordinary_focus_live&&ordinary_clears==clears_before);
 assert(quickstep_native_bonus_context(SEQ,NULL,NULL,NULL)&&!LD32(ROOT(0)+0xb0)&&!LD32(WINDOW(1)+0xb0));
 menu=false;ST8(UI+0x20,0);tick(&c);assert(!ordinary_focus_live);
 assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_CONFIRM)&&opens==1);ST8(UI+0x20,1);clears_before=ordinary_clears;ordinary_focus_live=true;tick(&c);assert(ordinary_focus_live&&ordinary_clears==clears_before);
 c.x[20]=event;ST32(event,0x201);assert(quickstep_native_input_event(&c)&&picker_confirms==1);c.x[20]=UI;
 picker=false;ST8(UI+0x20,0);quickstep_native_bonus_picker_closed(SEQ);tick(&c);assert(!ordinary_focus_live);
 assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_LEFT));
 // Native units/projection and entry scales are independent. The complete
 // original glyph group is translated at1x; its main icon and badge stay1x.
 for(unsigned variant=0;variant<3;variant++){
  float ax=variant==0?.6f:variant==1?1.f:1.3f,px=variant==2?.8f:1.f,shift=variant==1?17.f:0;
  float ay=variant==1?.7f:1.f,py=variant==2?1.2f:1.f;layout(ax,ay,shift,px,py);
  for(unsigned rate=30;rate<=165;rate=rate==30?120:rate==120?165:166){
   ST32(UI+0x24,0);tick(&c);now+=1000000000ull;ST32(UI+0x24,13);float prev=1e9;
   float edge=(490*ax+6+shift)*px,left=edge+4,source_center=(280.75f*ax+6+shift)*px;
   for(unsigned fr=0;fr<=(rate*120+999)/1000;fr++){
    tick(&c);assert(lf(ROOT(42)+0xdc)==1&&lf(ROOT(42)+0xe0)==1);
    assert(lf(ROOT(300)+0xdc)==1&&lf(ROOT(302)+0xdc)==1&&lf(ROOT(43)+0xdc)==1);
    float actual=lf(WINDOW(1)+0xd0)*px;assert(actual>=left-.001f&&actual<=left+18.001f&&actual<=prev+.001f);prev=actual;
    // Background is a genuine corner-preserving patch, not a crushed old row.
    assert(fabsf(lf(WINDOW(1)+0xdc)*32*px-60)<.001f);
    assert(fabsf(lf(WINDOW(1)+0xe0)*32*(720.f/1280)*py-44)<.001f);
    assert(fabsf(source_center+lf(ROOT(42)+0xd0)*ax*px-(actual+30))<.001f);
    assert(fabsf(lf(WINDOW(0)+0xdc)*32*px-64)<.001f);
    assert(fabsf(lf(WINDOW(0)+0xe0)*32*(720.f/1280)*py-48)<.001f);
    now+=1000000000ull/rate;
   }
   now+=200000000ull;tick(&c);assert(fabsf(lf(WINDOW(1)+0xd0)*px-left)<.001f);
  }
 }
 layout(1,1,0,1,1);item_uid=0;assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_RIGHT));quickstep_native_bonus_picker_closed(SEQ);tick(&c);
 for(unsigned g=0;g<2;g++)for(unsigned k=0;k<4;k++)assert(colors[g][k]==1);
 assert(colors[2][0]==0&&colors[2][1]>1&&colors[2][2]==0);
 for(unsigned g=0;g<3;g++)for(unsigned k=0;k<4;k++)assert(offsets[g][k]==0);
 item_uid=0x30001;quickstep_native_bonus_picker_closed(SEQ);tick(&c);for(unsigned k=0;k<4;k++)assert(colors[2][k]==1);
 assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_LEFT));
 unsigned characters[]={1,2,3,4,5,6,7,8,1,7,14,15};
 for(unsigned i=0;i<12;i++)for(unsigned gear=0;gear<5;gear++){
  ST64(UI+0x10,i<8?CONTEXT:CONTEXT+0x1000);
  ST32(SEQ+0x18b8,characters[i]);ST32(UI+0x24,gear<2?5:13);ST8(UI+0x328+13,gear<2?0:1);tick(&c);
  assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_RIGHT));assert(quickstep_native_bonus_context(SEQ,NULL,&ch,NULL)&&ch==characters[i]);
  for(unsigned phase=0;phase<6;phase++){
   // Reproduce native re-selection after an equip or chooser/context close,
   // at different entry/focus animation times. Only the small frame owns focus.
   now+=phase*17000000ull;ordinary_focus_live=true;
   if(phase&1)quickstep_native_bonus_picker_closed(SEQ);
   tick(&c);assert(!ordinary_focus_live&&rim_color[0]==1&&rim_color[1]==1);
   assert(fabsf(lf(WINDOW(0)+0xdc)*32-64)<.001f&&fabsf(lf(WINDOW(0)+0xe0)*32*(720.f/1280)-48)<.001f);
   check_filter(&c,true);
  }
  c.x[20]=event;ST32(event,0x20d);assert(!quickstep_native_input_event(&c));c.x[20]=UI;
  menu=true;ST8(UI+0x20,1);tick(&c);assert(quickstep_native_bonus_context(SEQ,NULL,NULL,NULL));
  menu=false;ST8(UI+0x20,0);tick(&c);
  unsigned restores_before=ordinary_restores;
  assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_LEFT));assert(ordinary_focus_live&&ordinary_restores==restores_before+1);
  tick(&c);assert(ordinary_focus_live&&ordinary_restores==restores_before+1&&rim_color[0]<.5f);
 }
 assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_RIGHT));unsigned off_restores=ordinary_restores;
 g_settings.quickstep_slot=false;tick(&c);assert(ordinary_focus_live&&ordinary_restores==off_restores+1&&!quickstep_native_bonus_context(SEQ,NULL,NULL,NULL));
 tick(&c);assert(ordinary_restores==off_restores+1);g_settings.quickstep_slot=true;tick(&c);
 c.x[20]=event;ST32(event,208);assert(!quickstep_native_input_event(&c));ST32(event,0x208);ST8(event+4,0);assert(quickstep_native_input_event(&c));c.x[20]=UI;
 unsigned teardown_restores=ordinary_restores;c.x[0]=SEQ;before=c;quickstep_native_release_sequence(&c);assert(ordinary_restores==teardown_restores);assert(!memcmp(&c,&before,sizeof c));c.x[0]=UI;assert(deletes==1&&retired==2);check_filter(&c,false);
 quickstep_native_release(&c);assert(deletes==1&&retired==2);
 initialize();construct(&c);alias_root=true;tick(&c);assert(deletes==1);alias_root=false;check_filter(&c,false);quickstep_native_release(&c);
 initialize();construct(&c);alias_icon=true;tick(&c);assert(deletes==2);alias_icon=false;quickstep_native_release(&c);
 initialize();construct(&c);change_character=true;tick(&c);assert(deletes==3&&!quickstep_native_bonus_context(SEQ,NULL,NULL,NULL));quickstep_native_release(&c);
 initialize();construct(&c);alias_root=change_character=true;uint8_t source_root[0x98];memcpy(source_root,(void*)ORIGINAL,sizeof source_root);
 tick(&c);assert(deletes==3&&!memcmp(source_root,(void*)ORIGINAL,sizeof source_root));alias_root=false;quickstep_native_release(&c);
 initialize();construct(&c);ST64(FACTORY,0x7000b460d8ull);unsigned old_calls=calls,old_creates=creates;tick(&c);tick(&c);assert(calls==old_calls&&creates==old_creates);quickstep_native_release(&c);
 initialize();construct(&c);ST64(0x7000b432a8ull+0x20,0x7000585040ull);old_calls=calls;tick(&c);tick(&c);assert(calls==old_calls);quickstep_native_release(&c);
 initialize();construct(&c);budget=false;unsigned old_windows=windows;tick(&c);assert(windows==old_windows&&creates==old_creates);budget=true;quickstep_native_release(&c);
 initialize();construct(&c);alias_badge=true;tick(&c);alias_badge=false;quickstep_native_release(&c);
 initialize();construct(&c);alias_under=true;tick(&c);alias_under=false;quickstep_native_release(&c);
 initialize();construct(&c);tick(&c);check_filter(&c,true);quickstep_native_release(&c);check_filter(&c,false);
 // Actual public teardown + new construction occurs inside the old factory
 // guest call. Distinct old/new scratch and native objects must survive cleanup.
 initialize();construct(&c);reenter=true;unsigned before_deletes=deletes;tick(&c);
 Cpu render=c;render.x[19]=ROOT(742);assert(quickstep_native_filter_background(&render));
 render.x[19]=ROOT(42);assert(!quickstep_native_filter_background(&render));
 assert(deletes==before_deletes+1&&last_scratch==MEMORY+0x1000);
 assert(LD64(MEMORY+0x580)==CLONE&&LD64(MEMORY+0x1000+0x580)==CLONE2);
 assert(LD32(CLONE2+0x18)==1);quickstep_native_release(&c);
 initialize();construct(&c);tick(&c);assert(quickstep_native_input(&c,SEQ,QUICKSTEP_NATIVE_RIGHT));
 reenter_binding=true;before_deletes=deletes;tick(&c);assert(deletes==before_deletes+1);
 render=c;render.x[19]=ROOT(742);assert(quickstep_native_filter_background(&render));
 assert(last_scratch==MEMORY+0x2000&&LD32(CLONE2+0x18)==1);quickstep_native_release(&c);
 puts("native Quick Step independent resource/focus/lifetime contracts: PASS");
}
