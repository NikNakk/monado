import argparse, pathlib, subprocess, os, time, json
parser=argparse.ArgumentParser();parser.add_argument('--output',type=pathlib.Path,required=True)
args=parser.parse_args();root=pathlib.Path.cwd();out=args.output.resolve()
if out.exists() and any(out.iterdir()):raise RuntimeError('Use an empty output directory')
out.mkdir(parents=True,exist_ok=True)
plist=pathlib.Path.home()/'Library/LaunchAgents/org.freedesktop.monado.service.plist'
domain=f'gui/{os.getuid()}'
(out/'installed-service.plist').write_bytes(plist.read_bytes())
(out/'commit.txt').write_text(subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True))
(out/'working-tree.patch').write_bytes(subprocess.check_output(['git','diff'],cwd=root))
(out/'displays.txt').write_text(subprocess.check_output(['system_profiler','SPDisplaysDataType'],text=True))
state=subprocess.run(['launchctl','print',domain+'/org.freedesktop.monado.service'],capture_output=True,text=True)
(out/'installed-service-before.txt').write_text(state.stdout+state.stderr)
stopped=False
try:
 subprocess.run(['launchctl','bootout',domain,str(plist)],check=True,timeout=15);stopped=True
 print('Installed service stopped; allowing USB teardown',flush=True)
 time.sleep(2)
 for fault in ['missing','disconnect']:
  print('Headset scenario: '+fault,flush=True)
  status=subprocess.run(['caffeinate','-dimu','python3','scripts/macos/run-gpu-sync-service.py','--headset','--suite','--fault',fault,'--output',str(out/fault)],cwd=root,timeout=60)
  (out/(fault+'-exit.json')).write_text(json.dumps({'exit':status.returncode})+'\n')
  if status.returncode: break
finally:
 if stopped:
  print('Restoring installed service registration',flush=True)
  subprocess.run(['launchctl','bootstrap',domain,str(plist)],check=True,timeout=15)
  subprocess.run(['launchctl','kickstart',domain+'/org.freedesktop.monado.service'],check=True,timeout=15)
  (out/'installed-service-after.txt').write_text(subprocess.check_output(['launchctl','print',domain+'/org.freedesktop.monado.service'],text=True))
