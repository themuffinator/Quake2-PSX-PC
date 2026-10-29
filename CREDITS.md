# Credits and licence

Q2PSX-PC is a native reimplementation for interoperability and preservation.
The repository and release archives contain no commercial game assets. You must
supply your own legally obtained Quake II PlayStation disc. Quake II is a
trademark of id Software LLC.

The code here is licensed under the **GNU General Public License, version 2** — see
[`LICENSE`](LICENSE). Copyright the Q2PSX-PC contributors.

The fixed-point triangle and line scan conversion is adapted from the Mednafen
Team's [PlayStation GPU implementation](https://github.com/libretro-mirrors/mednafen-git/tree/master/src/psx),
licensed GPLv2 or later. The same reference informed VRAM transfer chunking and
texture-cache indexing/invalidation. CLUT retention is checked against the
published ps1-tests capture, including reuse of an 8-bit palette in 4-bit mode.
Renderer reference fixtures and packet cases use Jakub Czekański's MIT-licensed
[ps1-tests](https://github.com/JaCzekanski/ps1-tests/tree/f727802fead11f1daa7549285548392ef87749cb).
Their provenance, coverage and licence are recorded in
[the reference fixture record](https://github.com/themuffinator/Quake2-PSX-PC/blob/main/tests/fixtures/README.md).

The SPU Gaussian coefficient table comes from the GPLv2-or-later
[PCSX-Redux audio implementation](https://github.com/pcsx-redux/pcsx-redux/blob/26c1c9c8428ae7c0a9cea62d5c4c93faec20e4c8/src/spu/gauss.h).
Decoder, interpolation and envelope checks use the PCSX-Redux authors' MIT-licensed
[Nugget hardware tests and PlayStation captures](https://github.com/pcsx-redux/nugget/tree/4dc70d2931121a0fde858a0354192b6df4fa22e8/tests/spu).
The retained notice is in the [SPU fixture licence](https://github.com/themuffinator/Quake2-PSX-PC/blob/main/tests/fixtures/spu_reference_LICENSE.txt);
the [audio audit](https://github.com/themuffinator/Quake2-PSX-PC/blob/main/docs/AUDIO-AUDIT.md)
records what these references establish.

The CD-XA interpolation coefficients and software reference fixtures use the
Mednafen Team's GPLv2-or-later
[CD controller implementation in Beetle PSX](https://github.com/libretro/beetle-psx-libretro/blob/de6e32cf78f4403766167bfbe83f33074efeb83e/mednafen/psx/cdc.c).
The client mixes its interpolated XA and native SPU voices at 44,100 Hz.
Agreement with this software reference is not a hardware waveform claim.

## Local selector artwork

The optional tank backdrop and title use the original **Quake II PlayStation PAL cover**,
credited to the original game's rights holders, from the
[cover scan archived by MobyGames](https://www.mobygames.com/game/259036/quake-ii/cover/group-20365/cover-49748/).
The local installation loads `launcher/cover.bmp` beside `q2psx.exe`; it expects
that complete PAL cover converted to BMP, preserving its aspect ratio. The UI
crops the tank's head and shoulders above the printed title, then fades that
unaltered artwork into the background. The title is the cover's actual white
wordmark, isolated from the same scan at runtime. `Q2PSX_LAUNCHER_ART` can select another
local file for testing. Missing art leaves a functional metal-themed selector.
Cover art and the font cache remain local user assets, outside Git and release
archives; the code's GPL licence does not license the original artwork.
