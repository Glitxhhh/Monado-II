#define _GNU_SOURCE
// [XCDE] LINUX-103: adapted from Monado-Reborn contributors (MIT),
// commit 3cc9eeb46dc879fe36929d13758754c429b8377b; see NOTICE.md.
// Named grass choices are latched once per boot. Restart is explicit because
// the expanded pool cannot be resized while guest resources remain in flight.
#include "foliage_distance.h"
#include "../grass_lod.h"
#include "../settings.h"
#include "../hle.h"
#include <stdatomic.h>
static _Atomic float active_scale;
float xcde_foliage_active_scale(void) {
    float scale=atomic_load_explicit(&active_scale,memory_order_acquire);
    if(scale==0.f) {
        float wanted=grass_lod_scale(g_settings.grass_lod),empty=0.f;
        atomic_compare_exchange_strong_explicit(&active_scale,&empty,wanted,memory_order_release,memory_order_acquire);
        scale=atomic_load_explicit(&active_scale,memory_order_acquire);
    }
    return scale;
}
bool xcde_foliage_expanded(void) { return xcde_foliage_active_scale()>1.f; }
float xcde_foliage_requested_scale(void) { return xcde_foliage_active_scale(); }
float xcde_foliage_distance(float near_r,float far_r) {
    return xcde_foliage_far_scaled(near_r,far_r,xcde_foliage_active_scale());
}
float xcde_foliage_shading_distance(float near_r,float far_r) { return xcde_foliage_distance(near_r,far_r); }
uint32_t xcde_foliage_pool_layout(int part) {
    uint32_t bitmap=xcde_foliage_expanded()?XCDE_FOLIAGE_BITMAP_BYTES:0x2000u;
    uint32_t pool=xcde_foliage_expanded()?XCDE_FOLIAGE_POOL_BYTES:0x380000u;
    uint32_t metadata=bitmap+pool,scratch=metadata+XCDE_FOLIAGE_METADATA_BYTES;
    switch(part) {
    case 0:return scratch+0x3a20u;
    case 1:return bitmap;
    case 2:return scratch;
    case 3:return scratch+0x1e00u;
    case 4:return metadata;
    case 5:return pool;
    default:return 0;
    }
}
#define FOLIAGE_ARENA_BASE (HLE_HEAP+HLE_HEAP_SIZE)
#define FOLIAGE_ARENA_STRIDE 0x02000000ull
#define FOLIAGE_ARENA_SLOTS 8
_Static_assert(FOLIAGE_ARENA_BASE+FOLIAGE_ARENA_STRIDE*FOLIAGE_ARENA_SLOTS<=ALIAS_BASE,"foliage arena overlaps alias memory");
_Static_assert(XCDE_FOLIAGE_BITMAP_BYTES+XCDE_FOLIAGE_POOL_BYTES+XCDE_FOLIAGE_METADATA_BYTES+0x3a20u<=FOLIAGE_ARENA_STRIDE,"foliage allocation exceeds slot");
static uint64_t owners[FOLIAGE_ARENA_SLOTS];
static pthread_mutex_t arena_lock=PTHREAD_MUTEX_INITIALIZER;
uint64_t xcde_foliage_pool_alloc(uint64_t owner) {
    if(!xcde_foliage_expanded())return 0; // original/low use the original allocator and exact native layout
    if(!owner)FATAL("foliage allocation without owner\n");
    pthread_mutex_lock(&arena_lock);
    unsigned slot;
    for(slot=0;slot<FOLIAGE_ARENA_SLOTS;slot++)if(owners[slot]==owner) {
        pthread_mutex_unlock(&arena_lock);FATAL("duplicate foliage owner allocation\n");
    }
    for(slot=0;slot<FOLIAGE_ARENA_SLOTS && owners[slot];slot++){}
    if(slot==FOLIAGE_ARENA_SLOTS) {pthread_mutex_unlock(&arena_lock);FATAL("too many simultaneous foliage maps\n");}
    uint64_t base=FOLIAGE_ARENA_BASE+slot*FOLIAGE_ARENA_STRIDE;
    plat_commit(base,xcde_foliage_pool_layout(0));owners[slot]=owner;
    pthread_mutex_unlock(&arena_lock);return base;
}
bool xcde_foliage_pool_free(uint64_t address) {
    if(address<FOLIAGE_ARENA_BASE || (address-FOLIAGE_ARENA_BASE)%FOLIAGE_ARENA_STRIDE)return false;
    uint64_t slot=(address-FOLIAGE_ARENA_BASE)/FOLIAGE_ARENA_STRIDE;
    if(slot>=FOLIAGE_ARENA_SLOTS)return false;
    pthread_mutex_lock(&arena_lock);
    if(!owners[slot]) {pthread_mutex_unlock(&arena_lock);FATAL("double release of foliage storage\n");}
    if(!plat_reset_guest(address,address+FOLIAGE_ARENA_STRIDE)) {pthread_mutex_unlock(&arena_lock);FATAL("could not release foliage storage\n");}
    owners[slot]=0;pthread_mutex_unlock(&arena_lock);return true;
}
RT_ON_RESET(foliage_state_reset) {
    memset(owners,0,sizeof owners);
    atomic_store_explicit(&active_scale,0.f,memory_order_release);
}
