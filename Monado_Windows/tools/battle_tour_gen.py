"""Battle tour (tests/tours/battle_types.txt): one fight per enemy species (same arts set = same monster at another
level) at a spawn that stays after the story (S_FLG_MAX = the table maximum), for a finished save (XCDE_BATCH_SAVE).
  python tools/battle_tour_gen.py
"""
import os, re, sys, subprocess, ast, collections
sys.path.insert(0,'tools')
os.environ['PYTHONIOENCODING']='utf-8'
BD='targets/romfs/bdat'
def rows(fn, table):
    out=subprocess.run([sys.executable,'tools/bdat1.py',os.path.join(BD,fn),table,'100000'],capture_output=True,encoding='utf-8',env=os.environ).stdout
    res=[];cur=False
    for l in out.splitlines():
        if l.startswith('== '): cur=l[3:].split(' (')[0]==table; continue
        if cur and l.strip():
            i=l.index(' '); res.append((int(l[:i]),ast.literal_eval(l[i+1:])))
    return res
tot=0; allids=set(); out=[]; maps=0
for fn in sorted(os.listdir(BD)):
    m=re.match(r'bdat_(ma\d{4})\.bdat$',fn)
    if not m: continue
    name=m.group(1); r=rows(fn,'poplist'+name[2:])
    if not r: continue
    smax=max(x['S_FLG_MAX'] for _,x in r)
    seen=set(); picks=[]
    for rid,x in r:
        if x['S_FLG_MAX']<smax: continue   # story-only spawns: gone in a finished save
        ids={x['ene%dID'%k] for k in range(1,6) if x['ene%dID'%k]}
        new=ids-seen
        if new and (x['posX'],x['posY'],x['posZ'])!=(0,0,0):
            seen|=ids; picks.append((rid,x,sorted(ids)))
    allids|=seen; tot+=len(picks); maps+=1
    a,s=int(name[2:4]),int(name[4:6])
    for rid,x,ids in picks:
        out+=['# %s enemies %d (types %s)'%(name,rid,' '.join(map(str,ids))),'warp %d %d %g %g %g 0'%(a,s,x['posX'],x['posY']+2,x['posZ']),'battle 2400']
    print(name,len(r),'spawns,',len(picks),'battles,',len(seen),'types')
print(maps,'maps',tot,'battles',len(allids),'enemy types')
open('tests/tours/battle_types.txt','w').write('\n'.join(out)+'\n')

# species: the same arts set = the same monster at another level (same model, effects)
out2=[]; species=set(); n=0; unk=0
for fn in sorted(os.listdir(BD)):
    m=re.match(r'bdat_(ma\d{4})\.bdat$',fn)
    if not m: continue
    name=m.group(1); r=rows(fn,'poplist'+name[2:])
    if not r: continue
    el={rid:x for rid,x in rows(fn,'BTL_enelist'+name[2:])}
    if n==0: print('enelist ids', min(el), max(el), 'sample pop ids', [x['ene1ID'] for _,x in r[:5]])
    smax=max(x['S_FLG_MAX'] for _,x in r)
    a,s=int(name[2:4]),int(name[4:6])
    for rid,x in r:
        if x['S_FLG_MAX']<smax or (x['posX'],x['posY'],x['posZ'])==(0,0,0): continue
        keys=set()
        for k in range(1,6):
            i=x['ene%dID'%k]
            if not i: continue
            e=el.get(i)
            if e is None: unk+=1; keys.add(('id',i)); continue
            keys.add((e['family'],e['atk1'],e['arts1'],e['arts2'],e['arts3'],e['named']))
        new=keys-species
        if new:
            species|=keys; n+=1
            out2+=['# %s enemies %d'%(name,rid),'warp %d %d %g %g %g 0'%(a,s,x['posX'],x['posY']+2,x['posZ']),'battle 2400']
print('species battles', n, 'species', len(species), 'unknown ids', unk)
open('tests/tours/battle_types.txt','w').write('\n'.join(out2)+'\n')
