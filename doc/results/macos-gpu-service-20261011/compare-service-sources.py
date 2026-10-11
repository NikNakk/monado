import subprocess, tempfile, pathlib, json, time, argparse
root=pathlib.Path(__file__).resolve().parents[3]
parser=argparse.ArgumentParser();parser.add_argument('--output',type=pathlib.Path,required=True)
args=parser.parse_args();out=args.output.resolve()
if out.exists() and any(out.iterdir()):raise RuntimeError('Use an empty output directory')
out.mkdir(parents=True,exist_ok=True)
paths=['src/xrt/compositor/main/comp_compositor.c','src/xrt/compositor/multi/comp_multi_compositor.c','src/xrt/compositor/multi/comp_multi_private.h','src/xrt/compositor/util/comp_swapchain_gpu_reuse.c','src/xrt/compositor/util/comp_swapchain_gpu_reuse.h','src/xrt/compositor/util/comp_semaphore.c','src/xrt/compositor/util/comp_semaphore.h']
modified={p:(root/p).read_bytes() for p in paths}
baseline={p:subprocess.check_output(['git','show','805316bebe976256a16c591ee135a5422ffe73a3:'+p],cwd=root) for p in paths}
for p in paths:
 fixture=pathlib.Path(__file__).parent/'baseline-source-overrides'/p
 if fixture.exists():baseline[p]=fixture.read_bytes()
# The debug-image selection/layout changes and test client are identical in A and B.
(out/'baseline-sources.patch').write_bytes(subprocess.check_output(['git','diff','805316bebe976256a16c591ee135a5422ffe73a3','HEAD','--']+paths,cwd=root))
active=modified
try:
 for mode,name in [('A','baseline-0'),('B','modified-0'),('B','modified-1'),('A','baseline-1')]:
  desired=baseline if mode=='A' else modified
  for p in paths:
   if (root/p).read_bytes()!=active[p]:raise RuntimeError('Concurrent source modification: '+p)
   (root/p).write_bytes(desired[p])
  active=desired
  print('Building '+name,flush=True)
  with (out/(name+'-build.txt')).open('w') as log:
   subprocess.run(['cmake','--build','build/arm64','--parallel','--target','monado-service','openxr_monado'],cwd=root,stdout=log,stderr=log,check=True,timeout=120)
  print('Measuring '+name,flush=True)
  subprocess.run(['python3','scripts/macos/run-gpu-sync-service.py','--output',str(out/name),'--benchmark','--client','build/arm64/tests/tests_macos_service_gpu_sync','normal','600'],cwd=root,check=True,timeout=45)
finally:
 for p in paths:
  if (root/p).read_bytes()!=active[p]:
   (out/(pathlib.Path(p).name+'.unexpected')).write_bytes((root/p).read_bytes())
  (root/p).write_bytes(modified[p])
 print('Restored modified sources; rebuilding all targets',flush=True)
 with (out/'restored-build.txt').open('w') as log:
  subprocess.run(['cmake','--build','build/arm64','--parallel'],cwd=root,stdout=log,stderr=log,check=True,timeout=120)
