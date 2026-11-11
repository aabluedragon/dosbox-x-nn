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
- Remote play guard (capability `inputnetShellGuard`): a viewer's keyboard and mouse go straight to
  the emulated keyboard and mouse (never to DOSBox-X's hotkeys or menus), and only while a DOS program
  other than a command shell runs, or a guest system was booted. At the DOS prompt (DOSBox-X's shell,
  a nested `COMMAND`, or a COMMAND.COM/4DOS/NDOS loaded from disk) and while DOSBox-X's own programs
  (MOUNT, CONFIG, ...) run, viewer input is dropped, keys viewers hold are released, and when a game
  ends the keystrokes still waiting in the keyboard buffers are discarded. The log says
  `INPUTNET: remote input blocked/allowed` at each change.
- Remote play mouse (capability `inputnetMouse`): a viewer's mouse, captured on the viewer's side, moves
  the host's emulated mouse by relative motion even though the host window has not captured the mouse:
  the INT 33h position no longer snaps back to the host's own cursor, and the motion counters (function
  0Bh) advance, so mouse-driven DOS games (Bonus, for one) work for the remote player.
- Wayland without XWayland: with no X display (`DISPLAY` unset) and a Wayland one, DOSBox-X-NN uses SDL's Wayland
  video driver instead of failing at start ("Can't init SDL x11 not available"); X11 (XWayland included) stays the
  default. The Linux packages' SDL is built with Wayland, loaded from the system when used.
- FILEMOUNT from asar archives (capability `filemountAsar`): a host path inside an Electron asar archive
  (`.../resources/app.asar/res/gameres/IPXSETUP.EXE`, where NukemNet keeps its DOS helpers) is read from the
  archive: packed files are extracted to a private temporary file (deleted when DOSBox-X exits) and mapped,
  unpacked ones are mapped from `app.asar.unpacked`, links are followed.
- Remote play motion and hidden screen fixes (capability `inputnetMotion`): INT 33h function 0Bh and the
  PS/2 mouse report the viewer's motion while a viewer is connected (they reported nothing unless the host
  window had captured the mouse, so games that turn by the motion counters, such as Rise of the Triad,
  could not be turned); on Windows the hidden screen no longer resizes itself on every frame (the native
  menu bar wrapped in the narrow window, the placeholder blinked and no video reached the viewers); a
  viewer's error message no longer opens behind its window.
- Remote play over slower links and SVGA (capability `inputnetAdaptive`): viewers report the newest frame
  they decoded and the video fragments that arrived; the host encodes each frame against a frame every
  viewer has (a lost frame no longer stops the picture until the next key frame), paces video to the rate
  the link takes (less on loss, more while there is none, from 1.5 MB/s) and skips frames instead of
  queueing them. 32-bit frames of 256-colour modes (DOSBox-X draws SVGA 8-bit modes such as Shadow
  Warrior's 800x600 that way) go out as palette and indices, and a frame that compresses better alone
  than as a difference goes out as a key frame. Viewers and hosts without it keep working together with
  it, as before. Before, an 800x600 game froze the viewer's picture after its first frames.
- Hidden remote play screen (capability `inputnetHideScreen`): `-inputnet-hidescreen` or
  `[inputnet] hostscreen = false` on a remote play host keeps its window small (menu bar and a short
  notice, on the surface output), opens it minimized and without the focus, so it never covers the
  window in front (some window managers, labwc for one, put new windows on top whatever they ask), and
  keeps its sound off this machine's speakers, while the viewers still get the full picture and sound.
  Streaming goes on while it is minimized, and a `priority` that pauses unfocused windows is ignored.
  NukemNet uses it for the copies that play for remote players. Restore it from the taskbar for
  *Video > Show screen (remote play)* (mapper event `rpscreen`, no default key), which shows or hides
  the picture at any time.

Packages are published under `https://storage.nukemnet.com/misc/dosbox-x-nn/`, each with an
`nn-capabilities.json` containing `{"win95Ipx":true,"win95FastBoot":true,"inputnetRemote":true,"filemount":true,"inputnetHideScreen":true,"inputnetShellGuard":true,"inputnetMouse":true,"inputnetMotion":true,"filemountAsar":true,"inputnetAdaptive":true}`.

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
