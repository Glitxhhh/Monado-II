#!/usr/bin/env python3
"""Actual navigation-query patches and selected ARM64 repeat consumers.

Synthetic input only; no game/UI, human settings or saves. Game-derived fixtures
are extracted at test time from the selected input and remain in ignored runs/.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import resource
import signal
import struct
import subprocess
import sys

import test_xcde_input_repeat as repeat

ROOT, BASE, DATA = repeat.ROOT, repeat.BASE, repeat.DATA
digest, f32, bits = repeat.digest, repeat.f32, repeat.bits

NATIVE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include "runtime/hle/framepace.c"
Settings g_settings;
/*FUNCTIONS*/
static void (*const original[])(Cpu *) = { /*ORIGINALS*/ };
static void (*const patched[])(Cpu *) = { /*PATCHES*/ };
static const unsigned dest[] = { /*DEST*/ }, base[] = { /*BASE*/ }, offset[] = { /*OFFSET*/ };
int main(void) {
    uint32_t record[2]; unsigned calls = 0;
    while (fread(record, sizeof record, 1, stdin) == 1) {
        g_settings.menu_navigation_speed = (int32_t)record[0];
        Settings setting_before = g_settings;
        float value; memcpy(&value, &record[1], 4);
        uint32_t expected_bits = record[1];
        if ((int32_t)record[0] == 1 && isfinite(value) && value > 0) {
            value = value + value; memcpy(&expected_bits, &value, 4);
        }
        for (unsigned site = 0; site < sizeof dest / sizeof *dest; site++) {
            unsigned char memory[1024], before[1024]; Cpu cpu, expected;
            for (unsigned i = 0; i < sizeof memory; i++) memory[i] = (unsigned char)(i * 11 + calls);
            memcpy(memory + offset[site], &record[1], 4); memcpy(before, memory, sizeof memory);
            for (unsigned i = 0; i < sizeof cpu; i++) ((unsigned char *)&cpu)[i] = (unsigned char)(i * 7 + calls);
            if (base[site] == 31) cpu.sp = (uintptr_t)memory;
            else cpu.x[base[site]] = (uintptr_t)memory;
            expected = cpu; original[site](&expected);
            assert(expected.v[dest[site]] == record[1]);
            expected.v[dest[site]] = expected_bits;
            patched[site](&cpu);
            assert(!memcmp(&cpu, &expected, sizeof cpu));
            assert(!memcmp(memory, before, sizeof memory));
            assert(!memcmp(&setting_before, &g_settings, sizeof g_settings));
            uint32_t result = (uint32_t)cpu.v[dest[site]];
            assert(fwrite(&result, sizeof result, 1, stdout) == 1); calls++;
        }
    }
    assert(feof(stdin));
    fprintf(stderr, "PASS %u actual checked-query patch calls; complete CPU, scalar upper lanes, borrowed memory and settings preserved\n", calls);
}
'''


def scenarios():
    rows = []
    def add(name, times, buttons=None, sticks=None, pad_mode=0, tiers=None):
        rows.append(dict(name=name, times=list(map(f32, times)), buttons=buttons or [0x1000] * len(times),
                         sticks=sticks or [(0., 0., 0., 0.)] * len(times), pad_mode=pad_mode,
                         tiers=tiers or [(0., 0., 1)]))
    for fps in (30, 60, 120, 165, 240): add(f'{fps}fps-hold', [1/fps] * (2*fps))
    add('variable', [1/165, 1/30, 1/120, 1/240, 1/60] * 25)
    add('zero-and-hitches', [0., .4, 1/165, 1/30, .01, 1.] * 10)
    add('taps', [1/165] * 10, buttons=[0, 0x1000, 0, 0, 0x1000, 0x1000, 0, 0, 0x2000, 0])
    add('release-and-repress', [1/165] * 420, buttons=[0x1000]*180+[0]*30+[0x2000]*180+[0]*30)
    add('digital-cardinals', [1/165] * 480, buttons=[b for b in (0x1000,0x2000,0x4000,0x8000) for _ in range(120)])
    add('analog-cardinals', [1/165] * 480, buttons=[0]*480,
        sticks=[(*axis,*axis) for axis in ((1.,0.),(0.,1.),(-1.,0.),(0.,-1.)) for _ in range(120)])
    add('dpad-vector-mode', [1/165] * 360, pad_mode=1)
    add('long-hold', [1/165] * 1650)
    add('custom-tiers', [1/165] * 330, tiers=[(.2,8.,3),(.8,16.,5)])
    add('face-and-shoulder', [1/165] * 330, buttons=[0xff]*330)
    add('direction-chord', [1/165] * 360, buttons=[0x3000]*120+[0x1000]*120+[0x8000]*120)
    return rows


class Selected:
    def __init__(self, nso, sites, repeat_sites):
        import unicorn
        from unicorn import arm64_const as a
        self.a = a; self.uc = unicorn.Uc(unicorn.UC_ARCH_ARM64, unicorn.UC_MODE_ARM)
        self.uc.mem_map(BASE, nso.size); self.uc.mem_write(BASE, bytes(nso.image)); self.uc.mem_map(DATA, 0x20000)
        self.sites, self.repeat_sites = sites, set(repeat_sites)
        self.repeater,self.raw,self.update,self.callback,self.zero = [DATA+i*0x1000 for i in range(5)]
        self.stop = DATA+0x1f000; self.values = set(); self.reached = set(); self.native = None
        self.uc.hook_add(unicorn.UC_HOOK_CODE, self.hook)
        self.put(BASE+0xb8a960,'Q',self.zero)
    def put(self,address,fmt,*values): self.uc.mem_write(address,struct.pack('<'+fmt,*values))
    def get(self,address,fmt): return struct.unpack('<'+fmt,self.uc.mem_read(address,struct.calcsize('<'+fmt)))
    def hook(self, uc, address, size, ignored):
        a = self.a
        if address in self.repeat_sites:
            uc.reg_write(a.UC_ARM64_REG_Q1,bits(self.delta)); uc.reg_write(a.UC_ARM64_REG_PC,address+4); return
        if address in self.sites:
            word=self.sites[address]; rn=(word>>5)&31; reg=word&31; offset=((word>>10)&4095)*4
            pointer=uc.reg_read(a.UC_ARM64_REG_SP if rn==31 else a.UC_ARM64_REG_X0+rn)
            value=self.get(pointer+offset,'I')[0]; self.values.add(value); self.reached.add(address)
            if self.variant in ('normal','fast'):
                result=self.native[(int(self.variant=='fast'),value)][address]
                uc.reg_write(a.UC_ARM64_REG_Q0+reg,result); uc.reg_write(a.UC_ARM64_REG_PC,address+4)
            return
        if address==BASE+0x25b90: uc.reg_write(a.UC_ARM64_REG_X0,self.raw)
        elif address==BASE+0x8b6dc8:
            dst,value,count=[uc.reg_read(r) for r in (a.UC_ARM64_REG_X0,a.UC_ARM64_REG_X1,a.UC_ARM64_REG_X2)]
            assert count<=0x1000; uc.mem_write(dst,bytes([value&255])*count)
        elif address==BASE+0x8b6cc8:
            y,x=[struct.unpack('<f',struct.pack('<I',uc.reg_read(r)))[0] for r in (a.UC_ARM64_REG_S0,a.UC_ARM64_REG_S1)]
            uc.reg_write(a.UC_ARM64_REG_Q0,bits(math.atan2(y,x)))
        elif address==BASE+0x8b6c28:
            guard=uc.reg_read(a.UC_ARM64_REG_X0); uc.reg_write(a.UC_ARM64_REG_X0,int(uc.mem_read(guard,1)[0]==0))
        elif address==BASE+0x8b6c38: uc.mem_write(uc.reg_read(a.UC_ARM64_REG_X0),b'\1')
        elif address==BASE+0x578ab0:
            ptr=uc.reg_read(a.UC_ARM64_REG_X1)
            self.events.append(dict(frame=self.frame,time=self.elapsed,code=self.get(ptr,'I')[0],count=self.get(ptr+4,'I')[0]))
            uc.reg_write(a.UC_ARM64_REG_X0,0)
        else: return
        uc.reg_write(a.UC_ARM64_REG_PC,uc.reg_read(a.UC_ARM64_REG_X30))
    def call(self,address,*args):
        for i,v in enumerate(args): self.uc.reg_write(self.a.UC_ARM64_REG_X0+i,v)
        self.uc.reg_write(self.a.UC_ARM64_REG_X30,self.stop); self.uc.reg_write(self.a.UC_ARM64_REG_SP,DATA+0x1e000)
        self.uc.emu_start(BASE+address,self.stop,count=10000)
        assert self.uc.reg_read(self.a.UC_ARM64_REG_PC)==self.stop,'selected instruction bound'
    def run(self,row,consumer,variant):
        self.variant=variant; self.events=[]; self.elapsed=0.; states=[]; previous=0
        self.uc.mem_write(self.repeater,bytes(0x400)); self.uc.mem_write(self.callback,bytes(0x100))
        self.call(0x578940,self.repeater); assert self.get(self.repeater+0x40,'f')[0]==.5
        self.put(self.repeater+0x50,'I',row['pad_mode']); self.put(self.callback+0x68,'Q',len(row['tiers']))
        for index,tier in enumerate(row['tiers']): self.put(self.callback+8+12*index,'ffI',*tier)
        for self.frame,(dt,held,sticks) in enumerate(zip(row['times'],row['buttons'],row['sticks'],strict=True),1):
            self.elapsed+=dt; self.delta=min(dt,f32(2*f32(1/30)))
            self.uc.mem_write(self.raw,bytes(32)); self.put(self.raw,'IIII',held&~previous,previous&~held,held,(~held)&0xffffffff)
            self.put(self.raw+16,'4f',*sticks); self.put(self.update,'Iffff',0,f32(1/30),0.,0.,0.)
            borrowed=bytes(self.uc.mem_read(self.raw,32))+bytes(self.uc.mem_read(self.update,20))
            self.call(0x578b50,self.repeater,self.update,self.zero,0)
            # Independent oracle: leave timers/query instructions unchanged;
            # halve configured delays/tier thresholds and double frequencies.
            # Face/shoulder controls intentionally retain Normal policy.
            oracle=variant=='half-policy' and row['name']!='face-and-shoulder'
            if oracle:
                self.put(self.repeater+0x40,'f',.25); self.put(self.repeater+0x54,'f',20.)
                for i,(delay,rate,count) in enumerate(row['tiers']): self.put(self.callback+8+12*i,'ffI',delay/2,rate*2,count)
            if consumer==0x164930: self.call(consumer,self.callback,self.repeater)
            else: self.call(consumer,self.repeater)
            if oracle:
                self.put(self.repeater+0x40,'f',.5); self.put(self.repeater+0x54,'f',10.)
                for i,tier in enumerate(row['tiers']): self.put(self.callback+8+12*i,'ffI',*tier)
            assert borrowed==bytes(self.uc.mem_read(self.raw,32))+bytes(self.uc.mem_read(self.update,20)),'borrowed input changed'
            states.append(digest_bytes(bytes(self.uc.mem_read(self.repeater,0x400))))
            previous=held
        return dict(events=list(self.events),state_sequence_sha256=digest_bytes(''.join(states).encode()))


def digest_bytes(data): return hashlib.sha256(data).hexdigest()


def main():
    p=argparse.ArgumentParser(description=__doc__); p.add_argument('--main',required=True,type=Path); p.add_argument('--output',required=True,type=Path); args=p.parse_args()
    if sys.platform!='linux' or platform.machine()!='x86_64': p.error('Linux x86-64 required')
    main_path=args.main.resolve(strict=True)
    if digest(main_path)!=repeat.MAIN_SHA256: p.error('selected DE 2.0.0 input mismatch')
    output=args.output.absolute()
    if output!=output.resolve() or not output.is_relative_to(ROOT/'runs') or output.exists(): p.error('fresh ordinary output under runs/ required')
    resource.setrlimit(resource.RLIMIT_CORE,(0,0)); os.umask(0o077); output.mkdir(parents=True)
    sys.path.insert(0,str(ROOT/'tools/lifter'))
    import unicorn
    from gen import Module
    from patchfmt import PatchMismatch,load_patches
    module=Module('main',str(main_path),BASE,simd='c'); patches=load_patches(str(ROOT/'games/xcde/patches.py'))
    sites={addr:module.word(addr) for addr,patch in patches.items() if patch.req=='LINUX-014'}
    assert len(sites)==14; sites=dict(sorted(sites.items())); repeat_sites=[addr for addr,patch in patches.items() if patch.req=='LINUX-012']
    def snapshot(): return dict(sorted({**repeat.snapshot(),'tools/test_xcde_navigation_speed.py':digest(Path(__file__))}.items()))
    receipt=dict(status='running',scope=__doc__,argv=sys.argv,cwd=str(ROOT),input_path=str(main_path),input_sha256=digest(main_path),
                 build_id=module.nso.build_id,host=list(platform.uname()),python=sys.version,unicorn=unicorn.__version__,sources_before=snapshot(),commands=[],
                 limitations=['Synthetic input and callback recording; no HID, game/UI, human profiles, saves or live battle/title coverage.',
                              'The existing elapsed-step correction is supplied as min(dt,2/30); its production helper has separate coverage.',
                              'Bounded memset, raw-pad lookup, atan2f and C++ guard adapters; Unicorn is a test oracle only.',
                              'Frame-quantized events retain selected-code float boundary behavior; Fast does not guarantee exactly half every observed inter-event interval.'])
    def save(): (output/'receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    def run(name,argv,data=None):
        env={'PATH':'/usr/bin:/bin','LANG':'C.UTF-8','LC_ALL':'C.UTF-8','TMPDIR':str(output)}
        command=dict(name=name,argv=list(map(str,argv)),cwd=str(ROOT),environment=env,timeout_seconds=60); receipt['commands'].append(command); save()
        child=subprocess.Popen(command['argv'],cwd=ROOT,env=env,stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,start_new_session=True)
        try: stdout,stderr=child.communicate(data,timeout=60)
        except BaseException:
            os.killpg(child.pid,signal.SIGKILL); child.communicate(timeout=5); raise
        (output/(name+'.stdout')).write_bytes(stdout); (output/(name+'.stderr')).write_bytes(stderr); command['exit']=child.returncode; save()
        if child.returncode: raise RuntimeError(f'{name} failed; see {output}')
        return stdout
    save()
    try:
        functions=[]; registers=[]; bases=[]; offsets=[]
        for i,(addr,word) in enumerate(sites.items()):
            assert word&0xffc00000==0xbd400000,'expected scalar unsigned-offset LDR'
            registers.append(word&31); bases.append((word>>5)&31); offsets.append(((word>>10)&4095)*4)
            functions.append(f'static void original_{i}(Cpu *c) {{ {module.lift(addr).code} }}')
            functions.append(f'static void patched_{i}(Cpu *c) {{ {patches[addr].apply(module,addr)} }}')
            original_word=module.word; module.word=lambda address,original_word=original_word:original_word(address)^1
            try: patches[addr].apply(module,addr)
            except PatchMismatch: pass
            else: raise AssertionError('mismatch guard accepted altered input')
            finally: module.word=original_word
        fixture=NATIVE.replace('/*FUNCTIONS*/','\n'.join(functions))
        for key,values in [('ORIGINALS',[f'original_{i}' for i in range(14)]),('PATCHES',[f'patched_{i}' for i in range(14)]),('DEST',registers),('BASE',bases),('OFFSET',offsets)]: fixture=fixture.replace('/*'+key+'*/',','.join(map(str,values)))
        source=output/'query_fixture.c'; source.write_text(fixture); compiler=ROOT/'.deps/bin/clang-cc'; binary=output/'query_fixture'
        receipt['compiler_sha256']=digest(compiler); run('compiler-version',[compiler,'--version'])
        run('compile',[compiler,'-O1','-UNDEBUG','-march=x86_64_v2','-ffp-contract=off','-fno-strict-aliasing','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-I',ROOT,source,'-lm','-o',binary])
        runner=Selected(module.nso,sites,repeat_sites); rows=scenarios(); expected={}
        for row in rows:
            for consumer in (0x164930,0x1c1ef0):
                key=(row['name'],consumer)
                expected[key]={variant:runner.run(row,consumer,variant) for variant in ('original','half-policy')}
                assert expected[key]['original']['state_sequence_sha256']==expected[key]['half-policy']['state_sequence_sha256']
        assert runner.reached==set(sites),'some query hooks not reached'
        boundaries=[0,0x80000000,1,0x007fffff,0x00800000,0x3e800000,0x3f000000,0x3f800000,0xbf800000,0x7f000000,0x7f7fffff,0x7f800000,0xff800000,0x7fc12345,0x7f812345,0xffc12345]
        values=sorted(runner.values|set(boundaries)); records=[(mode,value) for mode in (0,1,-1,2) for value in values]
        payload=b''.join(struct.pack('<iI',*row) for row in records); (output/'native-input.bin').write_bytes(payload)
        returned=run('native-query-patches',[binary],payload); assert len(returned)==len(records)*14*4
        lookup={key:dict(zip(sites,struct.unpack_from('<14I',returned,i*56),strict=True)) for i,key in enumerate(records)}
        runner.native=lookup; results=[]
        for row in rows:
            for consumer in (0x164930,0x1c1ef0):
                key=(row['name'],consumer); normal=runner.run(row,consumer,'normal'); fast=runner.run(row,consumer,'fast')
                assert normal==expected[key]['original'],f'{key}: Normal diverged'
                assert fast==expected[key]['half-policy'],f'{key}: Fast differs from independent half-delay/double-frequency oracle'
                assert normal['state_sequence_sha256']==fast['state_sequence_sha256'],f'{key}: raw held-duration state changed'
                if row['name']=='taps': assert [x['frame'] for x in fast['events']]==[2,5,9]
                if row['name'].endswith('fps-hold'):
                    assert len(fast['events'])>len(normal['events'])
                    assert .25<=fast['events'][1]['time']<=.4 and .5<=normal['events'][1]['time']<=.7
                results.append(dict(name=row['name'],consumer=hex(BASE+consumer),frames=len(row['times']),normal=normal,fast=fast,raw_state_identical=True,half_policy_exact=True))
        receipt.update(results=results,query_sites=list(map(hex,sites)),all_query_sites_reached=True,all_mismatch_guards_passed=True,native_patch_calls=len(records)*14,native_values=len(values),native_settings=[0,1,-1,2])
        receipt['sources_after']=snapshot(); assert receipt['sources_before']==receipt['sources_after'],'sources changed during run'; assert digest(main_path)==receipt['input_sha256']
        receipt['status']='passed'
    except BaseException as error:
        receipt.update(status='failed',error=f'{type(error).__name__}: {error}'); raise
    finally:
        receipt['artifacts']={x.name:digest(x) for x in sorted(output.iterdir()) if x.is_file() and x.name!='receipt.json'}; save()
    print(f"PASS {len(receipt['results'])} selected cases, {receipt['native_patch_calls']} native checked-query calls; {output/'receipt.json'}")

if __name__=='__main__': main()
