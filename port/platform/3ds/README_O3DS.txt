Resident Evil Code: Veronica X - native port, original 3DS build
================================================================

Version 0.1.0, for the original 3DS, 3DS XL and 2DS.  EXPERIMENTAL: expect
slow motion.  On a New 3DS, use the regular release: it runs far better.

1. Put your own dump of the US PS2 disc (SLUS-20184) in this folder as a
   .iso or .bin (any file name), next to recvx_o3ds.3dsx:
       /3ds/recvx/recvx_o3ds.3dsx
       /3ds/recvx/RE code veronica.iso
2. Sound needs the 3DS's DSP firmware dump, /3ds/dspfirm.cdc (run the DSP1
   homebrew once to make it).  Without it the game runs silent.
3. Start "Resident Evil CVX (Old 3DS)" from the Homebrew Launcher.

Differences from the New 3DS build: much less speed, a smaller texture
cache (more pauses and pop-in), no 3D slider, no "Check for updates", and
frame skip on by default.  It shares this folder's saves and settings with
the New 3DS app; if settings.txt already sets frame skip, set it to ON in
Settings (the cog on the bottom screen).

Controls and the bottom screen are the same as the New 3DS build:
   D-pad / Circle Pad = move        C-stick = right stick
   A = Cross   B = Circle (run, back)   Y = Square   X = Triangle (map)
   L = L1   R = R1 (aim)   ZL = L2   ZR = R2   START   SELECT
   L + R + START + SELECT = quit

To report a problem, turn on Logging in Settings and send recvx_log.txt, or
a photo of the crash screen.

No game data is included.  You supply the disc image.
