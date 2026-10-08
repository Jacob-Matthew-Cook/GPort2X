# GPort2X for PortMaster

GPort2X runs GP2X software on modern Linux handhelds: your own Payback SD
card image, with your own GP2X firmware, read-only. Nothing from the game or
the firmware is included; you supply both.

## Your files

    ports/gport2x/firmware/    your GP2X firmware dump: the root of the GP2X's
                               filesystem, with bin/, lib/, usr/gp2x/ and so on
    ports/gport2x/card/        your Payback SD card image, a single .img file

The card image is never written: settings and saves go to
`ports/gport2x/saves/`. Delete that folder to start fresh.

## Launchers

The **Native** entries run the game's own ARM code directly on the CPU (the
native engine: needs a kernel that runs 32-bit ARM programs, as ROCKNIX's
does) and are the fast ones. The others use GPort2X's interpreter with its
AArch64 JIT, which works everywhere but runs far slower on handhelds.

* **GPort2X - Payback**: starts the game the way the GP2X menu does, through
  the card's `Payback.gpe` script and your firmware's shell. QUIT in the
  game's main menu brings you back to your handheld's menu.
* **GPort2X - GP2X Menu**: boots your firmware's menu with the card mounted;
  start Payback from the Game section. When you QUIT the game here, the GP2X
  menu's Auto Run (on by default) starts it again, as on a GP2X with this
  card; Select + Start held for a second leaves.

## Controls (by button position)

| GP2X | handheld |
|---|---|
| A (left face button) | left face button |
| B (right face button) | right face button |
| X (bottom face button) | bottom face button |
| Y (top face button) | top face button |
| L / R | L1 / R1 |
| START / SELECT | Start / Select |
| stick / d-pad | d-pad or left stick |
| stick click | left stick click |
| VOL- / VOL+ | L2 / R2 |
| quit | Select + Start held for a second (or the guide button) |

## Notes

* `log.txt` in this folder has the last run's log; it starts with the
  device's glibc version (this build needs glibc 2.38 or newer).
* The Native entries run the game at the GP2X's own pace (25 frames per
  second on an RK3566). The JIT entries are slower; on a slow device the
  game lowers its own frame rate, and the game logic keeps time.
* No sound? Payback keeps its own volume in its settings
  (`saves/Payback/Data/Config/Payback.ini`), and L2 (VOL-) lowers it, down to
  silence. Hold R2 (VOL+) in the game for a second or two to raise it; the
  game saves the new level with its settings.
