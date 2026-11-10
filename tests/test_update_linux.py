"""Authored transaction and save-preservation fixtures; no real game or profile."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import types
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import update_linux as u
import update_release as r
if sys.platform=='linux':
    import fcntl


def manifest(root, files):
    files = dict(files, **{'publication-policy.json': b'{"schema":1,"licensed_media":{}}'})
    for name, data in files.items():
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(data)
        if name.endswith('.sh'):p.chmod(0o755)
    (root/'publication-manifest.json').write_text(json.dumps({'schema':1,'project':'Monado II','source_only':True,'files':{n:{'bytes':len(d),'sha256':hashlib.sha256(d).hexdigest()} for n,d in files.items()}}))


def image(root):
    data=bytearray(128);data[:4]=b'\x7fELF';data[8:11]=b'AI\x02'
    p=root/u.IMAGE;p.write_bytes(data);p.chmod(0o755)
    receipt={'status':'passed','artifact_kind':'local-launcher-appimage','bytes':128,'sha256':hashlib.sha256(data).hexdigest(),'archive_readback_verified':True}
    receipt.update({k:False for k in ['contains_game_executable','contains_game_files','contains_generated_game_code','contains_vendor_sdk']})
    Path(str(p)+'.receipt.json').write_text(json.dumps(receipt))


@unittest.skipUnless(sys.platform=='linux','Linux installation/lease fixtures')
class Transactions(unittest.TestCase):
    def setUp(self):
        tmp=tempfile.TemporaryDirectory(prefix='monado-updater-owned-');self.addCleanup(tmp.cleanup)
        self.base=Path(tmp.name);self.owner=self.base/'owner';self.owner.mkdir();self.port=self.owner/'Monado_Linux'
        self.state=self.port/'play/launcher';(self.state/'save').mkdir(parents=True)
        (self.state/'.session.lock').write_bytes(b'')
        (self.state/'save/slot').write_bytes(b'AUTHORED SAVE SENTINEL');(self.state/'xcde.ini').write_bytes(b'fps = 165\nframe_gen = 0\n')
        (self.owner/'game_files/exefs').mkdir(parents=True);(self.owner/'game_files/romfs').mkdir()
        (self.owner/'game_files/romfs/synthetic').write_bytes(b'AUTHORED INPUT SENTINEL')
        self.game=self.port/'build/old-game/xcde-linux';self.gui=self.port/'build/old-gui/monado-launcher'
        for p in [self.game,self.gui]:p.parent.mkdir(parents=True);p.write_bytes(b'NONEXECUTED AUTHORED PLACEHOLDER')
        (self.game.parent/'receipt.json').write_text('{"dlss_enabled":true,"framegen_enabled":true}')
        helper=(Path(__file__).resolve().parents[1]/'Monado_Linux/scripts/install_layout.py').read_bytes()
        self.files={'README.md':b'# Authored fixture\n','Build_Monado_II.sh':b'#!/bin/sh\nexit 1\n','Monado_Linux/scripts/install_layout.py':helper,'Monado_Linux/runtime/fixture.c':b'// authored runtime\n','Monado_Linux/tools/launcher/launcher.cpp':b'// old GUI\n','update-version.json':b'{"schema":1,"protocol":1,"version":"0.1.3"}'}
        for name in ('Rollback_Monado_II.sh','tools/update_linux.py','tools/installation_version.py'):
            self.files[name]=(Path(__file__).resolve().parents[1]/name).read_bytes()
        manifest(self.owner,self.files);self.candidate=self.base/'candidate';self.candidate.mkdir()
        self.new=dict(self.files);self.new['update-version.json']=b'{"schema":1,"protocol":1,"version":"0.1.4"}';self.new['Monado_Linux/tools/launcher/launcher.cpp']=b'// new GUI\n';manifest(self.candidate,self.new)
        self.before=self.protected();self.commands=[];self.events=[]
        # Build/image payloads above are deliberately non-executable; runtime
        # loading is independently exercised by authored ELF fixtures.
        runtime_patch=mock.patch.object(u,'verify_runtime_selection',return_value={'status':'passed','scope':'authored fixture'})
        self.runtime_check=runtime_patch.start();self.addCleanup(runtime_patch.stop)
    def protected(self):
        return {str(p.relative_to(self.owner)):(p.read_bytes(),p.stat().st_mtime_ns) for d in [self.state,self.owner/'game_files'] for p in d.rglob('*') if p.is_file()}
    def fetch(self,offer,destination,**kwargs):destination.mkdir();return Path(shutil.copytree(self.candidate,destination/'source'))
    def runner(self,command,workspace,log,emit,cancel):
        self.commands.append(command);self.assertFalse((self.port/'updates/active.json').exists());self.assertEqual(self.before,self.protected());image(workspace);log.write_text('authored completion\n')
    def session(self,**kwargs):
        defaults={'discover':lambda _:dict(version='0.1.4',tag='v0.1.4'),'fetch':self.fetch,'runner':self.runner};defaults.update(kwargs)
        s=u.Session(self.owner,self.state,self.gui,self.game,**defaults);self.addCleanup(s.close);return s
    def prepare(self,s,cancel=None):return s.install(s.check()['id'],self.events.append,cancel or threading.Event())
    def assertProtected(self):self.assertEqual(self.before,self.protected())
    def lease(self):
        fd=os.open(self.state/'.session.lock',os.O_RDWR|os.O_CREAT,0o600)
        fcntl.flock(fd,fcntl.LOCK_EX|fcntl.LOCK_NB)
        return fd
    def sync(self,workspace):
        fd=self.lease()
        try:return u.sync_installation_version(workspace,self.state,fd)
        finally:os.close(fd)
    def rollback(self,workspace):
        fake=types.SimpleNamespace(acquire_lock=lambda state:self.lease())
        with mock.patch.object(u,'ROOT',workspace),mock.patch.dict(sys.modules,{'launcher_linux':fake}):
            u.rollback(self.owner)
    def test_no_update_is_quiet_and_creates_no_update_paths(self):
        self.assertIsNone(self.session(discover=lambda _:None).check());self.assertFalse((self.port/'updates').exists());self.assertProtected()
    def test_launcher_only_game_reuse_and_post_session_activation(self):
        s=self.session();p=self.prepare(s);self.assertFalse(p['rebuild']);self.assertIn('--reuse-game',self.commands[0]);self.assertEqual(self.commands[0][-1],str(self.game));self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
        command=s.activate(p);record=u.json_read(self.port/'updates/active.json');self.assertEqual(record['version'],'0.1.4');self.assertEqual(command[-2:],['--state-root',str(self.state)]);self.assertProtected();self.assertEqual((self.owner/'README.md').read_bytes(),self.files['README.md'])
        managed=self.owner/record['workspace'];self.assertEqual(u.json_read(managed/'installation.json')['game_port'],str(self.port));self.assertEqual(u.dispatch_active(self.owner,['--dry-run'])[-1],'--dry-run');self.assertIsNone(u.dispatch_active(managed,[]))
        self.assertEqual(u.version(self.owner),'0.1.4');u.installed_sources(self.owner)
        self.assertEqual(u.json_read(self.port/'updates/previous.json')['schema'],2)
    def test_runtime_change_triggers_automatic_game_rebuild(self):
        self.new['Monado_Linux/runtime/fixture.c']+=b'// changed\n';manifest(self.candidate,self.new);s=self.session();p=self.prepare(s);self.assertTrue(p['rebuild']);self.assertNotIn('--reuse-game',self.commands[0]);binding=u.json_read(self.owner/p['record']['workspace']/'installation.json');self.assertEqual(binding['game_port'],str(self.owner/p['record']['workspace']/'Monado_Linux'));self.assertProtected()
        self.assertNotIn('--without-dlss',self.commands[0])
    def test_older_standard_game_without_framegen_requires_full_rebuild_even_for_launcher_update(self):
        for recorded in (False, None):
            with self.subTest(recorded=recorded):
                receipt={'dlss_enabled':True}
                if recorded is not None:receipt['framegen_enabled']=recorded
                (self.game.parent/'receipt.json').write_text(json.dumps(receipt))
                s=self.session();p=self.prepare(s)
                self.assertTrue(p['rebuild']);self.assertNotIn('--reuse-game',self.commands[-1])
                self.assertNotIn('--without-dlss',self.commands[-1])
                binding=u.json_read(self.owner/p['record']['workspace']/'installation.json')
                self.assertEqual(binding['game_port'],str(self.owner/p['record']['workspace']/'Monado_Linux'))
                self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
                s.close()
    def test_foley_diagnostic_full_rebuild_stops_before_build_and_preserves_state(self):
        (self.game.parent/'receipt.json').write_text('{"dlss_enabled":true,"framegen_enabled":true,"foley_trace":true}')
        self.new['Monado_Linux/runtime/fixture.c']+=b'// changed\n';manifest(self.candidate,self.new)
        with self.assertRaisesRegex(ValueError,'diagnostic game build requires a manual rebuild'):
            self.prepare(self.session())
        self.assertFalse(self.commands);self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
    def test_missing_runtime_prevents_preparation_and_keeps_previous_selection(self):
        self.runtime_check.side_effect=ValueError('authored missing libbluray.so.3')
        session=self.session()
        with self.assertRaisesRegex(ValueError,'missing libbluray'):
            self.prepare(session)
        self.assertFalse((self.port/'updates/active.json').exists())
        self.assertEqual((self.owner/'update-version.json').read_bytes(),self.files['update-version.json'])
        self.assertProtected()
    def test_runtime_changed_after_build_cannot_activate_or_touch_version(self):
        session=self.session();prepared=self.prepare(session)
        self.runtime_check.side_effect=ValueError('authored runtime provider changed')
        before=self.protected();marker=(self.owner/'update-version.json').read_bytes()
        with self.assertRaisesRegex(ValueError,'runtime provider changed'):
            session.activate(prepared)
        self.assertFalse((self.port/'updates/active.json').exists())
        self.assertFalse((self.port/'updates/previous.json').exists())
        self.assertEqual((self.owner/'update-version.json').read_bytes(),marker)
        self.assertEqual(self.protected(),before)
        self.assertProtected()
    def test_failed_build_preserves_sources_pointer_saves_and_input(self):
        def failure(*_):raise ValueError('authored compiler failure')
        s=self.session(runner=failure)
        with self.assertRaisesRegex(ValueError,'compiler failure'):self.prepare(s)
        self.assertFalse((self.port/'updates/active.json').exists());u.installed_sources(self.owner);self.assertIsNone(s.lock_fd);self.assertProtected()
    def test_cancel_before_fetch_does_not_create_update_tree(self):
        s=self.session();cancel=threading.Event();cancel.set()
        with self.assertRaises(r.UpdateCancelled):self.prepare(s,cancel)
        self.assertFalse((self.port/'updates').exists());self.assertProtected()
    def test_cancel_after_build_does_not_activate(self):
        cancel=threading.Event()
        def runner(*args):self.runner(*args);cancel.set()
        s=self.session(runner=runner)
        with self.assertRaises(r.UpdateCancelled):self.prepare(s,cancel)
        self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
        self.assertEqual((self.owner/'update-version.json').read_bytes(),self.files['update-version.json'])
        self.assertFalse((self.port/'updates/bootstrap-version.json').exists())
    def test_local_source_edits_are_preserved_and_install_refused(self):
        p=self.owner/'README.md';p.write_bytes(b'user local edits\n');s=self.session()
        with self.assertRaisesRegex(ValueError,'Local source changes'):self.prepare(s)
        self.assertEqual(p.read_bytes(),b'user local edits\n');self.assertFalse((self.port/'updates').exists());self.assertProtected()
    def test_downloaded_version_mismatch_stops_before_build(self):
        self.new['update-version.json']=self.files['update-version.json'];manifest(self.candidate,self.new);s=self.session()
        with self.assertRaisesRegex(ValueError,'version metadata'):self.prepare(s)
        self.assertFalse(self.commands);self.assertProtected()
    def test_damaged_built_image_is_rejected(self):
        def bad(command,workspace,*_):image(workspace);(workspace/u.IMAGE).write_bytes(b'authored corruption')
        s=self.session(runner=bad)
        with self.assertRaisesRegex(ValueError,'pair failed'):self.prepare(s)
        self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
    def test_stale_offer_and_unowned_prepared_value_are_rejected(self):
        s=self.session();offer=s.check()
        with self.assertRaisesRegex(ValueError,'stale'):s.install('incorrect',self.events.append,threading.Event())
        p=s.install(offer['id'],self.events.append,threading.Event())
        with self.assertRaisesRegex(ValueError,'owned prepared'):s.activate(dict(p))
        self.assertProtected()
    def test_update_lock_serializes_different_profiles(self):
        first=self.session();self.prepare(first)
        with self.assertRaisesRegex(ValueError,'already running'):self.prepare(self.session())
        self.assertProtected()
    def test_modified_prepared_source_cannot_activate(self):
        s=self.session();p=self.prepare(s);(self.owner/p['record']['workspace']/'README.md').write_bytes(b'authored corruption\n')
        with self.assertRaisesRegex(ValueError,'Local source changes'):s.activate(p)
        self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
    def test_commit_failure_after_replace_restores_old_selection(self):
        s=self.session();p=self.prepare(s);real=u.durable_replace
        def fail(path,value):
            real(path,value)
            if path.name=='active.json':raise OSError('authored fsync failure')
        with mock.patch.object(u,'durable_replace',side_effect=fail):
            with self.assertRaisesRegex(OSError,'fsync'):s.activate(p)
        self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
        self.assertEqual(u.version(self.owner),'0.1.3');u.installed_sources(self.owner)
    def test_pointer_traversal_and_symlink_are_refused(self):
        d=self.port/'updates';d.mkdir();u.write_new(d/'active.json',{'schema':1,'workspace':'../../elsewhere'})
        with self.assertRaisesRegex(ValueError,'active update'):u.dispatch_active(self.owner,[])
        (d/'active.json').unlink();(d/'active.json').symlink_to(self.owner/'README.md')
        with self.assertRaisesRegex(ValueError,'canonical'):u.dispatch_active(self.owner,[])
        self.assertProtected()
    def test_dlss_optout_and_standard_framegen_capability_are_preserved(self):
        path=self.game.parent/'receipt.json';path.write_text('{"dlss_enabled":false}')
        for rebuild in (False,True):
            command=u.build_command(self.candidate,self.game,rebuild)
            self.assertIn('--without-dlss',command);self.assertNotIn('--framegen',command)
        path.write_text('{"dlss_enabled":true,"framegen_enabled":true}')
        full=u.build_command(self.candidate,self.game,True)
        self.assertEqual(full,[str(self.candidate/'Build_Monado_II.sh')])
        self.assertIn('--reuse-game',u.build_command(self.candidate,self.game,False))
        self.assertIn('--framegen',u.build_command(self.candidate,self.game,False))

    def test_direct_reuse_cannot_silently_drop_standard_framegen_capability(self):
        path=self.game.parent/'receipt.json'
        for receipt in ({'dlss_enabled':True,'framegen_enabled':False},{'dlss_enabled':True}):
            path.write_text(json.dumps(receipt))
            with self.subTest(receipt=receipt),self.assertRaisesRegex(ValueError,'full rebuild is required'):
                u.build_command(self.candidate,self.game,False)

    def test_without_dlss_launcher_update_can_reuse_opted_out_game(self):
        (self.game.parent/'receipt.json').write_text('{"dlss_enabled":false,"framegen_enabled":false}')
        s=self.session();p=self.prepare(s)
        self.assertFalse(p['rebuild']);self.assertIn('--reuse-game',self.commands[0])
        self.assertIn('--without-dlss',self.commands[0]);self.assertNotIn('--framegen',self.commands[0])
        self.assertProtected()

    def test_invalid_framegen_receipt_is_not_silently_reused(self):
        path=self.game.parent/'receipt.json'
        for value in ({'dlss_enabled':True,'framegen_enabled':'false'},
                      {'dlss_enabled':False,'framegen_enabled':True}):
            path.write_text(json.dumps(value))
            for rebuild in (False,True):
                with self.subTest(value=value,rebuild=rebuild),self.assertRaisesRegex(ValueError,'invalid frame-generation'):
                    u.build_command(self.candidate,self.game,rebuild)

    def test_root_startup_without_active_version_is_read_only(self):
        self.assertFalse(self.sync(self.owner))
        self.assertFalse((self.port/'updates').exists());self.assertProtected()

    def test_old_activation_startup_migrates_marker_and_repairs_known_crash_state(self):
        s=self.session();p=self.prepare(s);workspace=self.owner/p['record']['workspace']
        u.durable_replace(self.port/'updates/previous.json',{'schema':1,'selection':None})
        u.durable_replace(self.port/'updates/active.json',p['record']);s.close()
        self.assertEqual(u.version(self.owner),'0.1.3')
        self.assertTrue(self.sync(workspace));self.assertEqual(u.version(self.owner),'0.1.4')
        self.assertFalse(self.sync(workspace));u.installed_sources(self.owner)
        # Simulate an interruption after active.json became durable but before
        # its derived marker was replaced. Only verified historical bytes repair.
        (self.owner/'update-version.json').write_bytes(self.files['update-version.json'])
        with self.assertRaisesRegex(ValueError,'installed-version marker'):u.installed_sources(self.owner)
        self.assertTrue(self.sync(workspace));self.assertEqual(u.version(self.owner),'0.1.4')
        self.assertProtected()

    def test_marker_failure_after_replacement_restores_pointer_and_original_source_bytes(self):
        s=self.session();p=self.prepare(s);real=u._replace_metadata
        calls=[]
        def fail(path,data):
            real(path,data);calls.append(data)
            if len(calls)==1:raise OSError('authored marker fsync failure')
        with mock.patch.object(u,'_replace_metadata',side_effect=fail):
            with self.assertRaisesRegex(OSError,'marker fsync'):s.activate(p)
        self.assertFalse((self.port/'updates/active.json').exists())
        self.assertEqual((self.owner/'update-version.json').read_bytes(),self.files['update-version.json'])
        u.installed_sources(self.owner);self.assertProtected()

    def test_new_rollback_restores_original_marker_and_exact_source_verification(self):
        s=self.session();p=self.prepare(s);workspace=self.owner/p['record']['workspace'];s.activate(p)
        self.assertEqual(u.version(self.owner),'0.1.4')
        self.rollback(workspace)
        self.assertFalse((self.port/'updates/active.json').exists())
        self.assertEqual((self.owner/'update-version.json').read_bytes(),self.files['update-version.json'])
        u.installed_sources(self.owner);self.assertProtected()

    def test_rollback_pointer_failure_restores_current_marker_and_selection(self):
        s=self.session();p=self.prepare(s);workspace=self.owner/p['record']['workspace'];s.activate(p)
        selected=self.port/'updates/active.json';before=selected.read_bytes();real=u._fsync_directory
        failed=[]
        def fail(path):
            if path==self.port/'updates' and not selected.exists() and not failed:
                failed.append(True);raise OSError('authored rollback directory failure')
            return real(path)
        with mock.patch.object(u,'_fsync_directory',side_effect=fail):
            with self.assertRaisesRegex(OSError,'rollback directory'):self.rollback(workspace)
        self.assertEqual(selected.read_bytes(),before);self.assertEqual(u.version(self.owner),'0.1.4')
        u.installed_sources(self.owner);self.assertProtected()

    def test_unknown_root_wrapper_marker_or_backup_are_never_overwritten(self):
        s=self.session();p=self.prepare(s)
        wrapper=self.owner/'Rollback_Monado_II.sh';wrapper.write_bytes(b'#!/bin/sh\n# USER OWNED\n')
        with self.assertRaisesRegex(ValueError,'Local source changes'):s.activate(p)
        self.assertEqual(wrapper.read_bytes(),b'#!/bin/sh\n# USER OWNED\n')
        self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()

    def test_unknown_untracked_legacy_wrapper_is_not_replaced_or_adopted(self):
        self.files.pop('Rollback_Monado_II.sh');manifest(self.owner,self.files)
        wrapper=self.owner/'Rollback_Monado_II.sh';wrapper.write_bytes(b'#!/bin/sh\n# USER OWNED\n')
        s=self.session();p=self.prepare(s)
        with self.assertRaisesRegex(ValueError,'unknown or modified'):s.activate(p)
        self.assertEqual(wrapper.read_bytes(),b'#!/bin/sh\n# USER OWNED\n')
        self.assertFalse((self.port/'updates/bootstrap-version.json').exists())
        self.assertEqual((self.owner/'update-version.json').read_bytes(),self.files['update-version.json'])

    def test_legacy_owner_without_new_recovery_sources_migrates_then_restores014(self):
        for name in ('Rollback_Monado_II.sh','tools/installation_version.py'):
            self.files.pop(name);(self.owner/name).unlink()
        self.files['tools/update_linux.py']=b'# Authored legacy bootstrap placeholder\n'
        original=b'{"schema":1,"protocol":1,"version":"0.1.4"}\n'
        self.files['update-version.json']=original;manifest(self.owner,self.files)
        source_manifest=(self.owner/'publication-manifest.json').read_bytes()
        self.new['update-version.json']=b'{"schema":1,"protocol":1,"version":"0.1.6"}\n'
        manifest(self.candidate,self.new)
        s=self.session(discover=lambda _:dict(version='0.1.6',tag='v0.1.6'));p=self.prepare(s)
        workspace=self.owner/p['record']['workspace']
        u.durable_replace(self.port/'updates/previous.json',{'schema':1,'selection':None})
        u.durable_replace(self.port/'updates/active.json',p['record']);s.close()
        self.assertTrue(self.sync(workspace));self.assertEqual(u.version(self.owner),'0.1.6')
        self.assertEqual((self.owner/'Rollback_Monado_II.sh').read_bytes(),self.new['Rollback_Monado_II.sh'])
        self.assertFalse((self.owner/'tools/installation_version.py').exists())
        self.assertEqual((self.owner/'publication-manifest.json').read_bytes(),source_manifest)
        u.installed_sources(self.owner)
        self.rollback(workspace)
        self.assertFalse((self.port/'updates/active.json').exists())
        self.assertEqual((self.owner/'update-version.json').read_bytes(),original)
        for name,data in self.files.items():self.assertEqual((self.owner/name).read_bytes(),data)
        u.installed_sources(self.owner);self.assertProtected()

    def test_only_owner_version_marker_gets_a_manifest_bound_override(self):
        s=self.session();p=self.prepare(s);workspace=self.owner/p['record']['workspace'];s.activate(p)
        for name,data in self.files.items():
            if name!='update-version.json':self.assertEqual((self.owner/name).read_bytes(),data)
        (self.owner/'README.md').write_bytes(b'USER AUTHORED CHANGE\n')
        with self.assertRaisesRegex(ValueError,'Local source changes'):u.installed_sources(self.owner)
        (self.owner/'README.md').write_bytes(self.files['README.md'])
        bad=workspace/'Monado_Linux/updates';bad.mkdir()
        (bad/'bootstrap-version.json').write_bytes((self.port/'updates/bootstrap-version.json').read_bytes())
        with self.assertRaisesRegex(ValueError,'only the original installation'):u.installed_sources(workspace)

    def test_version_sync_rejects_the_wrong_profile_lease_before_writes(self):
        fd=os.open(self.owner/'wrong-lock',os.O_RDWR|os.O_CREAT,0o600)
        try:
            with self.assertRaisesRegex(ValueError,'profile lease mismatch'):
                u.sync_installation_version(self.owner,self.state,fd)
        finally:os.close(fd)
        self.assertFalse((self.port/'updates').exists());self.assertProtected()

    def test_mutated_backup_or_unrecognized_marker_refuses_repair(self):
        s=self.session();p=self.prepare(s);workspace=self.owner/p['record']['workspace'];s.activate(p)
        marker=self.owner/'update-version.json';good=marker.read_bytes()
        marker.write_bytes(b'{"schema":1,"protocol":1,"version":"9.9.9"}')
        with self.assertRaisesRegex(ValueError,'installed-version marker'):self.sync(workspace)
        self.assertEqual(u.version(self.owner),'9.9.9');marker.write_bytes(good)
        backup=self.port/'updates/bootstrap-version.json';document=u.json_read(backup)
        document['original_metadata']='{"schema":1,"protocol":1,"version":"9.9.9"}'
        backup.write_text(json.dumps(document))
        with self.assertRaisesRegex(ValueError,'original source manifest'):self.sync(workspace)
        self.assertEqual(marker.read_bytes(),good);self.assertProtected()

    def test_original014_managed015_then016_rollback_uses015_marker_and014_backup(self):
        original=b'{"schema":1,"protocol":1,"version":"0.1.4"}'
        self.files['update-version.json']=original;manifest(self.owner,self.files)
        # Emulate an old 0.1.4 -> 0.1.5 activation: it leaves owner metadata at014.
        self.new['update-version.json']=b'{"schema":1,"protocol":1,"version":"0.1.5"}';manifest(self.candidate,self.new)
        s=self.session(discover=lambda _:dict(version='0.1.5',tag='v0.1.5'));p=self.prepare(s)
        old_workspace=self.owner/p['record']['workspace']
        u.durable_replace(self.port/'updates/previous.json',{'schema':1,'selection':None})
        u.durable_replace(self.port/'updates/active.json',p['record']);s.close()
        self.new['update-version.json']=b'{"schema":1,"protocol":1,"version":"0.1.6"}';manifest(self.candidate,self.new)
        gui=old_workspace/self.gui.relative_to(self.owner);game=old_workspace/self.game.relative_to(self.owner)
        for path in (gui,game):path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(b'AUTHORED NEVER EXECUTED')
        (game.parent/'receipt.json').write_text('{"dlss_enabled":true,"framegen_enabled":true}')
        session=u.Session(old_workspace,self.state,gui,game,
            discover=lambda _:dict(version='0.1.6',tag='v0.1.6'),fetch=self.fetch,
            runner=lambda command,workspace,log,emit,cancel:image(workspace))
        self.addCleanup(session.close)
        prepared=session.install(session.check()['id'],self.events.append,threading.Event())
        new_workspace=self.owner/prepared['record']['workspace'];session.activate(prepared)
        self.assertEqual(u.version(self.owner),'0.1.6');u.installed_sources(self.owner)
        self.rollback(new_workspace)
        self.assertEqual(u.version(self.owner),'0.1.5')
        self.assertEqual(u.json_read(self.port/'updates/active.json')['version'],'0.1.5')
        self.assertEqual(u.json_read(self.port/'updates/version-bridge.json')['workspace'],new_workspace.relative_to(self.owner).as_posix())
        backup=u.json_read(self.port/'updates/bootstrap-version.json')
        self.assertEqual(backup['original_metadata'].encode(),original)
        u.installed_sources(self.owner);self.assertProtected()

if __name__=='__main__':unittest.main()
