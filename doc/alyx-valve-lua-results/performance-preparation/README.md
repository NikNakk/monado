# Native performance preparation,20October2026

Twelfth user run: performance subjectively better and RAM warning gone.
Manifest and guest /proc startup agree on24576MiB. First compositor submission
is present and the log contains no fatal OUT OF MEMORY message. This remains a
simulated-HMD result, not PS VR2 acceptance or measured Wine parity.

The original8417a1d debug library and working host remain byte-identical to the
successful twelfth run. An optimized build of that same xrizer source is retained
at /private/tmp/xrizer-8417a1d-release.dylib. A separate shared clone/branch at
/private/tmp/xrizer-performance, codex/native-alyx-performance, commit66399cb,
adds optional API wall-clock spans only. Both debug/release builds are retained
under /private/tmp/xrizer-perf-target. Explicit library selection and optional
capture are in signed VeRantyx7e27157; default launch still selects the original
debug library. MoltenVK and the24GiB automatic host budget remain defaults.

## GPU timing correction already present in Wine

The user correctly recalled the Wine fix. The local windows-openxr-d3d11 branch
has cb2986a (shared frame history and replacement of synthetic times) and
3fffac0 (bounded D3D11 GPU queries). Current Wine branch is c823e31. Native
Vulkan branch8417a1d forked at03e5dac and retains the earlier8/1/9ms synthetic
GPU values. The existing FrameHistory, delayed-result matching, ABI prefix
copying and tests are reusable. Actual query objects are D3D11-specific; its
base GraphicsBackend hooks return no samples on other backends.

There was also a separate SteamVR/DXMT fix8624164, documented in
~/Code/macos-wine-xr/docs/results/steamvr-gpu-timing-20261014/README.md: false
D3D10 resource interfaces prevented SteamVR's own timing initialization. That
SteamVR/D3D11 path is not used by the native Vulkan client.

Do not treat native OpenVR GetFrameTiming's GPU fields as measurements or CPU
wall duration as GPU execution. Placeholder values may affect automatic quality;
this has not been measured. The original API values remain unchanged in this
candidate so an optimization comparison does not also change game feedback.

Next GPU implementation should reuse the Wine history and validity policy,
then add bounded Vulkan timestamp query/command buffers, valid-bit/wrap and
period handling, nonblocking availability reads, explicit session reset/teardown,
and synchronization for the actual application's queue. Confirm queue ordering
and pre-submit versus copy/compositor intervals before reporting a sample.
Do not add a queue-idle wait to obtain timings. Missing data must remain missing
rather than becoming a made-up GPU duration. Cherry-picking the entire Wine
commits also changes frame failures, overlays and Windows code; a scoped port
needs reconciliation with the native session/stage paths and regression tests.

## Optional API trace

XRIZER_API_TRACE names a new CSV file, opened with create_new. The launcher
--api-trace selects output/xrizer-api.csv. Captures wait_poses, per-eye submit
and handoff wall intervals, including blocking. Disabled by default. Files are
buffered/flushed on handoff; the last incomplete tail can be lost at termination.
Timing ends before capture I/O; nested capture I/O/locking can perturb outer
calls. Use the same instrumentation for both debug/release comparisons and
repeat with it off if overhead is material. Never sum nested handoff/wait spans.
Cadence is API invocation cadence, not proof of presented or unique images.

83 unit tests pass, one native GPU test intentionally ignored and three input
tests skipped consistently with branch macOS CI. Executing the real optimized
overlay pipeline test explicitly on MoltenVK passes. Actual fake-runtime unit
calls emit87 valid CSV records covering all three entry points: validation data,
not performance results. Launcher regression and a selected-library dry run pass;
parser fixture checks eye grouping, distributions and time-window filtering.
Two existing unused-stage-function warnings remain. No game run was initiated.

For an initial optimized user run, use the command in the headset checkpoint.
Load the same save/scene with the same graphics settings and simulated-HMD
resolution, and remain stationary for30seconds after loading. Record approximate
elapsed time at level entry so the analysis can exclude startup/loading.
Repeat on the instrumented debug build by selecting its explicit library and a
new output directory. For controlled results use warmed caches, equal thermal
state and A-B-B-A order; report first/cold compilation separately. No measured
native/Wine speed ratio, GPU time, memory-pressure or thermal conclusion yet.
