#!/usr/bin/env python3
# Copyright 2026, Nick Kennedy
# SPDX-License-Identifier: MIT
"""Rebuild one changed Mesa object into an isolated copy of its static archive.

All other objects come from the existing tested build. This diagnoses a narrow
driver change without changing that build; it is not a clean Mesa build.
"""
import hashlib
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

base, source, out = (Path(p).resolve() for p in sys.argv[1:])
out.mkdir(parents=True, exist_ok=False)
original_archive = base/'src/kosmickrisp/vulkan/libkk.a'
before = hashlib.sha256(original_archive.read_bytes()).hexdigest()
entries = json.loads((base/'compile_commands.json').read_text())
entry = next(c for c in entries if c['file'].endswith('/kk_cmd_draw.c'))
compile_cmd = shlex.split(entry['command'])
for flag, value in [('-o', out/'kk_cmd_draw.c.o'), ('-MF', out/'kk_cmd_draw.c.o.d'),
                    ('-MQ', out/'kk_cmd_draw.c.o')]:
    compile_cmd[compile_cmd.index(flag)+1] = str(value)
compile_cmd[compile_cmd.index('-c')+1] = str(source/'src/kosmickrisp/vulkan/kk_cmd_draw.c')
commands = [compile_cmd]
subprocess.run(compile_cmd, cwd=base, check=True)
archive = out/'libkk.a'
shutil.copyfile(original_archive, archive)
replace = ['ar', 'rcs', str(archive), str(out/'kk_cmd_draw.c.o')]
commands.append(replace)
subprocess.run(replace, check=True)
link = shlex.split(subprocess.check_output(
    ['ninja', '-t', 'commands', 'src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib'],
    cwd=base, text=True).splitlines()[-1])
library = out/'libvulkan_kosmickrisp.dylib'
link[link.index('-o')+1] = str(library)
link[link.index('src/kosmickrisp/vulkan/libkk.a')] = str(archive)
commands.append(link)
subprocess.run(link, cwd=base, check=True)
assert hashlib.sha256(original_archive.read_bytes()).hexdigest() == before
icd = json.loads((base/'src/kosmickrisp/vulkan/kosmickrisp_mesa_devenv_icd.aarch64.json').read_text())
icd['ICD']['library_path'] = str(library)
(out/'icd.json').write_text(json.dumps(icd, indent=2)+'\n')
(out/'build-identity.json').write_text(json.dumps({
    'kind': 'isolated single-object overlay; not clean build',
    'base_build': str(base), 'source': str(source), 'commands': commands,
    'base_archive_sha256': before,
    'source_sha256': hashlib.sha256((source/'src/kosmickrisp/vulkan/kk_cmd_draw.c').read_bytes()).hexdigest(),
    'library_sha256': hashlib.sha256(library.read_bytes()).hexdigest(),
}, indent=2)+'\n')
