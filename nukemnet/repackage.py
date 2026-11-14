#!/usr/bin/env python3
"""Turns a Windows or macOS CI artifact into the package NukemNet downloads.

    repackage.py <platform> <artifact directory> <output directory> <release> <commit>

platform: windows-x64, windows-ia32, windows-arm64, macos-x64 or macos-arm64. The artifact
directory holds the extracted CI artifact (for macOS, the zip inside it is extracted too).
Only the folders NukemNet runs are kept: the SDL2 build on Windows, both SDL builds on macOS
(SDL1 serves Macs older than macOS 12). nn-capabilities.json goes beside each executable, or
beside the .app so the bundle stays unchanged.
"""
import hashlib
import shutil
import sys
import zipfile
from pathlib import Path

platform, artifact, output, release, commit = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], sys.argv[5]
keep = {
    'windows-x64': ['mingw-build/mingw-sdl2'],
    'windows-ia32': ['mingw-build/mingw-sdl2'],
    'windows-arm64': ['bin/ARM64/Release SDL2'],
    'macos-x64': ['dosbox-x', 'dosbox-x-sdl2'],
    'macos-arm64': ['dosbox-x', 'dosbox-x-sdl2'],
}[platform]
macos = platform.startswith('macos')

if macos:
    inner = sorted(artifact.glob('*.zip'))
    if inner:
        shutil.unpack_archive(inner[0], artifact)
root = next((candidate for candidate in [artifact, *artifact.iterdir()] if (candidate / keep[0]).is_dir()), None)
if root is None:
    sys.exit(f'{keep[0]} not found in {artifact}')

stage = output / f'stage-{platform}'
shutil.rmtree(stage, ignore_errors=True)
for folder in keep:
    source = root / folder
    if not source.is_dir():
        sys.exit(f'{folder} not found in {root}')
    shutil.copytree(source, stage / folder, symlinks=True)
    executable = (stage / folder / 'dosbox-x.app') if macos else (stage / folder / 'dosbox-x.exe')
    if not executable.exists():
        sys.exit(f'{executable} is missing')
    (stage / folder / 'nn-capabilities.json').write_text('{"win95Ipx":true,"win95FastBoot":true,"inputnetRemote":true,"filemount":true,"inputnetHideScreen":true,"inputnetShellGuard":true,"inputnetMouse":true,"inputnetMotion":true,"filemountAsar":true,"inputnetAdaptive":true,"inputnetPointer":true,"diskLayer":true,"inputnetLockdown":true,"inputnetPredict":true,"inputnetPredictView":true,"inputnetBlockKeys":true,"inputnetReportInput":true,"nnControl":true}\n')
license_file = next((root / name for name in ['COPYING', 'COPYING.txt'] if (root / name).is_file()), None)
if license_file:
    shutil.copy2(license_file, stage / 'COPYING')
elif not macos:
    sys.exit('COPYING is missing')
(stage / 'README-NUKEMNET.txt').write_text(f'''DOSBox-X (dosbox-x-nn {release}) for NukemNet, {platform}
Built by the fork's GitHub Actions workflows, from NukemNet's DOSBox-X fork:
https://github.com/aabluedragon/dosbox-x-nn (commit {commit})
The fork receives full Ethernet IPX frames (needed for Windows 95 multiplayer) and adds
guest-controlled turbo so Windows 95 can restore normal speed before launching a game.
Source: https://github.com/aabluedragon/dosbox-x-nn/archive/{commit}.tar.gz
Original project: https://github.com/joncampbell123/dosbox-x
DOSBox-X is distributed under the GNU General Public License, version 2 (see COPYING).
''')

archive_path = output / f'dosbox-x-nn-{platform}-{release}.zip'
with zipfile.ZipFile(archive_path, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for path in sorted(stage.rglob('*')):
        relative = path.relative_to(stage).as_posix()
        if path.is_symlink():
            # Keep framework symlinks inside the macOS bundle as symlinks.
            info = zipfile.ZipInfo(relative)
            info.external_attr = 0o120777 << 16
            archive.writestr(info, str(path.readlink()))
        elif path.is_file():
            info = zipfile.ZipInfo.from_file(path, relative)
            info.compress_type = zipfile.ZIP_DEFLATED
            archive.writestr(info, path.read_bytes())
shutil.rmtree(stage)
print(archive_path.name, hashlib.sha256(archive_path.read_bytes()).hexdigest())
