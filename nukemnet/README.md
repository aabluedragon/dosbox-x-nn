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
- RollbackNet (`[inputnet] mode = rollback`, capability `rollbackNet`): every player runs the game, and the
  machines exchange only input and stay identical, with each player's own input used at once. Clusters (capability
  `rollbackNetCluster`) run one copy per player on every computer for games that play over IPX or a null modem, and
  players can join a running game and take a free copy over. See [RollbackNet](#rollbacknet) below.
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
- Remote play host input reports (capability `inputnetReportInput`): `[inputnet] report input = <port>` sends every
  remote player's input to 127.0.0.1:<port> as one JSON datagram per event, `{"p":"<player>","t":"key","k":"esc","d":1,"b":0}`
  (b: blocked by `block keys`), `{"t":"button","n":0,"d":1}`, `{"t":"move","dx":..,"dy":..}` (motion gathered ten
  times a second per player). NukemNet's server automation gets them as inputnetInput events.
- NukemNet control channel (capability `nnControl`, any DOSBox-X-NN, remote play or not): env `NN_DOSBOX_CONTROL_PORT`
  and `NN_DOSBOX_CONTROL_TOKEN` (16+ characters) open a UDP port. A datagram from 127.0.0.1 is `<token> <commands>`:
  one command, or several separated by newlines, queued in the order given: `type <text>` (Shift where a character
  needs it), `keys <name> [name...]`, `key <name> down|up`, `wait <ms>` (a pause of up to 10000 ms before what
  follows) and `sync <id>`, which answers the datagram `done <id>` to the sender, from the same port, once everything
  before it has gone out (the id: up to 32 characters, sent back as they came). Key steps go out one every 40 ms;
  anything else is ignored. NukemNet's `nn.typeInGame()` types chat messages into the host's own game with it.
  `block <name> [name...]` (capability `inputnetBlockKeysLive`) replaces `[inputnet] block keys` at once, while the
  game runs (`block none`: no key); NukemNet sends it to the copies that serve viewers when the room's "Blocked keys"
  change during a game.
- Remote play viewers choose how the picture is enlarged (capability `inputnetViewerScaling`): `[inputnet] viewer
  scaling = nearest | bilinear | sharp`, Ctrl+F7 in the viewer switches. Only the viewer does it, with the graphics card,
  after the frame arrived: the host sends the same picture whatever is chosen (no bandwidth, no latency); a software
  renderer keeps nearest. sharp enlarges by whole pixels into a render target, then linearly to the window.
- Who may connect to a remote play host (capability `inputnetAuth`, feature bit 32 in a viewer's HELLO). A host started
  with env `NN_RP_AUTH_SECRET` (16+ characters; NukemNet gives one per room, from the environment because other users
  can read a command line) lets in only viewers whose HELLO carries, after the features, a token (u8 length + text) for
  the name it gives: the first 16 bytes of HMAC-SHA256(secret, name in lower case) in hex (`RP_AuthToken`); a viewer
  reads its own from env `NN_RP_AUTH_TOKEN`. Others get RP_REFUSE ("not invited to this room", or "needs a newer
  NukemNet and DOSBox-X-NN" for a viewer without the bit). One slot per name: the same name from another address
  replaces the old slot, a different name cannot take over an address in use. Such a host sends no video or sound to a
  viewer until it has proved its address: WELCOME carries a cookie (and the host's stream id and the viewer's session),
  the viewer's next HELLO echoes it (u32 after the token). So a HELLO with a forged source address gets one small
  WELCOME back, never a stream. Everything the host sends after WELCOME carries its stream id (u32) right after the
  header; the viewer drops packets without it (a forged BYE, MOVED or frame), and RP_REFUSE ends with the viewer's
  session. A viewer's RP_BYE ends with its session too, which such a host requires. Cookies and the stream id are
  HMACs keyed with the secret (no OS random source needed). Without the secret a host takes anyone, as before.
- Video per viewer (Nov 2026; needs nothing new from viewers, any that decode against named frames). Every viewer has
  its own video rate and queue: loss or late frames on one viewer's link change that viewer's rate only (there was one
  rate and one queue for all, so one slow or lossy viewer took everyone down to its rate). A frame is stored once
  (`VideoPackets`, its datagrams back to back in one buffer) and every viewer takes it at its own pace (`SendJob`). A
  viewer whose queue is full leaves whole frames out; from then on it does not have what the others have, so it goes
  "at its own pace" (`Viewer::straggler`): the others' frames are encoded against a frame they have, as if it was not
  there, and it gets an encoding of its own of the same frame against a frame it has (`Enc_Private`: one difference
  and one compression, no scroll search), at most one a frame and only when it is done before the game shows its next
  picture (ten a second when there is never such time). A frame's content is the same whoever it was encoded for, so
  any frame a viewer decoded can be a base (`Enc_Base`: the newest frame every viewer of a group was sent, has
  decoded by now, and still keeps). A key frame goes to every viewer that takes a frame. A viewer that asks for a key
  frame, and one that joins, gets it for itself: the others get no key frame for it. After a second of taking every
  frame a viewer is with the others again. The host keeps a viewer's base (the newest frame it said it decoded, its
  last key frame) beyond the newest 16 frames. With one viewer (every copy of an IPX/NullModem-over-InputNet game)
  nothing is encoded twice. The host's log line names every viewer's rate and what it was sent ("*": at its own pace).
  Measured on one PC with Duke Nukem 3D (70 pictures/s), a viewer "fast" straight to the host and one behind a
  600 KB/s bottleneck: fast alone 65-70 frames/s, 11-18 ms from input to screen; with the slow one 68-70 frames/s,
  10-13 ms (slow: 13-15 frames/s, what its link carries next to 180 KB/s of sound); with the build before, 18-24
  frames/s and 137-187 ms for fast, 4-8 frames/s for slow. With 5% loss on the slow one's link instead: fast 57-70
  frames/s at 8-12 ms (before: 4-15 frames/s, 200-800 ms). Two slow viewers at different rates and a fast one: the
  fast one as with two viewers without bottlenecks (two interleaved rounds). Sound is not paced: a link below its
  1.4 Mbit/s (16-bit stereo, uncompressed) loses packets whatever the video does.
- Local only (capability `inputnetLocalLinks`): env `NN_RP_LOCAL_ONLY=1` makes the remote play server and the IPX server
  (`IPXNET STARTSERVER`) take datagrams from 127.0.0.1 only: NukemNet sets it on every copy of an IPX/NullModem-over-
  InputNet game (its proxy and the other copies are on the host's computer). The null modem's ENet server now reports
  a client's real address (it reported its own, `::`, so `nonlocal:0` refused even 127.0.0.1), so such a link no longer
  needs `nonlocal:1`.
- Remote play hardening (Nov 2026 audit): the viewer refuses frames larger than 2560x2048 and fragment counts beyond the
  largest such frame (one datagram could make it allocate 77 MB), checks a frame's piece sizes in 64 bits (a 32-bit
  build wrapped around), does not analyse pictures under 16 pixels for view prediction (read past the buffer), reopens
  the sound device at most once in 2 s, and gives up after 90 s without an answer. The host drops non-finite mouse
  motion, Delete while a remote player holds Ctrl and Alt (Ctrl+Alt+Del rebooted the machine), more than one key frame
  request per viewer in 300 ms, and control channel key presses while the guard blocks (automation never types at the
  DOS prompt); it keeps viewer names to 40 printable characters. RP_MOVED may carry a reason (u8 length + text) the
  viewer shows in its title for a few seconds.
- Remote play viewers can be moved (capability `inputnetMove`, feature bit 16 in a viewer's HELLO): the datagram
  `NNRP`, version, type 12 (RP_MOVED) from the address the viewer talks to makes it start over as a new session,
  keeping its window. NukemNet's proxy (IPX/NullModem-over-InputNet: one copy of the game per player) sends it when
  the host gives a player another copy; the proxy tells the old copy the player left (RP_BYE) and routes the new
  session to the new one. A viewer without the bit gets RP_BYE instead and closes.
- Remote play host key blocking (capability `inputnetBlockKeys`): `[inputnet] block keys` lists keys, by keymapper
  name (`esc f10 lalt kp_5`, space or comma separated), that the host drops from every remote player's input; the
  host's own keyboard is not affected and unknown names are logged and skipped. NukemNet's Host Game field "Blocked
  keys" sets it on the host's copies that serve viewers.
- Remote play view prediction is **off by default** (capability `inputnetPredictView`: `[inputnet] predict view`,
  default false; Ctrl+F12 in the viewer switches it; NukemNet's Host Game option "Experimental prediction" turns it on
  for a room's viewers). Without it the viewer shows every frame exactly as the host sent it. Why: a first-person
  view cannot be moved ahead of the host without it showing. Whatever stays on the screen in the game (the weapon, the
  crosshair, a message) either moves with the view and returns with the next frame, or is held in place, and then the
  part of the world it covered has to be made up; both were built, and the player saw each one (Duke Nukem 3D and
  Doom, November 2026: marks between the letters of messages and leftover pixels beside the weapon when held, the
  weapon and crosshair sliding with every turn when moved). Measured with the same input on one PC, 50 ms each way:
  off, the weapon, crosshair and text were out of place in 0 of 195 frames; on, in all 195 (the view is also shown
  2.5% larger). The pointer dot (below) is not affected and stays on. What follows describes the view prediction
  when it is turned on; the viewer learns in the background either way, so the switch works at once.
- Remote play view prediction (capability `inputnetPredict`, viewer side, with `predict view` on, see above): what
  a player feels as lag in a streamed first-person game is the view, which turns one round
  trip and the game's own reaction after the hand. The viewer shows the newest frame already moved by the input that
  frame does not show yet, and draws it again between frames whenever that changes, so the view follows the mouse
  and the turn keys at once and the frames that arrive later land where the picture already is. Nothing about a game
  is configured; the viewer learns from the stream while it is played (`ViewPredict` in `remoteplay.cpp`):
  - how far the picture moved between two frames: the pixels that changed, matched against the frame before, first
    in a small copy of the picture, then to a fraction of a pixel (`VP_Measure`; a status bar, a border and a weapon
    held still do not change and so do not count);
  - which input each frame shows: the host says which input event it had applied when it took the frame, and now
    also when (`RP_FRAMEINFO`);
  - the picture's movement per mouse count and the game's own delay (a regression over delays of 0 to 200 ms);
  - the lens: how much more the sides of a view in perspective move than its middle, from 8 strips of the picture
    (a 2D view moves as one piece), learned only from movement the player's input made;
  - what each held key does to the view by how long it has been held, per combination of Shift, Ctrl and Alt;
  - the view's rectangle (not the status bar, a border or a side panel): the rows and columns that change when the
    view moves, some of it explained by the movement, and where most pixels do not stay. A status bar whose numbers
    change while the player turns, or that a flash of colour changes all over, used to count as view; its numbers
    then slid across their panels (Duke Nukem 3D, November 2026). Nothing is moved or enlarged until the rectangle
    has been measured in the current video mode: until then it is the whole picture, and a viewer that predicted from
    it enlarged the status bar and moved its numbers. Reproduced with Duke's brightness key (F11) pressed while
    turning, which changes every colour as its pickup and damage flashes do: the old viewer took 320x380 of 320x400 as
    the view, status bar included; this one keeps 320x316 (rows 16-331) in a NukemNet IPX-over-InputNet room;
  - the pixels in it that never move with it (a weapon, a crosshair, text on the screen), which are left out of
    measuring (see below);
  - layers: bands of the view that an engine scrolls evenly at a rate of its own (`VP_Bands`). Build's sky does:
    measured in Duke Nukem 3D at 1.24 times the ground's movement in the middle of the view, the same in every
    column, where the lens moves the sides 1.8 times as far. Each of 6 bands is tried both ways against a frame a
    few frames back, and a band that an even scroll explains clearly better is moved at its own rate and left out
    of the view's own measurement; while layers are in view the lens is not learned (low in the view the strips
    mislead) and is taken to be the usual 90 degrees if no other frames have taught it.
  A prediction is shown only while arriving frames keep agreeing with what was predicted for them, so it switches
  itself off in menus, pauses and cutscenes and on again in the game; Escape, Tab and the function keys switch it off
  at once. Only turning and looking are predicted: walking, other players and shots keep the connection's delay.
  Nothing is drawn that the frame does not have, and nothing is resampled. Once the view is known to turn in
  perspective (a first-person game), the viewer shows it a little larger, a fortieth of its width and height cut off
  at each side (the status bar is shown as it is), and moves the picture as one piece, by whole pixels as games move
  it, by at most that margin: a turn uncovers nothing on the screen. The enlargement is done when the picture is
  scaled to the window: by whole pixels first, then to the window with linear filtering, so every pixel is as wide as
  the next and nothing crawls when the picture moves. Filling an uncovered edge was tried three ways (the view
  mirrored there, pictures kept by direction, a smear of the nearest pixel) and moving the sides less than the middle
  once (a stretch that looked wobbly); each was taken out after a player saw it. The price is on fast turns, where the
  view is moved by at most 2.5% of its width: over the Doom stream between two computers 70 ms apart it was 13 pixels
  (of 320) from where the hand had turned it on average, against 19.6 without prediction; slow aiming is followed
  closely (Duke Nukem 3D looking around: 10.8 without prediction, 5.6 with). A turn in perspective moves the sides a
  little further than the middle; that is left to the next frame.
  Everything inside the view moves with it, the weapon, the crosshair and messages too, so every pixel on the screen
  is a pixel of the frame. Small things that stay (a crosshair, the letters of a message) used to be held in place,
  with the pixel beside them drawn where they had been and where their moved copy would land; players saw marks
  between the letters ("AMMO=FOR_SHOTGUN") and took it out (November 2026).
  What stays in place (a weapon, a crosshair, text on the screen) is learned only from pixels of the world: a
  pixel compared with one that stays proves nothing (one arm of a crosshair, moved by its size, looked like the
  world bringing the other; the plain pixels beside it looked as if they stayed). 1-pixel strokes count as a whole,
  holes enclosed by what stays are filled (the plain inside of a weapon or a hand), and a weapon that bobs or fires,
  or a crosshair that changes its shade, keeps counting for two seconds unless the world is seen moving through it.
  Measured (November 2026) with scripted mouse and key input in the viewer. One PC, 50 ms added each way (112 ms
  from input to screen): Doom follows the mouse after 7 ms; the picture was 6 to 9 pixels (of 320) behind the hand
  without prediction and 0.4 to 0.7 off with it; arrow-key turns 23 to 26 behind without, 1 to 3 off with; lens found
  as 90 degrees in a 288-pixel view (f 144 to 146; Doom's is 144), learned within about two seconds of turning. Duke
  Nukem 3D outdoors: 0.50 pixels a count after the game's own 43 to 49 ms, 9 behind without, 1.0 to 1.7 off with;
  two of six bands found to scroll evenly at 1.24. Between two computers 70 ms apart (a Windows host with Doom in an
  IPX-over-InputNet room, 88 to 96 ms from input to screen plus the net game's own 47 ms): the view followed the mouse
  after 7 ms, 0.4 off where it had moved 4 to 7, key turns 2.2 off where it had moved 34; while frames arrived 200 to
  340 ms late for a moment the view still followed the mouse to within 3 pixels. A menu opened with Escape: off at
  once, on again two stretches of frames after the game answers the mouse. Cost in the viewer: about 1 ms a frame to
  learn and 0.5 to 0.8 ms to move the picture at 320x200 (before the edge memory was taken out; see above), 2.5 ms
  a frame to learn at 800x600. The mean difference in brightness between what was on the screen and the frame the
  game later drew for that moment: Doom on one PC 11.9 as received, 6.2 as shown.
  Not verified in a game: looking up and down (measured and moved the same way, tested with synthetic pictures
  only). A game that changes how the mouse turns (sensitivity, a zoom) is learned again after about a second.
  A 2D side-scroller gains nothing: in Jazz Jackrabbit the view is recognised as one that moves as one piece, but
  its scrolling follows the character's momentum, not the keys held, so the predictions keep failing their check
  and the picture is shown as received (prediction was on in 0 to 8% of the stretches).
- Remote play pointer dot for DOS games (viewer, with a host from this release on): a game that draws its own
  pointer (a strategy game, an adventure, a menu) shows it one round trip late like everything else. The host now
  reports with every frame where the DOS mouse driver has its pointer and how far a count of motion moves it
  (`Mouse_NNPointer`, sent in `RP_FRAMEINFO`); the viewer adds the motion the host had not applied yet and draws a
  small dot there at once, as it does over a Windows guest, and the game's pointer follows it. Whether a game shows
  a pointer at the driver's place at all is found out by looking: when the place moves, the picture has to change
  around the old and the new place and hardly anywhere else (`judgePointer`); the dot shows after three such moves.
  It starts from the newest report the host has sent, whichever frame is on the screen (`pointerTold`): a report is
  a datagram of its own, and tied to the frame on the screen every lost one switched the dot off for three moves
  (with 3% of the host's datagrams dropped on one PC the dot was off at 3 of 8 checks that way, and on all of the
  time this way).
  Tested with a DOS test program that draws its own pointer and with Bonus (a mouse-only game), on one PC with
  50 ms added each way and between two computers 70 ms apart (a Windows host in a NukemNet Inputnet room, 77 to
  88 ms from input to screen): the game's pointer was where the driver has it in every move of nearly every
  five-second stretch (19 of 19, 31 of 31, 34 of 34; 21 of 33 in one of them on one PC), the dot led the game's
  arrow, and it was on all of the time once it had come on. In Doom, which turns with the mouse, 1 of 73 moves on
  one PC and none of 51 between the two computers, and no dot. The viewer also checks its own arithmetic: every
  report is compared with where the one before it and the motion sent since said the pointer would be. Between the
  two computers that was the same place (0.00 pixels apart) in 1,253 of 1,254 reports, and 15 pixels apart once,
  when the game moved its pointer by itself at the start; on one PC also with wide sweeps into the edges of the
  screen, where the driver stops its pointer at every step and so does the dot. The viewer's log says "pointer dot
  on/off", for how much of the time it was shown and how far the pointer came to be from it.
- Remote play encoding takes a fifth of the time: where the encoder's time went is now in the host's log
  ("encoding a frame: ..."). For a 320x200 game it was mostly wasted: 2 of 5 ms looking for a scroll in every frame
  of a view that turns in perspective and never has one (now every 8th or 16th frame after four without); 1 ms
  compressing the frame alone as well as its difference to send the smaller (now one of them, chosen by how much of
  the frame is as in its base: over Doom and Duke Nukem 3D streams the difference was smaller every time with three
  tenths or more unchanged, the frame alone nearly every time with less; a link that limits the video still gets
  both tried); and frames from 48 KB on are now compressed in pieces on all threads (128 KB before, just above a
  320x200 picture with its lines doubled). Doom on one Linux PC: 4.8 to 5.2 ms a frame before, 1.4 to 1.7 after, the
  same bytes per second.
- A Windows remote play host is no longer slowed down for not being in front: Windows 11 puts a process whose window
  is minimized or hidden on its slow cores and takes its one-millisecond timers away, which is exactly what a copy
  that plays for a remote player is. The host now asks to be left out (`Host_NoThrottle`:
  `SetProcessInformation(ProcessPowerThrottling)`, looked up at run time). On the test laptop (Core i7-13700H) a
  hidden host with Doom ran at 184,000 to 271,000 cycles and took 5.8 to 7.0 ms to encode a frame before; with this
  and the encoder changes 820,000 to 853,000 cycles and 0.9 to 1.1 ms. Over the real link to that laptop the viewer
  measured 1.0 to 1.5 ms of encoding in the input-to-screen time where it had measured 7.2 to 7.7.
- Remote play sound no longer drifts late: the viewer's queue of sound not yet played grew with every burst of
  packets and only emptied at a quarter of a second (thrown away). Above 70 ms it is now played 3% faster (8% above
  130 ms), by interpolation, until it is back at 45 ms; the viewer's log says how long sound waits on average (over
  the real link: 127 ms after a hiccup, back to 49 ms within seconds).
- FILEMOUNT for files a game writes itself (capability `filemountKeep`): `FILEMOUNT /K C:\DIR\NAME.EXT <host file>`
  maps a file the game creates (a log, a scratch file) to a host path outside the mounted folder, so nothing lands
  in the player's game folder. The host file need not exist yet (its folder must). Unlike a plain mapping, which a
  delete drops, this one stays: when the game deletes the file the host file is removed (emptied when it cannot be
  removed), the game sees no file, and its next create makes the file again at the same host path. A real file of that
  name in the mounted folder stays hidden and untouched throughout. NukemNet maps Quake's `-condebug` console log
  this way (Quake deletes that log at every start) and reads it to know when the host's server is up. A writer's open
  of a mapped file that is not there now answers "file not found" (it answered "access denied").
  Tests: `python3 tests/filemount_keep_tests.py`.

Packages are published under `https://storage.nukemnet.com/misc/dosbox-x-nn/`, each with an
`nn-capabilities.json` containing `{"win95Ipx":true,"win95FastBoot":true,"inputnetRemote":true,"filemount":true,"inputnetHideScreen":true,"inputnetShellGuard":true,"inputnetMouse":true,"inputnetMotion":true,"filemountAsar":true,"inputnetAdaptive":true,"inputnetPointer":true,"diskLayer":true,"inputnetLockdown":true,"inputnetPredict":true,"inputnetPredictView":true}`.

- Linux x64, ARM64 and ARMv7: `nukemnet/linux` (portable Debian Bullseye build with bundled
  libraries), run by the *NukemNet Linux packages* workflow.
- Windows (MinGW x64 and x86, Visual Studio ARM64) and macOS (Intel and Apple silicon): the
  standard workflows in `.github/workflows`, repackaged by NukemNet with the capability file.

## RollbackNet

`[inputnet] mode = rollback` (capability `rollbackNet`; NukemNet's "RollbackNet" Net choice, next to Inputnet). For
the games remote play is for (two or more players on one PC), but every player runs the game, and only input crosses
the network, so nobody waits for a picture: each player's own keys and mouse reach their machine in the next frame,
whatever the distance to the others.

How it works:

- Every machine counts the same frames of `frame` emulated milliseconds (10). A frame starts right after a timer
  tick (`INPUTNET_RollbackTick`, called from `Normal_Loop`). The server's session starts at once; players join it at
  any time (see [Joining](#joining-at-the-dos-prompt-or-a-running-game) below).
- At every frame start the machine is saved: a lean save state of the devices (no RAM, video memory, TLB, renderer,
  MIDI; the PIC queue, the VGA line drawer and the VGA frame clock exactly, without the rounding the save state
  format has) and the RAM and video memory pages that changed since the last frame start (an undo log against a
  copy). Guest RAM pages are found by the host's page protection: RAM is write-protected, the first write to a page
  faults once and marks it (`mprotect` and a SIGSEGV/SIGBUS handler, `VirtualProtect` and a vectored exception
  handler on Windows); pages unchanged for 8 frames are protected again. Video memory is compared whole. A frame
  start costs about 0.3 ms with 64 MB of RAM (it was 1.3 ms with 16 MB, comparing all of it).
- Every frame runs at once with the input that is there; another player's input that has not arrived is predicted
  as "nothing new". When it arrives and had something in it, the machine goes back to that frame start and runs the
  frames again at full speed before going on (nothing of them is drawn or played; with vsync, presenting each would
  wait for the display). The window is `rollback window` frames (16); a machine that runs further ahead than that
  waits. Machines that run ahead of the others drop a tick now and then, so all stay near the same frame.
- Machine checksums (RAM and video memory pages, CPU registers) of final frames (everybody's input there, and no
  wrong guess before them waiting for its correction) are compared all the time. Should they differ, the server saves
  its machine in the save state format at a frame start where its host stack is steady, loads that copy itself and
  sends it; the joining machine loads it at such a frame start too and replays to real time (both stand still).
- `delay` (0 to 8 frames, 0 by default in this mode): how long a player's own input waits before it is used. Each
  frame more makes their own input 10 ms later and the corrections of the others' moves 10 ms smaller.

What has to be the same everywhere, checked when a player joins (refused, naming what differs): the precision of the
timing arithmetic (80-bit `long double` on x86 builds: Linux, macOS Intel and the MinGW Windows builds play together;
ARM builds do not play with them), `cpu core`, `cputype`, `cycles`, `machine`, `memsize`, the Sound Blaster's type,
port, IRQ, DMA and OPL settings and `force goldplay`, GUS, PC speaker, Tandy, Disney, the joystick type and the mixer
rate. Settings that would make machines differ are forced in this mode: the mixer runs at 48000 Hz (a whole number of
samples per millisecond), there is no emulated disk speed (its delay idles in nested emulation loops), cycles never
adjust themselves (`max`/`auto` stay at the speed the session started with: give a fixed value), mouse motion is
applied as relative motion whatever each computer's window does, and the forced Goldplay Sound Blaster stays on its
timer driven DMA. NukemNet passes `core=normal`, fixed cycles, `force goldplay=true` and `joysticktype=none`.

The disk is part of the machine too: every file a game reads has to be the same on every computer. With the state,
the server sends the list of the files on its mounted folders (drive, DOS path, size, hash; FILEMOUNT mappings as the
guest sees them, 8.3 names only). The joining machine asks for every file that differs or is missing and maps what it
gets over its own for the session (FILEMOUNT /K, from a folder in the system's temporary folder, deleted when the
session ends): its own files stay as they are. A file over 16 MB that differs means another version of the game and
the join is refused, naming it.

Host stack: host code runs nested emulation loops (a program's EXEC from the shell, DOS waiting for a key through
`CALLBACK_Idle`, a page fault the normal core runs the guest's handler for, every BIOS call DOS's console makes through
`CALLBACK_RunRealInt`), and their state is on the host's stack, not in a save state. Each run of `DOSBOX_RunMachine`
gets a number (`dosbox_runmachine_context` in `dosbox.cpp`, plus the shell's command line reads): a saved frame can be
loaded only while the same innermost loop runs. A rollback goes back to the newest such frame at or before the first
wrong one (the replay makes the nested loops again); found at a frame start inside a nested loop of a moment (Bonus,
which reads keys and writes the console through DOS, starts a frame inside a BIOS call now and then), the correction
waits for the next frame start. Where no frame back to the oldest one with unknown input can be loaded at two frame
starts in a row (the DOS prompt, DOS waiting for a key), the frames run in lockstep with a longer input delay; input
beyond any rollback is applied late and the machines are made equal again from the server's state.

Other state that had to join the lean snapshots or stop depending on the host: the console device's own state (ANSI
parser, read cache, binary mode), the CMOS clock itself (only its registers were saved), device files in the DOS file
table (by name), open files (kept, with their position; reopening them leaked a host handle per rollback), the VGA's
frame timing (a frame the host's renderer refused, while its window was made again after a mode change, moved the
guest's retrace timing), mouse motion (applied as relative motion), the Sound Blaster with forced Goldplay (stays on
its timer events when a game's DMA turns Goldplay off). Nothing of a replayed frame is drawn, and the whole picture is
drawn again afterwards.

### Clusters: IPX and null modem games (IPX/NullModem-over-RollbackNet)

A game that plays over IPX or a null modem runs one copy per player on every computer, all identical: copy c plays
for participant c (the server's player has copy 0). `[inputnet] cluster = <copies>` (2 to 8), `cluster copy`,
`cluster role = leader|follower` and `cluster port`: the leader is the copy of the computer's own player (its window,
its sound, its network session); the followers have no window (`cluster hide window = true`) and talk to the leader
over 127.0.0.1 on the cluster port. At every frame start each follower sends its frame, its machine checksum and what
its game sent (IPX packets, null modem bytes and the RTS/DTR lines), and waits for the leader's go with what it
receives; the leader routes in copy order (IPX broadcasts to every copy but the sender, unicasts by node address
10.0.0.(c+1); null modem copy 0 to 1 and back, the lines becoming CTS/DSR/CD), so all computers deliver the same
packets at the same frame. A rollback rewinds every copy (`REWIND`), packets included. The checksum the computers
compare covers all copies. `ipx = true` gives each copy an IPX driver with no server behind it; `serialN=rbnullmodem`
a null modem that sends a byte per byte time at the configured speed (NukemNet uses 57600 baud).

### Seats: taking another copy of a running cluster

Every computer keeps the same seats: the participant who plays each copy (participant c plays copy c until a seat
changes). Input goes to a participant's copy by them. The server's control channel (env `NN_DOSBOX_CONTROL_PORT` and
`NN_DOSBOX_CONTROL_TOKEN`, see remoteplay.cpp) takes `rbseat <copy> <player>` (the player's name in the session, `@host`
for the server's own; capability `rollbackNetSeats`): that player plays that copy, and whoever played it gets the first
free copy (the one the player left among them). Seats change only with a resync, which carries them, so every computer
switches at the same frame. A computer whose player gets another copy trades machines between its leader and the
follower that ran that copy (the follower takes the leader's former copy number, `SETCOPY`, each loads the other's
machine, the IPX node follows the copy), so the window, the sound and the local input stay with that computer's player.
After every join, leave and seat change the server reports who plays which copy to a local UDP port (env
`NN_RB_SEAT_REPORT`, `{"seats":["@host","nick",null]}`). NukemNet's Instances window uses both (Take, Release, Move).
Keys sent through the control channel are not typed in RollbackNet (one machine would differ from the others).

### Joining at the DOS prompt or a running game

A joining computer sends HELLO (with the copies it runs and the copy it wants: its player's own, or the first one
nobody plays), gets WELCOME (the server's frame, whether its game runs, the autorun command), then the file lists of
every copy (see the disk, above). At the DOS prompt it waits for the server's machines; when the server's game runs it
types the autorun command into its own copies at once, so they load the game while it waits, and asks for the
machines once they are steady (no input is sent or applied meanwhile). The server then saves every copy at one frame
start (each follower saves and loads its own state back, the leader too, so the server goes on from exactly what it
sends), and sends all of them in one compressed transfer, acknowledged per participant (256 pieces at a time, sent
again after 300 ms, dropped after 30 s without progress), with the list of who joined and who left. Every computer
loads them at a frame start where all its copies are steady and replays to real time. A player who leaves (BYE, or
too far behind) frees their copy for the next one; the others go on. A client keeps waiting for missing input while it still hears the server (which
sends its own input while it waits), up to six timeouts: the server drops the one who is gone and hands the machines over
(with three players, the one who stayed used to time out first and leave too). A joining player in a running game plays the
copy as the game knows it (its name in the game is the one it was started with).

What a save state does not hold and a joining computer has to take from the server (it would keep its own, from its
DOS prompt): the VGA frame clock (set before the load as well, since the load compares the mode's rate with it), how
frames are drawn (render on demand, for the frame being drawn and for the mode), the BIOS's current video mode (INT 10h
`CurMode`), the DMA channels' transfer modes, the mixer channels' rates, the CMOS clock, the DOS date and the time stamp
counter's base. After every load the VGA's memory handlers are set up again and the renderer's frame is dropped. Each
was found by a game at 800x600 (a VESA mode, drawn on demand) that went out of step a few frames after a join, while
320x200 never did: test joins in an SVGA mode too.

For NukemNet: `[inputnet] players` and `autorun` make the server type a command at the DOS prompt once that many
participants are in, as its own input, so every machine starts the game at the same moment (NukemNet's
`C:\NNRBGO.BAT`); at the DOS prompt nobody's key presses reach the machines (they would run a command on every
computer, `DEL` included). A server started with env `NN_RP_AUTH_SECRET` takes only participants whose HELLO
carries the room token for the name they give (env `USERNAME`/`USER` and `NN_RP_AUTH_TOKEN`, as for the remote play
viewer).

Debugging (environment variables): `NN_RB_TRACE=1` logs every frame start (`RBTRACE` with the frame, tick, host
context, PIC ticks, registers, memory hash and machine hash, `RBSB` with the Sound Blaster's state; compare two
participants' logs line by line, the last line of each frame counts); `NN_RB_SELFTEST=<n>` goes back 4 frames every
n frames and checks that every replayed frame start equals the original (it reports the differing component and
bytes); `NN_RB_DUMP=<frame>` writes that frame start's RAM, video memory and devices to the working folder
(`+n`: n frames after the newest load), `NN_RB_FULLDUMP=+n[,+m]` that frame start's whole state as a save state
(`rbfull-<frame>.sav`); `NN_RB_EVLOG=<frames>` logs every PIC event (`RBEV`: its time, the handler's offset from
`PIC_RunQueue`, PIC, Sound Blaster and DMA state) for that many frames after each load (2 by default): diff two
computers' `RBEV` lines to find the first event that differs, and `nm` the handler;
`NN_RB_PROFILE=1` logs each component's size and time once; `NN_RB_NOTRACK=1` compares all of RAM instead of the
write tracking; `NN_RB_FORCE_RESYNC=<frame>` makes the server send its state at that frame; `NN_RB_CTXLOG=1` (Linux)
logs who starts nested emulation loops during a session, away from the DOS prompt (`addr2line -f -e dosbox-x` the
offsets). The statistics line every 5 s also says how many frames ran in lockstep and how often the host context
changed.

Tested November 2026 on Linux between two copies through a delay proxy, with random input on both: Epic Pinball
(Sound Blaster digital sound, keys) with 0 desyncs at 0, 60 and 160 ms round trip (over 6,000 frames each,
rollbacks up to 16 frames deep, replays 3-12 ms on average), and Bonus (FM music, keys and mouse, console output and
key reads through DOS) with 0 desyncs at 20, 40 and 80 ms round trip; the self-test replayed about 450 times per run
without a difference, and a forced resync left both identical. Through two NukemNet instances, the joining one
routed through NukemNet's relay (12-25 ms round trip): Bonus with keys, mouse and clicks from both players, about
80 rollbacks a minute on each side, 0 desyncs, both screens pixel for pixel identical.
Clusters and joins, November 2026, Duke Nukem 3D Atomic Edition at 800x600 with random play (moving, turning, aiming,
firing, jumping): IPX-over-RollbackNet, a server alone with two copies and a second computer joining the running
game through a 60 ms round trip, then 1 to 8 minutes of play: every frame after the join identical on both computers
(traces compared frame by frame: 6,231 frames; NullModem-over-RollbackNet 47,286 frames), 0 desyncs; a 5-minute IPX
run at 320x200 likewise. Through two NukemNet instances: IPX-over-RollbackNet joined mid-game, 7.8 minutes, 45,000
checks, 0 desyncs; NullModem-over-RollbackNet joined mid-game, 3 minutes, 16,600 checks, 0 desyncs; plain RollbackNet
(Bonus) joined mid-game, both screens pixel for pixel identical, the joining player's mouse started the match.
Between this Linux PC and a Windows 11 laptop (the MinGW 64-bit build) through NukemNet's relay, about 67 ms round
trip, each joining a running Duke Nukem 3D match: IPX-over-RollbackNet hosted on Linux with play from both computers,
7 minutes, 41,700 checks, 0 desyncs (up to 32-frame rollbacks on the laptop); hosted on Windows (its machines sent to
the Linux player), 3.9 minutes, 23,300 checks, 0 desyncs; NullModem-over-RollbackNet hosted on Linux, 3.6 minutes,
21,400 checks, 0 desyncs. With the laptop playing too (keys posted to its window): IPX-over-RollbackNet hosted on the
laptop, a Linux player joining the running match, 3 minutes, then the laptop's host took the Linux player's copy (the
Linux player got the host's), 3 more minutes: 36,300 checks, 0 desyncs, 600 to 1,100 rollbacks on each side;
NullModem-over-RollbackNet hosted on the laptop, the same with a take: 35,000 checks, 0 desyncs.
Seats on Linux: a host taking a joining player's copy and releasing it again (each player's keys moved their new copy:
the other screen's ammo stayed), 13,600 checks, 0 desyncs; three copies per computer, a player moved to a free copy and
then the host taking that one, 29,466 frames identical on both computers, 0 desyncs; three NukemNet players, one
killed (the other two went on), the remaining joiner moved to the free copy, 36,000 checks, 0 desyncs.
Three computers (two Linux NukemNet copies and the laptop), Duke Nukem 3D with three copies started for the host alone:
both other players joined the running match into the waiting copies (the laptop's 11 MB took 15 seconds through the relay,
and nobody dropped meanwhile), then all three played: 35,700 checks each, 0 desyncs; the host then took the laptop's copy.
At that seat change the laptop (only it) found its machines different at the change's own frame; the automatic resync
five frames later made them equal, and 13,000 frames of play afterwards had no difference. Not reproduced on Linux or
in any other seat change; the cause is not found yet (a traced run on Windows is next).
Not tested yet: macOS builds, a game with Gravis or MIDI sound, CD images (`IMGMOUNT` files are not in the file list),
more than two computers.

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
