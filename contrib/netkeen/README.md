# NetKeen display timing repair

NetKeen 2013-03-26 can show duplicate or partly erased sprites and a damaged scorebox
once a match starts, especially while scrolling. Its `VL_WaitDisplay` tries to tell
horizontal blanking from vertical blanking by counting port reads. That depends on
CPU and I/O timing. In a captured DOSBox-X run it repeatedly timed out after three
game ticks and changed pages during active display. The game could then reuse its
old page before the VGA had stopped showing it. Raising CPU cycles or removing I/O
delays changed the symptoms, without reliably fixing them.

The repair changes only `VL_WaitDisplay` / `VW_SetScreen` in the game executable:

1. Wait until an existing vertical retrace has ended.
2. Program the next display address and horizontal panning together, with
   interrupts disabled only for the register writes.
3. Wait for the next vertical retrace, when the new address has been latched,
   before returning the old page to the game for drawing.

This keeps VGA timing and other games' emulation unchanged. Game logic, maps,
controls, the IPX protocol, and network timing code are unchanged. The normal
translucent flashing during player invulnerability remains part of the game.

## Apply or undo

Use Python 3; no assembler or third-party Python packages are needed:

```sh
python3 contrib/netkeen/fix_vsync.py ~/games/netkeen/netkeen.exe
```

The installer accepts only the exact original release with SHA-256
`b395a912f5c2d0b35124a236a7704b93b3f33c472dd7ea8401f0e542c184004a`.
It saves `netkeen.exe.original`, then replaces the executable atomically. Repeating
the command is harmless. Existing backups are never overwritten.

To restore the original:

```sh
python3 contrib/netkeen/fix_vsync.py ~/games/netkeen/netkeen.exe --restore
```

To create a separate repaired copy instead:

```sh
python3 contrib/netkeen/fix_vsync.py ~/games/netkeen/netkeen.exe --output ~/games/netkeen/NKSTABLE.EXE
```

Use the usual game arguments, including `-ext CK5`, `-ext CK6`, `-nodes`, and
`-server`. No new network options are required. For a repeatable local setup,
`machine=svga_s3`, `core=auto`, and `cycles=fixed 20000` work well; the repair also
works with other cycle settings.

The executable and game data are not distributed here. The original release,
including the source (`NETKEEN/src/ID_VL_A.ASM` and `ID_RF.C`), is available in the
[NetKeen 2013-03-26 archive](https://files.shikadi.net/keenwiki/Fangames/f.lemm_et_al.netkeen.2013-03-26.zip).
`vsync.asm` is the readable replacement; the Python installer contains its assembled
bytes, so applying it does not depend on NASM.

## Verification

The tests use temporary copies of a supplied original executable. They check
assembly reproducibility (when NASM is installed), backup and exact restoration,
repeat application, output isolation, and refusing unknown versions or conflicting
backups without modifying them:

```sh
python3 tests/netkeen_vsync_tests.py --executable ~/games/netkeen/netkeen.exe.original -v
```

For visual checks, record the native game frames using DOSBox-X's video capture
hotkey (Host+I; Host is F12 on Linux), enter a local match through the in-game menu,
and scroll, jump, and shoot. Compare the original and repaired executable with the
same CPU and video settings. Check both CK5 and CK6 content; invulnerability
flashing is expected, duplicate sprites and tearing of the scorebox are not.

Checked on the Linux SDL2 build with CK5 and CK6 content, fixed 3,000 and 20,000
cycles and `cycles=max`, including a one-minute CK5 run and a two-peer IPX match
with movement and shooting. At 20,000 cycles the repaired display updates every
two VGA refreshes (35 Hz); the original repeatedly took three refreshes while
its display wait timed out. Native AVI captures showed duplicate player sprites
before the repair and complete sprites afterward.
