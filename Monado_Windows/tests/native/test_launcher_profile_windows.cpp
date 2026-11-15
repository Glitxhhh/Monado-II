// Authored arguments/profile validation, no UI, input, game process or user profile.
#define _CRT_RAND_S
#include "../../tools/launcher/windows_profile.h"
#include <shellapi.h>
#include <assert.h>
#include <stdio.h>
int main(void) {
    using namespace monado_launcher_windows;
    std::string error;Options o;
    assert(parse_options({},o,error)&&o.profile.empty()&&o.shot.empty());
    assert(parse_options({"--shot","fixture.bmp","4","precompile","--profile-root","C:/résumé/雪/$()-`"},o,error));
    assert(o.profile=="C:/résumé/雪/$()-`"&&o.shot_tab==4&&o.shot_precompile);
    for(auto args:std::vector<std::vector<std::string>>{{"--unknown"},{"--profile-root"},{"--profile-root","C:/one","--profile-root","C:/two"},{"--shot"},{"--shot","file","5"},{"--shot","file","garbage"},{"--shot","a","--shot","b"}}){Options bad;assert(!parse_options(args,bad,error));}
    assert(runtime_executable("C:/runtime $(literal)-`/雪")=="C:/runtime $(literal)-`/雪/build/xcde.exe");
    int count=0;wchar_t **args=CommandLineToArgvW(GetCommandLineW(),&count);assert(args&&count==2);
    std::string profile=utf8(args[1]);LocalFree(args);
    ProfileLease lease;std::string canonical;assert(lease.acquire(profile,canonical,error));
    assert(!canonical.empty()&&wide(canonical).find(L"雪")!=std::wstring::npos);
    DWORD attributes=GetFileAttributesW(wide(canonical+"/build").c_str());assert(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY));
    // A separately selected runtime never changes profile identity or cwd choice.
    assert(runtime_executable("C:/other-runtime")!="C:/other-runtime"+canonical);
    puts("PASS native launcher explicit canonical owned profile, Unicode/literal argument paths, strict unknown/duplicate options, legacy shot syntax and separate runtime executable");return 0;
}
