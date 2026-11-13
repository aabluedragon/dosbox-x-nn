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
- Remote play latency: the viewer measures how long its input takes to reach its screen (the host tells it
  which input each frame is the first to show, in `RP_FRAMEINFO`) and writes the average to the log every
  5 seconds, split into viewer, network, the host waiting for the game's next frame, encoding, the frame
  arriving, and decoding and drawing. Ctrl+F11 in the viewer shows it in the window title. The game's own
  reaction time is not part of it. Big frames (128 KB and more, so 800x600 ones) are compressed on up to four
  threads into one ordinary zlib stream that older viewers read as before, which takes about half the time.
  Frames are encoded and sent on a thread of their own (`Enc_Thread`; hosts with one processor encode on the
  emulation thread as before): the emulation thread only copies the picture, about 0.3 ms a frame at 800x600
  where encoding took it 3 to 6 ms, and a newer picture replaces one the encoder has not started on. The
  host's 5 second log line shows the encoding time and what the game's thread gives. `host_lock` guards what
  the two threads share and `send_lock` keeps queued video leaving in order. While the picture keeps moving a
  lot (the frame alone was smaller than its difference three times in a row) three frames in four are compressed
  alone only, not both ways, unless the link limits the video. When every viewer says it can
  (`RP_FEAT_PIECES`), a frame carries a table of its compressed pieces and the viewer inflates them on several
  threads; other viewers get the plain stream. The viewer draws a frame the
  moment it is complete (no vsync, straight into the texture) and looks for input every millisecond. The host's
  and the viewer's UDP sockets ask for 4 MB kernel buffers (`NetWrapper_WidenUdpBuffers`): with the defaults (64 KB
  on Windows) part of a big frame's burst was dropped whenever the receiver was busy, and the loss made the host
  pace its video, which delayed every frame.
- Remote play frame timing (`[inputnet] present = flip | retrace | scanout`, default `flip`; only while a remote
  play server runs): a VGA takes a new start address at the start of vertical retrace and then spends a refresh
  showing that page, and the emulation handed the frame over when its last line was drawn, so a page a game
  flipped to reached the players 14 to 28 ms later at 70 Hz. A host now renders 256-colour VGA modes on demand
  (as SVGA modes already were), a frame not drawn yet takes the start address the game has set by then
  (`retrace`), and with `flip` the frame is drawn the moment the start address moves to another page
  (`NN_EarlyFrameOK`, `NN_FramePoll` in `vga_draw.cpp`). Measured with test programs that change the screen on
  a key press, through a 42 ms link: 64 ms from key to picture became 45 ms for a page flip (CRTC start address
  as DOS Doom does it, or VBE 4F07h), 51 ms with `retrace`; a game that copies its frame to the screen is shown
  whole one refresh earlier. A mode that uses pixel panning, a split screen or a preset row scan keeps the VGA's
  own timing until the next mode change (the address and the panning must change together there). On a host,
  reading port 3DAh and rewriting an unchanged panning value no longer draw part of the frame (the VESA BIOS
  does both with every page flip). With `flip` a host also shows a frame as soon as a game has copied one onto
  the page shown (`NN_CopyPoll`): NukemNet starts the Build games with `machine=vesa_nolfb`, where they copy
  every frame and never flip, and most 320x200 games copy too. Nothing tells the card a copy is complete, so 256
  pixels of the frame are looked at every quarter of a millisecond; bands changing from the top down, most of
  the screen, not to one colour, and then nothing for two polls, is a frame. Measured the same way: 54 ms from
  key to picture became 47 ms, and a copy caught half-way (2 frames in 150 at 3000 cycles) no longer happens.
  Shadow Warrior at 800x600 is shown on average 6 ms into the 14 ms refresh instead of at its end.
  Both are guesses at when a frame is complete, and a game may go on drawing: a pointer drawn onto a page after
  flipping to it, sprites drawn onto the screen after the background was copied. So each video mode has to earn
  early frames and keeps being checked (`NN_Verify`): the video memory of every such frame is kept and compared
  at vertical retrace. A page a game flipped to must be unchanged. A copied frame may have been followed by the
  next one (a game that draws faster than the display shows): that is another whole copy, or at least a frame's
  worth of writing (`vga_nn_writes` counts bytes in chained 256-colour modes and banks switched in banked SVGA
  modes, so a frame copied again counts even when it looks the same; where nothing is counted, timing decides).
  A mode starts with frames at retrace and only the checks; early frames begin after 20 checks with the last 16
  clean, stop after 3 failures in 12 (a failed frame is drawn again at retrace), and are tried again after 140
  clean checks, twice as many each time. The host log says when a mode changes over. With two test programs
  that draw late on purpose (a box 2.5 ms after copying a frame, a block 2.5 ms after flipping a page) the
  players got an incomplete frame on every key press before this and on about one in eight after it, the same as
  with `present=retrace`. `retrace` and `scanout` remain as manual settings.
