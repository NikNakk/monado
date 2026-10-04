# Native Sony Sense LED oracle test vector

This CSV is a compact, copyright-minimal protocol-fact summary of the successful Windows behavioural-oracle capture
`steamvr-success.pcapng.gz` (SHA-256
`2d2ed18dc26006f1d6c6a588d1086a7917cf515bb385cc57ccdda86e3c783a22`).

Source of knowledge: Sony Windows driver used as a behavioural oracle.
Method: Bluetooth HCI/ETW capture; only CRC-valid outgoing HIDP A2/report-31 records were accepted.
Observed behaviour: consecutive runs of identical LED schedule fields.
Implementation provenance: protocol facts only; no Sony implementation material is included.

Columns are directly observable A2/31 fields. `start_s` is relative to the first accepted A2/31 packet, and
`duration_s`/report counts summarize the interval before the next schedule-field change. Durations/counts are rounded
for readability and are not intended as timing tolerances.

Use the vector to check invariants rather than clone Sony's exact policy:

- PRESCAN period 40 and absolute `cycle_position`;
- BROAD period 42 and BG period 30 with relative/signed `cycle_position`;
- `cycle_length=50050050` for the observed 16,683,350 ns frame cycle;
- schedule sequence is a latch/generation, not a packet counter;
- the same sequence can persist for >16 s / >1200 output reports;
- a new sequence can relatch identical schedule bytes (runs 19→20);
- STABLE/phase 4 is absent from this successful capture.
