# NukemNet packages

This fork is the DOSBox-X that [NukemNet](https://nukemnet.com) downloads automatically. It adds:

- Full Ethernet IPX frames (1,500 bytes) in the IPX client and server, so Windows 95 guests can play
  IPX multiplayer games. Upstream truncates them at 1,424 bytes: the lobby can work while the match
  fails to start. DOS IPX games are unaffected.
- An integration-device turbo register (`0x55504303`) that lets a guest read and set turbo, which
  NukemNet's Windows 95 launcher uses to boot quickly and play at normal speed.
- `FILEMOUNT C:\DIR\NAME.EXT <host file>`, which maps one host file into a mounted folder without
  copying it there. A real file of that name is hidden while the mapping exists (and left untouched);
  the folder must exist. NukemNet uses it to add maps, mods and patched files to a game's folder for a
  launch, while everything the game writes still goes to the real folder (capability `filemount`).
- Inputnet, for games whose only multiplayer is two players on one PC. In the default remote play
  mode (`[inputnet] mode = remote`) the host runs the game with `-inputnet-server <port>` and streams
  its picture and sound. A joining player starts DOSBox-X with `-inputnet-connect <address:port>`,
  which opens a viewer window instead of an emulated machine and sends keyboard and mouse input back,
  so the joining player needs neither the game nor matching settings. `[inputnet] stream fps` caps
  the frame rate to save upload bandwidth. `mode = lockstep` keeps the older mode in which every
  player runs an identical machine.

Packages are published under `https://storage.nukemnet.com/misc/dosbox-x-nn/`, each with an
`nn-capabilities.json` containing `{"win95Ipx":true,"win95FastBoot":true,"inputnetRemote":true,"filemount":true}`.

- Linux x64, ARM64 and ARMv7: `nukemnet/linux` (portable Debian Bullseye build with bundled
  libraries), run by the *NukemNet Linux packages* workflow.
- Windows (MinGW x64 and x86, Visual Studio ARM64) and macOS (Intel and Apple silicon): the
  standard workflows in `.github/workflows`, repackaged by NukemNet with the capability file.

## Publishing a release

1. Change the source, then pin its commit, archive checksum and `RELEASE` in `nukemnet/linux/build.sh`
   and `release` in `nukemnet/linux/bundle.py`.
2. Run the *NukemNet Linux packages* workflow and the standard MinGW 64-bit, MinGW 32-bit, 64-bit
   Visual Studio (ARM64 job) and macOS workflows. Each standard workflow cancels its own earlier runs
   when it starts, so avoid pushing while they run.
3. Repackage the Windows and macOS artifacts with `nukemnet/repackage.py`, copy the Linux outputs, and
   upload everything with a `dosbox-x-nn-<release>.sha256` file to `storage.nukemnet.com/misc/dosbox-x-nn/`.
4. Write the signed release list from the checksum file in NukemNet's `app/` folder,
   `node snippets/dosbox-x-nn-manifest.js dosbox-x-nn-<release>.sha256 latest.json` (it uses the app update signing
   key), and upload it as `storage.nukemnet.com/misc/dosbox-x-nn/latest.json` after the packages. Running NukemNet
   clients offer the release from it (a Yes/No prompt), without a NukemNet update.
5. Update `RELEASE` in `site/src/lib/dosbox-x-nn.ts` (`DOSBOX_X_NN_RELEASE`), which nukemnet.com's Downloads page links
   to. The next NukemNet release should also pin it with its SHA-256 values in `app/src/lib/dosbox-x-download.ts`
   (`PINNED_DOSBOX_X_NN`): new installations and players whose release check fails get that one.
