# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
import argparse, hashlib, json, os, pathlib, shutil, subprocess, time
parser=argparse.ArgumentParser()
parser.add_argument('--output',type=pathlib.Path,required=True)
args=parser.parse_args()
root=pathlib.Path.cwd(); out=args.output.resolve()
if out.exists() and any(out.iterdir()): raise RuntimeError('Use an empty output directory')
out.mkdir(parents=True)
paths=['src/xrt/compositor/main/comp_compositor.c','src/xrt/compositor/multi/comp_multi_compositor.c','src/xrt/compositor/multi/comp_multi_private.h','src/xrt/compositor/util/comp_swapchain_gpu_reuse.c','src/xrt/compositor/util/comp_swapchain_gpu_reuse.h','src/xrt/compositor/util/comp_semaphore.c','src/xrt/compositor/util/comp_semaphore.h']
original='805316bebe976256a16c591ee135a5422ffe73a3'
modified={p:(root/p).read_bytes() for p in paths}
baseline={p:subprocess.check_output(['git','show',original+':'+p],cwd=root) for p in paths}
for p in paths:
 fixture=root/'doc/results/macos-gpu-service-20261011/baseline-source-overrides'/p
 if fixture.exists(): baseline[p]=fixture.read_bytes()
for mode, sources in [('baseline-sources',baseline),('modified-sources',modified)]:
 for p,data in sources.items():
  dest=out/mode/p;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
(out/'scope.json').write_text(json.dumps({'head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'baseline':original,'frames_per_run':7200,'warmup_client_frames':120,'order':['A0','B0','B1','A1'],'changed_sources':paths,'source_hashes':{mode:{p:hashlib.sha256(data).hexdigest() for p,data in src.items()} for mode,src in [('A',baseline),('B',modified)]}},indent=2)+'\n')
(out/'working-tree.patch').write_bytes(subprocess.check_output(['git','diff']))
shutil.copy(__file__,out/'run-comparison.py')
plist=pathlib.Path.home()/'Library/LaunchAgents/org.freedesktop.monado.service.plist'
domain=f'gui/{os.getuid()}'
(out/'installed-service.plist').write_bytes(plist.read_bytes())
(out/'installed-service-before.txt').write_text(subprocess.check_output(['launchctl','print',domain+'/org.freedesktop.monado.service'],text=True))
active=modified; stopped=False
try:
 subprocess.run(['launchctl','bootout',domain,str(plist)],check=True,timeout=15);stopped=True
 time.sleep(2)
 for mode,name in [('A','baseline-0'),('B','modified-0'),('B','modified-1'),('A','baseline-1')]:
  desired=baseline if mode=='A' else modified
  for p in paths:
   if (root/p).read_bytes()!=active[p]:raise RuntimeError('Concurrent source modification: '+p)
   (root/p).write_bytes(desired[p])
  active=desired
  print('Building '+name,flush=True)
  with (out/(name+'-build.txt')).open('w') as log:
   subprocess.run(['cmake','--build','build/arm64','--parallel','--target','monado-service','openxr_monado','tests_macos_service_gpu_sync'],stdout=log,stderr=log,check=True,timeout=120)
  binary_paths=['build/arm64/src/xrt/targets/service/monado-service','build/arm64/src/xrt/targets/openxr/libopenxr_monado.dylib','build/arm64/tests/tests_macos_service_gpu_sync']
  (out/(name+'-binary-hashes.json')).write_text(json.dumps({p:hashlib.sha256((root/p).read_bytes()).hexdigest() for p in binary_paths},indent=2)+'\n')
  print('Measuring '+name+' (7200 frames; approximately 60 seconds)',flush=True)
  subprocess.run(['caffeinate','-di','python3','scripts/macos/run-gpu-sync-service.py','--headset','--benchmark','--client-timeout','95','--output',str(out/name),'--client','build/arm64/tests/tests_macos_service_gpu_sync','normal','7200','90'],check=True,timeout=120)
finally:
 try:
  for p in paths:
   if (root/p).read_bytes()!=active[p]:
    dest=out/'unexpected-sources'/p;dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes((root/p).read_bytes())
   (root/p).write_bytes(modified[p])
  print('Restored modified sources; rebuilding all targets',flush=True)
  with (out/'restored-build.txt').open('w') as log:
   subprocess.run(['cmake','--build','build/arm64','--parallel'],stdout=log,stderr=log,check=True,timeout=120)
 finally:
  if stopped:
   print('Restoring installed service',flush=True)
   subprocess.run(['launchctl','bootstrap',domain,str(plist)],check=True,timeout=15)
   subprocess.run(['launchctl','kickstart',domain+'/org.freedesktop.monado.service'],check=True,timeout=15)
   (out/'installed-service-after.txt').write_text(subprocess.check_output(['launchctl','print',domain+'/org.freedesktop.monado.service'],text=True))
