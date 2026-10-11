import os, subprocess, tempfile, time, json
from pathlib import Path
root=Path('/Users/nickkennedy/Code/monado-2')
out=root/'doc/results/macos-gpu-sync-20261011'
with tempfile.TemporaryDirectory(prefix='monado-sync-service-') as temp:
 env=os.environ.copy(); env.update(XDG_RUNTIME_DIR=temp,XDG_CONFIG_HOME=temp,SIMULATED_ENABLE='1',XRT_COMPOSITOR_NULL='1',MONADO_OPENXR_RUNTIME_PATH=str(root/'build/arm64/src/xrt/targets/openxr/libopenxr_monado.dylib'))
 with (out/'null-service.log').open('w') as log:
  service=subprocess.Popen([str(root/'build/arm64/src/xrt/targets/service/monado-service')],env=env,stdout=log,stderr=log)
  results=[]
  try:
   deadline=time.monotonic()+5
   while not (Path(temp)/'monado_comp_ipc').exists() and service.poll() is None and time.monotonic()<deadline: time.sleep(.05)
   for i in range(2):
    r=subprocess.run([str(root/'build/arm64/tests/tests_macos_openxr_loaderless_probe')],env=env,text=True,capture_output=True,timeout=10)
    (out/f'null-service-client-{i}.txt').write_text(r.stdout+r.stderr)
    results.append(dict(client=i,exit=r.returncode,service_pid=service.pid,service_alive=service.poll() is None,gpu_rendering=False))
  finally:
   service.terminate()
   try: service.wait(timeout=5)
   except subprocess.TimeoutExpired: service.kill(); service.wait()
  (out/'null-service-results.json').write_text(json.dumps(results,indent=2)+'\n')
  print(json.dumps(results))
