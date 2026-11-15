// [XCDE] Native Windows adaptation of accepted LINUX-099 diagnostics + immutable
// run_settings_log policy. No Linux driver environment implies Windows support.
#include "run_diagnostics_windows.h"
#include "../settings.h"
#include "../platform/windows_library_identity.h"
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <stdatomic.h>

enum { RD_MAX=32768, RD_RUNS=16, RD_ENTRIES=64 };
static const char rd_owner[]="{\"schema\":1,\"owner\":\"monado-windows-game-runs\"}\n";
static SRWLOCK rd_lock=SRWLOCK_INIT;
static HANDLE rd_file=INVALID_HANDLE_VALUE,rd_lease=INVALID_HANDLE_VALUE;
static WsGuard rd_guard;
static char rd_id[33]="direct",rd_data[RD_MAX],rd_directory[16384];
static unsigned rd_used,rd_rows[4];
static atomic_bool rd_started;
static bool rd_limited;
static uint64_t rd_sample_time;
static char rd_last[4][2048];

static bool rd_digest(const void *data,size_t size,char out[65]) {
    BCRYPT_ALG_HANDLE a=NULL;BCRYPT_HASH_HANDLE h=NULL;unsigned char digest[32];bool ok=false;
    if(BCryptOpenAlgorithmProvider(&a,BCRYPT_SHA256_ALGORITHM,NULL,0)<0 ||
       BCryptCreateHash(a,&h,NULL,0,NULL,0,0)<0 || BCryptHashData(h,(PUCHAR)data,(ULONG)size,0)<0 ||
       BCryptFinishHash(h,digest,sizeof digest,0)<0)goto done;
    for(unsigned i=0;i<32;i++)sprintf(out+2*i,"%02x",digest[i]);out[64]=0;ok=true;
done:if(h)BCryptDestroyHash(h);if(a)BCryptCloseAlgorithmProvider(a,0);return ok;
}
static unsigned long long rd_utc(void) {
    FILETIME f;GetSystemTimeAsFileTime(&f);ULARGE_INTEGER n={.LowPart=f.dwLowDateTime,.HighPart=f.dwHighDateTime};
    return n.QuadPart>=116444736000000000ull?(n.QuadPart-116444736000000000ull)/10000000ull:0;
}
static void rd_safe(char *out,size_t cap,const char *in) {
    size_t n=0;for(;in&&in[n]&&n+1<cap;n++){unsigned char c=in[n];out[n]=c>=32&&c<127&&c!='"'&&c!='\\'?(char)c:'?';}out[n]=0;
}
static double rd_number(double n){return isfinite(n)?n:-1.;}
static const char *rd_method(void){return g_settings.frame_gen||g_settings.cutscene_mode==2?"windows-sdk-requested":g_settings.frame_gen_smooth_motion?"external-selected-unavailable":"off";}
static bool rd_write(const void *bytes,unsigned size) {
    const char *p=bytes;while(size){DWORD put=0;if(!WriteFile(rd_file,p,size,&put,NULL)||!put)return false;p+=put;size-=put;}return true;
}
static void rd_emit_locked(const char *row) {
    fprintf(stderr,"[run] id=%s %s",rd_id,row);
    size_t n=strlen(row);
    if(rd_file==INVALID_HANDLE_VALUE)return;
    if(rd_used+n+256>=RD_MAX){rd_limited=true;return;}
    if(rd_write(row,(unsigned)n)){memcpy(rd_data+rd_used,row,n);rd_used+=(unsigned)n;}
    else {CloseHandle(rd_file);rd_file=INVALID_HANDLE_VALUE;}
}
static void rd_emit(const char *format,...) {
    char row[2048];va_list ap;va_start(ap,format);int n=vsnprintf(row,sizeof row,format,ap);va_end(ap);
    if(n<0||n>=(int)sizeof row)return;AcquireSRWLockExclusive(&rd_lock);if(rd_started)rd_emit_locked(row);ReleaseSRWLockExclusive(&rd_lock);
}
static void rd_change(unsigned category,const char *row) {
    AcquireSRWLockExclusive(&rd_lock);
    if(rd_started&&strcmp(row,rd_last[category])){
        snprintf(rd_last[category],sizeof rd_last[category],"%s",row);
        if(rd_rows[category]<32){rd_rows[category]++;rd_emit_locked(row);}
    }ReleaseSRWLockExclusive(&rd_lock);
}
static HANDLE rd_open(const char *path,DWORD access,DWORD share,DWORD creation,bool directory) {
    WsGuard g;if(ws_guard_open(&g,path))return INVALID_HANDLE_VALUE;
    HANDLE h=CreateFileW(g.path,access|READ_CONTROL,share,NULL,creation,FILE_FLAG_OPEN_REPARSE_POINT|(directory?FILE_FLAG_BACKUP_SEMANTICS:0),NULL);
    if(h!=INVALID_HANDLE_VALUE&&(!ws_plain(h,directory,!directory)||!wl_owned(h))){CloseHandle(h);h=INVALID_HANDLE_VALUE;}
    ws_guard_close(&g);return h;
}
static bool rd_directory_open(const char *path) {
    if(ws_mkdir(path,0700)&&errno!=EEXIST)return false;
    HANDLE h=rd_open(path,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,OPEN_EXISTING,true);
    if(h==INVALID_HANDLE_VALUE)return false;CloseHandle(h);return true;
}
static bool rd_read(HANDLE h,char *out,unsigned max,unsigned *size) {
    LARGE_INTEGER n;DWORD got=0;
    if(!GetFileSizeEx(h,&n)||n.QuadPart<0||n.QuadPart>max||!ReadFile(h,out,(DWORD)n.QuadPart,&got,NULL)||got!=n.QuadPart)return false;
    *size=got;return true;
}
static bool rd_name(const wchar_t *name) {
    if(wcslen(name)!=42||wcsncmp(name,L"run-",4)||wcscmp(name+36,L".jsonl"))return false;
    for(unsigned i=4;i<36;i++)if(!((name[i]>=L'0'&&name[i]<=L'9')||(name[i]>=L'a'&&name[i]<=L'f')))return false;return true;
}
static bool rd_record_valid(HANDLE h,const wchar_t *name) {
    char data[RD_MAX+1],prefix[128],hex[65],id[33];unsigned n;
    if(!rd_read(h,data,RD_MAX,&n))return false;data[n]=0;
    for(unsigned i=0;i<32;i++)id[i]=(char)name[i+4];id[32]=0;
    snprintf(prefix,sizeof prefix,"{\"schema\":1,\"owner\":\"monado-windows-game-run\",\"run_id\":\"%s\"",id);
    static const char footer[]="{\"event\":\"record-sealed\",\"sha256\":\"";
    size_t tail=strlen(footer)+64+3;
    if(n<=tail||memcmp(data,prefix,strlen(prefix))||memcmp(data+n-tail,footer,strlen(footer))||memcmp(data+n-3,"\"}\n",3))return false;
    return rd_digest(data,n-tail,hex)&&!memcmp(data+n-tail+strlen(footer),hex,64);
}
static bool rd_prune(void) {
    wchar_t pattern[WS_PATH];WsGuard g;if(ws_guard_open(&g,rd_directory))return false;
    if(swprintf(pattern,WS_PATH,L"%ls\\*",g.path)<0){ws_guard_close(&g);return false;}
    WIN32_FIND_DATAW entry;HANDLE find=FindFirstFileW(pattern,&entry);unsigned count=0,entries=0;
    HANDLE handles[RD_RUNS];FILETIME times[RD_RUNS];bool ok=find!=INVALID_HANDLE_VALUE;
    if(find!=INVALID_HANDLE_VALUE)do {
        if(!wcscmp(entry.cFileName,L".")||!wcscmp(entry.cFileName,L".."))continue;
        if(++entries>RD_ENTRIES){ok=false;break;}
        if(!wcscmp(entry.cFileName,L"owner.json")||!wcscmp(entry.cFileName,L".lock"))continue;
        if(!rd_name(entry.cFileName)||count==RD_RUNS){ok=false;break;}
        char leaf[64],path[16384];WideCharToMultiByte(CP_UTF8,0,entry.cFileName,-1,leaf,sizeof leaf,NULL,NULL);
        if(ws_join(path,sizeof path,rd_directory,leaf)){ok=false;break;}
        HANDLE h=rd_open(path,GENERIC_READ|DELETE,FILE_SHARE_READ,OPEN_EXISTING,false);
        if(h==INVALID_HANDLE_VALUE){ok=false;break;}
        if(!rd_record_valid(h,entry.cFileName)){CloseHandle(h);ok=false;break;}
        BY_HANDLE_FILE_INFORMATION info;if(!GetFileInformationByHandle(h,&info)){CloseHandle(h);ok=false;break;}
        handles[count]=h;times[count++]=info.ftLastWriteTime;
    }while(FindNextFileW(find,&entry));
    if(find!=INVALID_HANDLE_VALUE){DWORD e=GetLastError();if(ok&&e!=ERROR_NO_MORE_FILES)ok=false;FindClose(find);}
    if(ok&&count==RD_RUNS){unsigned oldest=0;for(unsigned i=1;i<count;i++)if(CompareFileTime(&times[i],&times[oldest])<0)oldest=i;
        FILE_DISPOSITION_INFO remove={TRUE};ok=SetFileInformationByHandle(handles[oldest],FileDispositionInfo,&remove,sizeof remove)!=0;}
    for(unsigned i=0;i<count;i++)CloseHandle(handles[i]);ws_guard_close(&g);return ok;
}
const char *run_diag_id(void){return rd_id;}
bool run_diag_begin(bool precompile) {
    if(precompile){fprintf(stderr,"[run] id=direct {\"event\":\"runtime-start\",\"kind\":\"shader-precompile\"}\n");return false;}
    run_diag_end();AcquireSRWLockExclusive(&rd_lock);
    unsigned char random[16];if(BCryptGenRandom(NULL,random,sizeof random,BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0){ReleaseSRWLockExclusive(&rd_lock);return false;}
    for(unsigned i=0;i<16;i++)sprintf(rd_id+2*i,"%02x",random[i]);rd_id[32]=0;
    memset(rd_rows,0,sizeof rd_rows);memset(rd_last,0,sizeof rd_last);rd_used=0;rd_limited=false;rd_sample_time=0;rd_started=true;
    wchar_t cwd[WS_PATH];char profile[16384],logs[16384],owner[16384],lock[16384],record[16384],leaf[64];
    DWORD cwd_size=GetCurrentDirectoryW(WS_PATH,cwd);
    bool ok=cwd_size>0&&cwd_size<WS_PATH&&WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,cwd,-1,profile,sizeof profile,NULL,NULL)>0;
    HANDLE state=ok?rd_open(profile,FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,OPEN_EXISTING,true):INVALID_HANDLE_VALUE;
    ok=ok&&state!=INVALID_HANDLE_VALUE;if(state!=INVALID_HANDLE_VALUE)CloseHandle(state);
    ok=ok&&!ws_join(logs,sizeof logs,profile,"logs")&&rd_directory_open(logs)&&!ws_join(rd_directory,sizeof rd_directory,logs,"game-runs")&&rd_directory_open(rd_directory);
    snprintf(leaf,sizeof leaf,"run-%s.jsonl",rd_id);
    // Hold profile/logs/game-runs for the ENTIRE transaction, including the
    // marker/lock checks and old-record enumeration and handle disposition.
    if(ok)ok=!ws_join(record,sizeof record,rd_directory,leaf)&&!ws_guard_open(&rd_guard,record);
    const char *parents[]={profile,logs,rd_directory};
    for(unsigned i=0;ok&&i<3;i++){HANDLE h=rd_open(parents[i],FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,OPEN_EXISTING,true);ok=h!=INVALID_HANDLE_VALUE;if(ok)CloseHandle(h);}
    if(ok){ok=!ws_join(owner,sizeof owner,rd_directory,"owner.json")&&!ws_join(lock,sizeof lock,rd_directory,".lock");}
    if(ok){rd_lease=rd_open(lock,GENERIC_READ|GENERIC_WRITE,0,OPEN_ALWAYS,false);LARGE_INTEGER n;
        ok=rd_lease!=INVALID_HANDLE_VALUE&&GetFileSizeEx(rd_lease,&n)&&n.QuadPart==0;}
    if(ok){HANDLE marker=rd_open(owner,GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING,false);
        if(marker==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_FILE_NOT_FOUND){
            // A fresh marker may claim only our just-created empty lock file.
            wchar_t pattern[WS_PATH];WsGuard marker_guard;ok=!ws_guard_open(&marker_guard,owner);
            if(ok){wchar_t *slash=wcsrchr(marker_guard.path,L'\\');*slash=0;swprintf(pattern,WS_PATH,L"%ls\\*",marker_guard.path);WIN32_FIND_DATAW e;HANDLE f=FindFirstFileW(pattern,&e);
                if(f==INVALID_HANDLE_VALUE)ok=false;else{do{if(wcscmp(e.cFileName,L".")&&wcscmp(e.cFileName,L"..")&&wcscmp(e.cFileName,L".lock")){ok=false;break;}}while(FindNextFileW(f,&e));FindClose(f);}ws_guard_close(&marker_guard);}
            if(ok){marker=rd_open(owner,GENERIC_WRITE,FILE_SHARE_READ,CREATE_NEW,false);DWORD put=0;ok=marker!=INVALID_HANDLE_VALUE&&WriteFile(marker,rd_owner,sizeof rd_owner-1,&put,NULL)&&put==sizeof rd_owner-1&&FlushFileBuffers(marker);}
        }else if(marker!=INVALID_HANDLE_VALUE){char data[sizeof rd_owner];unsigned n=0;ok=rd_read(marker,data,sizeof rd_owner,&n)&&n==sizeof rd_owner-1&&!memcmp(data,rd_owner,n);}else ok=false;
        if(marker!=INVALID_HANDLE_VALUE)CloseHandle(marker);}
#ifdef MONADO_RUN_DIAGNOSTICS_TEST
    if(ok){extern void run_diag_test_phase(unsigned);run_diag_test_phase(1);}
#endif
    if(ok)ok=rd_prune();
    if(ok)rd_file=rd_open(record,GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ,CREATE_NEW,false);
#ifdef MONADO_RUN_DIAGNOSTICS_TEST
    if(rd_file!=INVALID_HANDLE_VALUE){extern void run_diag_test_phase(unsigned);run_diag_test_phase(2);}
#endif
    if(rd_file==INVALID_HANDLE_VALUE){ws_guard_close(&rd_guard);if(rd_lease!=INVALID_HANDLE_VALUE)CloseHandle(rd_lease);rd_lease=INVALID_HANDLE_VALUE;}
    char row[256];snprintf(row,sizeof row,"{\"schema\":1,\"owner\":\"monado-windows-game-run\",\"run_id\":\"%s\",\"event\":\"runtime-start\",\"kind\":\"game\",\"pid\":%lu,\"utc_epoch\":%llu,\"recorded\":%s}\n",rd_id,GetCurrentProcessId(),rd_utc(),rd_file!=INVALID_HANDLE_VALUE?"true":"false");
    rd_emit_locked(row);bool recorded=rd_file!=INVALID_HANDLE_VALUE;ReleaseSRWLockExclusive(&rd_lock);
    static bool registered;if(!registered){registered=true;atexit(run_diag_end);}return recorded;
}
void run_diag_end(void) {
    AcquireSRWLockExclusive(&rd_lock);
    if(rd_file!=INVALID_HANDLE_VALUE){char hex[65],footer[160];if(rd_digest(rd_data,rd_used,hex)){
        int n=snprintf(footer,sizeof footer,"{\"event\":\"record-sealed\",\"sha256\":\"%s\"}\n",hex);rd_write(footer,(unsigned)n);FlushFileBuffers(rd_file);}
        CloseHandle(rd_file);rd_file=INVALID_HANDLE_VALUE;}
    if(rd_lease!=INVALID_HANDLE_VALUE){CloseHandle(rd_lease);rd_lease=INVALID_HANDLE_VALUE;}ws_guard_close(&rd_guard);rd_started=false;ReleaseSRWLockExclusive(&rd_lock);
}
static bool rd_value(const char *key,const char *value) {
    if(!*key||strchr(value,'|'))return false;
    for(const unsigned char *p=(const unsigned char*)key;*p;p++)if(!((*p>='a'&&*p<='z')||(*p>='0'&&*p<='9')||*p=='_'))return false;
    static const struct{const char *key,*values;} enums[]={
        {"aspect_ratio","|native|original|16:10|21:9|32:9|"},{"resolution","|match|dynamic|75%|50%|25%|1|2|3|4|5|6|7|8|"},
        {"upscaler","|bilinear|fsr|fsr3|fsr4_lite|dlss|"},{"upscale_quality","|native|quality|balanced|performance|ultra|"},
        {"antialias","|smaa|off|"},{"cutscenes","|same|30|30fg|"},{"menu_navigation_speed","|normal|fast|"},
        {"language","|auto|ja|en-US|fr|de|it|es|zh-CN|ko|nl|pt|ru|"},{"launcher_lang","|en|es|"},
        {"quality_preset","|original|low|medium|high|ultra|custom|"},{"grass_distance","|original|low|high|ultra|"}};
    if(strlen(value)>48)return false;char needle[64];snprintf(needle,sizeof needle,"|%s|",value);
    for(unsigned i=0;i<sizeof enums/sizeof *enums;i++)if(!strcmp(key,enums[i].key))return strstr(enums[i].values,needle)!=NULL;
    for(int i=0;i<GFX_COUNT;i++)if(!strcmp(key,gfx_keys[i])){for(int v=0;v<gfx_count(i);v++)if(!strcmp(value,gfx_choice(i,v)))return true;return false;}
    if(!strcmp(key,"window")){unsigned w,h;char tail;return sscanf(value,"%5ux%5u%c",&w,&h,&tail)==2;}
    if(!strcmp(key,"fps")&&!strcmp(value,"unlimited"))return true;
    if(!strcmp(key,"upscale_sharpness")&&!strcmp(value,"default"))return true;
    static const char known[]="|fullscreen|fps|volume|stick_deadzone|frame_gen|frame_gen_smooth_motion|frame_gen_match_display|depth_of_field|game_blur|vsync|party_switch|show_fps|show_fps_overlay|quickstep_slot|movie_audio|input_rumble|taa|dynamic_min_height|dynamic_max_height|upscale_sharpness|cheat_no_fall_damage|cheat_damage_dealt|cheat_damage_received|cheat_exp|cheat_ap|cheat_sp|cheat_gold|cheat_damage_dealt_percent|cheat_damage_received_percent|cheat_exp_percent|cheat_ap_percent|cheat_sp_percent|cheat_gold_percent|";
    snprintf(needle,sizeof needle,"|%s|",key);if(!strstr(known,needle))return false;
    if(!strcmp(value,"true")||!strcmp(value,"false"))return true;
    if(!*value||strlen(value)>20)return false;for(const char *p=value;*p;p++)if(!strchr("0123456789.+-",*p))return false;
    char *end;double n=strtod(value,&end);return !*end&&isfinite(n)&&fabs(n)<=1000000000.;
}
void run_diag_requested_file(const char *path,bool ignored) {
    if(!rd_started)return;
    if(ignored){rd_emit("{\"event\":\"requested-settings\",\"status\":\"intentionally-ignored\"}\n");return;}
    HANDLE h=rd_open(path,GENERIC_READ,FILE_SHARE_READ,OPEN_EXISTING,false);char *data=malloc(262145);unsigned n=0;
    if(h==INVALID_HANDLE_VALUE||!data||!rd_read(h,data,262144,&n)||memchr(data,0,n)){
        rd_emit("{\"event\":\"requested-settings\",\"status\":\"missing-or-untrusted\"}\n");goto done;}
    data[n]=0;char hex[65];if(rd_digest(data,n,hex))rd_emit("{\"event\":\"requested-settings\",\"source_sha256\":\"%s\"}\n",hex);
    unsigned rows=0,redacted=0;char *start=data;if(n>=3&&!memcmp(start,"\xef\xbb\xbf",3))start+=3;
    while(*start&&rows<128){char *next=strchr(start,'\n');if(next)*next++=0;else next=start+strlen(start);
        char *eq=strchr(start,'=');if(eq){*eq++=0;while(isspace((unsigned char)*start))start++;while(isspace((unsigned char)*eq))eq++;
            char *e=start+strlen(start);while(e>start&&isspace((unsigned char)e[-1]))*--e=0;e=eq+strlen(eq);while(e>eq&&isspace((unsigned char)e[-1]))*--e=0;
            if(strlen(start)<60&&rd_value(start,eq)){rd_emit("{\"event\":\"requested-setting\",\"key\":\"%s\",\"value\":\"%s\"}\n",start,eq);rows++;}else redacted++;
        }start=next;}
    rd_emit("{\"event\":\"requested-settings-complete\",\"known_rows\":%u,\"other_rows_omitted\":%u}\n",rows,redacted);
done:free(data);if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
}
void run_diag_settings(const char *event) {
    extern bool g_party_switch;char safe[48],row[2048];rd_safe(safe,sizeof safe,event);
    snprintf(row,sizeof row,"{\"event\":\"%s\",\"method\":\"%s\",\"window\":[%d,%d],\"fullscreen\":%d,\"aspect\":%d,\"resolution\":%d,\"upscaler\":%d,\"quality\":%d,\"configured_fps\":%d,\"vsync\":%d,\"fsr_requested\":%d,\"smooth_selected\":%d,\"external_active\":\"unavailable\",\"match_display\":%d,\"cutscene_mode\":%d,\"fps_overlay\":%d,\"antialias\":%d,\"dof\":%d,\"game_blur\":%d,\"navigation\":%d,\"quickstep\":%d,\"party_switch\":%d,\"language\":%d,\"movie_audio\":%d,\"volume\":%.6g,\"quality_preset\":%d,\"dynamic_height\":[%d,%d],\"sharpness\":%.6g,\"taa\":%d,\"rumble\":%d,\"grass\":%d,\"cheat_enabled\":[%d,%d,%d,%d,%d,%d,%d],\"cheat_percent\":[%d,%d,%d,%d,%d,%d],\"period_override\":%d,\"interval_override\":%d,\"ini_override\":%d,\"hidden\":%d}\n",
        safe,rd_method(),g_window_w,g_window_h,g_settings.fullscreen,g_settings.aspect,g_settings.res_mode,g_settings.upscaler,g_settings.upscale_quality,g_settings.fps,g_settings.vsync,g_settings.frame_gen,g_settings.frame_gen_smooth_motion,g_settings.frame_gen_match_display,g_settings.cutscene_mode,g_settings.show_fps_overlay,g_settings.antialias,!g_settings.dof_off,g_settings.game_blur,g_settings.menu_navigation_speed,g_settings.quickstep_slot,g_party_switch,g_settings.language,g_settings.movie_audio,rd_number(g_settings.volume),g_settings.quality_preset,g_settings.dynamic_min_height,g_settings.dynamic_max_height,rd_number(g_settings.upscale_sharpness),!g_settings.taa_off,g_settings.input_rumble,g_settings.grass_lod,
        g_settings.cheat_damage_dealt,g_settings.cheat_damage_received,g_settings.cheat_exp,g_settings.cheat_ap,g_settings.cheat_sp,g_settings.cheat_gold,g_settings.cheat_no_fall_damage,g_settings.cheat_damage_dealt_percent,g_settings.cheat_damage_received_percent,g_settings.cheat_exp_percent,g_settings.cheat_ap_percent,g_settings.cheat_sp_percent,g_settings.cheat_gold_percent,getenv("XCDE_FPS_PERIOD_SCALE")!=NULL,getenv("XCDE_FPS_INTERVAL")!=NULL,getenv("XCDE_INI")!=NULL,getenv("LARECOMP_HIDDEN")!=NULL);
    size_t used=strlen(row);
    if(used>=2&&used+256<sizeof row){used-=2;used+=(size_t)snprintf(row+used,sizeof row-used,",\"graphics\":[");
        for(int i=0;i<GFX_COUNT;i++)used+=(size_t)snprintf(row+used,sizeof row-used,"%s%d",i?",":"",g_settings.graphics.value[i]);
        snprintf(row+used,sizeof row-used,"]}\n");}
    rd_change(0,row);
}
void run_diag_renderer(const char *event,const char *gpu,const char *mode,unsigned w,unsigned h,float hz,float target,float cap,bool ready) {
    char e[48],g[160],m[32],row[768];rd_safe(e,sizeof e,event);rd_safe(g,sizeof g,gpu);rd_safe(m,sizeof m,mode);run_diag_settings("effective-settings");
    snprintf(row,sizeof row,"{\"event\":\"%s\",\"gpu\":\"%s\",\"app_present\":\"%s\",\"output\":[%u,%u],\"display_hz\":%.6g,\"policy_target_fps\":%.6g,\"qualified_fg_cap\":%.6g,\"fg_ready\":%d,\"method\":\"%s\"}\n",e,g,m,w,h,rd_number(hz),rd_number(target),rd_number(cap),ready,rd_method());rd_change(1,row);
}
void run_diag_pacing(float target,bool cut) {
    char row[384];snprintf(row,sizeof row,"{\"event\":\"effective-target\",\"configured_fps\":%d,\"policy_target_fps\":%.6g,\"cutscene_30\":%d,\"method\":\"%s\",\"match_display\":%d}\n",g_settings.fps,rd_number(target),cut,rd_method(),g_settings.frame_gen_match_display);rd_change(2,row);
}
void run_diag_limiter(int interval,float base,float applied) {
    // The native game interprets its raw zero interval as the default two.
    // Preserve the observed value and identify that interpretation separately.
    int effective=interval==0?2:interval;
    double nominal=effective>0&&base>0&&applied>0?60.*base/applied/effective:0.;char row[384];
    snprintf(row,sizeof row,"{\"event\":\"applied-limiter\",\"interval\":%d,\"effective_interval\":%d,\"base_period\":%.9g,\"applied_period\":%.9g,\"nominal_fps\":%.6g,\"measured\":false}\n",interval,effective,rd_number(base),rd_number(applied),rd_number(nominal));rd_change(3,row);
}
void run_diag_sample(uint64_t now,bool selected,bool game_ready,double game,bool available,bool ready,double output,uint64_t accepted,bool inputs) {
    if(!selected)return;AcquireSRWLockExclusive(&rd_lock);
    if(rd_started&&(!rd_sample_time||now<rd_sample_time||now-rd_sample_time>=2000000000ull)){
        rd_sample_time=now;char row[640];snprintf(row,sizeof row,"{\"event\":\"measured-output\",\"game_ready\":%d,\"game_fps\":%.6g,\"output_available\":%d,\"output_ready\":%d,\"output_fps\":%.6g,\"accepted_total\":%llu,\"generated\":\"unavailable\",\"inputs_ready\":%d,\"physical_scanout\":false}\n",game_ready,rd_number(game),available,ready,rd_number(output),(unsigned long long)accepted,inputs);rd_emit_locked(row);
    }ReleaseSRWLockExclusive(&rd_lock);
}
