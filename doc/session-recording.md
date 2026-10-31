# Session recording and stage coordinates

`XRT_SESSION_RECORD_DIR` enables the optional server-side JSONL input recorder.
It captures head and controller poses in the space overseer's root coordinates,
eye-relative views, input-state changes and user marks.

Version 2 also writes the STAGE pose relative to root in every sample. A replay
which presents its root as STAGE must transform recorded root-space head and
hand poses by the inverse of this pose. Eye-relative view poses remain unchanged.
Recording the transform per sample preserves changes from floor calibration
and recenter gestures during a session. The validity field applies to STAGE too.

Version 1 recordings do not contain this transform. They cannot in general
reconstruct the original floor, centre or heading. Choosing a head-relative
anchor is an approximation and must not be presented as an exact reproduction.

The schema does not restore game saves or make physics, AI or scheduling
deterministic. Controller-profile conversions and eye gaze remain separate
replay requirements. No installed service or hardware acceptance is implied by
building this recorder.
