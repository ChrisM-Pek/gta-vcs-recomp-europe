#!/usr/bin/env python3
"""Build the experimental SLES_546.22 port from the player's own ISO.

This build has only been tested through the opening and initial movement.
It uses the original PS2 game paths where PAL-specific native hooks are absent.
"""
import argparse
import hashlib
import os
import re
import subprocess
import sys

import toolchain as tc
import vcs_disc

SHA1 = '47048caa4bd2ce8bd5223847b7abe02aa790bc3e'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('iso', help='European SLES_546.22 ISO')
    args = parser.parse_args()
    iso = tc.find_iso(args.iso)
    image = vcs_disc.IsoImage(iso)
    try:
        cnf = image.find('SYSTEM.CNF')
        if not cnf or b'SLES_546.22' not in image.read_file(cnf).upper():
            tc.fail('not an SLES_546.22 disc', 'wrong_disc')
        entry = image.find('SLES_546.22')
        if not entry or entry['dir']:
            tc.fail('SLES_546.22 missing from disc', 'wrong_disc')
        elf = image.read_file(entry)
    finally:
        image.f.close()
    if hashlib.sha1(elf).hexdigest() != SHA1:
        tc.fail('unsupported SLES_546.22 executable build', 'wrong_disc')

    analysis = os.path.join(tc.BUILD, 'analysis')
    os.makedirs(analysis, exist_ok=True)
    elf_path = os.path.join(tc.BUILD, 'elf', 'SLES_546.22')
    os.makedirs(os.path.dirname(elf_path), exist_ok=True)
    with open(elf_path, 'wb') as f:
        f.write(elf)
    stage = os.path.join(analysis, 'pal_stage')
    fresh = os.path.join(analysis, 'pal_fresh')
    vu = os.path.join(analysis, 'pal_vu1.cpp')
    pal = os.path.join(tc.ROOT, 'game', 'vcs', 'pal')
    env = tc.build_env()
    defines = {'VCS_GENERATED_DIR': stage.replace(os.sep, '/'), 'VCS_VU1_SOURCE': vu.replace(os.sep, '/'),
               'VCS_EXPERIMENTAL_PAL': 'ON', 'PS2X_LTO': 'OFF', 'PS2X_PGO': ''}
    tc.configure(env, defines=defines)
    if not tc.cmake_build(env, 'ps2_recomp', 'build_recomp_pal.log'):
        tc.fail('PAL recompiler build failed', 'recompiler_failed')

    with open(os.path.join(pal, 'vcs.toml'), encoding='utf-8') as f:
        config = f.read()
    for key, value in (('input', elf_path), ('output', fresh), ('ghidra_output', os.path.join(pal, 'functions.csv'))):
        config = re.sub(r'(?m)^' + key + r' = .*$', lambda _: key + ' = "' + value.replace(os.sep, '/') + '"', config)
    cfg = os.path.join(analysis, 'pal_build.toml')
    with open(cfg, 'w', encoding='utf-8') as f:
        f.write(config)
    subprocess.run([tc.RECOMP_EXE, cfg], cwd=tc.ROOT, env=env, check=True)
    with open(os.path.join(pal, 'host_aliases.h'), encoding='utf-8') as f:
        aliases = f.read()
    with open(os.path.join(fresh, 'ps2_recompiled_functions.h'), 'a', encoding='utf-8') as f:
        f.write('\n' + aliases)
    subprocess.run(tc.PY + [tc.tool('sync_generated.py'), fresh, stage], cwd=tc.ROOT, check=True)
    subprocess.run(tc.PY + [tc.tool('vu1_recomp.py'), elf_path, vu,
                           os.path.join(tc.ROOT, 'game', 'vcs', 'vu1_uploads_pal.txt')], cwd=tc.ROOT, check=True)
    tc.configure(env, force=True, defines=defines)
    if not tc.cmake_build(env, 'ps2EntryRunner', 'build_pal.log'):
        tc.fail('PAL game build failed', 'build_failed')
    print('Experimental PAL executable:', tc.VCS_EXE)


if __name__ == '__main__':
    main()
