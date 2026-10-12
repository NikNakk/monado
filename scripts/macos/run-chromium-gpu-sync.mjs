#!/usr/bin/env node
// Copyright 2026, Nick Kennedy; SPDX-License-Identifier: BSL-1.0
// Run inside run-gpu-sync-service.py --client, with an isolated simulated HMD.
import {parseArgs} from 'node:util';
import {readFileSync, writeFileSync, mkdirSync, existsSync, createWriteStream} from 'node:fs';
import {resolve, dirname, join} from 'node:path';
import {fileURLToPath} from 'node:url';
import {createServer} from 'node:http';
import {spawn, spawnSync} from 'node:child_process';

const {values} = parseArgs({options: {
  chrome: {type: 'string'}, output: {type: 'string'}, healthy: {type: 'string'},
  'force-copy': {type: 'boolean', default: false},
  'pixel-ms': {type: 'string'},
}});
const pixelMs = Number(values['pixel-ms'] ?? process.env.M3_CHROMIUM_PIXEL_MS ?? 3000);
if (!Number.isInteger(pixelMs) || pixelMs < 250 || pixelMs > 60000)
  throw Error('--pixel-ms must be between 250 and 60000');
const producer = process.env.M3_CHROMIUM_PRODUCER || 'angle';
if (!['angle', 'dawn'].includes(producer)) throw Error('Invalid producer');
const mode = process.env.M3_CHROMIUM_MODE || 'faults';
if (!['faults', 'pixels', 'benchmark'].includes(mode)) throw Error('Invalid M3_CHROMIUM_MODE');
values.chrome ||= process.env.M3_CHROMIUM_PATH;
values.output ||= process.env.M3_CHROMIUM_OUTPUT;
values.healthy ||= process.env.M3_HEALTHY_NATIVE_PATH;
values['force-copy'] ||= process.env.M3_CHROMIUM_FORCE_COPY === '1';
if (!values.chrome || !values.output || !process.env.MONADO_OPENXR_RUNTIME_PATH)
  throw Error('Requires --chrome --output and the isolated service environment');
const output = resolve(values.output);
mkdirSync(join(output, 'media'), {recursive: true});
const media = join(output, 'media');
const page = resolve(dirname(fileURLToPath(import.meta.url)), '../../tests/chromium_gpu_sync_video.html');
const encoded = spawnSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y',
  '-f', 'lavfi', '-i', 'testsrc2=size=128x128:rate=15:duration=12',
  '-an', '-c:v', 'libx264', '-preset', 'ultrafast', '-profile:v', 'baseline',
  '-level:v', '3.0', '-pix_fmt', 'yuv420p', '-g', '15', '-keyint_min', '15',
  '-sc_threshold', '0', '-f', 'dash', '-seg_duration', '1',
  '-init_seg_name', 'init.mp4', '-media_seg_name', 'segment-$Number$.m4s',
  join(media, 'clip.mpd')], {encoding: 'utf8', timeout: 30000});
