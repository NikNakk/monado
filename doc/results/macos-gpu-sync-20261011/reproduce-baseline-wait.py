from pathlib import Path
import subprocess
root=Path('/Users/nickkennedy/Code/monado-2')
source=root/'src/xrt/compositor/multi/comp_multi_compositor.c'
fixed=source.read_bytes()
base=Path('/tmp/monado-gpu-sync-original/src/xrt/compositor/multi/comp_multi_compositor.c').read_bytes()
out=root/'doc/results/macos-gpu-sync-20261011'
try:
 source.write_bytes(base)
 r=subprocess.run(['cmake','--build','build/arm64','--target','tests_ipc_local_session','--parallel','4'],cwd=root,text=True,capture_output=True)
 (out/'baseline-regression-build.txt').write_text(r.stdout+r.stderr)
 if r.returncode: raise RuntimeError('baseline regression build failed')
 r=subprocess.run(['build/arm64/tests/tests_ipc_local_session','failed producer wait never makes a frame schedulable'],cwd=root,text=True,capture_output=True,timeout=10)
 (out/'baseline-regression.txt').write_text(r.stdout+r.stderr+'\nexit='+str(r.returncode)+'\n')
 print('baseline regression exit:',r.returncode)
 if not r.returncode: raise RuntimeError('baseline unexpectedly passes regression')
finally:
 source.write_bytes(fixed)
 r=subprocess.run(['cmake','--build','build/arm64','--parallel','4'],cwd=root,text=True,capture_output=True)
 (out/'modified-build.txt').write_text(r.stdout+r.stderr)
 print('restored build exit:',r.returncode)
