"""LINUX-099 real private Play IPC and authored native process/guard fixtures."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import types
import unittest
from unittest import mock
if sys.platform=='linux':import fcntl
ROOT=Path(__file__).resolve().parents[1]
PORT=ROOT/'Monado_Linux'

def load(name,path):
    spec=importlib.util.spec_from_file_location(name,path);module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module
broker_module=load('authored_game_broker',PORT/'scripts/launcher_game_broker.py') if sys.platform=='linux' else None
if sys.platform=='linux':
    sys.path.insert(0,str(PORT/'scripts'))
    try:launcher=load('authored_game_launcher',PORT/'scripts/launcher_linux.py')
    finally:sys.path.pop(0)
else:launcher=None

CLIENT=r'''
#include <cassert>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <vector>
#include <signal.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern "C" bool rt_linux_state_guard(const char*,char*,size_t);
namespace launcher_linux {
static std::string project,game_project,game,error;
static std::vector<std::string> test_environment;
static pid_t game_pid,precompile_pid;
static bool game_seen;static int last_result,legacy_calls;
static bool update_applying(){return false;}
// Use the production shared preparation/activity state. Play's actual parser
// and poller also consume FSR4 status frames; a local stub would hide that gate.
#include "BROKER_ACTIVITY_SOURCE"
static pid_t spawn(const std::vector<std::string>&,const std::string&){legacy_calls++;return 0;}
static bool process_tree_has_game(pid_t,const std::string&){return false;}
#include "BROKER_SOURCE"
}
int main(int argc,char**argv){
 using namespace launcher_linux;std::string state,mode="normal";int fd=-1;
 for(int i=1;i+1<argc;i+=2){if(!strcmp(argv[i],"--game-fd"))fd=atoi(argv[i+1]);else if(!strcmp(argv[i],"--state-root"))state=argv[i+1];else if(!strcmp(argv[i],"--mode"))mode=argv[i+1];else assert(!strcmp(argv[i],"--lock-fd")||!strcmp(argv[i],"--game-executable"));}
 if(mode=="parser"){
  assert(!game_adopt_fd(0));int bad[2];assert(!pipe(bad));assert(!game_adopt_fd(bad[0]));close(bad[0]);close(bad[1]);assert(!socketpair(AF_UNIX,SOCK_STREAM,0,bad));assert(!game_adopt_fd(bad[0]));close(bad[0]);close(bad[1]);
  game_pid=-1;game_request=1;
  assert(!game_receive("1\trunning\t2\twrong request"));assert(!game_receive("1\texit\t1\t999\tbad"));
  assert(!game_receive("1\texit\t1\t-1\tbad"));assert(!game_receive("1\trunning\t01\tbad"));
  assert(game_receive("1\trunning\t1\tGame running"));assert(game_seen);
  assert(game_receive("1\texit\t1\t0\tdone")&&!game_pid&&!last_result);
  game_broker_configured=false;assert(!start_game(state,true)&&!legacy_calls);
  fsr4_active=true;assert(activity_busy());assert(!start_game(state,false)&&!legacy_calls);
  fsr4_active=false;
  assert(!start_game(state,false)&&legacy_calls==1);return 0;
 }
 assert(fd>=3&&!state.empty());
 if(mode=="badhello"){
  std::string bad="1\thello\t"+std::to_string(getpid()+1);assert(send(fd,bad.data(),bad.size(),MSG_NOSIGNAL)>0);char reply[16];assert(recv(fd,reply,sizeof reply,0)==0);return 0;
 }
 assert(game_adopt_fd(fd));
 char errorbuf[512];assert(rt_linux_state_guard(state.c_str(),errorbuf,sizeof errorbuf));
 // GUI confinement stays active, while the host child will write this authored
 // sibling marker to prove it did not inherit the GUI's Landlock ruleset.
 std::string outside=state+"/../host-launched.marker";errno=0;int denied=open(outside.c_str(),O_WRONLY|O_CREAT,0600);assert(denied<0&&errno==EACCES);
 for(int i=0;i<500&&!game_broker_ready&&game_fd>=0;i++){game_broker_poll();usleep(1000);}assert(game_broker_ready);
 if(mode=="badpeer"){
  pid_t p=fork();assert(p>=0);if(!p){const char*bad="1\tplay\t1";assert(send(fd,bad,strlen(bad),MSG_NOSIGNAL)>0);_exit(0);}int status;assert(waitpid(p,&status,0)==p);
  for(int i=0;i<500&&game_fd>=0;i++){game_broker_poll();usleep(1000);}assert(game_fd<0);return 0;
 }
 if(mode=="badcommand"||mode=="oversized"){
  std::string bad=mode=="oversized"?std::string(2049,'x'):"1\tplay\t1\t/arbitrary-command";assert(send(fd,bad.data(),bad.size(),MSG_NOSIGNAL)==(ssize_t)bad.size());
  for(int i=0;i<500&&game_fd>=0;i++){game_broker_poll();usleep(1000);}assert(game_fd<0);return 0;
 }
 unsigned runs=mode=="twice"?2:1;
 for(unsigned run=0;run<runs;run++){
  assert(start_game(state,true));assert(!start_game(state,true)); // pending is busy
  for(int i=0;i<1000&&game_pid;i++){
   std::string message=poll_game();if(!message.empty())printf("%s\n",message.c_str());
   if(game_seen&&(mode=="close"||mode=="crash")){fflush(stdout);if(mode=="crash")_exit(19);return 0;}
   if(game_seen&&mode=="replay"){
    assert(game_send("1\tplay\t"+std::to_string(game_request)));
    for(int n=0;n<500&&game_fd>=0;n++){game_broker_poll();usleep(1000);}assert(game_fd<0);return 0;
   }
   usleep(1000);
  }
  assert(!game_pid);
  if(mode=="error")assert(last_result==1&&!game_seen);else assert(last_result==0&&game_seen);
 }
 return 0;
}
'''
GAME=r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
int main(int argc,char**argv){assert(argc==2);struct timespec t={0,180000000};nanosleep(&t,0);char file[4096];snprintf(file,sizeof file,"%s/finished",argv[1]);int f=open(file,O_WRONLY|O_CREAT|O_APPEND,0600);assert(f>=0&&write(f,"finished\n",9)==9);close(f);return 0;}
'''
HELPER='''import argparse,json,os
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--executable');p.add_argument('--state-root');p.add_argument('--env',action='append');a=p.parse_args()
s=Path(a.state_root)
(s.parent/'host-launched.marker').write_text('fresh host child, outside GUI guard')
data=vars(a);data['inherited_files']=[os.readlink('/proc/self/fd/'+n) for n in os.listdir('/proc/self/fd') if n.isdigit() and int(n)>2 and os.path.exists('/proc/self/fd/'+n)]
(s/'argv.json').write_text(json.dumps(data))
os.execv(a.executable,[a.executable,a.state_root])
'''

@unittest.skipUnless(sys.platform=='linux','Linux credentialed Play broker')
class GameBrokerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        import resource
        resource.setrlimit(resource.RLIMIT_CORE,(0,0))
        cls.cc=os.environ.get('MONADO_TEST_CC') or shutil.which('clang')
        cls.cxx=os.environ.get('MONADO_TEST_CXX') or shutil.which('clang++')
        if not cls.cc or not cls.cxx:raise unittest.SkipTest('Clang C and C++ required')
        cls.temp=tempfile.TemporaryDirectory(prefix='monado-host-play-');cls.addClassCleanup(cls.temp.cleanup);cls.root=Path(cls.temp.name)
        cls.client=cls.root/'client';cls.game=cls.root/'authored-game';source=(PORT/'tools/launcher/linux_host.h').read_text();a=source.index('// [XCDE] LINUX-099: a host-owned');b=source.index('// [XCDE] LINUX-017:',a)
        pa=source.index('static std::string poll_game() {');pb=source.index('static bool copy_file(',pa)
        # The shared preparation/activity declarations precede the Play broker
        # block. Keep them from the same production header, including the real
        # activity gate; the separate FSR4 fixture exercises its full protocol.
        aa=source.index('static bool fsr4_active, fsr4_models_ready;')
        ab=source.index('static bool game_seen;',aa)
        (cls.root/'activity.inc').write_text(source[aa:ab])
        (cls.root/'broker.inc').write_text(source[a:b]+source[pa:pb]);(cls.root/'client.cpp').write_text(CLIENT.replace('BROKER_ACTIVITY_SOURCE',str(cls.root/'activity.inc')).replace('BROKER_SOURCE',str(cls.root/'broker.inc')));(cls.root/'game.c').write_text(GAME)
        flags=['-O1','-UNDEBUG','-pthread'];san=['-fsanitize=undefined','-fno-sanitize-recover=all'] if os.environ.get('MONADO_TEST_UBSAN') else []
        commands=[[cls.cc,*flags,*san,'-c',str(PORT/'runtime/platform/linux_state_guard.c'),'-o',str(cls.root/'guard.o')], [cls.cxx,*flags,*san,str(cls.root/'client.cpp'),str(cls.root/'guard.o'),'-o',str(cls.client)], [cls.cc,*flags,*san,str(cls.root/'game.c'),'-o',str(cls.game)]]
        for command in commands:
            result=subprocess.run(command,capture_output=True,text=True,timeout=90)
            if result.returncode:raise AssertionError(result.stdout+result.stderr)
    def setUp(self):
        self.case=tempfile.TemporaryDirectory(prefix='case-',dir=self.root);self.addCleanup(self.case.cleanup);self.base=Path(self.case.name);self.state=self.base/'state';self.state.mkdir();(self.state/'logs').mkdir()
        self.port=self.base/'Monado_Linux';(self.port/'scripts').mkdir(parents=True);self.helper=self.port/'scripts/run_linux.py';self.helper.write_text(HELPER)
        self.lock=os.open(self.state/'.session.lock',os.O_RDWR|os.O_CREAT,0o600);self.addCleanup(os.close,self.lock);fcntl.flock(self.lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
        self.runtime=os.open(self.state/'.runtime-lease',os.O_RDWR|os.O_CREAT,0o600);self.addCleanup(os.close,self.runtime);fcntl.flock(self.runtime,fcntl.LOCK_SH|fcntl.LOCK_NB)
        self.log=os.open(self.state/'logs/gui.log',os.O_WRONLY|os.O_APPEND|os.O_CREAT,0o600);self.addCleanup(os.close,self.log)
        identity=hashlib.sha256(self.game.read_bytes()).hexdigest()
        def plan(executable,state):
            if Path(executable)!=self.game or Path(state)!=self.state:raise ValueError('wrong fixed selection')
            return {'argv':[str(self.game)],'state':str(self.state),'input':str(self.base/'authored-input'),'binary_sha256':hashlib.sha256(self.game.read_bytes()).hexdigest()}
        self.policy=types.SimpleNamespace(ROOT=self.port,plan=plan)
        self.environment={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8'}
        self.broker=broker_module.GameBroker(state=self.state,launcher=self.client,executable=self.game,executable_sha256=identity,policy=self.policy,environment=self.environment,profile_lock_fd=self.lock,runtime_fd=self.runtime,log_fd=self.log)
        self.addCleanup(self.broker.finish)
    def session(self,mode='normal',namespace=False):
        command=[str(self.client),'--state-root',str(self.state),'--mode',mode]
        if namespace:
            probe=subprocess.run(['/usr/bin/bwrap','--unshare-user','--ro-bind','/','/','--','/usr/bin/true'],capture_output=True)
            if probe.returncode:self.skipTest('host sandbox denies namespace creation')
            command=['/usr/bin/bwrap','--unshare-user','--unshare-net','--die-with-parent','--ro-bind','/','/','--bind',str(self.base),str(self.base),'--proc','/proc','--',*command]
        with mock.patch.object(launcher,'ROOT',self.port):
            code=launcher.wait_session(command,self.environment,self.lock,self.log,games=self.broker,runtime_fd=self.runtime)
        if code not in (0,19):self.fail('authored GUI exit '+str(code)+'\n'+(self.state/'logs/gui.log').read_text())
        return code
    def test_actual_host_launch_keeps_gui_guard_and_fixed_selection(self):
        self.assertEqual(self.session(),0)
        self.assertEqual((self.state/'finished').read_text(),'finished\n')
        self.assertTrue((self.base/'host-launched.marker').is_file())
        args=json.loads((self.state/'argv.json').read_text());self.assertEqual(args['executable'],str(self.game));self.assertEqual(args['state_root'],str(self.state));self.assertEqual(args['env'],['XCDE_STRICT=1','XCDE_NO_ERROR_DIALOG=1','XCDE_INI='+str(self.state/'xcde.ini')]);self.assertIn(str(self.state/'.session.lock'),args['inherited_files']);self.assertIn(str(self.state/'.runtime-lease'),args['inherited_files'])
    def test_game_survives_normal_gui_close(self):
        self.assertEqual(self.session('close'),0);self.assertTrue((self.state/'finished').is_file())
    def test_game_survives_gui_crash(self):
        self.assertEqual(self.session('crash'),19);self.assertTrue((self.state/'finished').is_file())
    def test_abrupt_helper_exit_recovers_exact_adopted_native(self):
        # Deliberately fail the authored helper immediately after it creates a
        # native orphan, before the usual 30 ms process-tree observation.
        helper=HELPER.replace('os.execv(a.executable,[a.executable,a.state_root])',
            'import subprocess\nsubprocess.Popen([a.executable,a.state_root],close_fds=False)\nos._exit(17)')
        self.helper.write_text(helper);self.broker.helper_sha256=broker_module.digest(self.helper)
        self.assertEqual(self.session('close'),0);self.assertTrue((self.state/'finished').is_file());self.assertTrue(self.broker.seen)
    def test_second_play_and_duplicate_pending_are_bounded(self):
        self.assertEqual(self.session('twice'),0);self.assertEqual((self.state/'finished').read_text().count('finished'),2)
    def test_replay_disconnect_does_not_kill_running_game(self):
        self.assertEqual(self.session('replay'),0);self.assertTrue((self.state/'finished').is_file());self.assertFalse(self.broker.connected)
    def test_descendant_cannot_impersonate_verified_gui(self):
        self.assertEqual(self.session('badpeer'),0);self.assertFalse((self.state/'finished').exists());self.assertFalse(self.broker.connected)
    def test_no_arbitrary_command_fields_are_admitted(self):
        self.assertEqual(self.session('badcommand'),0);self.assertFalse((self.base/'host-launched.marker').exists())
    def test_wrong_hello_pid_is_rejected(self):
        self.assertEqual(self.session('badhello'),0);self.assertFalse(self.broker.connected);self.assertFalse((self.base/'host-launched.marker').exists())
    def test_oversized_packet_is_rejected(self):
        self.assertEqual(self.session('oversized'),0);self.assertFalse(self.broker.connected);self.assertFalse((self.base/'host-launched.marker').exists())
    def test_changed_game_receipt_stops_before_host_spawn(self):
        self.broker.executable_sha256='0'*64;self.assertEqual(self.session('error'),0);self.assertFalse((self.base/'host-launched.marker').exists())
    def test_precompile_activity_blocks_play(self):
        self.broker.native_idle=False
        # The real client sends activity idle, so override only the observed
        # extra child query to model a still-owned shader worker.
        original=broker_module.children
        def active(pid):return {999999} if pid==self.broker.gui_pid else original(pid)
        with mock.patch.object(broker_module,'children',side_effect=active):self.assertEqual(self.session('error'),0)
        self.assertFalse((self.base/'host-launched.marker').exists())
    def test_changed_helper_stops_before_host_spawn(self):
        self.helper.write_text(HELPER+'\n# changed authored helper\n');self.assertEqual(self.session('error'),0);self.assertFalse((self.base/'host-launched.marker').exists())
    def test_update_install_blocks_play(self):
        self.broker.activity=lambda:True;self.assertEqual(self.session('error'),0);self.assertFalse((self.base/'host-launched.marker').exists())
    def test_native_parser_and_no_broker_smooth_fail_closed(self):
        result=subprocess.run([str(self.client),'--mode','parser'],capture_output=True,text=True,timeout=5);self.assertEqual(result.returncode,0,result.stdout+result.stderr)
    def test_actual_namespace_transport_and_gui_guard(self):
        self.assertEqual(self.session(namespace=True),0);self.assertTrue((self.base/'host-launched.marker').is_file())
    def test_no_updates_argument_does_not_disable_host_game_service(self):
        # Exercise the actual main --no-updates admission path, not only the
        # lower-level wait loop. Selection validation is an authored fixture;
        # broker construction, descriptor transport, guard and child exec are real.
        shutil.copyfile(PORT/'scripts/launcher_game_broker.py',self.port/'scripts/launcher_game_broker.py')
        launch={'argv':[str(self.client),'--state-root',str(self.state),'--mode','normal','--game-executable',str(self.game)],'state':str(self.state),'input':str(self.base/'authored-input'),'host_game_broker_capable':True,'game_sha256':hashlib.sha256(self.game.read_bytes()).hexdigest(),'game_library_path':'','library_path':''}
        with mock.patch.object(launcher,'ROOT',self.port),mock.patch.object(launcher,'recover_input',return_value=True),mock.patch.object(launcher,'prepare',return_value=launch),mock.patch.object(launcher,'check_input',return_value={}),mock.patch.object(launcher,'acquire_lock',side_effect=lambda _state:os.dup(self.lock)),mock.patch.object(launcher,'clean_environment',return_value=self.environment),mock.patch.object(launcher.run_linux,'ROOT',self.port),mock.patch.object(launcher.run_linux,'plan',side_effect=self.policy.plan),mock.patch.object(launcher.run_linux,'validate_runtime'),mock.patch.object(launcher.install_lease,'acquire_runtime',side_effect=lambda _root:os.dup(self.runtime)),mock.patch.object(launcher.install_lease,'release',side_effect=os.close):
            self.assertEqual(launcher.main(['--no-updates','--launcher-executable',str(self.game),'--game-executable',str(self.game),'--state-root',str(self.state)]),0)
        self.assertTrue((self.state/'finished').is_file())
