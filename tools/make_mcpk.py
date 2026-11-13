#!/usr/bin/env python3
"""Make an MCPK v1 archive without changing any asset bytes.

Example:
    python tools/make_mcpk.py "E:/Juegos/CS/cstrike" "E:/Juegos/CS-packed/cstrike/assets.pak"
    python tools/make_mcpk.py "E:/Juegos/CS/valve" "E:/Juegos/CS-packed/valve/assets.pak"

Compatible with engine/platform/ps2/ps2_fastpak.c and Geometry Dash PS2 MCPK v1.
Paths inside the archive are relative to the chosen input folder.
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct

HEADER = struct.Struct('>4s7I')
RECORD = struct.Struct('>4I')
ALIGNMENT = 64

def fnv(key):
    h = 0x811c9dc5
    for value in key.lower().replace('\\', '/').encode('utf-8'):
        h = ((h ^ value) * 0x01000193) & 0xffffffff
    return h

def align(n, amount):
    return (n + amount - 1) // amount * amount

def priority(key):
    lower = key.lower()
    if lower.startswith('maps/') and lower.endswith('.bsp'): return (0, lower)
    if lower.startswith('models/'): return (1, lower)
    if lower.startswith(('sprites/', 'gfx/')): return (2, lower)
    if lower.startswith(('sound/', 'events/')): return (3, lower)
    return (4, lower)

def source_files(root):
    items, keys = [], set()
    for src in root.rglob('*'):
        if src.is_symlink():
            raise ValueError('Symlinks unsupported: %s' % src)
        if not src.is_file(): continue
        rel = src.relative_to(root).as_posix()
        if rel.lower() == 'assets.pak':
            raise ValueError('Source contains assets.pak; remove it from the source copy before packing')
        if any(segment in ('', '.', '..') for segment in rel.split('/')):
            raise ValueError('Unsafe archive name: %s' % rel)
        folded = rel.casefold()
        if folded in keys: raise ValueError('Case-insensitive duplicate: %s' % rel)
        keys.add(folded)
        size = src.stat().st_size
        if size >= 0x80000000: raise ValueError('Asset too large for PS2: %s' % rel)
        items.append((rel, src, size))
    if not items or len(items) > 65536: raise ValueError('Invalid file count')
    return sorted(items, key=lambda t: (fnv(t[0]), t[0].lower()))

def make(root, target, alignment, verify):
    root = root.resolve()
    target = target.resolve()
    if not root.is_dir(): raise ValueError('Missing input directory: %s' % root)
    if target == root or root in target.parents:
        raise ValueError('Output must be outside input folder')
    files = source_files(root)
    names, offsets = bytearray(), {}
    for name, _, _ in files:
        offsets[name] = len(names)
        names.extend(name.encode('utf-8') + b'\0')
    base = align(HEADER.size + RECORD.size * len(files) + len(names), alignment)
    ordered = sorted(files, key=lambda item: priority(item[0]))
    extents = {}
    pos = base
    for name, _, size in ordered:
        if pos + size > 0x7fffffff: raise ValueError('Archive exceeds PS2 2GiB offset limit')
        extents[name] = pos
        pos = align(pos + size, alignment)
    if pos > 0x7fffffff: raise ValueError('Archive exceeds PS2 2GiB limit')
    target.parent.mkdir(parents=True, exist_ok=True)
    tmp = target.with_name(target.name + '.tmp')
    try:
        with tmp.open('wb') as out:
            out.write(HEADER.pack(b'MCPK', 1, len(files), HEADER.size,
                                  HEADER.size + RECORD.size * len(files),
                                  len(names), alignment, 0))
            for name, _, size in files:
                out.write(RECORD.pack(fnv(name), offsets[name], extents[name], size))
            out.write(names)
            for name, src, size in ordered:
                out.write(b'\0' * (extents[name] - out.tell()))
                with src.open('rb') as inp:
                    while True:
                        chunk = inp.read(1024 * 1024)
                        if not chunk: break
                        out.write(chunk)
                if out.tell() != extents[name] + size:
                    raise ValueError('Asset changed during pack: %s' % src)
        if verify:
            with tmp.open('rb') as packed:
                for name, src, size in files:
                    packed.seek(extents[name])
                    archive_hash, original_hash = hashlib.sha256(), hashlib.sha256()
                    remaining = size
                    while remaining:
                        buf = packed.read(min(remaining, 1024 * 1024))
                        if not buf: raise ValueError('Archive truncated: %s' % name)
                        archive_hash.update(buf)
                        remaining -= len(buf)
                    with src.open('rb') as inp:
                        for chunk in iter(lambda: inp.read(1024 * 1024), b''):
                            original_hash.update(chunk)
                    if archive_hash.digest() != original_hash.digest():
                        raise ValueError('SHA256 mismatch: %s' % name)
        os.replace(tmp, target)
    finally:
        if tmp.exists(): tmp.unlink()
    print('MCPK ready: %d files, %d bytes: %s' % (len(files), target.stat().st_size, target))

if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('source', type=Path)
    p.add_argument('destination', type=Path)
    p.add_argument('--alignment', type=int, default=ALIGNMENT)
    p.add_argument('--no-verify', action='store_true')
    args = p.parse_args()
    if args.alignment < 64 or args.alignment & (args.alignment - 1):
        p.error('--alignment must be a power of two >= 64')
    make(args.source, args.destination, args.alignment, not args.no_verify)
