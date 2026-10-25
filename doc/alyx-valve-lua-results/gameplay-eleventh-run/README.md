# First native ARM64 gameplay:20October2026

User-confirmed on the simulated HMD: intro video, menu, save restoration and
level entry. Subjective frame rate below the best Wine run; no matched FPS data.
Menu warns of low RAM. This is not a PS VR2 hardware acceptance result.

Source: VeRantyx b8b3ccf, Klepton44985ec, xrizer8417a1d(debug), MoltenVK1.4.2.
The tested binary hashes match ../vm-tenth-run/identity.json. Log facts:
first compositor submission, save restoration;896x1007 recommended per eye,
90Hz;6144MiB synthetic guest budget;323 unsupported stage-override warnings.
No requested/enabled FDM or fragment-shading-rate device extension observed.

Host RAM read-only query:25769803776bytes=24576MiB. VeRantyx74396b5 selects
that existing memory-budget override in the native launcher, without changing
Klepton's generic default. Inert launcher regression and real automatic-RAM
dry-run pass; the next user run must confirm whether the warning disappears.

Retain the working baseline. Next profile an isolated optimized build with
matched scene, resolution, graphics settings and warm caches; measure CPU/GPU
frame time and separate pipeline compilation/presentation effects. No performance
cause or KosmicKrisp gameplay compatibility is established by this checkpoint.
No full engine log, account identifier, save data or game binary is included.
