// Actual production setup policy. No SDL/Win32 window, process, game, profile,
// filesystem, registry, environment or user input is opened by this fixture.
#include "setup_policy_windows.h"
#include <cassert>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace monado_setup_policy;
struct Result {
    std::wstring status,launcher;
    std::vector<std::wstring> arguments;
    bool open_settings=false;
    unsigned long exit=0;
};

int main() {
    unsigned checked=0;
    const std::wstring shown=LR"(C:\Downloads\MonadoII)",a=LR"(D:\Chosen installation 漢字 $() `)",
        b=LR"(E:\Fresh attempt)",explicit_target=LR"(F:\Explicit manager target)",
        launcher=LR"(D:\Chosen installation 漢字 $() `\build\xcde_launcher.exe)",
        profile=LR"(D:\Preserved human profile 漢字)",hint=LR"(C:\Unfinished progress destination)";
    Selection fresh;
    // Fresh startup omits --destination so the backend can return needs-setup.
    assert(destination_for(L"resume",true,L"",shown,fresh).empty());++checked;
    assert(primary(fresh)==Primary::build);++checked;
    assert(destination_for(L"build",false,L"",shown,fresh)==shown);++checked;
    // Reopening cannot substitute the EXE-adjacent default for registry.last.
    Selection known{a,L"",true,false};
    assert(destination_for(L"resume",true,L"",shown,known).empty());++checked;
    assert(destination_for(L"resume",true,explicit_target,shown,known)==explicit_target);++checked;
    assert(destination_for(L"resume",false,L"",shown,known)==a);++checked;
    assert(destination_for(L"build",false,L"",shown,known)==shown);++checked;
    for(auto action:{L"locate-input",L"check-update",L"update",L"prepare-fsr4",L"build-fsr4-tools",L"select-providers"}) {
        assert(destination_for(action,false,L"",shown,known)==a);++checked;
    }
    // Progress, malformed success, cancellation and failed children cannot
    // replace checked A with unfinished B. The old profile is never an output.
    for(auto status:{L"running",L"error",L"cancelled",L"needs-setup",L"download-ready",L"success",L"needs-input",L"up-to-date",L"update-available"}) {
        for(bool terminal:{false,true})for(unsigned long exit:{0ul,1ul,259ul}) {
            Selection state=known;
            bool committed=apply_result(state,terminal,exit,status,b,launcher);
            bool accepted=terminal&&!exit&&(std::wstring(status)==L"success"||std::wstring(status)==L"needs-input"||
                std::wstring(status)==L"up-to-date"||std::wstring(status)==L"update-available");
            assert(committed==accepted);assert(state.progress_installation==b);
            if(!accepted){assert(state.installation==a&&state.checked&&!state.needs_input);}
            ++checked;
        }
    }
    assert(!apply_result(known,true,0,L"success",b,L""));assert(known.installation==a);++checked;
    assert(!apply_result(known,true,0,L"success",L"",launcher));assert(known.installation==a);++checked;
    assert(!apply_result(known,false,0,L"success",hint,launcher));assert(primary(known)==Primary::open);++checked;
    Selection recovery;
    assert(apply_result(recovery,true,0,L"needs-input",a,L""));
    assert(recovery.installation==a&&recovery.needs_input&&!recovery.checked&&primary(recovery)==Primary::locate);++checked;
    // Cancelling the locate picker emits no completion; a cancelled operation
    // likewise keeps the checked missing-input record and Locate action.
    assert(!apply_result(recovery,true,1,L"cancelled",b,L""));
    assert(recovery.installation==a&&primary(recovery)==Primary::locate);++checked;
    assert(apply_result(recovery,true,0,L"success",a,launcher));
    assert(recovery.checked&&!recovery.needs_input&&primary(recovery)==Primary::open);++checked;
    assert(apply_result(recovery,true,0,L"update-available",a,L""));
    assert(recovery.checked&&recovery.installation==a);++checked;
    Selection offer;
    assert(apply_result(offer,true,0,L"up-to-date",a,L""));
    assert(!offer.checked&&primary(offer)==Primary::build);++checked;
    // Build hands off only in the branded flow; reopening/adoption/Locate do
    // so after real checked success, while explicit management startup stays.
    assert(request_settings(L"build",false,false,true));++checked;
    assert(!request_settings(L"build",false,false,false));++checked;
    assert(!request_settings(L"resume",true,true,true));++checked;
    for(auto action:{L"resume",L"adopt",L"locate-input"}) {
        assert(request_settings(action,false,true,true));++checked;
    }
    for(auto action:{L"update",L"select-providers",L"check-update",L"prepare-fsr4",L"build-fsr4-tools"}) {
        assert(!request_settings(action,false,false,true));++checked;
    }
    for(auto status:{L"success",L"cancelled",L"error",L"needs-setup",L"needs-input",L"up-to-date",L"update-available"}) {
        for(unsigned long exit:{0ul,1ul,259ul}) {
            Result result{status,launcher,{L"--profile-root",profile},true,exit};
            unsigned opened=0,handed_off=0;
            auto open=[&](const auto &path,const auto &args){assert(path==launcher&&args==result.arguments);++opened;};
            auto finish=[&]{++handed_off;};
            bool expected=std::wstring(status)==L"success"&&!exit;
            assert(finish_checked_settings(result,open,finish)==expected);
            assert(opened==unsigned(expected)&&handed_off==unsigned(expected));
            if(expected)assert(!finish_checked_settings(result,open,finish));
            ++checked;
        }
    }
    Result failed_open{L"success",launcher,{L"--profile-root",profile},true,0};
    bool handed_off=false,raised=false;
    try{finish_checked_settings(failed_open,[](const auto&,const auto&){throw std::runtime_error("authored open failure");},[&]{handed_off=true;});}
    catch(const std::runtime_error&){raised=true;}
    assert(raised&&!handed_off&&!failed_open.open_settings);++checked;
    assert(path_controls_enabled(false)&&!path_controls_enabled(true));++checked;
    void *visible=(void*)0x1234,*bridge=(void*)0x5678;
    assert(picker_owner(visible,bridge)==visible&&picker_owner((void*)nullptr,bridge)==bridge);++checked;
    std::printf("PASS production setup policy: %u cases; first launch, registry reopen, explicit management, checked selection, cancellation, exact settings-only handoff and picker owner; no window/game/profile I/O\n",checked);
}
