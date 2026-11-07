#!/usr/bin/env python3
"""[XCDE] LINUX-020: actual monotonic helper and extracted ordinary-WSI control flow; authored Vulkan mocks, no GPU/game/UI."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import resource
import subprocess
import time

ROOT = Path(__file__).resolve().parents[1]
SOURCES = ('scripts/framegen_presentation_tests_linux.py', 'tests/native/test_framegen_pacing_linux.c',
           'runtime/nvn/vulkan/framegen_pacing_linux.h', 'runtime/nvn/vulkan/device.c',
           'tests/native/test_temporal_framegen_linux.c', 'runtime/nvn/vulkan/temporal_fg_linux.inc',
           'runtime/nvn/vulkan/framegen_linux.h', 'runtime/settings.h', 'runtime/paths.h')

HARNESS = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>
typedef struct {VkBuffer buf;uint64_t off;void *ptr;} VkSpan;
static struct {VkQueue queue;VkPhysicalDevice phys;VkSwapchainKHR swapchain;VkFormat sc_format;VkExtent2D sc_extent;VkImage sc_images[8];} g_vk;
static struct {uint32_t image;VkSemaphore acquired;bool ok;} cur_frame;
static VkSemaphore done_sem[8];
static bool fg_present_second,rendering,hash_flag,flicker_flag,make_pair;
static unsigned scenario,presents,submits,copies,blits,redraws,acquires,records,waits,resumes,shots,hashes,recreates,pace_waits,invalidations;
static uint64_t fg_submitted_generated,fg_submitted_real;
static unsigned colors[8],present_colors[8];
static uint64_t sequence=1;
static char events[256];static unsigned n_events;
static void event(char c){assert(n_events+1<sizeof events);events[n_events++]=c;events[n_events]=0;}
static int mock_clock(clockid_t c,struct timespec *t){assert(c==CLOCK_MONOTONIC);*t=(struct timespec){4,0};event('T');return 0;}
static int mock_sleep(clockid_t c,int flags,const struct timespec *t,struct timespec *remain){
 assert(c==CLOCK_MONOTONIC&&flags==TIMER_ABSTIME&&!remain);assert(t->tv_sec==4&&t->tv_nsec==8000000);pace_waits++;event('D');return 0;}
#define clock_gettime mock_clock
#define clock_nanosleep mock_sleep
#include "PACING_HEADER"
#undef clock_gettime
#undef clock_nanosleep
#define LOG(...) do{}while(0)
static const char *volatile g_rq_where="caller";
static void vk_rq_at(const char *where){g_rq_where=where;}
static VkCommandBuffer vk_cmd(void){return (VkCommandBuffer)(uintptr_t)0x10;}
static uint64_t vk_pending_seq(void){return sequence;}
static void vk_wait_seq(uint64_t seq){assert(seq>0&&seq<sequence);waits++;event('W');}
static void vk_full_barrier(VkCommandBuffer cb){assert(cb==vk_cmd()&&!rendering);}
static unsigned image_index(VkImage image){for(unsigned i=0;i<8;i++)if(g_vk.sc_images[i]==image)return i;assert(!"unknown image");return 0;}
static void image_barrier(VkCommandBuffer cb,VkImage im,VkImageLayout from,VkImageLayout to){(void)from;(void)to;assert(cb==vk_cmd()&&!rendering);image_index(im);}
static bool diag_hash_wanted(void){return hash_flag;}
bool diag_flicker_wanted(void){return flicker_flag;}
static uint64_t diag_frame(void){return 4;}
static void diag_frame_hash(uint64_t frame,const uint8_t *p,uint32_t w,uint32_t h){assert(frame==5&&p&&w&&h);hashes++;}
void vk_save_png(const char *path,const uint8_t *p,uint32_t w,uint32_t h){assert(path&&p&&w&&h);shots++;}
void diag_flicker_check(uint64_t frame,const uint8_t *p,uint32_t w,uint32_t h,void(*save)(const char*,const uint8_t*,uint32_t,uint32_t)){
 assert(frame==5&&p&&w&&h&&save==vk_save_png);}
static void save_window_image(VkCommandBuffer cb,VkSpan *shot){static uint8_t data[4096];assert(cb==vk_cmd()&&!rendering);shot->ptr=data;}
static VkSemaphore binary_semaphore(void){static uintptr_t id=0x90;return (VkSemaphore)id++;}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cb){assert(cb==vk_cmd()&&rendering);rendering=false;event('E');}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageCopy *copy){
 assert(cb==vk_cmd()&&!rendering&&src==(VkImage)(uintptr_t)0x200&&sl==VK_IMAGE_LAYOUT_GENERAL&&dl==VK_IMAGE_LAYOUT_GENERAL&&count==1);
 assert(copy->extent.width<=g_vk.sc_extent.width&&copy->extent.height<=g_vk.sc_extent.height);
 colors[image_index(dst)]=2;copies++;event('C');}
static void vk_frame_resume(void){assert(!rendering);rendering=true;resumes++;event('R');}
void tmp_fg_invalidate(void){invalidations++;}
void vk_present_redraw(VkCommandBuffer cb){assert(cb==vk_cmd()&&rendering);colors[cur_frame.image]=2;redraws++;event('B');}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice physical,VkFormat format,VkFormatProperties *properties){
 assert(physical==g_vk.phys&&(format==VK_FORMAT_B8G8R8A8_UNORM||format==VK_FORMAT_R8G8B8A8_UNORM));memset(properties,0,sizeof *properties);
 if(scenario!=10)properties->optimalTilingFeatures=VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT;}
VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cb,VkImage src,VkImageLayout sl,VkImage dst,VkImageLayout dl,uint32_t count,const VkImageBlit *region,VkFilter filter){
 assert(cb==vk_cmd()&&!rendering&&src==(VkImage)(uintptr_t)0x200&&sl==VK_IMAGE_LAYOUT_GENERAL&&dl==VK_IMAGE_LAYOUT_GENERAL&&count==1&&filter==VK_FILTER_NEAREST);
 assert(region->srcOffsets[1].x==4&&region->srcOffsets[1].y==4);
 assert(region->dstOffsets[1].x==(int32_t)g_vk.sc_extent.width&&region->dstOffsets[1].y==(int32_t)g_vk.sc_extent.height);
 colors[image_index(dst)]=2;blits++;event('L');}
bool tmp_fg_record_present(VkCommandBuffer cb,VkImage scene,VkImage *base,float *ms){
 assert(cb==vk_cmd()&&!rendering);records++;event('F');if(!make_pair)return false;
 colors[image_index(scene)]=1;*base=(VkImage)(uintptr_t)0x200;*ms=16.f;return true;}
static void vk_submit_extra(VkSemaphore wait,VkSemaphore signal,VkCommandBuffer extra){
 assert(!rendering&&wait==cur_frame.acquired&&signal==done_sem[cur_frame.image]&&!extra);submits++;sequence++;event('S');}
static VkResult present_mock(VkQueue queue,const VkPresentInfoKHR *p){
 assert(queue==g_vk.queue&&p->waitSemaphoreCount==1&&p->swapchainCount==1&&*p->pSwapchains==g_vk.swapchain);
 assert(*p->pWaitSemaphores==done_sem[*p->pImageIndices]);assert(submits==presents+1);
 present_colors[presents]=colors[*p->pImageIndices];presents++;event('P');
 if(presents==1&&scenario==2)return VK_ERROR_OUT_OF_DATE_KHR;
 if(presents==1&&scenario==8)return VK_SUBOPTIMAL_KHR;
 return VK_SUCCESS;}
static PFN_vkQueuePresentKHR sc_present=present_mock;
static void recreate_swapchain(void){recreates++;g_vk.swapchain=(VkSwapchainKHR)(uintptr_t)0x35;}
static bool vk_frame_begin(const float clear[4]){
 assert(clear[0]==0&&clear[1]==0&&clear[2]==0&&!cur_frame.ok&&!rendering);acquires++;event('A');
 if(scenario==3)return false;
 cur_frame.image=scenario==9?0:1;cur_frame.acquired=(VkSemaphore)(uintptr_t)(0x71+acquires);cur_frame.ok=true;rendering=true;colors[cur_frame.image]=0;
 if(scenario==4){g_vk.sc_extent=(VkExtent2D){6,6};g_vk.swapchain=(VkSwapchainKHR)(uintptr_t)0x36;}
 if(scenario==5||scenario==10){g_vk.sc_format=VK_FORMAT_R8G8B8A8_UNORM;g_vk.swapchain=(VkSwapchainKHR)(uintptr_t)0x37;}
 return true;}
#include "FRAME_END_SOURCE"
static void reset(unsigned mode){
 memset(&g_vk,0,sizeof g_vk);memset(done_sem,0,sizeof done_sem);memset(colors,0,sizeof colors);memset(present_colors,0,sizeof present_colors);
 g_vk.queue=(VkQueue)(uintptr_t)0x20;g_vk.phys=(VkPhysicalDevice)(uintptr_t)0x21;g_vk.swapchain=(VkSwapchainKHR)(uintptr_t)0x30;g_vk.sc_format=VK_FORMAT_B8G8R8A8_UNORM;g_vk.sc_extent=(VkExtent2D){4,4};
 for(unsigned i=0;i<8;i++)g_vk.sc_images[i]=(VkImage)(uintptr_t)(0x100+i);
 cur_frame.image=0;cur_frame.acquired=(VkSemaphore)(uintptr_t)0x70;cur_frame.ok=true;rendering=true;colors[0]=2;
 scenario=mode;fg_present_second=false;hash_flag=false;flicker_flag=false;make_pair=true;sequence=1;
 presents=submits=copies=blits=redraws=acquires=records=waits=resumes=shots=hashes=recreates=pace_waits=invalidations=n_events=0;
 fg_submitted_generated=fg_submitted_real=0;events[0]=0;
}
int main(int argc,char **argv){
 assert(argc==2);unsigned mode=(unsigned)strtoul(argv[1],NULL,10);reset(mode);
 if(mode==1)make_pair=false;if(mode==7)hash_flag=true;
 vk_frame_end(mode==6?"authored.png":NULL);
 assert(!cur_frame.ok&&!rendering&&!fg_present_second);
 if(mode==0||mode==9){assert(presents==2&&submits==2&&records==1&&copies==1&&acquires==1&&pace_waits==1&&waits==1);
   assert(present_colors[0]==1&&present_colors[1]==2);assert(strchr(events,'W')<strchr(events,'P')&&strchr(events,'D')<strchr(events,'A'));}
 else if(mode==1){assert(presents==1&&records==1&&!copies&&!waits&&!pace_waits&&!acquires);assert(present_colors[0]==2);}
 else if(mode==2||mode==8){assert(presents==1&&recreates==1&&!acquires&&!pace_waits);}
 else if(mode==3){assert(presents==1&&acquires==1&&!copies&&pace_waits==1);}
 else if(mode==4||mode==5){assert(presents==2&&blits==1&&!copies&&!redraws&&present_colors[1]==2);}
 else if(mode==10){assert(presents==2&&!blits&&!copies&&redraws==1&&present_colors[1]==2);}
 else if(mode==6){assert(presents==1&&!records&&shots==1&&!copies&&!acquires);}
 else if(mode==7){assert(presents==1&&!records&&hashes==1&&!copies&&!acquires);}
 else assert(!"unknown case");
 if(mode==0||mode==4||mode==5||mode==9||mode==10)assert(fg_submitted_generated==1&&fg_submitted_real==1);
 if(mode==1||mode==6||mode==7)assert(fg_submitted_generated==0&&fg_submitted_real==1);
 printf("actual vk_frame_end scenario %u passed: %s\n",mode,events);return 0;
}
'''


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--pacing-only', action='store_true')
    args = parser.parse_args()
    out = args.output.absolute()
    if out != out.resolve() or out.exists() or not out.is_relative_to(ROOT / 'runs'):
        parser.error('fresh canonical output below Linux runs/ required')
    out.mkdir(parents=True); os.umask(0o077); resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = {'PATH': '/usr/bin:/bin', 'LANG': 'C.UTF-8', 'LC_ALL': 'C.UTF-8', 'TMPDIR': str(out)}
    cc = ROOT / '.deps/bin/clang-cc'
    sysroot = Path((ROOT / '.deps/graphics/active-sysroot.txt').read_text().strip())
    receipt = {'scope': __doc__, 'status': 'running', 'host': list(platform.uname()), 'argv': os.sys.argv,
               'compiler_sha256': digest(cc), 'sources_before': {s: digest(ROOT / s) for s in SOURCES}, 'commands': []}

    def save():
        (out / 'receipt.json').write_text(json.dumps(receipt, indent=2) + '\n')

    def run(name, argv, expected=0, timeout=10):
        row = {'name': name, 'argv': list(map(str, argv)), 'cwd': str(ROOT), 'environment': env,
               'timeout_seconds': timeout, 'expected_exit': expected}
        receipt['commands'].append(row); save(); started = time.monotonic()
        child = subprocess.run(row['argv'], cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        row.update(exit=child.returncode, seconds=time.monotonic() - started)
        (out / (name + '.stdout.log')).write_bytes(child.stdout)
        (out / (name + '.stderr.log')).write_bytes(child.stderr); save()
        if child.returncode != expected:
            raise RuntimeError(name + ': ' + child.stdout.decode(errors='replace') + child.stderr.decode(errors='replace'))
        return child.stdout.decode()

    try:
        flags = ['-O1', '-UNDEBUG', '-march=x86_64_v2', '-ffp-contract=off', '-fno-strict-aliasing',
                 '-Werror=implicit-function-declaration', '-I' + str(sysroot / 'usr/include')]
        for mode, define in [('mock', []), ('real', ['-DFRAMEGEN_PACING_REAL_CLOCK'])]:
            binary = out / ('pacing-' + mode)
            run('compile-pacing-' + mode, [cc, *flags, *define, ROOT / SOURCES[1], '-lm', '-o', binary], timeout=30)
            print(run('pacing-' + mode, [binary]), end='')
        if not args.pacing_only:
            run('compile-temporal-adapter', [cc, *flags, ROOT / SOURCES[4], '-lm', '-o', out / 'temporal-adapter'], timeout=30)
            print(run('temporal-adapter', [out / 'temporal-adapter']), end='')
            source = (ROOT / SOURCES[3]).read_text()
            start = source.index('void vk_frame_end(const char *shot_path) {')
            end = source.index('\nvoid vk_save_png(', start)
            extracted = source[start:end]
            (out / 'device-frame-end.inc').write_text(extracted)
            harness = HARNESS.replace('PACING_HEADER', str(ROOT / SOURCES[2])).replace('FRAME_END_SOURCE', str(out / 'device-frame-end.inc'))
            harness = harness.replace('#include <stdio.h>', '#include <stdio.h>\n#include <stdlib.h>')
            (out / 'presentation.c').write_text(harness)
            run('compile-presentation', [cc, *flags, '-DXCDE_LINUX_FRAMEGEN', out / 'presentation.c', '-lm', '-o', out / 'presentation'], timeout=30)
            for mode in range(11):
                print(run('presentation-' + str(mode), [out / 'presentation', str(mode)]), end='')
        receipt['sources_after'] = {s: digest(ROOT / s) for s in SOURCES}
        if receipt['sources_before'] != receipt['sources_after']:
            raise RuntimeError('sources changed during focused tests')
        receipt['status'] = 'passed'
    except Exception as error:
        receipt.update(status='failed', error=str(error)); raise
    finally:
        save(); print(out)


if __name__ == '__main__':
    main()
