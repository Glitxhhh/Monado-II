"""Native authored launcher profile/argument fixture; no game or visible UI."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parent))
from native_renderer_support import ROOT,PORT,compiler_command,compiler_environment,graphics_prefix

@unittest.skipUnless(sys.platform=='win32','Native Windows profile API')
class LauncherProfileNative(unittest.TestCase):
    def test_production_custom_choices_quickstep_and_native_monitor_hint(self):
        command=compiler_command();prefix=graphics_prefix()
        if not command or not prefix:self.skipTest('Pinned native compiler and SDL headers required')
        if command[-1]=='cc':command=command[:-1]+['c++']
        source=(PORT/'tools/launcher/launcher.cpp').read_text(encoding='utf-8')
        def function(name):
            import re
            match=re.search(r'^static [^\n]*\b'+name+r'\([^\n]*\)\s*\{',source,re.M)
            self.assertIsNotNone(match,name)
            start=source.index('{',match.start());depth=1;end=start+1
            while depth:
                depth+=(source[end]=='{')-(source[end]=='}');end+=1
            return source[match.start():end]
        def choice(key):
            import re
            match=re.search(r'^.*launcher_theme::combo.*g_custom\.erase\("'+key+r'"\).*$',source,re.M)
            self.assertIsNotNone(match,key)
            at=match.start()
            start=source.rfind('\n#',0,at)+1;end=source.index('\n#endif',at)+len('\n#endif')
            return 'static void choice_'+key+'(){\n'+source[start:end]+'\n}'
        settings=source[source.index('enum { NACT'):source.index('// [XCDE] launcher language')]
        mods=source[source.index('struct Mod {'):source.index('\n\nstatic int find(')]
        quick_start=source.index('\n#',source.index('static void tab_mods()'))+1
        quick_end=source.index('    // [XCDE] LINUX-072: keep the existing setting',quick_start)
        preamble=r'''
#define _CRT_RAND_S
#include <SDL3/SDL.h>
#include "settings.h"
#include "quality_policy.h"
#include "gpu_selection.h"
#include "windows_profile.h"
#include <algorithm>
#include <map>
#include <cassert>
static int notes=0,next_choice=-1,quick_boxes=0;static bool edit_quick=false;
namespace ImGui {static void TextDisabled(const char*,...){++notes;}static void TextWrapped(const char*,...){} }
namespace launcher_theme {
static bool combo(const char*,int *value,const char*const*,int count){if(next_choice<0)return false;assert(next_choice<count);bool changed=*value!=next_choice;*value=next_choice;return changed;}
static void section(const char*){}
static bool checkbox(const char*,bool *value){++quick_boxes;if(edit_quick){*value=!*value;return true;}return false;}
}
'''
        bodies='\n'.join((function('find'),function('load'),function('emit_settings')))
        generated=preamble+settings+'\n'+mods+r'''
static std::string g_root;
static HWND g_native_window;
static std::string g_fsr4_assets,g_fsr4_component;
static const char *T(const char *text){return text;}
struct TList {const char *p[48];};
'''+function('TL')+'\n'+function('path')+'\n'+bodies+'\n'+'\n'.join(choice(k) for k in ('window','resolution','language'))+'\nstatic void choice_quickstep(){\n'+source[quick_start:quick_end]+'\n}\n'+function('windows_game_arguments')+r'''
static std::string serialized(){FILE *file=tmpfile();assert(file);emit_settings(file);assert(!fflush(file)&&!fseek(file,0,SEEK_END));long size=ftell(file);assert(size>0&&!fseek(file,0,SEEK_SET));std::string text(size,0);assert(fread(text.data(),1,text.size(),file)==text.size());fclose(file);return text;}
int main(int argc,char **argv){
    assert(argc==2);g_root=argv[1];g=Cfg{};load();g.icons=1;
    assert(g_custom.at("window")=="1500x950"&&g_custom.at("resolution")=="4"&&g_custom.at("language")=="pt");
    assert(!g.quickstep_slot);auto before=serialized();assert(before.find("window = 1500x950\n")!=std::string::npos&&before.find("resolution = 4\n")!=std::string::npos&&before.find("language = pt\n")!=std::string::npos&&before.find("quickstep_slot = 0\n")!=std::string::npos);
    choice_window();choice_resolution();choice_language();choice_quickstep();assert(notes==3&&quick_boxes==1&&g_custom.size()==3&&!g.quickstep_slot);
    next_choice=1;choice_window();next_choice=2;choice_resolution();next_choice=1;choice_language();edit_quick=true;choice_quickstep();
    assert(g_custom.empty()&&g.quickstep_slot);auto after=serialized();assert(after.find("window = 1600x900\n")!=std::string::npos&&after.find("resolution = 2\n")!=std::string::npos&&after.find("language = en-US\n")!=std::string::npos&&after.find("quickstep_slot = 1\n")!=std::string::npos);
    assert(monado_launcher_windows::launch_monitor_arguments(nullptr).empty());
    g_native_window=CreateWindowExW(0,L"STATIC",L"authored invisible monitor fixture",WS_POPUP,0,0,10,10,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);assert(g_native_window);
    MONITORINFOEXW info{};info.cbSize=sizeof info;assert(GetMonitorInfoW(MonitorFromWindow(g_native_window,MONITOR_DEFAULTTONEAREST),&info));
    auto hint=monado_launcher_windows::launch_monitor_arguments(g_native_window);assert(hint.size()==2&&hint[0]==L"--launch-monitor"&&hint[1]==info.szDevice);
    auto game=windows_game_arguments(false);assert(game.size()==3&&game[0]==hint[0]&&game[1]==hint[1]&&game[2]==L"targets");
    g_fsr4_assets="C:/authored models";g_fsr4_component="C:/authored component.dll";game=windows_game_arguments(false);assert(game.size()==7&&game[1]==hint[1]&&game[2]==L"--fsr4-assets"&&game.back()==L"targets");
    auto preparation=windows_game_arguments(true);assert(preparation.size()==6&&preparation[0]==L"--fsr4-assets"&&preparation[4]==L"--precompile"&&preparation[5]==L"targets");
    auto runtime=g_root+"/authored separate runtime";assert(CreateDirectoryW(monado_launcher_windows::wide(runtime).c_str(),nullptr));assert(CreateDirectoryW(monado_launcher_windows::wide(runtime+"/build").c_str(),nullptr));assert(!monado_launcher_windows::runtime_available(runtime));FILE *image=monado_launcher_windows::open(monado_launcher_windows::runtime_executable(runtime),"wb");assert(image);fputs("authored runtime gate fixture, never executed",image);fclose(image);assert(monado_launcher_windows::runtime_available(runtime)&&!monado_launcher_windows::runtime_available(g_root));
    assert(DestroyWindow(g_native_window));puts("PASS production native launcher custom selections, Quickstep checkbox/default/serialization and native checked launch monitor hint");
}
'''
        with tempfile.TemporaryDirectory(prefix='launcher-selection-',dir=PORT/'build') as temporary:
            root=Path(temporary);cpp=root/'authored.cpp';binary=root/'authored.exe'
            cpp.write_text(generated,encoding='utf-8')
            (root/'xcde.ini').write_text('window = 1500x950\nresolution = 4\nlanguage = pt\n',encoding='utf-8')
            args=command+['-std=c++17','-O1','-UNDEBUG','-ffp-contract=off','-fno-fast-math','-I',str(PORT/'runtime'),'-I',str(PORT/'tools/launcher'),'-I',str(prefix/'include'),str(cpp),str(prefix/'lib/libSDL3.dll.a'),'-luser32','-o',str(binary)]
            result=subprocess.run(args,env=compiler_environment(),capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            environment=compiler_environment();environment['PATH']=str(prefix/'bin')+os.pathsep+environment.get('PATH','')
            result=subprocess.run([str(binary),str(root)],env=environment,capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS production native launcher custom selections',result.stdout)

    def test_runtime_and_profile_paths_and_strict_arguments(self):
        command=compiler_command()
        if not command:self.skipTest('Pinned native compiler required')
        if command[-1]=='cc':command=command[:-1]+['c++']
        with tempfile.TemporaryDirectory(prefix='monado-profile-') as temporary:
            root=Path(temporary);profile=root/'profile-résumé-雪-$()-`';profile.mkdir()
            binary=root/'profile-fixture.exe'
            args=command+['-std=c++17','-O1','-UNDEBUG',str(PORT/'tests/native/test_launcher_profile_windows.cpp'),'-ladvapi32','-lshell32','-o',str(binary)]
            result=subprocess.run(args,env=compiler_environment(),capture_output=True,text=True,timeout=120)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(binary),str(profile)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn('PASS native launcher explicit canonical owned profile',result.stdout)
            self.assertTrue((profile/'build').is_dir())

if __name__=='__main__':unittest.main()
