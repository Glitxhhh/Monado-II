// Native production fma/fmaf dispatch; synthetic operands only, no game input.
#include <assert.h>
#include <fenv.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "../../runtime/platform/guest_fp_windows.h"
extern int monado_fma_test_supported(void),monado_fma_test_hardware(void);
extern int monado_fma_test_capable(unsigned ecx,uint64_t xcr0);
extern void monado_fma_test_software(int enabled);
typedef union {double f;uint64_t u;} D;
typedef union {float f;uint32_t u;} F;
#define COUNT 4096
static D xd[COUNT],yd[COUNT],zd[COUNT],rd[COUNT];
static F xf[COUNT],yf[COUNT],zf[COUNT],rf[COUNT];
static int flags_d[COUNT],flags_f[COUNT];
static uint64_t random_state=0x842b7166295c0de1ull;
static uint64_t random_word(void){random_state^=random_state<<13;random_state^=random_state>>7;random_state^=random_state<<17;return random_state;}
static uint64_t double_bits(double x){D value={.f=x};return value.u;}
static uint32_t float_bits(float x){F value={.f=x};return value.u;}
static DWORD WINAPI race(void *argument){
    (void)argument;assert(monado_windows_guest_fp_init());
    for(unsigned i=0;i<10000;i++){assert(fmaf(1.5f,2.f,-1.f)==2.f);assert(fma(1.5,2.,-1.)==2.);}
    return 0;
}
static void differential(void){
    for(unsigned i=0;i<COUNT;i++){
        xd[i].u=random_word();yd[i].u=random_word();zd[i].u=random_word();
        xf[i].u=(uint32_t)random_word();yf[i].u=(uint32_t)random_word();zf[i].u=(uint32_t)random_word();
    }
    // Explicit signed zero, cancellation, tiny products, overflow, infinities,
    // quiet/signalling NaNs and payloads augment the deterministic random set.
    const uint64_t edges[]={0,1,0x8000000000000000ull,0x8000000000000001ull,0xfffffffffffffull,0x10000000000000ull,
        0x3ff0000000000000ull,0x3ff0000000000001ull,0x7fefffffffffffffull,0x7ff0000000000000ull,
        0xfff0000000000000ull,0x7ff8000000001234ull,0x7ff0000000000042ull};
    const uint32_t edgesf[]={0,1,0x80000000u,0x80000001u,0x7fffffu,0x800000u,0x3f800000u,0x3f800001u,
        0x7f7fffffu,0x7f800000u,0xff800000u,0x7fc01234u,0x7f800042u};
    unsigned at=0;
    for(unsigned i=0;i<13;i++)for(unsigned j=0;j<13;j++)for(unsigned k=0;k<13;k++){
        xd[at].u=edges[i];yd[at].u=edges[j];zd[at].u=edges[k];
        xf[at].u=edgesf[i];yf[at].u=edgesf[j];zf[at].u=edgesf[k];++at;
    }
    const int rounding[]={FE_TONEAREST,FE_DOWNWARD,FE_UPWARD,FE_TOWARDZERO};
    for(unsigned r=0;r<4;r++)for(unsigned flush=0;flush<4;flush++){
        assert(monado_windows_guest_fp_init() && !fesetround(rounding[r]));
        unsigned control=_mm_getcsr()|((flush&1)?0x8000:0)|((flush&2)?0x40:0);_mm_setcsr(control);
        monado_fma_test_software(1);assert(!monado_fma_test_hardware());
        for(unsigned i=0;i<COUNT;i++){
            feclearexcept(FE_ALL_EXCEPT);rd[i].f=fma(xd[i].f,yd[i].f,zd[i].f);flags_d[i]=fetestexcept(FE_ALL_EXCEPT);
            feclearexcept(FE_ALL_EXCEPT);rf[i].f=fmaf(xf[i].f,yf[i].f,zf[i].f);flags_f[i]=fetestexcept(FE_ALL_EXCEPT);
        }
        assert((_mm_getcsr()&~0x3f)==(control&~0x3f) && fegetround()==rounding[r]);
        monado_fma_test_software(0);assert(monado_fma_test_hardware()==monado_fma_test_supported());
        for(unsigned i=0;i<COUNT;i++){
            feclearexcept(FE_ALL_EXCEPT);uint64_t got=double_bits(fma(xd[i].f,yd[i].f,zd[i].f));int flags=fetestexcept(FE_ALL_EXCEPT);
            feclearexcept(FE_ALL_EXCEPT);uint32_t gotf=float_bits(fmaf(xf[i].f,yf[i].f,zf[i].f));int flagsf=fetestexcept(FE_ALL_EXCEPT);
            if(got!=rd[i].u||gotf!=rf[i].u){
                fprintf(stderr,"dispatch mismatch rm=%u flush=%u i=%u d=%llx/%llx f=%x/%x\n",r,flush,i,(unsigned long long)got,(unsigned long long)rd[i].u,gotf,rf[i].u);abort();
            }
            if(flags!=flags_d[i]||flagsf!=flags_f[i]){fprintf(stderr,"exception mismatch rm=%u flush=%u i=%u d=%x/%x f=%x/%x\n",r,flush,i,flags,flags_d[i],flagsf,flags_f[i]);abort();}
        }
        assert((_mm_getcsr()&~0x3f)==(control&~0x3f) && fegetround()==rounding[r]);
    }
    assert(monado_windows_guest_fp_init());
}
static double ticks(void){LARGE_INTEGER n,f;assert(QueryPerformanceCounter(&n)&&QueryPerformanceFrequency(&f));return(double)n.QuadPart/(double)f.QuadPart;}
static volatile double sink;
static void benchmark(void){
    // Finite normal arithmetic resembling scalar/vector lane FMA helper work.
    // Eight independent accumulators avoid measuring a single dependency chain.
    const unsigned iterations=400000;double elapsed[2];uint64_t digest[2];
    for(int mode=0;mode<2;mode++){
        assert(monado_windows_guest_fp_init());monado_fma_test_software(mode==0);
        double d[4]={.1,.2,.3,.4};float f[4]={.1f,.2f,.3f,.4f};double start=ticks();
        for(unsigned i=0;i<iterations;i++)for(unsigned k=0;k<4;k++){
            d[k]=fma(d[k],0.9999993,0.0000001*(k+1));f[k]=fmaf(f[k],0.99999f,0.000001f*(float)(k+1));
        }
        elapsed[mode]=ticks()-start;digest[mode]=0;for(int k=0;k<4;k++){sink=d[k]+f[k];digest[mode]^=double_bits(d[k])^float_bits(f[k]);}
    }
    assert(digest[0]==digest[1]);
    printf("BENCH helpers=%u float=%u double=%u software_seconds=%.6f auto_seconds=%.6f ratio=%.3f hardware_supported=%d checksum=%llx; helper workload only, no game FPS claim\n",
        iterations*8,iterations*4,iterations*4,elapsed[0],elapsed[1],elapsed[0]/elapsed[1],monado_fma_test_supported(),(unsigned long long)digest[0]);
}
int main(void){
    const unsigned required=(1u<<12)|(1u<<26)|(1u<<27)|(1u<<28);
    assert(monado_fma_test_capable(required,6));
    for(unsigned bit=0;bit<32;bit++)if(required&(1u<<bit))assert(!monado_fma_test_capable(required&~(1u<<bit),6));
    for(uint64_t xcr=0;xcr<8;xcr++)assert(monado_fma_test_capable(required,xcr)==((xcr&6)==6));
    differential();monado_fma_test_software(0);HANDLE threads[8];
    for(unsigned i=0;i<8;i++){threads[i]=CreateThread(NULL,0,race,NULL,0,NULL);assert(threads[i]);}
    assert(WaitForMultipleObjects(8,threads,TRUE,30000)==WAIT_OBJECT_0);for(unsigned i=0;i<8;i++)CloseHandle(threads[i]);
    puts("PASS exact software/auto native differential:65536 double+65536 float results, all4rounding modes, FTZ/DAZ combinations, NaN payloads, CPU/OS capability negatives and concurrent first dispatch");
    benchmark();return 0;
}
