# Portable DOSBox-X for Linux (NukemNet)

Builds this fork, at the commit pinned in `build.sh`, with its included SDL 2.32.10 on Debian
Bullseye for x64, ARM64 or ARMv7. ARMv7 requires hard float with VFPv3-D16. All require an
X11/XWayland display. Debugging is disabled. IPX, ENet serial, SLIRP, overlays and the dynamic
CPU core are enabled. The launcher uses the bundled loader and libraries, so it needs neither
root installation nor FUSE. Software video output is the default; later `-conf` arguments can
override it. The bundled loader may not load a newer host's Mesa drivers; OpenGL then falls back
to software output.

`bundle.py` includes the ELF dependency closure, SDL's runtime-loaded device/X11/audio libraries
(including `libudev.so.1`, which otherwise can fail against the bundled glibc), copyright notices
and exact dependency source versions, and writes `nn-capabilities.json`. The build defines
`_FILE_OFFSET_BITS=64` before system headers so ARM32 directory enumeration works on filesystems
with large inodes or directory offsets.

With Docker or Podman, from the repository root:

```sh
podman build --build-arg TARGET_ARCH=x64 -t nn-dosbox-x nukemnet/linux
mkdir -p output && podman run --rm -v "$PWD/output:/output" nn-dosbox-x
```

Use `TARGET_ARCH=arm64` or `TARGET_ARCH=armv7l` for the ARM cross-builds. The *NukemNet Linux
packages* workflow builds all three. To change the source, update `COMMIT`, its archive checksum
and `RELEASE` in `build.sh`, and `release` in `bundle.py`.

`verify-ipx.py /path/to/package/bin/dosbox-x` checks that the IPX server forwards a 1,470-byte
packet without truncation. For other architectures add `--qemu qemu-arm --loader ld-linux-armhf.so.3`
(ARMv7) or `--qemu qemu-aarch64 --loader ld-linux-aarch64.so.1` (ARM64). It does not replace a
Windows 95 multiplayer test.

Publish each package with its build recipe, the source archive, the dependency source archive and
checksums under `https://storage.nukemnet.com/misc/dosbox-x-nn/`.