- Hidden remote play screen (capability `inputnetHideScreen`): `-inputnet-hidescreen` or
  `[inputnet] hostscreen = false` on a remote play host keeps its window small (menu bar and a short
  notice, on the surface output), opens it minimized and without the focus, so it never covers the
  window in front (some window managers, labwc for one, put new windows on top whatever they ask), and
  keeps its sound off this machine's speakers, while the viewers still get the full picture and sound.
  Streaming goes on while it is minimized, and a `priority` that pauses unfocused windows is ignored.
  NukemNet uses it for the copies that play for remote players. Restore it from the taskbar for
  *Video > Show screen (remote play)* (mapper event `rpscreen`, no default key), which shows or hides
  the picture at any time.
- Remote play pointer for Windows guests (capability `inputnetPointer`): Windows 3.x/9x with DOSBox-X's pointer
  integration driver (DBOXMPI) asks the integration device where the pointer is, which was the host's own pointer over
  the host's window only: a viewer's mouse never moved the Windows pointer (it sat in the top-left corner). The host
  now answers with the place of the viewer's pointer in its picture (`nn_remote_cursor*` in `bios.cpp`; the host's own
  pointer takes over again when it moves), and tells viewers in each frame header (flag 4) that the guest takes the
  pointer's place. Such a viewer does not capture the mouse: its pointer works over the picture as in any window,
  drawn as a small dot that shows at once where it is going while the guest's own pointer follows in the picture.
  Ctrl+F10 still captures the mouse (relative motion) for games that turn with it. Older viewers and hosts keep
  working together with it as before.
- Disk layers (capability `diskLayer`): `IMGMOUNT C image.img -size ... -nnlayer <file>` opens a raw hard disk image
  for reading only, without the lock that keeps other emulators out, and writes every sector the guest writes to the
  layer file (`NN_LayerAttach` in `bios_disk.cpp`, which documents the format). Several emulators can then run from
  one image at the same time, each with a layer of its own, and the image stays as it was until a launcher writes a
  layer into it (NukemNet does: `app/src/lib/win95-layer.ts`) or throws it away. A sector is stored once, so a layer
  never grows beyond the size of the image; an existing layer of the same image is taken up again; without its
  layer the image is not mounted at all. (The differencing VHDs DOSBox-X already has were no option: `VHDMAKE -link`
  crashes on Linux with a double free and stores parent paths as ASCII.)
