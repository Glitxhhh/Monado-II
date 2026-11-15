#define _GNU_SOURCE
// [XCDE] LINUX-103: adapted from Monado-Reborn contributors (MIT),
// commit 3cc9eeb46dc879fe36929d13758754c429b8377b; see NOTICE.md.
// [XCDE] T-GRAPHICS-SCALABILITY: atomic UI -> guest render-configuration copy.
#include "../graphics_options.h"
#include "../hle.h"
#include "../settings.h"
#include <stdatomic.h>
#include <pthread.h>
#include "../prop_lod.h"
static _Atomic uint64_t request;
static _Atomic bool request_initialized;
void xcde_graphics_publish(const GraphicsOptions *o) {
    atomic_store_explicit(&request,gfx_pack(o),memory_order_release);
    atomic_store_explicit(&request_initialized,true,memory_order_release);
}
static uint64_t graphics_request(void) {
    if(!atomic_load_explicit(&request_initialized,memory_order_acquire))
        xcde_graphics_publish(&g_settings.graphics); // first guest use, before F1 changes
    return atomic_load_explicit(&request,memory_order_acquire);
}
int xcde_graphics_option(int index) { return gfx_value(graphics_request(),index); }
// Per-view configurations can be reused without another memcpy. Restore only
// the fields our previous overlay actually changed, before native camera setup.
// A new native copy supersedes that saved state. This makes On/Off -> Original
// reversible while preserving camera restrictions and authored/mod values.
static const unsigned overlay_offsets[]={0x3ad,0x17c,0x19c,0x492,0x3c,0xec,0x520,0x204,0x53d,0x368,0x47c,0x2b0,0x2b4};
static unsigned overlay_width(unsigned field) { return field<9 ? 1 : 4; }
_Static_assert(sizeof overlay_offsets/sizeof *overlay_offsets<=32,"overlay field mask");
typedef struct { uint64_t address; uint32_t changed; uint8_t before[13][4],after[13][4]; } GraphicsView;
static GraphicsView views[128];
static pthread_mutex_t view_lock=PTHREAD_MUTEX_INITIALIZER;

static int valid_view(uint64_t a) { return a>=GUEST_LO && a<=GUEST_HI-0x1220 && !(a&7); }
static GraphicsView *find_view(uint64_t a,int create) {
    GraphicsView *empty=NULL;
    for(unsigned i=0;i<sizeof views/sizeof *views;i++) {
        if(views[i].address==a)return &views[i];
        if(!views[i].address && !empty)empty=&views[i];
    }
    if(create && empty) { empty->address=a; return empty; }
    return NULL;
}
void xcde_graphics_copy(uint64_t destination) {
    if(!valid_view(destination))return;
    pthread_mutex_lock(&view_lock);
    GraphicsView *v=find_view(destination,0);
    if(v)memset(v,0,sizeof *v); // the fresh native copy already replaced our old overlay
    pthread_mutex_unlock(&view_lock);
}
void xcde_graphics_forget(uint64_t destination) { xcde_graphics_copy(destination); }
void xcde_graphics_begin(uint64_t destination) {
    if(!valid_view(destination))return;
    pthread_mutex_lock(&view_lock);
    GraphicsView *v=find_view(destination,0);
    uint8_t *p=(uint8_t *)(uintptr_t)destination;
    if(v)for(unsigned i=0;i<sizeof overlay_offsets/sizeof *overlay_offsets;i++)
        if((v->changed&(1u<<i)) && !memcmp(p+overlay_offsets[i],v->after[i],overlay_width(i)))
            memcpy(p+overlay_offsets[i],v->before[i],overlay_width(i));
    if(v)v->changed=0;
    pthread_mutex_unlock(&view_lock);
}
void xcde_graphics_finish(uint64_t destination) {
    if(!valid_view(destination))return;
    uint64_t bits=graphics_request();
    uint8_t *p=(uint8_t *)(uintptr_t)destination;
    pthread_mutex_lock(&view_lock);
    GraphicsView *v=find_view(destination,1);
    if(v) {
        for(unsigned i=0;i<sizeof overlay_offsets/sizeof *overlay_offsets;i++)memcpy(v->before[i],p+overlay_offsets[i],overlay_width(i));
        gfx_overlay(p,bits);
        v->changed=0;
        for(unsigned i=0;i<sizeof overlay_offsets/sizeof *overlay_offsets;i++)
            {
                memcpy(v->after[i],p+overlay_offsets[i],overlay_width(i));
                if(memcmp(v->before[i],v->after[i],overlay_width(i)))v->changed|=1u<<i;
            }
    }
    pthread_mutex_unlock(&view_lock);
    if(!getenv("XCDE_GRAPHICS_TRACE"))return;
    static uint64_t last=UINT64_MAX;
    if(bits==last)return;
    last=bits;
    fprintf(stderr,"[graphics-final] options=%llx cloud=%u tssao=%u ssao=%u ssr=%u bloom=%u godrays=%u shafts=%u motion=%u fur=%u\n",
        (unsigned long long)bits,p[0x3ad],p[0x17c],p[0x19c],p[0x492],p[0x3c],p[0xec],p[0x520],p[0x204],p[0x53d]);
}

