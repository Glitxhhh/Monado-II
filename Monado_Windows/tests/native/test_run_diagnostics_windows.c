// Authored native diagnostic records/settings fixture; no game/profile/input.
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include "../../runtime/host/run_diagnostics_windows.h"
#include "../../runtime/settings.h"
bool bind_settings_load(const char *k,const char *v){(void)k;(void)v;return false;}
void bind_settings_save(FILE *f){(void)f;}
int bind_slot_from_name(const char *n){(void)n;return -1;}
static unsigned probe[3];
void run_diag_test_phase(unsigned phase){
    assert(phase==1||phase==2);probe[phase]++;
    assert(!MoveFileExW(L"logs/game-runs",L"logs/forbidden-renamed",0));
    assert(!MoveFileExW(L"logs",L"forbidden-parent-renamed",0));
}
static unsigned files(wchar_t first[512]) {
    WIN32_FIND_DATAW e;HANDLE h=FindFirstFileW(L"logs/game-runs/run-*.jsonl",&e);unsigned n=0;
    if(h!=INVALID_HANDLE_VALUE){do{if(!n&&first)swprintf(first,512,L"logs/game-runs/%ls",e.cFileName);n++;}while(FindNextFileW(h,&e));FindClose(h);}return n;
}
static void put(const wchar_t *p,const char *s){FILE *f=_wfopen(p,L"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(!fclose(f));}
int wmain(int argc,wchar_t **argv) {
    assert(argc==3&&SetCurrentDirectoryW(argv[1]));const wchar_t *scenario=argv[2];
    if(!wcscmp(scenario,L"precompile")){
        assert(!run_diag_begin(true));run_diag_settings("unused");run_diag_end();assert(GetFileAttributesW(L"logs")==INVALID_FILE_ATTRIBUTES);
    }else if(!wcscmp(scenario,L"normal")){
        put(L"xcde.ini","fullscreen = 1\nwindow = 2560x1440\nupscaler = dlss\nupscale_quality = native\nfps = 120\nframe_gen = 1\nframe_gen_match_display = 1\nshow_fps_overlay = 0\ncheat_exp = 1\ncheat_exp_percent = 225\ninput_rumble = 1\nprop_lod = high\nbind_key_a = TOP_SECRET_BINDING\nprivate_key = TOP_SECRET_PATH\nvolume = TOP_SECRET_INVALID\n");
        assert(run_diag_begin(false));settings_load();assert(g_settings.fps==120&&g_settings.frame_gen&&!g_settings.show_fps_overlay);
        Settings before=g_settings;run_diag_renderer("present","authored GPU\nforged=1\"","fifo",2560,1440,179.88f,89.94f,89.94f,true);
        run_diag_pacing(89.94f,false);run_diag_limiter(0,.997f,.997f);run_diag_limiter(1,.997f,.997f*(60.f/89.94f));
        run_diag_sample(1,true,false,0,false,false,0,0,false);
        run_diag_sample(1000000000ull,true,true,90,true,true,180,180,true);
        run_diag_sample(2000000001ull,true,true,89.9,true,true,179.8,360,true);
        assert(!memcmp(&before,&g_settings,sizeof before));assert(probe[1]&&probe[2]);run_diag_end();assert(files(NULL)==1);
    }else if(!wcscmp(scenario,L"bounded")){
        assert(run_diag_begin(false));for(int i=0;i<1000;i++){g_settings.fps=i;run_diag_settings("fixture-settings");run_diag_renderer("present","authored","fifo",1,2,180,(float)i,90,true);run_diag_pacing((float)i,false);run_diag_limiter(1,.997f,.997f/(i+1));run_diag_sample((uint64_t)(i+1)*2000000000ull,true,true,90,true,true,180,(uint64_t)i,true);}run_diag_end();assert(files(NULL)==1);
    }else if(!wcscmp(scenario,L"reparse")){
        assert(CreateDirectoryW(L"outside",NULL));wchar_t cwd[4096],target[4096];assert(GetCurrentDirectoryW(4096,cwd));swprintf(target,4096,L"%ls\\outside",cwd);
        // Runner creates the actual junction; this case checks an existing one.
        (void)target;assert(!run_diag_begin(false));run_diag_end();assert(files(NULL)==0);
    }else{
        for(unsigned i=0;i<20;i++){assert(run_diag_begin(false));run_diag_end();}assert(files(NULL)==16);
        if(!wcscmp(scenario,L"retention")){puts("retention16 PASS");return 0;}
        wchar_t first[512];assert(files(first)==16);HANDLE held=INVALID_HANDLE_VALUE;
        if(!wcscmp(scenario,L"modified")){FILE *f=_wfopen(first,L"ab");assert(f);fputs("modified",f);fclose(f);}
        else if(!wcscmp(scenario,L"locked")){held=CreateFileW(first,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);assert(held!=INVALID_HANDLE_VALUE);}
        else if(!wcscmp(scenario,L"hardlink"))assert(CreateHardLinkW(L"protected-hardlink",first,NULL));
        else if(!wcscmp(scenario,L"unknown"))put(L"logs/game-runs/human-note","keep unchanged");
        else assert(!"unknown scenario");
        assert(!run_diag_begin(false));run_diag_end();assert(files(NULL)==16);
        if(held!=INVALID_HANDLE_VALUE)CloseHandle(held);
    }
    puts("native run diagnostics PASS");return 0;
}
