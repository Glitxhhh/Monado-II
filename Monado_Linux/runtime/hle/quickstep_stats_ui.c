// [XCDE] LINUX-040: instance a genuine native effect row. The original resource,
// eight-record summaries, native fonts and all equipment/inventory stay intact.
#include "../hle.h"
#include "../settings.h"
#include "quickstep_native_ui.h"
#include "quickstep_stats_ui.h"
#include "quickstep_stats.h"
#include "quickstep_picker.h"
#include "quickstep_ui_memory.h"
#include <math.h>

enum { MAX_NODES=512, MAX_PARTS=2048, MEMORY=0x600, RESULT=0x10,
       WRAP=0x30, PATH=0x60, LOADER=0x100, CONFIG=0x300, STEM=0x340,
       ORIGINAL_ROWS=19, ROW_BYTES=0xc0,
       ROW_START=0x148, COUNT=0x1948, SHOWN=0x1950, BASE_COUNT=0x20 };
// Development headroom policy, not an atomic reservation or a complete bound
// on arbitrary concurrent native allocations. Never expand an engine arena.
static const uint64_t headroom=4ull*1024*1024;
static const uint64_t ROOT_CLASS=0x7000b66c18ull, PANEL_CLASS=0x7000b477f0ull,
                      ROW_CLASS=0x7000b478a0ull, UI_CLASS=0x7000b460d8ull;
