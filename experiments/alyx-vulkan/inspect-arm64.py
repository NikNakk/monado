#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: BSL-1.0
# Candidate references need manual disassembly confirmation, not reachability inference.
import struct, re, hashlib, json
from pathlib import Path
import argparse
parser = argparse.ArgumentParser(description='Read-only ELF strings and candidate AArch64 ADRP/ADD references; never loads guest code')
parser.add_argument('--depot', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = args.depot.resolve(strict=True)
args.output.mkdir(parents=True, exist_ok=False)

def elf(path):
    data = path.read_bytes()
    if data[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', data, 18)[0] != 183:
        raise ValueError(f'Expected little-endian ELF64 AArch64: {path}')
    off = struct.unpack_from('<Q', data, 40)[0]
    (size, n, names) = struct.unpack_from('<HHH', data, 58)
    sh = [struct.unpack_from('<IIQQQQIIQQ', data, off + i * size) for i in range(n)]
    strs = data[sh[names][4]:sh[names][4] + sh[names][5]]
    sections = []
    for s in sh:
        name = strs[s[0]:].split(b'\x00', 1)[0].decode()
        sections.append(dict(name=name, type=s[1], flags=s[2], va=s[3], offset=s[4], size=s[5], link=s[6], entsize=s[9]))
    syms = []
    for s in sections:
        if s['type'] != 11:
            continue
        st = sections[s['link']]
        strings = data[st['offset']:st['offset'] + st['size']]
        for o in range(s['offset'], s['offset'] + s['size'], s['entsize']):
            (nm, info, other, idx, val, length) = struct.unpack_from('<IBBHQQ', data, o)
            name = strings[nm:].split(b'\x00', 1)[0].decode(errors='replace')
            syms.append(dict(name=name, value=val, size=length, defined=idx != 0, type=info & 15))
    return (data, sections, syms)
reports = []
pattern = re.compile('fragment_density|fragment_shading_rate|foveat|fdm|eye.?track|IVRSystem_|IVRCompositor_|IVRInput_|IVRMailbox_|-vulkan|Steam.?Frame|steam_frame', re.I)
for path in sorted(root.glob('game/**/linuxsteamrtarm64/*')):
    if not path.is_file() or path.read_bytes()[:4] != b'\x7fELF':
        continue
    (data, secs, syms) = elf(path)
    hits = []
    for sec in secs:
        if not sec['flags'] & 2 or sec['type'] == 8:
            continue
        blob = data[sec['offset']:sec['offset'] + sec['size']]
        for m in re.finditer(b'[\\x20-\\x7e]{5,}', blob):
            text = m.group().decode()
            if pattern.search(text):
                hits.append(dict(va=sec['va'] + m.start(), section=sec['name'], text=text))
    if not hits and path.name not in ['libvscript.so', 'libvideo.so', 'libengine2.so', 'libtier0.so']:
        continue
    by_va = {x['va']: x for x in hits}
    refs = []
    for sec in secs:
        if not sec['flags'] & 4 or sec['type'] != 1:
            continue
        regs = {}
        for o in range(sec['offset'], sec['offset'] + sec['size'] - 3, 4):
            pc = sec['va'] + o - sec['offset']
            w = struct.unpack_from('<I', data, o)[0]
            if w & 2667577344 == 2415919104:
                imm = (w >> 5 & 524287) * 4 + (w >> 29 & 3)
                if imm & 1 << 20:
                    imm -= 1 << 21
                regs[w & 31] = ((pc & ~4095) + (imm << 12), pc)
            elif w & 4278190080 == 2432696320:
                rd = w & 31
                rn = w >> 5 & 31
                imm = (w >> 10 & 4095) << (12 if w & 1 << 22 else 0)
                if rn in regs and pc - regs[rn][1] <= 128:
                    value = regs[rn][0] + imm
                    if value in by_va:
                        refs.append(dict(instruction_va=pc, string_va=value, text=by_va[value]['text']))
                    regs[rd] = (value, pc)
                else:
                    regs.pop(rd, None)
            elif w & 4227858432 == 2483027968:
                regs = {k: v for (k, v) in regs.items() if k >= 19}
            elif w & 520093696 in (268435456, 285212672, 167772160, 184549376):
                regs.pop(w & 31, None)
            else:
                regs.pop(w & 31, None)
    buildids = []
    for s in secs:
        if s['type'] != 7:
            continue
        pos = s['offset']
        end = pos + s['size']
        while pos + 12 <= end:
            (ns, ds, ty) = struct.unpack_from('<III', data, pos)
            pos += 12
            name = data[pos:pos + ns]
            pos += ns + 3 & ~3
            desc = data[pos:pos + ds]
            pos += ds + 3 & ~3
            if name.rstrip(b'\x00') == b'GNU' and ty == 3:
                buildids.append(desc.hex())
    report = dict(file=str(path.relative_to(root)), sha256=hashlib.sha256(data).hexdigest(), bytes=len(data), build_ids=buildids, strings=hits, direct_string_references=refs, relevant_symbols=[x for x in syms if re.search('lua_VALVE|Plat_Virtual|luaL_newstate|lua_newstate|CreateInterface|VRClientCore|Source2Main', x['name'])])
    reports.append(report)
(args.output / 'inventory.json').write_text(json.dumps(reports, indent=2) + '\n')
for r in reports:
    print(r['file'], r['build_ids'], 'strings', len(r['strings']), 'refs', len(r['direct_string_references']))
    for x in r['direct_string_references']:
        if 'fragment_density' in x['text'] or 'Foveation Center' in x['text']:
            print(' ', hex(x['instruction_va']), hex(x['string_va']), x['text'])
verification = {r['file']: hashlib.sha256((root / r['file']).read_bytes()).hexdigest() == r['sha256'] for r in reports}
(args.output / 'unchanged.json').write_text(json.dumps(verification, indent=2) + '\n')
if not all(verification.values()):
    raise RuntimeError('A source changed during inspection; discard the affected analysis')
print(f'PASS: {len(verification)} inspected ELF files unchanged')