- Locked down copies (capability `inputnetLockdown`): `-inputnet-lockdown` is for an emulator that runs a copy of a
  game for a remote player (NukemNet's IPX/NullModem-over-InputNet with a Windows guest), who controls that guest
  completely and could run anything inside it. Such a copy may not change the host's files: `MOUNT` is always
  read-only; a disk image is written only through a disk layer and is read-only otherwise (raw, VHD and QCOW2);
  nothing is captured, printed or saved (screenshots, video, sound, MIDI, parallel and serial port files, printer
  pages, save states; also not when the guest asks through the integration device's capture register); and from the
  moment the guest boots the emulator is in DOSBox-X's secure mode, which cannot be left again: no `MOUNT`,
  `IMGMOUNT` or `BOOT` even after a guest reset, no DOS clipboard access. Checked with a DOS session that copies a
  file to a mounted folder, asks the integration device for captures and writes to an image: with the switch none of
  the three changed anything on the host; a locked Windows 95 copy held only its layer and its log open for writing.
- Remote play scroll prediction: a scrolling picture differs from the one before in nearly every pixel, so its
  difference compressed no better than the frame alone (Fire Fight at 640x480 in Windows 95 sent about 130 KB a
  frame while it scrolled, 3.5 to 4.5 MB/s). When parts of the picture have moved as a whole, the frame now goes out
  as the difference against a prediction: for every block of 8x8 pixels two bits say whether the block is predicted
  from the base as it is or from the base moved by one of up to three distances (layers that scroll at different
  speeds), each at most 24 pixels either way (`RP_FindShift`, `RP_ShiftBlocks`, `RP_ShiftXor` in `remoteplay.cpp`).
  Nothing is lost: the viewer makes the same prediction from the same base (`NN_RP_SHIFTCHECK=1` makes the host check
  every such frame). Used only when every viewer undoes it (viewer feature `RP_FEAT_SHIFT`), so older viewers get
  frames as before. Over a real link the base is not the frame before: the newest frame a viewer is known to have is
  two to four frames back, and the picture has moved further than can be searched. So every frame also notes how far
  its parts moved since the frame before it (`EncFrame::step`); for an older base these steps are added up, part by
  part, and the result is looked for a few pixels around there (`RP_FollowShift`, up to 127 pixels either way).
  Measured with Fire Fight: on one PC 1.2 to 1.5 MB/s at the same 32 frames a second (about a third), encoding 4.7
  instead of 3.2 ms a frame; between two computers 73 ms apart 1.5 to 2.6 MB/s instead of 4.4 to 4.6, prediction in
  79 to 94% of the frames (23 to 50% before the steps were added up). DOS Jazz Jackrabbit: 4 to 9 KB a frame while it
  scrolls. The host's log line says in how many frames it was used, and the viewer logs its picture every 5 seconds
  (frames/s, KB/s, KB a frame).
- Remote play rate follows delay, not only loss: viewers acknowledge each frame as soon as they have decoded it, and
  the host times that from the moment the frame's last fragment left (`Host_DelayFeedback`). The shortest time of the
  last 10 to 20 seconds is the way there and back; a time well above it (twice, and at least 60 ms more) means a
  queue stands somewhere on the way: the buffer of a slow link, or a program that forwards the packets and cannot
  keep up, which loses nothing for a long time and makes the picture seconds late instead. The rate is then set a
  little below what arrives, so the queue empties, and is not raised while frames still come back late. Before,
  only loss cut the rate: on a 700 KB/s link with a 3 MB queue, Shadow Warrior at 800x600 filled the queue until
  it overflowed (13,000 packets dropped in 24 s); now nothing is dropped and frames are acknowledged after about
  45 ms again. The host's log line shows the acknowledgement time, the shortest lately, and "late" while it is cut.
  With both, a Windows 95 game hosted for four players (three 640x480 streams from one computer through NukemNet's
  IPv6 tunnel, 73 ms away) plays at the game's full 30 to 34 frames a second for each viewer with the ping at the
  route's own; before, the tunnel's queue grew until pings passed a second and viewers got a few frames a second.

Packages are published under `https://storage.nukemnet.com/misc/dosbox-x-nn/`, each with an
`nn-capabilities.json` containing `{"win95Ipx":true,"win95FastBoot":true,"inputnetRemote":true,"filemount":true,"inputnetHideScreen":true,"inputnetShellGuard":true,"inputnetMouse":true,"inputnetMotion":true,"filemountAsar":true,"inputnetAdaptive":true,"inputnetPointer":true,"diskLayer":true,"inputnetLockdown":true}`.

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