// Native580E20 creates this exact58A6F0 cache handle for the new base loader.
// The original nested wrapper's +30 asset is an ownership reference; it does
// not establish the type of this independently requested selected asset.
static const uint64_t ASSET_CLASS=0x7000b673a8ull;
static const char container_path[]="InfoStatus_mc/rect_cont19_mc";
static const char row_path[]="InfoStatusAnim_mc/StatusEffect_08_mc";
typedef struct {
    uint64_t sequence,ui,panel,original,source,root,lower,nested,node,memory,serial,loader;
    uint64_t asset_class,selected_raw,expected_raw;
    uint64_t rows[8];
    uint8_t tail[ROW_BYTES];
    float transforms[8][4];
    uint32_t original_base,original_shown;
    uint32_t main_target,nested_target,component,character,polls;
    bool busy,failed,appended,positioned,wanted,alias,outer_owned,geometry_reported;
    bool loader_owned,pending,pending_reported,refresh_pending;
    const char *failure;
} Extra;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static Extra state;
static uint64_t serial,parent_retired;
static unsigned failures;
static bool range(uint64_t p,size_t n) { return p>=GUEST_LO&&p<GUEST_HI&&n<=GUEST_HI-p; }
static uint64_t row(uint64_t panel,unsigned i) { return panel+ROW_START+i*ROW_BYTES; }
static float scalar(uint64_t p) { float f;memcpy(&f,(void*)(uintptr_t)p,4);return f; }
static bool fail(Extra*s,const char*why) { s->failure=why;return false; }
static bool parent_identity(const Extra*s) {
    return range(s->sequence,0x8a18)&&s->ui==s->sequence+0x18d8
        &&LD64(s->sequence+0x1c48)==s->ui&&LD64(s->ui)==UI_CLASS
        &&LD64(s->ui+8)==s->original&&range(s->original,0x98)&&LD64(s->original)==ROOT_CLASS;
}
static bool identity(const Extra*s) {
    return parent_identity(s)&&LD64(s->sequence+0x8440)==s->panel
        &&range(s->panel,0x1ac1)&&LD64(s->panel)==PANEL_CLASS;
}
static bool current(const Extra*s) {
    pthread_mutex_lock(&lock);bool same=state.serial==s->serial&&state.panel==s->panel;
    pthread_mutex_unlock(&lock);return same&&identity(s);
}
static uint64_t call(Cpu*c,uint64_t fn,uint64_t a,uint64_t b,uint64_t d,
                     uint64_t out,const float*f,uint32_t*fp) {
    Cpu saved=*c;uint64_t fpcr=rt_get_fpcr(),fpsr=rt_get_fpsr();
    if((c->sp&15)||c->sp<GUEST_LO+0x2000||!range(c->sp-0x2000,0x2000))
        FATAL("[quickstep-stats-ui] invalid native helper stack\n");
    c->sp-=0x2000;c->x[0]=a;c->x[1]=b;c->x[2]=d;c->x[3]=0;c->x[8]=out;
    if(f){c->v[0]=c->v[1]=0;memcpy(&c->v[0],f,4);memcpy(&c->v[1],f+1,4);}
    guest_call(c,fn);uint64_t result=c->x[0];if(fp)memcpy(fp,&c->v[0],4);
    *c=saved;rt_set_fpcr(fpcr);rt_set_fpsr(fpsr);return result;
}
static uint64_t plain(Cpu*c,uint64_t fn,uint64_t a,uint64_t b,uint64_t d) {
    return call(c,fn,a,b,d,0,NULL,NULL);
}
static void visible(Cpu*c,uint64_t lower,bool show) {
    if(range(lower,0x288))plain(c,0x700074ea30ull,lower,show,0);
}
static void translate(Cpu*c,uint64_t lower,float x,float y) {
    const float f[]={x,y};call(c,0x700074e6a0ull,lower,0,0,0,f,NULL);
}
static uint64_t find(Cpu*c,const Extra*s,uint64_t root,const char*path,bool component) {
    size_t n=strlen(path)+1;if(n>LOADER-PATH)return 0;
    memcpy((void*)(uintptr_t)(s->memory+PATH),path,n);
    return plain(c,component?0x700057a770ull:0x700057aeb0ull,root,s->memory+PATH,0);
}
typedef struct { uint64_t root,lower,nodes,parts,resource;uint32_t n,p,table_offset;uint16_t owner[MAX_PARTS]; } Tree;
// Native579D80 writes descriptor+10's cumulative part start (57A220) and
// component-entry+8's owning node index (57A26C). Verify this complete dense
// partition directly; no guessed file name, padding byte or yielding lookup
// establishes ownership of a component that we are about to hide.
static bool tree(Extra*s,uint64_t root,Tree*t) {
    if(!range(root,0x98)||LD64(root)!=ROOT_CLASS)return fail(s,"tree-class");
    uint64_t n=LD64(root+0x70),p=LD64(root+0x80),lower=LD64(root+0x38);
    if(!n||n>MAX_NODES||p>MAX_PARTS||!range(lower,0x288))return fail(s,"tree-capacity");
    *t=(Tree){.root=root,.lower=lower,.nodes=LD64(root+0x68),.parts=LD64(root+0x78),.n=(uint32_t)n,.p=(uint32_t)p};
    if(!range(t->nodes,n*32)||(p&&!range(t->parts,p*16)))return fail(s,"tree-arrays");
    uint64_t resource=LD64(lower+0xc0);
    if(!range(resource,0x40))return fail(s,"tree-resource");
    t->resource=resource;t->table_offset=LD32(resource+0x3c);
    uint64_t table=resource+t->table_offset;
    if(!range(table,8)||LD32(table+4)!=n)return fail(s,"tree-resource-count");
    uint64_t next=0;
    for(uint32_t i=0;i<n;++i){
        uint64_t node=LD64(t->nodes+i*32);
        int32_t parent=(int32_t)LD32(t->nodes+i*32+0x14);
        if(!range(node,0x288)||(i==0?parent!=-1:parent<0||(uint32_t)parent>=i))
            return fail(s,"tree-parent");
        if(LD32(t->nodes+i*32+0x10)!=next)return fail(s,"tree-component-partition");
        while(next<p&&LD32(t->parts+next*16+8)==i)t->owner[next++]=(uint16_t)i;
    }
    if(next!=p)return fail(s,"tree-component-total");
    for(uint32_t i=0;i<p;++i){uint64_t part=LD64(t->parts+i*16);
        if(!range(part,0x40)||!range(LD64(part+0x38),0x288))return fail(s,"tree-component");}
    return true;
}
static bool independent(Extra*s,const Tree*a,const Tree*b) {
    if(a->root==b->root||LD64(a->root+0x38)==LD64(b->root+0x38)
        ||a->nodes==b->nodes||a->parts==b->parts||a->n!=b->n||a->p!=b->p)
        return fail(s,"clone-tree-alias");
    // Fresh scene instances must refer to the same immutable loaded resource,
    // while all their mutable wrappers/lower nodes remain independent.
    if(LD64(LD64(a->root+0x38)+0xc0)!=LD64(LD64(b->root+0x38)+0xc0))
        return fail(s,"clone-loaded-resource");
    // Check every live pointer against the complete original set, not only the
    // selected row. A shallow nested clone must never reach a visibility call.
    for(uint32_t i=0;i<a->n;++i){
        if(LD32(a->nodes+i*32+0x10)!=LD32(b->nodes+i*32+0x10)
            ||LD32(a->nodes+i*32+0x14)!=LD32(b->nodes+i*32+0x14))
            return fail(s,"clone-node-map");
        uint64_t v=LD64(a->nodes+i*32);
        for(uint32_t j=0;j<b->n;++j)if(v==LD64(b->nodes+j*32))return fail(s,"clone-node-alias");
        for(uint32_t j=0;j<b->p;++j)if(v==LD64(LD64(b->parts+j*16)+0x38))return fail(s,"clone-node-part-alias");
    }
    for(uint32_t i=0;i<a->p;++i){uint64_t v=LD64(a->parts+i*16),lower=LD64(v+0x38);
        if(a->owner[i]!=b->owner[i]||LD64(v)!=LD64(LD64(b->parts+i*16)))return fail(s,"clone-component-map");
        for(uint32_t j=0;j<b->p;++j){uint64_t old=LD64(b->parts+j*16);
            if(v==old||lower==LD64(old+0x38))return fail(s,"clone-component-alias");}
        for(uint32_t j=0;j<b->n;++j)if(lower==LD64(b->nodes+j*32))return fail(s,"clone-part-node-alias");
    }
    return true;
}
static int node_index(const Tree*t,uint64_t node){for(uint32_t i=0;i<t->n;++i)if(LD64(t->nodes+i*32)==node)return(int)i;return-1;}
static bool ancestor(const Tree*t,uint32_t i,uint32_t target){
    for(uint32_t depth=0;depth<t->n;++depth){if(i==target)return true;int32_t p=(int32_t)LD32(t->nodes+i*32+0x14);if(p<0)return false;i=(uint32_t)p;}return false;
}
static bool prune(Cpu*c,Extra*s,const Tree*t,int target,int selected_part){
    if(target<0||(uint32_t)target>=t->n)return fail(s,"clone-target");
    for(uint32_t i=0;i<t->n;++i){bool keep=ancestor(t,i,(uint32_t)target)||ancestor(t,(uint32_t)target,i);
        visible(c,LD64(t->nodes+i*32),keep);if(!current(s))return fail(s,"prune-owner");}
    for(uint32_t i=0;i<t->p;++i){bool keep=selected_part>=0?(int)i==selected_part:ancestor(t,t->owner[i],(uint32_t)target);
        visible(c,LD64(LD64(t->parts+i*16)+0x38),keep);if(!current(s))return fail(s,"prune-owner");}
    return true;
}
static bool physical(const Extra*s,bool appended){
    if(!identity(s)||LD64(s->panel+COUNT)!=(appended?20:19))return false;
    for(unsigned i=0;i<ORIGINAL_ROWS;++i){uint64_t r=row(s->panel,i);
        if(LD64(r)!=ROW_CLASS||LD64(r+0x18)!=s->panel||LD32(r+0x30)!=i||LD32(r+0x5c)!=i)return false;
        if(appended&&i>=11){uint64_t nodes=LD64(s->source+0x68),n=LD64(s->source+0x70);uint32_t index=LD32(r+0x10);
            if(LD64(r+8)!=s->source||n>MAX_NODES||index>=n||!range(nodes,n*32)||LD64(nodes+index*32)!=s->rows[i-11])return false;}}
    return !appended||(LD64(row(s->panel,19))==ROW_CLASS&&LD64(row(s->panel,19)+8)==s->nested
        &&LD64(row(s->panel,19)+0x18)==s->panel&&LD32(row(s->panel,19)+0x30)==19&&LD32(row(s->panel,19)+0x5c)==19);
}
static bool source_valid(Extra*s,const Tree*main,const Tree*nested,uint64_t lower){
    // The parent/panel lease alone does not establish that a linked component
    // survived a yielding loader or cache helper. Recheck the complete live
    // topology before following any pointers captured before that helper.
    if(!current(s)||!physical(s,false))return fail(s,"selected-source-owner");
    const Tree*snapshots[]={main,nested};
    for(unsigned i=0;i<2;++i){const Tree*t=snapshots[i];
        if(!range(t->root,0x98)||LD64(t->root)!=ROOT_CLASS
            ||LD64(t->root+0x38)!=t->lower||!range(t->lower,0x288)
            ||LD64(t->root+0x68)!=t->nodes||LD64(t->root+0x78)!=t->parts
            ||LD64(t->root+0x70)!=t->n||LD64(t->root+0x80)!=t->p
            ||LD64(t->lower+0xc0)!=t->resource||!range(t->resource,0x40)
            ||LD32(t->resource+0x3c)!=t->table_offset)
            return fail(s,"selected-source-snapshot");
    }
    Tree m,n;
    if(!tree(s,s->original,&m)||!tree(s,s->source,&n))return false;
    if(m.nodes!=main->nodes||m.parts!=main->parts||m.n!=main->n||m.p!=main->p
        ||n.nodes!=nested->nodes||n.parts!=nested->parts||n.n!=193||n.p!=480
        ||LD64(s->source+0x38)!=lower||LD64(lower+0xc0)!=s->expected_raw)
        return fail(s,"selected-source-topology");
    if(s->main_target>=m.n)return fail(s,"selected-source-parent");
    uint32_t first=LD32(m.nodes+s->main_target*32+0x10);
    if(first>m.p||s->component>=m.p-first
        ||m.owner[first+s->component]!=s->main_target
        ||LD64(m.parts+(first+s->component)*16)!=s->source)
        return fail(s,"selected-source-component");
    for(unsigned i=0;i<8;++i){uint64_t r=row(s->panel,11+i);uint32_t index=LD32(r+0x10);
        if(LD64(r+8)!=s->source||index>=n.n||LD64(n.nodes+index*32)!=s->rows[i])
            return fail(s,"selected-source-rows");}
    return true;
}
static bool owned_root(const Extra*s){
    return s->outer_owned&&range(s->root,0x98)&&LD64(s->root)==ROOT_CLASS
        &&LD64(s->root+0x38)==s->lower&&range(s->lower,0x288);
}
static void restore(Cpu*c,Extra*s,bool cleanup){
    // Hide our independently proved outer instance even if a replaced panel
    // prevents touching its inline vector or original row transforms.
    if(!cleanup&&!current(s))return;
    if(owned_root(s))visible(c,s->lower,false);
    if(!cleanup&&!current(s))return;
    if(!physical(s,true))return;
    if(s->positioned)for(unsigned i=0;i<8;++i){translate(c,s->rows[i],s->transforms[i][0],s->transforms[i][1]);if((!cleanup&&!current(s))||!physical(s,true))return;}
    s->positioned=false;ST8(row(s->panel,19)+0x34,0);visible(c,s->node,false);
}
static bool release(Cpu*c,Extra*s,bool before_parent_dtor){
    // A native parent owns the attached lower node. If its real lifetime has
    // retired while a yielding helper ran, neither that lower nor its upper
    // destructor may be touched. Retain the ambiguous capsule until reset.
    pthread_mutex_lock(&lock);
    bool owned=before_parent_dtor||state.serial==s->serial||s->serial>parent_retired;
    pthread_mutex_unlock(&lock);
    if(!owned||!parent_identity(s)){
        if((s->outer_owned||s->loader_owned)&&failures++<16)
            fprintf(stderr,"[quickstep-stats-ui] retired native parent; ambiguous instance retained until reset\n");
        return false;
    }
    // Unconditional with respect to the launcher toggle and focus. Restore the
    // exact native array before its destructor can see our borrowed wrapper.
    if(owned_root(s))visible(c,s->lower,false);
    bool detached=!s->appended;
    if(s->appended&&physical(s,true)){
        restore(c,s,true);
        if(physical(s,true)){
            plain(c,0x700023d6f0ull,row(s->panel,19),0,0);
            if(identity(s)&&LD64(s->panel+COUNT)==20){
                memcpy((void*)(uintptr_t)row(s->panel,19),s->tail,ROW_BYTES);
                ST64(s->panel+COUNT,19);ST32(s->panel+SHOWN,s->original_shown);ST32(s->panel+BASE_COUNT,s->original_base);
                detached=true;
            }
        }
    }
    // If ownership changed mid-helper, retaining a hidden instance is safer
    // than freeing a root still borrowed by a possibly live replacement row.
    if(!s->alias&&detached&&owned_root(s)){
        uint32_t refs=LD32(s->root+0x18);
        if(refs&&refs<=0x100000){ST32(s->root+0x18,refs-1);if(refs==1)plain(c,0x700057a590ull,s->root,0,0);}
    }
    // The native attached-child list owns the lower scene until its ordinary
    // deferred disposal pass. The original loaded component also keeps the
    // immutable asset alive. Destroy our loader through its real base dtor.
    if(detached&&s->loader_owned&&range(s->loader,0x188)
        &&LD64(s->loader)==0x7000b671f0ull){
        plain(c,0x7000583240ull,s->loader,0,0);s->loader_owned=false;
    }
    return detached&&!s->alias;
}
static bool asset_raw(Cpu*c,Extra*s,uint64_t asset,uint64_t*raw,
                      const Tree*main,const Tree*nested,uint64_t lower){
    s->asset_class=range(asset,0x28)?LD64(asset):0;
    if(s->asset_class!=ASSET_CLASS||!range(ASSET_CLASS,0x28))
        return fail(s,"selected-asset-class");
    uint64_t ready=LD64(ASSET_CLASS+0x18),get=LD64(ASSET_CLASS+0x20);
    if(ready!=0x700058a820ull||get!=0x700058a800ull)
        return fail(s,"selected-asset-methods");
    if(!(plain(c,ready,asset,0,0)&1))return fail(s,"selected-asset-ready");
    if(!source_valid(s,main,nested,lower))return false;
    uint64_t view=plain(c,get,asset,0,0);
    if(!source_valid(s,main,nested,lower))return false;
    if(!range(view,0x10))return fail(s,"selected-cache-view");
    uint64_t object=LD64(view+8);
    if(!range(object,0x258)||!range(LD64(object+0x250),0x40))
        return fail(s,"selected-cache-payload");
    *raw=s->selected_raw=LD64(object+0x250);return true;
}
static bool create(Cpu*c,Extra*s){
    s->pending=false;
    if(!physical(s,false)||LD32(s->panel+SHOWN)!=19||LD32(s->panel+BASE_COUNT)!=19)return fail(s,"original-physical-rows");
    s->source=LD64(row(s->panel,18)+8);
    if(!range(s->source,0x98)||LD64(s->source)!=ROOT_CLASS)return fail(s,"original-row-resource");
    Tree old_main,old_nested;
    if(!tree(s,s->original,&old_main)||!tree(s,s->source,&old_nested))return false;
    uint64_t original_source_lower=LD64(s->source+0x38);
    s->expected_raw=LD64(original_source_lower+0xc0);
    for(unsigned i=0;i<8;++i){uint64_t r=row(s->panel,11+i);if(LD64(r+8)!=s->source)return fail(s,"original-row-root");
        uint64_t nodes=LD64(s->source+0x68),n=LD64(s->source+0x70);uint32_t index=LD32(r+0x10);
        if(!n||n>MAX_NODES||index>=n||!range(nodes,n*32))return fail(s,"original-row-index");
        s->rows[i]=LD64(nodes+index*32);if(!range(s->rows[i],0x288))return fail(s,"original-row-node");
        static const unsigned offsets[]={0xd0,0xd4,0xdc,0xe0};
        for(unsigned j=0;j<4;++j)s->transforms[i][j]=scalar(s->rows[i]+offsets[j]);
        if(!isfinite(s->transforms[i][0])||!isfinite(s->transforms[i][1])||s->transforms[i][2]!=1.f||s->transforms[i][3]!=1.f)return fail(s,"original-row-transform");
    }
    // This filename is admitted only for the exact live linked stats resource,
    // including its eleven parameters/eight effects and original row wrappers.
    // Native cache identity below must resolve to that same immutable raw data.
    if(old_nested.n!=193||old_nested.p!=480)return fail(s,"loaded-stats-layout");
    int part=-1;
    for(uint32_t i=0;i<old_main.p;++i)if(LD64(old_main.parts+i*16)==s->source)part=(int)i;
    if(part<0)return fail(s,"loaded-container-part");
    s->main_target=old_main.owner[part];
    uint32_t first=LD32(old_main.nodes+s->main_target*32+0x10);
    if(first>(uint32_t)part)return fail(s,"loaded-component-index");
    s->component=(uint32_t)part-first;
    uint64_t parent=LD64(old_main.nodes+s->main_target*32);
    if(!range(parent,0x3d0)||s->component>=LD32(parent+0x3c4))return fail(s,"loaded-component-capacity");
    uint64_t source_lookup=find(c,s,s->original,container_path,true);
    if(!source_valid(s,&old_main,&old_nested,original_source_lower))return false;
    if(source_lookup!=s->source)return fail(s,"loaded-container-identity");
    uint64_t source_node=find(c,s,s->source,row_path,false);
    if(!source_valid(s,&old_main,&old_nested,original_source_lower))return false;
    if(!range(source_node,0x288)||s->rows[7]!=source_node)return fail(s,"loaded-row-wrapper");
    uint64_t factory=LD64(s->sequence+0x28);
    if(!range(factory,0x190))return fail(s,"factory-buffer");uint64_t cls=LD64(factory);
    if((cls!=0x7000b432a8ull&&cls!=0x7000b671f0ull)||!range(cls,0x28)
        ||LD64(cls+0x20)!=(cls==0x7000b432a8ull?0x70001b7a30ull:0x7000585040ull))return fail(s,"factory-method");
    if(!s->loader_owned){
        if(!quickstep_ui_memory_allow(c,headroom,"stats-loader")||!current(s))return fail(s,"native-memory-headroom");
        uint64_t config=s->memory+CONFIG;
        ST64(config,LD64(factory+8));ST64(config+8,LD64(factory+0x18));
        ST64(config+0x10,LD64(factory+0x18));ST64(config+0x18,LD64(factory+0x20));
        ST64(config+0x20,s->memory+STEM);ST64(config+0x28,LD64(factory+0x38));
        memcpy((void*)(uintptr_t)(s->memory+STEM),"mnu004_cont19",14);
        s->loader=s->memory+LOADER;
        plain(c,0x7000582fd0ull,s->loader,config,0);
        s->loader_owned=range(s->loader,0x188)&&LD64(s->loader)==0x7000b671f0ull;
        if(!s->loader_owned)return fail(s,"loader-constructor-owner");
        if(!source_valid(s,&old_main,&old_nested,original_source_lower))return false;
    }
    // The real loader initializes cached dependency/font bindings. Advance at
    // most once per UI tick; pending work is retried without a blocking loop.
    if(++s->polls>120)return fail(s,"loader-pending-timeout");
    if(!quickstep_ui_memory_allow(c,headroom,"stats-dependencies")||!current(s))return fail(s,"native-memory-headroom");
    plain(c,0x7000584d60ull,s->loader,0,0);
    if(!source_valid(s,&old_main,&old_nested,original_source_lower))return false;
    if(!(plain(c,0x70001b7ab0ull,s->loader,0,0)&1)){
        s->pending=true;return false;
    }
    if(!source_valid(s,&old_main,&old_nested,original_source_lower))return false;
    s->pending=false;
    uint64_t selected=LD64(s->loader+0x28),raw=0;
    if(!asset_raw(c,s,selected,&raw,&old_main,&old_nested,original_source_lower))return false;
    if(raw!=s->expected_raw)return fail(s,"selected-cache-raw");
    if(!quickstep_ui_memory_allow(c,headroom,"stats-instance"))return fail(s,"native-memory-headroom");
    if(!source_valid(s,&old_main,&old_nested,original_source_lower))return false;
    // Native NodeWrapper includes the component ordinal. 5859A0 appends to its
    // real parent component list; it preserves existing instances and inherits
    // the parent's transform, entry zoom, clipping, tab visibility and lifetime.
    ST64(s->memory+WRAP,s->original);ST32(s->memory+WRAP+8,s->main_target);
    ST32(s->memory+WRAP+12,s->component);
    uint64_t original_lowers[2*(MAX_NODES+MAX_PARTS)],source_lower=LD64(s->source+0x38);uint32_t original_count=0;
    for(uint32_t i=0;i<old_main.n;++i)original_lowers[original_count++]=LD64(old_main.nodes+i*32);
    for(uint32_t i=0;i<old_main.p;++i)original_lowers[original_count++]=LD64(LD64(old_main.parts+i*16)+0x38);
    for(uint32_t i=0;i<old_nested.n;++i)original_lowers[original_count++]=LD64(old_nested.nodes+i*32);
    for(uint32_t i=0;i<old_nested.p;++i)original_lowers[original_count++]=LD64(LD64(old_nested.parts+i*16)+0x38);
    call(c,0x70005859a0ull,s->loader,s->memory+WRAP,0,s->memory+RESULT,NULL,NULL);
    s->root=LD64(s->memory+RESULT);
    if(!range(s->root,0x98)||LD64(s->root)!=ROOT_CLASS)return fail(s,"factory-result");
    if(s->root==s->original||s->root==s->source||LD64(s->root+0x38)==source_lower){s->alias=true;return fail(s,"factory-alias");}
    s->lower=LD64(s->root+0x38);
    if(!range(s->lower,0x288))return fail(s,"factory-lower");
    for(uint32_t i=0;i<original_count;++i)if(s->lower==original_lowers[i]){
        s->alias=true;return fail(s,"factory-parent-alias");
    }
    s->outer_owned=true;
    s->nested=s->root;
    if(!source_valid(s,&old_main,&old_nested,original_source_lower))return false;
    // The new outer lower node was checked against every original lower node
    // and component before changing its visibility or making another call.
    visible(c,s->lower,false);if(!current(s))return fail(s,"factory-hide-owner");
    // A second synchronized snapshot makes the actual dev-build allocation
    // change observable. It is diagnostic; it does not reserve or alter memory.
    (void)quickstep_ui_memory_allow(c,1,"stats-created");
    if(!current(s))return fail(s,"factory-metrics-owner");
    Tree nested;
    if(!tree(s,s->nested,&nested))return false;
    if(!independent(s,&nested,&old_nested)){s->alias=true;return false;}
    s->node=find(c,s,s->nested,row_path,false);if(!current(s))return fail(s,"row-owner");
    int target=node_index(&nested,s->node);
    if(target<0)return fail(s,"nested-component-map");
    s->nested_target=(uint32_t)target;
    if(!prune(c,s,&nested,target,-1))return false;
    visible(c,LD64(s->root+0x38),false);if(!current(s)||!physical(s,false))return fail(s,"append-owner");
    memcpy(s->tail,(void*)(uintptr_t)row(s->panel,19),ROW_BYTES);s->original_base=LD32(s->panel+BASE_COUNT);s->original_shown=LD32(s->panel+SHOWN);
    plain(c,0x70001b3f20ull,s->memory+WRAP,s->nested,(uint32_t)target);
    if(!current(s)||!physical(s,false))return fail(s,"row-wrapper-owner");
    uint64_t added=plain(c,0x700023ec20ull,s->panel,s->memory+WRAP,0);
    uint64_t owned=row(s->panel,19);
    if(range(s->panel,0x1ac1)&&LD64(owned)==ROW_CLASS&&LD64(owned+8)==s->nested
        &&LD64(owned+0x18)==s->panel&&LD32(owned+0x30)==19)
        s->appended=true;
    if(s->appended&&current(s)&&LD64(s->panel+COUNT)==20){ST32(owned+0x5c,19);ST8(owned+0x34,0);}
    if(!current(s)||added!=row(s->panel,19)||LD64(s->panel+COUNT)!=20)return fail(s,"native-row-append");
    ST32(added+0x5c,19);ST8(added+0x34,0);s->appended=true;
    visible(c,s->node,false);return current(s)&&physical(s,true);
}
bool quickstep_stats_ui_ensure(Cpu*c,uint64_t panel,uint32_t character){
    uint64_t seq,ctx,generation;
    if(!g_settings.quickstep_slot||!quickstep_native_stats_context(panel,character,&seq,&ctx,&generation))return false;
    // The visible character/owner generation can change while the same panel
    // remains alive. Its immutable loaded root plus exact native row wrappers
    // below form the separate panel-lifetime identity, including during OFF.
    (void)ctx;(void)generation;
    pthread_mutex_lock(&lock);Extra s=state;
    if(s.panel){
        if(s.panel!=panel||s.busy||s.failed||!s.pending){
            bool ready=s.panel==panel&&!s.busy&&!s.failed&&s.appended;
            pthread_mutex_unlock(&lock);return ready&&physical(&s,true);
        }
        s.busy=true;s.character=character;state=s;
    }else{
        s=(Extra){.sequence=seq,.ui=seq+0x18d8,.panel=panel,.memory=state.memory,
                  .serial=++serial,.busy=true,.character=character};
        if(range(s.ui,0x370))s.original=LD64(s.ui+8);state=s;
    }
    pthread_mutex_unlock(&lock);
    if(!s.memory)s.memory=(uint64_t)(uintptr_t)hle_alloc(MEMORY);
    bool ok=range(s.memory,MEMORY)&&identity(&s);
    if(ok){if(!s.loader_owned)memset((void*)(uintptr_t)s.memory,0,MEMORY);ok=create(c,&s);}
    else s.failure="owner-or-scratch";
    if(!ok&&s.pending&&current(&s)){
        if(!s.pending_reported&&failures++<16)fprintf(stderr,"[quickstep-stats-ui] native stats dependency load pending; original rows preserved\n");
        s.pending_reported=true;
    }else if(!ok){s.pending=false;if(!release(c,&s,false))s.memory=0;s.root=s.nested=s.node=0;s.appended=false;s.failed=true;
        if(failures++<16)fprintf(stderr,"[quickstep-stats-ui] extra native row unavailable stage=%s asset_class=%llx selected_raw=%llx source_raw=%llx; original rows preserved\n",s.failure?s.failure:"unknown",(unsigned long long)s.asset_class,(unsigned long long)s.selected_raw,(unsigned long long)s.expected_raw);}
    pthread_mutex_lock(&lock);if(state.serial==s.serial){s.busy=false;state=s;}pthread_mutex_unlock(&lock);
    return ok;
}
static bool sync_path(Cpu*c,Extra*s,uint64_t copy,uint64_t original,uint32_t target,bool include){
    if(!range(copy,0x98)||!range(original,0x98))return false;
    uint64_t nodes=LD64(copy+0x68),source=LD64(original+0x68),n=LD64(copy+0x70);
    if(!n||n>MAX_NODES||LD64(original+0x70)!=n||!range(nodes,n*32)||!range(source,n*32)||target>=n)return false;
    int32_t at=include?(int32_t)target:(int32_t)LD32(nodes+target*32+0x14);
    for(uint64_t depth=0;at>=0&&depth<n;++depth){
        if((uint32_t)at>=n)return false;
        uint64_t from=LD64(source+(uint32_t)at*32),to=LD64(nodes+(uint32_t)at*32);
        if(!range(from,0x288)||!range(to,0x288))return false;
        if(range(LD64(from+0x280),0x24)){
            ST64(s->memory+WRAP,from);
            uint32_t index=(uint32_t)plain(c,0x70007499d0ull,s->memory+WRAP,0,0),time=0;
            if(!current(s))return false;
            call(c,0x7000749a10ull,s->memory+WRAP,0,0,0,NULL,&time);
            if(!current(s))return false;
            if(index!=UINT32_MAX){
                ST64(s->memory+WRAP,to);plain(c,0x7000749c00ull,s->memory+WRAP,index,0);
                if(!current(s))return false;
                float f[2]={0};memcpy(f,&time,4);call(c,0x7000749a50ull,s->memory+WRAP,0,0,0,f,NULL);
                if(!current(s))return false;
            }
        }
        visible(c,to,(LD32(from+0xb0)&1)!=0);if(!current(s))return false;
        at=(int32_t)LD32(nodes+(uint32_t)at*32+0x14);
    }
    return at==-1;
}
static bool path_visible(uint64_t root,uint32_t target,bool include){
    if(!range(root,0x98))return false;
    uint64_t nodes=LD64(root+0x68),n=LD64(root+0x70);
    if(!n||n>MAX_NODES||target>=n||!range(nodes,n*32))return false;
    int32_t at=include?(int32_t)target:(int32_t)LD32(nodes+target*32+0x14);
    for(uint64_t depth=0;at>=0&&depth<n;++depth){
        if((uint32_t)at>=n)return false;uint64_t lower=LD64(nodes+(uint32_t)at*32);
        if(!range(lower,0x288)||!(LD32(lower+0xb0)&1))return false;
        at=(int32_t)LD32(nodes+(uint32_t)at*32+0x14);
    }
    return at==-1;
}
static bool position(Cpu*c,Extra*s){
    // Selected DE2.0.0 loaded cont19 contract: the eight native effect-node
    // constants advance by24 layout units. The original/clone loaded-resource,
    // topology and all eight actual row wrappers were checked before append.
    // Computed world matrices can contain entry zoom or our previous spacing;
    // neither supplies a stable native layout distance. Fonts remain1x.
    const float step=24.f;
    for(unsigned i=0;i<8;++i){translate(c,s->rows[i],s->transforms[i][0],s->transforms[i][1]-i*step/8.f);if(!current(s)||!physical(s,true))return false;}
    // The independent copy of native row08 becomes row09 at the old final
    // baseline. Its font/scale stay1x; the eight original lines move closer.
    translate(c,s->node,s->transforms[7][0],s->transforms[7][1]);
    s->positioned=current(s)&&physical(s,true);return s->positioned;
}
void quickstep_stats_ui_layout(Cpu*c,uint64_t panel,bool ninth){
    pthread_mutex_lock(&lock);Extra s=state;if(s.panel!=panel||s.busy||!s.appended){pthread_mutex_unlock(&lock);return;}state.busy=true;pthread_mutex_unlock(&lock);
    s.wanted=ninth&&g_settings.quickstep_slot;
    if(current(&s)&&physical(&s,true)){
        if(!s.wanted)restore(c,&s,false);
        else if(sync_path(c,&s,s.nested,s.source,s.nested_target,false)
            &&(s.positioned||position(c,&s))&&current(&s)){
            visible(c,s.node,true);
            bool source_shown=path_visible(s.original,s.main_target,true)&&path_visible(s.source,s.nested_target,false);
            if(current(&s))visible(c,LD64(s.root+0x38),source_shown);
        }else if(!s.geometry_reported){
            s.geometry_reported=true;
            if(failures++<16)fprintf(stderr,"[quickstep-stats-ui] extra row awaiting native geometry/timeline; instance remains hidden\n");
        }
    }else if(current(&s)&&owned_root(&s))visible(c,s.lower,false);
    pthread_mutex_lock(&lock);if(state.serial==s.serial){s.busy=false;state=s;}pthread_mutex_unlock(&lock);
}
void quickstep_stats_ui_tick(Cpu*c){
    pthread_mutex_lock(&lock);Extra s=state;pthread_mutex_unlock(&lock);
    if(s.ui==c->x[0]&&s.pending&&!s.busy){
        if(!g_settings.quickstep_slot||!identity(&s)){quickstep_stats_ui_release_ui(c);return;}
        uint32_t ch=LD32(s.sequence+0x18b8);
        if(quickstep_stats_ui_ensure(c,s.panel,ch)){
            pthread_mutex_lock(&lock);
            if(state.serial==s.serial&&state.panel==s.panel)state.refresh_pending=true;
            pthread_mutex_unlock(&lock);
        }
        return;
    }
    if(s.ui!=c->x[0]||!s.appended||s.busy)return;
    if(s.refresh_pending&&g_settings.quickstep_slot&&identity(&s)){
        uint64_t list=LD64(s.sequence+0x4c10);
        if(range(list,0x20)&&(!LD8(list+0x19)||quickstep_native_picker_active(s.sequence))){
            pthread_mutex_lock(&lock);
            if(state.serial==s.serial)state.refresh_pending=false;
            pthread_mutex_unlock(&lock);
            quickstep_stats_refresh(c,s.sequence);
            return;
        }
    }
    bool show=s.wanted&&g_settings.quickstep_slot&&identity(&s)&&LD8(s.ui+0x21)&&LD8(s.ui+0x29)
        &&LD8(row(s.panel,19)+0x34);
    if(!show)quickstep_stats_ui_layout(c,s.panel,false);
    else quickstep_stats_ui_layout(c,s.panel,true);
}
static void close(Cpu*c,uint64_t key,int kind){
    pthread_mutex_lock(&lock);Extra s=state;
    // UI/Seq teardown may follow an already-closed panel while its helper is
    // still returning. Retire all older local leases even when state is empty.
    if(key&&kind)parent_retired=serial;
    bool match=key&&(kind==0?s.panel==key:kind==1?s.sequence==key:s.ui==key);
    uint64_t retired=0;
    // Do not recycle scratch or destroy a loader while an active helper still
    // uses it. Its local owner will reconcile after returning; a genuinely
    // retired parent takes the conservative retain path above.
    if(match){retired=++serial;state=(Extra){.serial=retired};}pthread_mutex_unlock(&lock);
    if(match&&!s.busy&&release(c,&s,true)){
        pthread_mutex_lock(&lock);
        if(state.serial==retired&&!state.panel)state.memory=s.memory;
        pthread_mutex_unlock(&lock);
    }
}
void quickstep_stats_ui_release_panel(Cpu*c){close(c,c->x[0],0);}
void quickstep_stats_ui_release_sequence(Cpu*c){close(c,c->x[0],1);}
void quickstep_stats_ui_release_ui(Cpu*c){close(c,c->x[0],2);}
RT_ON_RESET(quickstep_stats_ui_reset){pthread_mutex_lock(&lock);state=(Extra){0};++serial;parent_retired=0;failures=0;pthread_mutex_unlock(&lock);}
