"""Native static branded frontend/relink fixture; never creates a window/game."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sys
import unittest
import uuid

ROOT=Path(__file__).resolve().parents[1]
PORT=ROOT/'Monado_Windows'
spec=importlib.util.spec_from_file_location('authored_windows_branded_frontend',PORT/'tools/windows_branded_frontend.py')
frontend=importlib.util.module_from_spec(spec);sys.modules[spec.name]=frontend;spec.loader.exec_module(frontend)


@unittest.skipUnless(os.name=='nt','native static SDL3/ImGui frontend')
class NativeBrandedFrontend(unittest.TestCase):
    def test_hidden_software_theme_and_retained_relative_relink(self):
        run=PORT/'runs'/('branded-frontend-'+uuid.uuid4().hex)
        run.mkdir()
        source=run/'reviewed source 漢字 $() `'
        output=run/'package output 漢字'
        output.mkdir()
        for relative in ('tools/launcher/assets/fonts','tools/builder','third_party/imgui/backends'):
            (source/relative).mkdir(parents=True,exist_ok=True)
        for name in ('theme.h','theme_resource.py','assets/gaur_plain.png',
                     'assets/fonts/DejaVuSans.ttf','assets/fonts/DejaVuSerif-Bold.ttf',
                     'assets/fonts/DejaVuSerif-BoldItalic.ttf','assets/fonts/LICENSE-DejaVu.txt'):
            shutil.copyfile(PORT/'tools/launcher'/name,source/'tools/launcher'/name)
        for path in (PORT/'third_party/imgui').iterdir():
            if path.name in ('imgui.cpp','imgui_draw.cpp','imgui_tables.cpp','imgui_widgets.cpp') or path.suffix=='.h':
                shutil.copyfile(path,source/'third_party/imgui'/path.name)
        for name in ('imgui_impl_sdl3','imgui_impl_sdlrenderer3'):
            for extension in ('.cpp','.h'):
                shutil.copyfile(PORT/'third_party/imgui/backends'/(name+extension),source/'third_party/imgui/backends'/(name+extension))
        for name in ('builder.rc','builder.manifest'):
            shutil.copyfile(PORT/'tools/builder'/name,source/'tools/builder'/name)
        shutil.copyfile(PORT/'tests/native/test_windows_branded_static.cpp',source/'tools/builder/windows_builder.cpp')
        prefix=Path(json.loads((PORT/'.deps/windows-deps.json').read_bytes())['prefix'])
        receipt=frontend.compile_native_frontend(source,prefix,output,'Windows Experimental · 漢字')
        self.assertEqual(len(receipt['objects']),7)
        self.assertEqual({Path(item['path']).name for item in receipt['static_libraries']},{'libSDL3.a','libiconv.a'})
        self.assertFalse(any('sdl3' in name.lower() or 'iconv' in name.lower() for name in receipt['imports']))
        build=Path(receipt['base_exe']).parent
        for item in receipt['objects']+[receipt['resource']]+receipt['static_libraries']:
            raw=(build/item['path']).read_bytes()
            self.assertEqual(item['sha256'],hashlib.sha256(raw).hexdigest())
            self.assertEqual(item['bytes'],len(raw))
        environment=frontend._environment(build,frontend._compiler(),frontend._system_cwd())
        result=frontend.process.run([receipt['base_exe']],cwd=frontend._system_cwd(),
            log=run/'hidden-surface.txt',environment=environment,timeout=60)
        self.assertEqual(result['exit'],0,(run/'hidden-surface.txt').read_text('utf-8',errors='replace'))
        self.assertIn('no window',(run/'hidden-surface.txt').read_text('utf-8'))
        recipe=json.loads((build/'relink.json').read_bytes())
        self.assertEqual(recipe,receipt['relink_recipe'])
        self.assertNotIn(str(source),json.dumps(recipe))
        arguments=list(recipe['argv'])
        arguments[arguments.index('-o')+1]='relinked.exe'
        files={item['path'] for item in recipe['objects']+recipe['static_libraries']}|{recipe['resource']['path'],'relinked.exe'}
        arguments=[str(build/value) if value in files else value for value in arguments]
        linked=frontend.process.run([frontend._compiler(),*arguments],cwd=frontend._system_cwd(),
            log=run/'relink.txt',environment=environment,timeout=120)
        self.assertEqual(linked['exit'],0,(run/'relink.txt').read_text('utf-8',errors='replace'))
        self.assertEqual(frontend._imports(build/'relinked.exe'),receipt['imports'])
        check=frontend.process.run([build/'relinked.exe'],cwd=frontend._system_cwd(),
            log=run/'relinked-hidden-surface.txt',environment=environment,timeout=60)
        self.assertEqual(check['exit'],0)
        original=(build/'frontend-receipt.json').read_bytes()
        with self.assertRaises(OSError):
            frontend.compile_native_frontend(source,prefix,output,'Windows Experimental')
        self.assertEqual((build/'frontend-receipt.json').read_bytes(),original)
        bad_prefix=run/'authored changed native prefix'
        (bad_prefix/'include/SDL3').mkdir(parents=True)
        (bad_prefix/'lib').mkdir()
        header=b'// Authored dependency header fixture\n'
        (bad_prefix/'include/SDL3/SDL.h').write_bytes(header)
        inventory={'include/SDL3/SDL.h':{'bytes':len(header),'sha256':hashlib.sha256(header).hexdigest()}}
        for name in frontend.STATIC:
            data=b'Authored changed archive fixture'
            (bad_prefix/'lib'/name).write_bytes(data)
            inventory['lib/'+name]={'bytes':len(data),'sha256':'0'*64}
        (bad_prefix/'content-inventory.json').write_bytes(frontend.encoded(inventory))
        rejected=run/'rejected package output';rejected.mkdir()
        with self.assertRaisesRegex(ValueError,'Static frontend archive differs'):
            frontend.compile_native_frontend(source,bad_prefix,rejected,'Windows Experimental')
        self.assertFalse((rejected/'frontend').exists())
        (run/'test-receipt.json').write_bytes(frontend.encoded(dict(status='passed',
            game_or_window_started=False,static_link=True,relink_passed=True,frontend=receipt)))
        print('Native hidden branded fixture receipt: '+str(run/'test-receipt.json'),flush=True)


if __name__=='__main__':
    unittest.main()
