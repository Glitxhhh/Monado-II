// Authored native Unicode/argument/report boundaries; no UI, game or user state.
#define _CRT_RAND_S
#include "../../tools/launcher/windows_report.h"
#include <shellapi.h>
#include <assert.h>
#include <stdio.h>
int main(void) {
    const std::vector<std::wstring> cases={L"",L"two words",L"resume \u2192 \u96ea",L"trailing\\",L"quoted\"word",L"two\\\\\"tail",L"literal `$(text) ; &"};
    std::wstring command=L"fixture.exe";
    for(const auto &value:cases)command+=L" "+monado_launcher_windows::quote(value);
    int count=0;wchar_t **parsed=CommandLineToArgvW(command.c_str(),&count);
    assert(parsed && count==(int)cases.size()+1);
    for(unsigned i=0;i<cases.size();i++)assert(cases[i]==parsed[i+1]);LocalFree(parsed);
    assert(monado_launcher_windows::wide(std::string("\xff",1)).empty());
    parsed=CommandLineToArgvW(GetCommandLineW(),&count);assert(parsed && count==2);
    std::string root=monado_launcher_windows::utf8(parsed[1]),output,error;LocalFree(parsed);
    if(!monado_report::create(root,output,error)){fprintf(stderr,"report error: %s errno=%d root=%s\n",error.c_str(),errno,root.c_str());return 1;}printf("%s\n",output.c_str());return 0;
}
