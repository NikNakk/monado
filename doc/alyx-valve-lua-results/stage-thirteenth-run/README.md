# Thirteenth run: stage fallback panic and binary log decoding

The optimized candidate66399cb made first-frame submission, then aborted with
SIGABRT after32.04seconds. The backend correctly retained child exit-6, but its
log-display read_text raised UnicodeDecodeError on byte0xfc at offset1177463.
The outer launcher also used strict UTF-8 decoding, so it failed before writing
its final launch manifest. The raw backend log and API CSV remain local.
No complete engine log or account identifiers are copied here.

The xrizer panic is Eye0 has not been submitted, following stage swapchain
recreation and an unsupported-stage warning. The compositor computed complete
stereo readiness before attempting a stage. Recreating the stage target cleared
both submitted eyes; render_stage failed on Vulkan, and fallback trusted stale
readiness before unwrapping the cleared eyes. The same source exists in the
original debug build: optimization is not established as the cause.

A new bounded fake-runtime test constructs that complete stereo/stage/different
target-size condition and reproduces the exact Eye0 panic before the fix.
The fake runtime gains stereo view-configuration enumeration for this path.
Signed xrizer2b73755 adds an explicit backend stage-support check before any
stage swapchain mutation (only the existing Windows D3D11 renderer opts in),
logs unsupported stages once per controller, and revalidates eye readiness after
a supported stage attempt. Unsupported stage content retains the application/
skybox fallback; it is not a newly implemented Vulkan stage renderer. A second
regression advertises support but deliberately fails rendering after recreation,
verifying discarded eyes are not reused. Both pass in debug and optimized tests.

Full unit suite:85pass,1ignored native-GPU test,3known input skips as in branch CI.
These are fake-runtime frame/control-state tests, not image-correctness tests.
The prior optimized MoltenVK pipeline test remains separate GPU evidence; no
new GPU pipeline code is changed in this fix. Windows runtime behavior has not
been hardware-tested; its existing stage renderer retains advertised support.

Backend wrapper a8feb75 (native working worktree) /5c2b9c0 (performance clone)
and VeRantyx aa6ea3f display logs with explicit UTF-8 replacement while leaving
raw run.log bytes unchanged. The outer launcher saves its initial command,
environment and binary hashes before starting the child, and still writes the
final result afterward. An inert fixture runs both real wrappers with byte0xfc
and child exit7; both manifests preserve7, raw bytes are exact, and the printed
text is valid. The launcher selection/RAM regression also passes.

The instrumented optimized and debug libraries are rebuilt separately; original
working debug library and host remain unchanged. The previous optimized crash
candidate is preserved at /private/tmp/xrizer-perf-66399cb-release.dylib.
Use a new output directory for the next user run. No Monado service/runtime,
game binary, GPU timing feedback value or FDM behavior is changed.
