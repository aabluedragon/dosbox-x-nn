"""Bundle DOSBox-X and its dependency closure, including SDL's runtime-loaded backends."""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import zipfile

arch = os.environ.get('TARGET_ARCH', 'armv7l')
triple, loader = {
    'x64': ('x86_64-linux-gnu', 'ld-linux-x86-64.so.2'),
    'arm64': ('aarch64-linux-gnu', 'ld-linux-aarch64.so.1'),
    'armv7l': ('arm-linux-gnueabihf', 'ld-linux-armhf.so.3'),
}[arch]
release = '20261112'  # dosbox-x-nn commit ae63383 (keep in step with build.sh)
root = Path('/build/portable')
source = Path('/build/src')
if root.exists():
    shutil.rmtree(root)
root.mkdir(exist_ok=True)
(root / 'bin').mkdir(exist_ok=True)
(root / 'lib').mkdir(exist_ok=True)
(root / 'licenses').mkdir(exist_ok=True)
shutil.copy2(source / 'src/dosbox-x', root / 'bin/dosbox-x')
subprocess.run([triple + '-strip', str(root / 'bin/dosbox-x')], check=True)
directories = [Path(p) for p in ['/opt/nn-dosbox/lib', '/lib/' + triple,
    '/usr/lib/' + triple, '/usr/lib/' + triple + '/pulseaudio']]
queue = [root / 'bin/dosbox-x']
packages = {}

def include(name):
    dest = root / 'lib' / name
    if dest.exists():
        return
    lib = next((directory / name for directory in directories if (directory / name).is_file()), None)
    if not lib:
        raise RuntimeError('Missing library: ' + name)
    shutil.copy2(lib, dest)
    queue.append(dest)
    if str(lib).startswith('/opt/'):
        return  # SDL's matching source is included in the DOSBox-X source archive.
    owner = subprocess.check_output(['dpkg-query', '-S', str(lib)], text=True).split(': ')[0]
    package = owner.split(':')[0]
    packages[package] = subprocess.check_output(['dpkg-query', '-W', '-f', '${source:Package} ${source:Version}', owner], text=True)
    copyright = Path('/usr/share/doc') / package / 'copyright'
    if copyright.exists():
        shutil.copy2(copyright, root / 'licenses' / (package + '.txt'))

for name in [loader, 'libnss_files.so.2', 'libnss_dns.so.2', 'libresolv.so.2',
             'libudev.so.1', 'libasound.so.2', 'libpulse.so.0', 'libX11.so.6', 'libXext.so.6', 'libXcursor.so.1',
             'libXi.so.6', 'libXfixes.so.3', 'libXrandr.so.2', 'libXrender.so.1', 'libXss.so.1']:
    include(name)
while queue:
    binary = queue.pop()
    info = subprocess.check_output([triple + '-readelf', '-d', str(binary)], text=True)
    for name in re.findall(r'\(NEEDED\).*?\[(.*?)\]', info):
        include(name)

shutil.copy2(source / 'COPYING', root / 'COPYING')
shutil.copy2(source / 'vs/sdl2/LICENSE.txt', root / 'licenses/SDL2.txt')
for name in ['Nouveau_IBM.ttf', 'SarasaGothicFixed.ttf', 'FREECG98.BMP']:
    shutil.copy2(source / 'contrib/fonts' / name, root / 'bin' / name)
shutil.copy2(Path(__file__).parent / 'licenses/Sarasa-OFL.txt', root / 'licenses/Sarasa-OFL.txt')
(root / 'licenses/Nouveau-IBM.txt').write_text('Nouveau IBM: "Freeware" (embedded font copyright notice).\nDistributed unchanged from DOSBox-X contrib/fonts/Nouveau_IBM.ttf.\n')
# The C library, its loader and what has to match the system's graphics drivers go to lib/core. With them the build
# runs on old distributions, but it cannot load the system's OpenGL drivers (they need the system's newer C library),
# so DOSBox-X fell back to the surface output (no GLSL shaders such as "sharp"). Where the system's loader finds
# everything, the launcher uses it with the system's C library, C++ runtime and OpenGL, and the bundled rest.
core_prefixes = ('ld-linux', 'libc.so', 'libm.so', 'libpthread.so', 'libdl.so', 'librt.so', 'libresolv.so', 'libnss_',
                 'libutil.so', 'libanl.so', 'libstdc++.so', 'libgcc_s.so', 'libGL.so', 'libGLX.so', 'libGLdispatch.so')
