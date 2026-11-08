Resident Evil Code: Veronica X - native port, New Nintendo 3DS
==============================================================

Version 0.1.0.  New 3DS / New 3DS XL / New 2DS XL only.

1. Put your own dump of the US PS2 disc (SLUS-20184) in this folder
   as a .iso (any file name), next to recvx.3dsx.
2. Copy this "recvx" folder into the SD card's "3ds" folder:
       /3ds/recvx/recvx.3dsx
       /3ds/recvx/RE code veronica.iso
3. Start "Resident Evil CVX" from the Homebrew Launcher.

Sound needs the 3DS's DSP firmware dump, /3ds/dspfirm.cdc (run the DSP1
homebrew once to make it).  Without it the game runs silent.

Controls (PS2 -> 3DS):
   D-pad / Circle Pad = D-pad / left stick    C-stick = right stick
   A = Cross (confirm)   B = Circle (back)   Y = Square   X = Triangle
   L = L1   R = R1   ZL = L2   ZR = R2   START   SELECT
   L + R + START + SELECT = quit
   L + tap the bottom screen = write a frame dump to recvx_log.txt

The bottom screen during play: your condition, equipped weapon and ammo
at the top; a radar of the room below, which turns with the camera: the
floor, walls, you (green arrow), enemies (red), doors (blue outline),
items (yellow), item boxes (green) and typewriters (white, once you have
used them).  Footer buttons: Items, Map, Turn (a quick 180).
Settings also has Door scenes: Fast, and Save State / Load State: one
slot, any time you are in control, no typewriter or ink ribbon needed.
Load restarts the room with you where you saved, with your health, items
and progress; the room's enemies start as they do on entering a room.
Kept in savestate.bin here.
Touch the cog (bottom left): settings - frame skip auto/on/off (auto,
the default, works only in cutscenes that run slow, so they stay with
the voices; in play every frame is drawn), 4:3 or widescreen (a wider view of
the scene, not a stretched one), logging
(recvx_log.txt, off by default), and "Check for updates", which
downloads the newest release from GitHub over Wi-Fi and installs it in
this folder (your disc image, saves and settings stay; restart after;
needs cacert.pem in this folder).  Kept in settings.txt here.
   3D slider = stereoscopic 3D in rooms (off at the bottom)

With logging on (settings), the app writes recvx_log.txt in this folder.  Saves go to
the mc0 folder here.

Not done yet: movies (skipped), character shadows, sound reverb.  The full
list of known issues is on the release page:
https://github.com/yaboimanguy-a11y/recvx-3ds/releases

No game data is included.  You supply the disc image.
