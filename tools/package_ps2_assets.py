"""Build the two-PAK test installation from cs-ps2/out without changing it."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

from make_mcpk import make


def main():
    root = Path(__file__).resolve().parent.parent
    source_root = root / 'cs-ps2/out'
    output = root / 'cs-ps2/packed'
    elf = 'xash3d-cstrike-fastpak-usb-ps2.elf'
    for required in (source_root / elf,
                     source_root / 'valve/liblist.gam',
                     source_root / 'cstrike/liblist.gam'):
        if not required.is_file():
            raise FileNotFoundError(required)
    output.mkdir(parents=True, exist_ok=True)
    manifest = []
    for game in ('valve', 'cstrike'):
        source = source_root / game
        destination = output / game
        destination.mkdir(parents=True, exist_ok=True)
        # Hardlink staging avoids copying the full installation. The packer
        # only reads these files; originals are never edited or removed.
        with tempfile.TemporaryDirectory(prefix='stage-', dir=output) as work:
            stage = Path(work).resolve()
            if stage.parent != output.resolve():
                raise ValueError('Staging directory escaped output root')
            for item in source.rglob('*'):
                if item.is_symlink():
                    raise ValueError('Symbolic link: %s' % item)
                if not item.is_file():
                    continue
                relative = item.relative_to(source)
                # PCSX2 test autostart (map de_aztec); never in an install
                if relative.as_posix() == 'userconfig.d/perf.cfg':
                    continue
                # Archives must remain external: nested PAKs are not mounted.
                if item.suffix.lower() == '.pak':
                    if relative.as_posix().lower() == 'assets.pak':
                        raise ValueError('Source already contains assets.pak')
                    target = destination / relative
                    target.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(item, target)
                    continue
                target = stage / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                os.link(item, target)
            # PS2 project data always ships, newest version, even when
            # cs-ps2/out has an older copy. Only these files: the rest of
            # gamedata is PSP setup (userconfig.cfg turns the camera with
            # the face buttons, the PSP has no right stick).
            overlay = root / 'gamedata' / game
            if overlay.is_dir():
                for item in overlay.rglob('*'):
                    relative = item.relative_to(overlay).as_posix()
                    if not item.is_file() or not (relative == 'splitscreen.cfg'
                            or (relative.startswith('maps/') and relative.endswith('.nav'))):
                        continue
                    target = stage / item.relative_to(overlay)
                    target.parent.mkdir(parents=True, exist_ok=True)
                    if target.exists():
                        target.unlink()
                    os.link(item, target)
            pak = destination / 'assets.pak'
            make(stage, pak, 64, True)
        # Game identification happens before the game archives are mounted.
        shutil.copy2(source / 'liblist.gam', destination / 'liblist.gam')
        with pak.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        manifest.append({'path': pak.relative_to(output).as_posix(),
                         'bytes': pak.stat().st_size, 'sha256': digest})
    shutil.copy2(source_root / elf, output / elf)
    (output / 'pak-manifest.json').write_text(
        json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
