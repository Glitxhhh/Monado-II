// [XCDE] LINUX-103: adapted from Monado-Reborn contributors (MIT),
// commit 3cc9eeb46dc879fe36929d13758754c429b8377b; see NOTICE.md.
// [XCDE] T-PROP-LOD: bounded, selective mesh-transition edits in decoded props.
// Never shorten final visibility distances or touch meshes/instances/materials.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static inline uint32_t pl_u32(const uint8_t *p) { uint32_t v;memcpy(&v,p,4);return v; }
static inline int pl_span(size_t size,uint64_t off,uint64_t count,uint64_t stride) { return off<=size && count<=(size-off)/stride; }
typedef struct { size_t lod,props,entries,models,materials,model_base,material_base;uint32_t pc,lc,nm,nmat; } PropLodLayout;
static inline int prop_lod_layout(const uint8_t *p,size_t size,PropLodLayout *h) {
    if(size<0x70 || pl_u32(p)!=0x50000)return 0;
    h->model_base=pl_u32(p+12);h->material_base=pl_u32(p+16);h->lod=pl_u32(p+24);
    if(h->model_base<0x70 || h->model_base>=h->material_base || h->material_base>=h->lod || !pl_span(size,h->model_base,1,0x44) || !pl_span(size,h->material_base,1,8) || !pl_span(size,h->lod,1,32))return 0;
    h->pc=pl_u32(p+h->lod+4);h->lc=pl_u32(p+h->lod+12);
    h->props=h->lod+(uint64_t)pl_u32(p+h->lod+8);h->entries=h->lod+(uint64_t)pl_u32(p+h->lod+16);
    h->models=h->model_base+(uint64_t)pl_u32(p+h->model_base+28);h->nm=pl_u32(p+h->model_base+32);
    h->materials=h->material_base+(uint64_t)pl_u32(p+h->material_base);h->nmat=pl_u32(p+h->material_base+4);
    uint64_t instances=h->lod+(uint64_t)pl_u32(p+h->lod+24);uint32_t instance_count=pl_u32(p+h->lod+20);
    if(h->props<h->lod+0x70 || h->props+(uint64_t)h->pc*8>h->entries ||
       (instance_count && (h->entries+(uint64_t)h->lc*12>instances || !pl_span(size,instances,instance_count,112))))return 0;
    return h->pc>0 && h->pc<=8192 && h->lc>0 && h->lc<=65536 && h->nm>0 && h->nm<=65536 && h->nmat>0 && h->nmat<=65536 &&
        pl_span(size,h->props,h->pc,8) && pl_span(size,h->entries,h->lc,12) && pl_span(h->material_base,h->models,h->nm,68) && pl_span(h->lod,h->materials,h->nmat,116);
}
// Fingerprint only the stable LOD header/groups/thresholds. Changes elsewhere in
// a loaded resource do not cause a second multiplication on a repeated callback.
static inline uint64_t prop_lod_hash(const uint8_t *p,size_t size) {
    PropLodLayout h;if(!prop_lod_layout(p,size,&h))return 0;
    uint64_t hash=1469598103934665603ull;
    const size_t off[]={h.lod,h.props,h.entries}, len[]={24,h.pc*8u,h.lc*12u};
    for(int j=0;j<3;j++)for(size_t i=0;i<len[j];i++){hash^=p[off[j]+i];hash*=1099511628211ull;}
    return hash ? hash : 1;
}
static inline int pl_material(const uint8_t *p,size_t size,const PropLodLayout *h,unsigned index,int *strong) {
    (void)size; if(index>=h->nmat)return 0;
    size_t off=h->material_base+(uint64_t)pl_u32(p+h->materials+index*116u);
    if(off<h->material_base || off>=h->lod)return 0;
    char name[129];size_t i=0;
    for(;i<128 && off+i<h->lod && p[off+i];i++) { unsigned c=p[off+i];if(c<32 || c>126)return 0;name[i]=(char)(c>='A'&&c<='Z'?c+32:c); }
    if(!i || i==128 || off+i>=h->lod)return 0;name[i]=0;
    const char *deny[]={"bridge","ground","cliff","wall","terrain","building","iseki","doukutu","nopon","lamp","glass","camp_"};
    for(unsigned j=0;j<sizeof deny/sizeof *deny;j++)if(strstr(name,deny[j]))return 0;
    int match=strstr(name,"tree") || strstr(name,"ivy") || strstr(name,"tuta") || strstr(name,"leaf") || strstr(name,"rock") || strstr(name,"iwa");
    if(match)*strong=1;
    return match || !strncmp(name,"mlo_p",5) || strstr(name,"branch") || strstr(name,"brunch");
}
static inline int pl_model(const uint8_t *p,size_t size,const PropLodLayout *h,unsigned index,int *strong) {
    if(index>=h->nm)return 0;
    const uint8_t *m=p+h->models+index*68u;
    uint32_t count=pl_u32(m+4);uint64_t off=h->model_base+(uint64_t)pl_u32(m);
    if(!count || count>1024 || !pl_span(h->material_base,off,count,48))return 0;
    for(unsigned j=0;j<count;j++) {
        uint16_t mat;memcpy(&mat,p+off+j*48u+14,2);
        if(!pl_material(p,size,h,mat,strong))return 0;
    }
    return 1;
}
static inline unsigned prop_lod_adjust(uint8_t *p,size_t size,float scale) {
    if(scale==1 || !isfinite(scale) || scale<.5f || scale>2)return 0;
    PropLodLayout h;if(!prop_lod_layout(p,size,&h))return 0;
    uint16_t *refs=calloc(h.lc,sizeof *refs);float *planned=calloc(h.lc,sizeof *planned);
    if(!refs || !planned){free(refs);free(planned);return 0;}
    unsigned changed=0;
    for(unsigned i=0;i<h.pc;i++) {
        const uint8_t *g=p+h.props+i*8u;uint32_t first=pl_u32(g),n=pl_u32(g+4);
        if(!n || first>=h.lc || n>h.lc-first)goto done; // malformed layout: no writes
        for(unsigned j=0;j<n;j++)if(refs[first+j]<65535)refs[first+j]++;
    }
    for(unsigned i=0;i<h.pc;i++) {
        const uint8_t *g=p+h.props+i*8u;uint32_t first=pl_u32(g),n=pl_u32(g+4);
        if(n<2)continue;
        int strong=0,valid=1;float prev=0,final;
        memcpy(&final,p+h.entries+(first+n-1)*12u+4,4);
        if(!isfinite(final) || final<=0)continue;
        for(unsigned j=0;j<n;j++) {
            const uint8_t *e=p+h.entries+(first+j)*12u;float distance;memcpy(&distance,e+4,4);
            if(!isfinite(distance) || distance<=0 || distance<prev || !pl_model(p,size,&h,pl_u32(e+8),&strong)){valid=0;break;}
            prev=distance;
        }
        if(!valid || !strong)continue;
        for(unsigned j=0;j+1<n;j++)if(refs[first+j]==1) {
            float old;memcpy(&old,p+h.entries+(first+j)*12u+4,4);
            planned[first+j]=fminf(old*scale,nextafterf(final,0));
        }
    }
    for(unsigned i=0;i<h.lc;i++)if(planned[i]>0) {
        float old;memcpy(&old,p+h.entries+i*12u+4,4);
        if(old!=planned[i]){memcpy(p+h.entries+i*12u+4,planned+i,4);changed++;}
    }
done:
    free(refs);free(planned);return changed;
}
