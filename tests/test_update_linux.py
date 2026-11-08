"""Authored transaction and save-preservation fixtures; no real game or profile."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile
import threading
import unittest
from unittest import mock
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import update_linux as u
import update_release as r


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
        (self.state/'save/slot').write_bytes(b'AUTHORED SAVE SENTINEL');(self.state/'xcde.ini').write_bytes(b'fps = 165\n')
        (self.owner/'game_files/exefs').mkdir(parents=True);(self.owner/'game_files/romfs').mkdir()
        (self.owner/'game_files/romfs/synthetic').write_bytes(b'AUTHORED INPUT SENTINEL')
        self.game=self.port/'build/old-game/xcde-linux';self.gui=self.port/'build/old-gui/monado-launcher'
        for p in [self.game,self.gui]:p.parent.mkdir(parents=True);p.write_bytes(b'NONEXECUTED AUTHORED PLACEHOLDER')
        (self.game.parent/'receipt.json').write_text('{"dlss_enabled":true,"framegen_enabled":false}')
        helper=(Path(__file__).resolve().parents[1]/'Monado_Linux/scripts/install_layout.py').read_bytes()
        self.files={'README.md':b'# Authored fixture\n','Build_Monado_II.sh':b'#!/bin/sh\nexit 1\n','Monado_Linux/scripts/install_layout.py':helper,'Monado_Linux/runtime/fixture.c':b'// authored runtime\n','Monado_Linux/tools/launcher/launcher.cpp':b'// old GUI\n','update-version.json':b'{"schema":1,"protocol":1,"version":"0.1.3"}'}
        manifest(self.owner,self.files);self.candidate=self.base/'candidate';self.candidate.mkdir()
        self.new=dict(self.files);self.new['update-version.json']=b'{"schema":1,"protocol":1,"version":"0.1.4"}';self.new['Monado_Linux/tools/launcher/launcher.cpp']=b'// new GUI\n';manifest(self.candidate,self.new)
        self.before=self.protected();self.commands=[];self.events=[]
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
    def test_no_update_is_quiet_and_creates_no_update_paths(self):
        self.assertIsNone(self.session(discover=lambda _:None).check());self.assertFalse((self.port/'updates').exists());self.assertProtected()
    def test_launcher_only_game_reuse_and_post_session_activation(self):
        s=self.session();p=self.prepare(s);self.assertFalse(p['rebuild']);self.assertIn('--reuse-game',self.commands[0]);self.assertEqual(self.commands[0][-1],str(self.game));self.assertFalse((self.port/'updates/active.json').exists());self.assertProtected()
        command=s.activate(p);record=u.json_read(self.port/'updates/active.json');self.assertEqual(record['version'],'0.1.4');self.assertEqual(command[-2:],['--state-root',str(self.state)]);self.assertProtected();self.assertEqual((self.owner/'README.md').read_bytes(),self.files['README.md'])
        managed=self.owner/record['workspace'];self.assertEqual(u.json_read(managed/'installation.json')['game_port'],str(self.port));self.assertEqual(u.dispatch_active(self.owner,['--dry-run'])[-1],'--dry-run');self.assertIsNone(u.dispatch_active(managed,[]))
    def test_runtime_change_triggers_automatic_game_rebuild(self):
        self.new['Monado_Linux/runtime/fixture.c']+=b'// changed\n';manifest(self.candidate,self.new);s=self.session();p=self.prepare(s);self.assertTrue(p['rebuild']);self.assertNotIn('--reuse-game',self.commands[0]);binding=u.json_read(self.owner/p['record']['workspace']/'installation.json');self.assertEqual(binding['game_port'],str(self.owner/p['record']['workspace']/'Monado_Linux'));self.assertProtected()
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
    def test_pointer_traversal_and_symlink_are_refused(self):
        d=self.port/'updates';d.mkdir();u.write_new(d/'active.json',{'schema':1,'workspace':'../../elsewhere'})
        with self.assertRaisesRegex(ValueError,'active update'):u.dispatch_active(self.owner,[])
        (d/'active.json').unlink();(d/'active.json').symlink_to(self.owner/'README.md')
        with self.assertRaisesRegex(ValueError,'canonical'):u.dispatch_active(self.owner,[])
        self.assertProtected()
    def test_optout_and_custom_build_features_never_silently_dropped(self):
        path=self.game.parent/'receipt.json';path.write_text('{"dlss_enabled":false}')
        self.assertIn('--without-dlss',u.build_command(self.candidate,self.game,False));path.write_text('{"dlss_enabled":true,"framegen_enabled":true}')
        with self.assertRaisesRegex(ValueError,'manual rebuild'):u.build_command(self.candidate,self.game,True)
        self.assertIn('--reuse-game',u.build_command(self.candidate,self.game,False))

if __name__=='__main__':unittest.main()