if (encoded.status !== 0) throw Error('Media generation failed: '+encoded.stderr);
const manifest = join(output, 'runtime.json');
writeFileSync(manifest, JSON.stringify({file_format_version: '1.0.0',
  runtime: {library_path: process.env.MONADO_OPENXR_RUNTIME_PATH}}));
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
function withTimeout(promise, ms, label) {
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => reject(Error(label+' timeout')), ms);
    promise.then(value => { clearTimeout(timer); resolve(value); },
                 error => { clearTimeout(timer); reject(error); });
  });
}
const server = createServer((request, response) => {
  const name = new URL(request.url, 'http://localhost').pathname;
  if (name === '/') {
    response.setHeader('Content-Type', 'text/html'); response.end(readFileSync(page));
  } else if (/^\/media\/(init\.mp4|segment-\d+\.m4s)$/.test(name)) {
    response.setHeader('Content-Type', 'video/mp4');
    try { response.end(readFileSync(join(output, name))); }
    catch { response.writeHead(404).end(); }
  } else { response.writeHead(404).end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const url = `http://127.0.0.1:${server.address().port}/`;
const profile = join(output, 'profile');
mkdirSync(profile, {recursive: true});
const log = createWriteStream(join(output, 'chromium.log'));
const args = ['--user-data-dir='+profile, '--remote-debugging-port=0',
  '--no-first-run', '--no-default-browser-check', '--enable-features=OpenXR'+(producer === 'dawn' ? ',WebXRWebGPUBinding' : ''),
  '--use-gl=angle', '--use-angle=metal', '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
  '--disable-backgrounding-occluded-windows', '--enable-logging=stderr',
  '--vmodule=openxr_render_loop=2,openxr_graphics_binding_metal=2,openxr_api_wrapper=2',
  // Isolated service socket/Mach names are outside the production sandbox's
  // fixed allowlist. This is an integration fixture, not a sandbox validation.
  '--no-sandbox', '--app='+url];
if (mode === 'benchmark') args.splice(args.findIndex(arg => arg.startsWith('--vmodule=')), 1);
if (values['force-copy']) args.push('--openxr-metal-force-copy');
let healthy, healthyText = '', healthyExit;
const healthyFrames = () => healthyText.split('\n').filter(line => line.startsWith('{"event":"frame"')).length;
if (values.healthy && mode === 'faults') {
  const healthyLog = createWriteStream(join(output, 'healthy.txt'));
  healthy = spawn(values.healthy, ['overlay', '1200', '90'], {stdio: ['ignore', 'pipe', 'pipe']});
  healthy.stdout.pipe(healthyLog); healthy.stderr.pipe(healthyLog);
  healthy.stdout.on('data', bytes => { healthyText += bytes; });
  healthyExit = new Promise(resolve => healthy.once('exit', code => resolve(code)));
  const deadline = Date.now()+15000;
  while (healthyFrames() < 10 && Date.now() < deadline && healthy.exitCode === null) await sleep(50);
  if (healthyFrames() < 10) {
    healthy.kill('SIGKILL'); server.close(); log.end();
    throw Error('Concurrent native overlay did not start');
  }
}
const browser = spawn(values.chrome, args, {env: {...process.env, XR_RUNTIME_JSON: manifest},
  stdio: ['ignore', 'pipe', 'pipe']});
browser.stdout.pipe(log); browser.stderr.pipe(log);
let cdp, traceCdp, tracing = false;
const result = {status: 'FAIL', started: new Date().toISOString(),
  target: 'simulated HMD/debug_image', mode, producer, forceCopy: values['force-copy'], args,
  phases: [], gpuCrash: false, sandbox: 'disabled for isolated service names'};
class CDP {
  constructor(socket) {
    this.socket = socket; this.next = 0; this.pending = new Map(); this.events = new Map();
    socket.addEventListener('message', event => {
      const message = JSON.parse(event.data);
      if (message.id) {
        const call = this.pending.get(message.id); if (!call) return;
        this.pending.delete(message.id); clearTimeout(call.timer);
        if (message.error) call.reject(Error(JSON.stringify(message.error)));
        else call.resolve(message.result);
      } else {
        for (const listener of this.events.get(message.method) || []) listener(message.params);
      }
    });
  }
  call(method, params = {}, timeout = 15000) {
    return new Promise((resolve, reject) => {
      const id = ++this.next;
      const timer = setTimeout(() => { this.pending.delete(id); reject(Error(method+' timeout')); }, timeout);
      this.pending.set(id, {resolve, reject, timer});
      this.socket.send(JSON.stringify({id, method, params}));
    });
  }
  once(name) {
    return new Promise(resolve => {
      const listeners = this.events.get(name) || []; listeners.push(resolve); this.events.set(name, listeners);
    });
  }
  async evaluate(expression) {
    const response = await this.call('Runtime.evaluate', {expression, awaitPromise: true,
      returnByValue: true, userGesture: true});
    if (response.exceptionDetails) throw Error(JSON.stringify(response.exceptionDetails));
    return response.result.value;
  }
}
async function until(predicate, label, timeout = 15000) {
  const end = Date.now()+timeout;
  while (Date.now() < end) {
    if (browser.exitCode !== null) throw Error('Chromium exited: '+browser.exitCode);
    const value = await predicate(); if (value) return value;
    await sleep(100);
  }
  throw Error(label+' timeout');
}
async function state() {
  const current = await cdp.evaluate('JSON.parse(JSON.stringify({...testState, videoTime: document.querySelector("video").currentTime, videoPaused: document.querySelector("video").paused, perfNow: performance.now()}))');
  delete current.samples; current.healthyFrames = healthyFrames(); return current;
}
async function observe(phase, duration = 700) {
  const before = await state();
  await cdp.evaluate(`note(${JSON.stringify(phase)})`); await sleep(duration);
  await cdp.evaluate(`note(${JSON.stringify(phase+'_end')})`);
  const after = await state();
  result.phases.push({phase, before, after});
  if (after.errors.length) throw Error(JSON.stringify(after.errors));
  if (after.frames-before.frames < 5) throw Error(phase+': XR frames stopped');
  return {before, after};
}
try {
  const portFile = join(profile, 'DevToolsActivePort');
  await until(() => existsSync(portFile), 'DevTools startup');
  const [port, browserSocketPath] = readFileSync(portFile, 'utf8').trim().split('\n');
  // Page-level tracing filters out the XR utility process. Its unbound startup
  // writers then expire after 30 s and commit to invalid buffer zero. Use the
  // browser endpoint for the whole cross-process capture, and the page for JS.
  const traceSocket = new WebSocket(`ws://127.0.0.1:${port}${browserSocketPath}`);
  await new Promise((resolve, reject) => { traceSocket.addEventListener('open', resolve); traceSocket.addEventListener('error', reject); });
  traceCdp = new CDP(traceSocket);
  const target = await until(async () => {
    const pages = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
    return pages.find(page => page.type === 'page' && page.url === url);
  }, 'fixture page');
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((resolve, reject) => { socket.addEventListener('open', resolve); socket.addEventListener('error', reject); });
  cdp = new CDP(socket);
  await cdp.call('Runtime.enable');
  await cdp.call('Browser.grantPermissions', {origin: url, permissions: ['vr']});
  await until(() => cdp.evaluate('typeof prepare === "function"'), 'fixture load');
  await traceCdp.call('Tracing.start', {traceConfig: {includedCategories: ['xr', 'gpu', 'blink.user_timing']},
    transferMode: 'ReturnAsStream', streamFormat: 'proto'}); tracing = true;
  result.traceScope = 'browser';
  await cdp.evaluate(`window.useWebGPU = ${producer === 'dawn'}`);
  result.stage = 'prepare';
  await cdp.evaluate('prepare()');
  result.stage = 'play_before_XR';
  await cdp.evaluate('document.querySelector("video").play()');
  await until(() => cdp.evaluate('document.querySelector("video").readyState >= 2'), 'first decoded image');
  result.beforeXR = await state();
  result.stage = 'startXR';
  await cdp.evaluate('startXR()');
  if (mode === 'pixels') {
    // The fragment shader writes exact primary colors into the real XR image.
    // The service reads its Vulkan output only after its GPU fence completes.
    result.pixelProof = [];
    for (const [name, left, right] of [
      ['red_blue', [255,0,0,255], [0,0,255,255]],
      ['green_red', [0,255,0,255], [255,0,0,255]],
      ['blue_green', [0,0,255,255], [0,255,0,255]],
      ['red_blue_reuse', [255,0,0,255], [0,0,255,255]]]) {
      const color = {left: left.map(byte => byte/255), right: right.map(byte => byte/255)};
      await cdp.evaluate(`window.proofColor = ${JSON.stringify(color)}`);
      await sleep(500); // Exclude submissions already in flight at the transition.
      const before = readFileSync(join(output, 'service.log'), 'utf8').length;
      await observe('pixels_'+name, pixelMs);
      const logs = readFileSync(join(output, 'service.log'), 'utf8').slice(before);
      // Left/right samples lie inside each eye. The output center lies on
      // the distorted eye seam and can blend transparent border texels.
      const samples = [...logs.matchAll(/vk-target frame=(\d+).*rgbaL=\((\d+),(\d+),(\d+),(\d+)\).*rgbaC=\((\d+),(\d+),(\d+),(\d+)\).*rgbaR=\((\d+),(\d+),(\d+),(\d+)\)/g)]
        .map(match => ({frame: Number(match[1]), left: match.slice(2,6).map(Number),
          center: match.slice(6,10).map(Number), right: match.slice(10,14).map(Number)}));
      if (!samples.length || samples.some(sample => ['left','right'].some(eye =>
          (eye === 'left' ? left : right).some((byte, i) => Math.abs(sample[eye][i]-byte) > 1))))
        throw Error('Vulkan pixel mismatch '+name+': '+JSON.stringify(samples));
      result.pixelProof.push({name, expected: {left, right}, samples});
    }
  } else if (mode === 'benchmark') {
    await cdp.evaluate('appendMore()');
    await cdp.evaluate('document.querySelector("video").loop = true');
    await observe('warmup', 3000);
    await observe('normal_path', 20000);
  } else {
  await observe('playing');
  await cdp.evaluate('document.querySelector("video").pause()');
  const paused = await observe('paused');
  if (!paused.after.videoPaused || paused.after.videoTime-paused.before.videoTime > 0.01 || paused.after.decoded-paused.before.decoded > 1) throw Error('Video did not remain paused');
  await cdp.evaluate('document.querySelector("video").play()');
  const count = (await state()).waiting;
  await until(async () => (await state()).waiting > count, 'real MediaSource starvation');
  await observe('buffering', 1500);
  await observe('prolonged_buffering', 2500);
  await cdp.evaluate('appendMore()');
  await until(() => cdp.evaluate('document.querySelector("video").currentTime > 2.2'), 'buffer recovery');
  await observe('resumed');
  await cdp.evaluate('document.querySelector("video").currentTime = 1');
  await observe('seek');
  for (let i = 0; i < 8; ++i) {
    await cdp.evaluate('document.querySelector("video").pause()'); await sleep(50);
    await cdp.evaluate('document.querySelector("video").play()'); await sleep(50);
  }
  await observe('rapid_pause_resume');
  await cdp.evaluate('stopXR()');
  result.stage = 'startXR';
  await cdp.evaluate('startXR()');
  await observe('replacement_session');
  // GPU crash also terminates GPU-hosted decoder/context state. Its timing is
  // recorded separately from session replacement and successful GPU completion.
  result.nativeFramesAtGpuCrash = healthyFrames();
  await cdp.evaluate("note('gpu_crash')");
  await cdp.call('Browser.crashGpuProcess'); result.gpuCrash = true;
  await sleep(1500);
  result.afterGpuCrash = await state();
  if (healthy && healthyFrames()-result.nativeFramesAtGpuCrash < 5) throw Error('Concurrent native frames stopped during GPU failure');
  // GPU-hosted decoder termination may make the old media pipeline terminal.
  // Record that expected failure, then replace it rather than claiming that a
  // crashed decoder automatically resumed its old stream.
  if (result.afterGpuCrash.errors.some(error => !error.includes('decode')))
    throw Error('Unexpected GPU-fault error: '+JSON.stringify(result.afterGpuCrash.errors));
  await cdp.evaluate('stopXR()');
  result.stage = 'decoder_replacement';
  await cdp.evaluate('testState.errors = []; prepare()');
  await cdp.evaluate('document.querySelector("video").play()');
  result.stage = 'startXR';
  await cdp.evaluate('startXR()');
  await observe('gpu_recovery_replacement');
  }
  await cdp.evaluate('stopXR()');
  await cdp.evaluate("note('complete')");
  result.stage = 'complete';
  result.status = 'PASS';
} catch (error) {
  result.error = String(error); process.exitCode = 1;
  if (cdp) { try { result.failureState = await cdp.evaluate('JSON.parse(JSON.stringify({...testState, mediaReady: document.querySelector("video").readyState, videoError: document.querySelector("video").error?.message, mediaSourceState: typeof mediaSource === "undefined" ? null : mediaSource?.readyState}))'); } catch {} }
} finally {
  if (traceCdp && tracing) {
    try {
      const complete = traceCdp.once('Tracing.tracingComplete');
      await traceCdp.call('Tracing.end');
      const finished = await withTimeout(complete, 10000, 'trace export');
      const chunks = [];
      for (;;) {
        const part = await traceCdp.call('IO.read', {handle: finished.stream});
        chunks.push(Buffer.from(part.data, part.base64Encoded ? 'base64' : 'utf8'));
        if (part.eof) break;
      }
      writeFileSync(join(output, 'chromium.pftrace'), Buffer.concat(chunks));
      await traceCdp.call('IO.close', {handle: finished.stream});
    } catch (error) { result.traceError = String(error); result.status = 'FAIL'; process.exitCode = 1; }
  }
  if (traceCdp) traceCdp.socket.close();
  if (cdp) { try { await cdp.call('Browser.close', {}, 3000); } catch {} cdp.socket.close(); }
  if (browser.exitCode === null) browser.kill('SIGTERM');
  await Promise.race([new Promise(resolve => browser.once('exit', resolve)), sleep(3000)]);
  if (browser.exitCode === null) browser.kill('SIGKILL');
  if (healthy) {
    const exit = await withTimeout(healthyExit, 90000, 'native overlay completion').catch(() => null);
    if (exit !== 0) {
      healthy.kill('SIGKILL'); result.status = 'FAIL'; process.exitCode = 1;
    }
    result.nativeHealthy = {pid: healthy.pid, exit, frames: healthyFrames()};
  }
  server.close(); log.end();
  result.finished = new Date().toISOString();
  writeFileSync(join(output, 'video-summary.json'), JSON.stringify(result, null, 2)+'\n');
  console.log(JSON.stringify(result));
}