// Size and LOD choices are latched on first use: UI changes require a restart,
// keeping allocations, map dimensions and newly streamed groups consistent.
static _Atomic unsigned shadow_size;
uint32_t xcde_shadow_map_size(void) {
    unsigned size=atomic_load_explicit(&shadow_size,memory_order_acquire);
    if(!size) {
        int choice=gfx_value(graphics_request(),GFX_SHADOW_MAPS);
        unsigned wanted=choice==1 || choice==2 ? 2048u : 1024u, empty=0;
        atomic_compare_exchange_strong_explicit(&shadow_size,&empty,wanted,memory_order_release,memory_order_acquire);
        size=atomic_load_explicit(&shadow_size,memory_order_acquire);
    }
    return size;
}
static int prop_choice=-1;
typedef struct { uint64_t addr,hash;uint32_t size; } PropSeen;
static PropSeen *prop_seen;
static size_t prop_count;
static pthread_mutex_t prop_mutex=PTHREAD_MUTEX_INITIALIZER;
static void prop_lod_locked(uint64_t data,uint32_t size) {
    if(prop_choice<0)prop_choice=gfx_value(graphics_request(),GFX_PROP_LOD);
    if(!prop_choice || data<GUEST_LO || size<0x70 || size>64u*1024*1024 || data>GUEST_HI-size)return;
    uint8_t *p=(uint8_t *)(uintptr_t)data;
    uint64_t hash=prop_lod_hash(p,size);
    if(!hash)return;
    size_t slot=0;for(;slot<prop_count;slot++)if(prop_seen[slot].addr==data)break;
    if(slot<prop_count && prop_seen[slot].size==size && prop_seen[slot].hash==hash)return;
    static const float scales[]={1,.5f,1.5f,2};
    if(slot==prop_count) {
        if(prop_count>=8192) {
            static bool warned;
            if(!warned){warned=true;fprintf(stderr,"[prop-lod] tracking budget reached; retaining original thresholds for extra resources\n");}
            return;
        }
        PropSeen *more=realloc(prop_seen,(prop_count+1)*sizeof(*more));
        if(!more)return;prop_seen=more;prop_count++;
    }
    prop_seen[slot]=(PropSeen){data,hash,size};
    unsigned changed=prop_lod_adjust(p,size,scales[prop_choice>=1 && prop_choice<=3 ? prop_choice : 0]);
    if(changed) {
        prop_seen[slot].addr=data;prop_seen[slot].size=size;prop_seen[slot].hash=prop_lod_hash(p,size);
        fprintf(stderr,"[prop-lod] %u intermediate thresholds adjusted; final distances preserved\n",changed);
    }
}

void xcde_prop_lod(uint64_t data,uint32_t size) {
    pthread_mutex_lock(&prop_mutex);prop_lod_locked(data,size);pthread_mutex_unlock(&prop_mutex);
}

// [XCDE] LINUX-103: all guest threads are stopped before reset callbacks run.
// Release old resource identities and restart-only latches for the new boot.
RT_ON_RESET(graphics_state_reset) {
    memset(views,0,sizeof views);
    atomic_store_explicit(&request_initialized,false,memory_order_release);
    atomic_store_explicit(&shadow_size,0,memory_order_release);
    prop_choice=-1; free(prop_seen); prop_seen=NULL; prop_count=0;
}
