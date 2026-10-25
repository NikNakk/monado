# Tenth-run VM failure (20 October 2026)

The user heard sound. The engine reports a fatal allocation of4096bytes and
then deliberately writes0xdeadbeef through NULL in tier0's fatal handler.
This is an explicit engine abort, not evidence of the macOS memory killer.
The Panorama caller can reach it either through a failed virtual commit or
an arena-capacity failure; the headset log does not capture the failing errno.

A bounded headless fixture calls the owner's actual tier0 exports. A64KiB
reservation followed by4KiB commits reproduces EINVAL at offsets4096,8192
and12288 on the16KiB host; a host-aligned16KiB lifecycle succeeds. The adapter
makes every4KiB commit succeed and tests neighbouring data, decommit/recommit,
read-only/PROT_NONE host protections, a cross-host-page range, invalid ranges,
partial-release rejection and executable protection refusal. Zero shim stubs.
The unchanged control still fails, establishing the original ABI mismatch.

Panorama/OpenSSL/ICU constructor tests pass, and the actual Panorama ELF PLT
imports are verified to point at the new allocation/free adapters. Relocation
offsets were discovered with llvm-readelf for this input, not used as code
patch sites. Lua allocation/growth/GC/OOM recovery also passes with the adapter
installed; its input hashes remain unchanged. Launcher regression passes.

This adapter combines subpage protection bits within each host page. It cannot
emulate individual4KiB guard faults or different protections within one16KiB
page. It handles only reservations made through the Source platform VM exports,
preserving special/max-address paths and the generic libc/JIT refusal policy.
It neither changes game instructions nor supplies an allocation-success stub.
It is experimental; the next user run must confirm the actual crash advances.

The tenth log also has a separate Steam not-logged-in warning. Ownership and
full game authentication remain unverified; nothing bypasses that failure.
No full game log, user account identifier or game binary is committed here.