(root / 'lib/core').mkdir(exist_ok=True)
for lib in list((root / 'lib').iterdir()):
    if lib.is_file() and lib.name.startswith(core_prefixes):
        lib.rename(root / 'lib/core' / lib.name)
system_loader = {'x64': '/lib64/ld-linux-x86-64.so.2', 'arm64': '/lib/ld-linux-aarch64.so.1',
                 'armv7l': '/lib/ld-linux-armhf.so.3'}[arch]
(root / 'dosbox-x').write_text(f'''#!/bin/sh
set -eu
base=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# The system's loader when it resolves every library (with the system's OpenGL drivers); else the bundled one.
if [ -z "${{NN_DOSBOX_BUNDLED_ONLY:-}}" ] && [ -x {system_loader} ] &&
   ! {system_loader} --library-path "$base/lib" --list "$base/bin/dosbox-x" 2>&1 | grep -q -e 'not found' -e 'No such file'; then
    exec {system_loader} --library-path "$base/lib" "$base/bin/dosbox-x" -conf "$base/portable.conf" "$@"
fi
exec "$base/lib/core/{loader}" --library-path "$base/lib:$base/lib/core" "$base/bin/dosbox-x" -conf "$base/portable.conf" "$@"
''')
(root / 'dosbox-x').chmod(0o755)
(root / 'portable.conf').write_text('[sdl]\noutput=surface\n')
(root / 'sources.json').write_text(json.dumps(packages, indent=2) + '\n')
(root / 'nn-capabilities.json').write_text('{"win95Ipx":true,"win95FastBoot":true,"inputnetRemote":true,"filemount":true,"inputnetHideScreen":true,"inputnetShellGuard":true,"inputnetMouse":true,"inputnetMotion":true,"filemountAsar":true,"inputnetAdaptive":true}\n')
(root / 'README.txt').write_text(f'''DOSBox-X (dosbox-x-nn {release}) — NukemNet portable Linux {arch} build
Built from NukemNet's DOSBox-X fork, which is based on DOSBox-X 2026.10.01 and later upstream changes.
Requires Linux {arch} and an X11/XWayland display. ARMv7 requires VFPv3-D16 (hard float).
Run ./dosbox-x. No root installation or FUSE is required.
Non-debug build with SDL 2.32.10, IPX, ENet serial, SLIRP, overlays, and the dynamic CPU core.
The fork receives full Ethernet IPX frames (needed for Windows 95 multiplayer) and adds
guest-controlled turbo so Windows 95 can restore normal speed before launching a game.
The default is software video output for compatibility. Later -conf options can override it.
Fork: https://github.com/aabluedragon/dosbox-x-nn (commit 315f180186eb2c01444299e293ac0fbe7627bec8)
Build recipe: https://storage.nukemnet.com/misc/dosbox-x-nn/dosbox-x-nn-linux-{arch}-build-{release}.zip
DOSBox-X/SDL source: https://storage.nukemnet.com/misc/dosbox-x-nn/dosbox-x-nn-{release}-source.tar.gz
Dependency sources: https://storage.nukemnet.com/misc/dosbox-x-nn/dosbox-x-nn-linux-{arch}-dependency-sources-{release}.tar.gz
Original project: https://github.com/joncampbell123/dosbox-x
Dependency versions are in sources.json; copyright notices are in licenses/.
''')
output = Path('/build/output')
output.mkdir(exist_ok=True)
with zipfile.ZipFile(output / f'dosbox-x-nn-linux-{arch}-{release}.zip', 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
    for file in sorted(root.rglob('*')):
        if file.is_file():
            archive.write(file, file.relative_to(root))
print('Bundled', len(list((root / 'lib').iterdir())), 'libraries and', len(packages), 'dependency source packages')
