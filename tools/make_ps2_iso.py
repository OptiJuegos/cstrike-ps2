#!/usr/bin/env python3
"""Build a bootable PS2 ISO from the packed installation (cs-ps2/packed).

Layout: SYSTEM.CNF, the ELF under the disc id, and the valve/ and cstrike/
folders with their PAKs and liblist.gam. cdrom0: cannot list folders, so each
folder also gets dirlist.txt (one name per line, folders end with '/'); the
engine reads it instead (PS2_ListDiscDirectory in engine/common/filesystem.c).
Names are mapped like ps2_disc_path in engine/platform/ps2/sys_ps2.c.

Usage:
  python tools/package_ps2_assets.py
  python tools/make_ps2_iso.py
  python tools/make_ps2_iso.py --id SLUS_666.16 --vmode PAL
"""
import argparse
import io
import os
import re
import sys
from pathlib import Path

import pycdlib

ROOT = Path(__file__).resolve().parent.parent
PACKED = ROOT / 'cs-ps2' / 'packed'
GAME_DIRS = ('valve', 'cstrike')
NAME_MAX = 30  # ISO9660 level 2, ";1" included in the 32-byte limit


def disc_name(name: str) -> str:
    out = []
    for c in name.upper():
        out.append(c if ('A' <= c <= 'Z') or ('0' <= c <= '9') or c in '_.' else '_')
    mapped = ''.join(out)
    if len(mapped) > NAME_MAX:
        raise ValueError(f'name too long for the disc: {name}')
    return mapped


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--packed', type=Path, default=PACKED)
    ap.add_argument('--elf', type=Path, default=PACKED / 'xash3d-cstrike-fastpak-usb-ps2.elf')
    ap.add_argument('--id', default='SLUS_666.16', help='disc id, form SLUS_123.45')
    ap.add_argument('--vmode', default='NTSC', choices=['NTSC', 'PAL'])
    ap.add_argument('--out', type=Path, default=PACKED / 'CS16PS2.iso')
    opts = ap.parse_args()

    if not re.fullmatch(r'[A-Z]{4}_\d{3}\.\d{2}', opts.id):
        sys.exit(f'--id must look like SLUS_123.45, got {opts.id}')
    for need in [opts.elf] + [opts.packed / d / 'liblist.gam' for d in GAME_DIRS]:
        if not need.is_file():
            sys.exit(f'missing {need} (run tools/package_ps2_assets.py first)')

    iso = pycdlib.PyCdlib()
    iso.new(interchange_level=2, vol_ident='CS16PS2')
    cnf = f'BOOT2 = cdrom0:\\{opts.id};1\r\nVER = 1.00\r\nVMODE = {opts.vmode}\r\n'.encode('ascii')
    iso.add_fp(io.BytesIO(cnf), len(cnf), '/SYSTEM.CNF;1')
    iso.add_file(str(opts.elf), f'/{opts.id};1')

    def add_listing(iso_dir: str, names):
        data = ''.join(n + '\n' for n in names).encode('ascii')
        if len(data) >= 4096:
            raise ValueError(f'dirlist.txt over 4 KiB in {iso_dir or "/"}')
        iso.add_fp(io.BytesIO(data), len(data), f'{iso_dir}/DIRLIST.TXT;1')

    total = 0
    add_listing('', [d + '/' for d in GAME_DIRS])
    for game in GAME_DIRS:
        source = opts.packed / game
        iso_dir = '/' + disc_name(game)
        iso.add_directory(iso_dir)
        names = []
        for item in sorted(source.iterdir()):
            if item.is_dir():
                sys.exit(f'subfolder {item} would need its own listing; pack it instead')
            iso.add_file(str(item), f'{iso_dir}/{disc_name(item.name)};1')
            names.append(item.name.lower())
            total += item.stat().st_size
        add_listing(iso_dir, names)

    opts.out.parent.mkdir(parents=True, exist_ok=True)
    temporary = opts.out.with_name(opts.out.name + '.tmp')
    try:
        iso.write(str(temporary))
        iso.close()
        check = pycdlib.PyCdlib()
        try:
            check.open(str(temporary))
            out = io.BytesIO()
            check.get_file_from_iso_fp(out, iso_path='/SYSTEM.CNF;1')
            if out.getvalue() != cnf:
                raise IOError('SYSTEM.CNF verification failed')
            check.get_record(iso_path=f'/{opts.id};1')
            for game in GAME_DIRS:
                for item in (opts.packed / game).iterdir():
                    record = check.get_record(iso_path=f'/{disc_name(game)}/{disc_name(item.name)};1')
                    if record.data_length != item.stat().st_size:
                        raise IOError(f'size mismatch on the disc: {item}')
        finally:
            check.close()
        os.replace(temporary, opts.out)
    finally:
        if temporary.exists():
            temporary.unlink()
    print(f'ISO: {opts.out} ({opts.out.stat().st_size / 1048576:.1f} MiB), '
          f'game data {total / 1048576:.1f} MiB, id {opts.id}')


if __name__ == '__main__':
    main()
