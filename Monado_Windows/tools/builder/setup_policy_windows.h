// Authored setup state/routing policy, shared by the native bridge and fixtures.
// No filesystem, process, UI, profile or configuration operation occurs here.
#pragma once
#include <string>

namespace monado_setup_policy {
struct Selection {
    std::wstring installation;
    std::wstring progress_installation;
    bool checked=false;
    bool needs_input=false;
};

inline bool uses_current_installation(const std::wstring &action) {
    return action==L"locate-input"||action==L"resume"||action==L"check-update"||
        action==L"update"||action==L"prepare-fsr4"||action==L"build-fsr4-tools"||
        action==L"select-providers";
}

inline std::wstring destination_for(const std::wstring &action,bool startup,
        const std::wstring &explicit_installation,const std::wstring &displayed,
        const Selection &selection) {
    // Startup must ask Backend.selected() for its durable last installation.
    // A friendly field default or an unsaved edit is not registry authority.
    if(startup&&action==L"resume")return explicit_installation;
    if(uses_current_installation(action)&&!selection.installation.empty())return selection.installation;
    return displayed;
}

inline bool apply_result(Selection &selection,bool terminal,unsigned long exit_code,
        const std::wstring &status,const std::wstring &installation,const std::wstring &launcher) {
    if(!installation.empty())selection.progress_installation=installation;
    if(!terminal||exit_code||installation.empty())return false;
    bool ready=status==L"success"&&!launcher.empty();
    bool missing=status==L"needs-input";
    bool update=status==L"up-to-date"||status==L"update-available";
    if(!ready&&!missing&&!update)return false;
    if(selection.installation!=installation) {
        selection.checked=false;
        selection.needs_input=false;
    }
    selection.installation=installation;
    if(ready){selection.checked=true;selection.needs_input=false;}
    else if(missing)selection.needs_input=true;
    // An update offer alone does not prove that a game/settings pair was
    // checked. For the same known selection, preserve its existing status.
    return true;
}

enum class Primary {build,open,locate};
inline Primary primary(const Selection &selection) {
    if(selection.needs_input&&!selection.installation.empty())return Primary::locate;
    return selection.checked?Primary::open:Primary::build;
}

inline bool request_settings(const std::wstring &action,bool startup,bool manage,bool branded) {
    if(startup&&manage)return false;
    return action==L"resume"||action==L"adopt"||action==L"locate-input"||(branded&&action==L"build");
}

template<class Result,class Open,class Handoff>
bool finish_checked_settings(Result &result,Open open,Handoff handoff) {
    if(!result.open_settings||result.exit||result.launcher.empty()||result.status!=L"success")return false;
    result.open_settings=false;
    open(result.launcher,result.arguments);
    handoff();
    return true;
}

template<class Handle> Handle picker_owner(Handle visible,Handle bridge) {
    return visible?visible:bridge;
}

inline bool path_controls_enabled(bool busy) {return !busy;}
} // namespace monado_setup_policy
