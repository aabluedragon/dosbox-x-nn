# NukemNet packages

This fork is the DOSBox-X that [NukemNet](https://nukemnet.com) downloads automatically. It adds:

- Full Ethernet IPX frames (1,500 bytes) in the IPX client and server, so Windows 95 guests can play
  IPX multiplayer games. Upstream truncates them at 1,424 bytes: the lobby can work while the match
  fails to start. DOS IPX games are unaffected.
- An integration-device turbo register (`0x55504303`) that lets a guest read and set turbo, which
  NukemNet's Windows 95 launcher uses to boot quickly and play at normal speed.

Packages are published under `https://storage.nukemnet.com/misc/dosbox-x-nn/`, each with an
`nn-capabilities.json` containing `{"win95Ipx":true,"win95FastBoot":true}`.

- Linux x64, ARM64 and ARMv7: `nukemnet/linux` (portable Debian Bullseye build with bundled
  libraries), run by the *NukemNet Linux packages* workflow.
- Windows (MinGW x64 and x86, Visual Studio ARM64) and macOS (Intel and Apple silicon): the
  standard workflows in `.github/workflows`, repackaged by NukemNet with the capability file.
