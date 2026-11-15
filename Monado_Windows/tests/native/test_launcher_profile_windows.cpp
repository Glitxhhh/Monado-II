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
    Options fsr4;assert(parse_options({"--fsr4-assets","C:/local models/雪","--fsr4-component","C:/native/monado-fsr4.dll"},fsr4,error));
    assert(fsr4.fsr4_assets=="C:/local models/雪"&&fsr4.fsr4_component=="C:/native/monado-fsr4.dll");
    for(auto args:std::vector<std::vector<std::string>>{{"--fsr4-assets","C:/models"},{"--fsr4-component","C:/native.dll"},{"--fsr4-assets","C:/one","--fsr4-assets","C:/two"}}){Options bad;assert(!parse_options(args,bad,error));}
    assert(runtime_executable("C:/runtime $(literal)-`/雪")=="C:/runtime $(literal)-`/雪/build/xcde.exe");
    int count=0;wchar_t **args=CommandLineToArgvW(GetCommandLineW(),&count);assert(args&&count==2);
    std::string profile=utf8(args[1]);LocalFree(args);
    ProfileLease lease;std::string canonical;assert(lease.acquire(profile,canonical,error));
    assert(!canonical.empty()&&wide(canonical).find(L"雪")!=std::wstring::npos);
    DWORD attributes=GetFileAttributesW(wide(canonical+"/build").c_str());assert(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY));
    FILE *log=open_append(canonical+"/build/authored.log");assert(log);fputs("authored",log);fclose(log);
    std::wstring alias=wide(canonical+"/build/alias.log"),source=wide(canonical+"/build/authored.log");
    assert(CreateHardLinkW(alias.c_str(),source.c_str(),nullptr));assert(!open_append(canonical+"/build/alias.log"));
    assert(DeleteFileW(alias.c_str()));
    // A separately selected runtime never changes profile identity or cwd choice.
    assert(runtime_executable("C:/other-runtime")!="C:/other-runtime"+canonical);
    auto runtime=canonical+"/authored-separate-runtime";assert(CreateDirectoryW(wide(runtime).c_str(),nullptr));assert(CreateDirectoryW(wide(runtime+"/build").c_str(),nullptr));
    assert(!runtime_available(runtime));FILE *image=open(runtime_executable(runtime),"wb");assert(image);fputs("authored ordinary image gate fixture; never executed",image);fclose(image);
    assert(runtime_available(runtime));assert(!runtime_available(canonical)); // The stable profile has logs but no game executable.
    puts("PASS native launcher explicit canonical owned profile, Unicode/literal argument paths, strict unknown/duplicate options, legacy shot syntax and separate runtime executable");return 0;
}
