# Q2PSX-PC

A native PC recreation of **Quake II for the PlayStation**, based on Hammerhead’s
1999 edition. It reads your own disc and runs the recreated game logic on your PC.
No game data is included.

**[Download a release](https://github.com/themuffinator/Quake2-PSX-PC/releases)** ·
**[User guide](USER-GUIDE.html)** ·
**[Report a problem](https://github.com/themuffinator/Quake2-PSX-PC/issues)**

## Start playing

1. Download the archive for Windows x64, Linux x64 or macOS Apple silicon, then
   extract it completely. Keep the executable and runtime libraries together.
2. On Windows, open `q2psx.exe`. On Linux or macOS, run `./q2psx` from a terminal
   in the extracted folder.
3. Choose your disc in the selector, or use **BROWSE IMAGE** to open its CUE.
   Keep the CUE and every referenced BIN track together. Select **START GAME**.
4. At the title menu, choose **START → SINGLE PLAYER → NEW GAME**, then a
   difficulty. The opening film leads into the first level.

The selector remembers your last disc. Each release includes **USER-GUIDE.html**:
open it in a browser for controls, saves, multiplayer, display options and
troubleshooting. The guide works offline and has a print layout. GitHub displays
its HTML source; download the file or open the copy in a release to read it.

To select an image directly in Windows PowerShell:

```powershell
.\q2psx.exe --disc "D:\Games\Quake II\game.cue"
```

On Linux or macOS:

```sh
./q2psx --disc "/path/to/Quake II/game.cue"
```

## Disc compatibility

The identified releases are **Europe (SLES-01534)** and **USA (SLUS-00757)**.
The original PC and Nintendo 64 versions are not supported.

| Source | Support |
| --- | --- |
| CUE + BIN | Recommended; raw Mode 2 / 2352-byte images preserve music and films. Multi-file and multi-track CUEs are supported. |
| BIN / IMG | Raw images; a matching CUE is used when found. |
| ISO | Data supported; 2048-byte images lack the raw XA sectors needed for music and films. |
| Physical CD | Windows and Linux; raw-sector access is needed for music and films. Drive compatibility varies. |
| CHD, MDS/MDF, PBP | Unsupported. CCD descriptors and SUB sidecars are not parsed; open the associated IMG instead. |

Other disc revisions have not been established as supported. The
[format reference](docs/FORMATS.md) records build identification details.

## Controls at a glance

| Action | Keyboard / mouse |
| --- | --- |
| Move / look | WASD / mouse or arrow keys |
| Fire | Left mouse button, F or left Alt |
| Jump / swim up | Space or right mouse button; hold to swim up |
| Crouch | Hold left Ctrl or C |
| Change weapon | Mouse wheel, Q/E or brackets |
| Pause | Esc |
| Save / load | F7 / F8 |
| Quick save / quick load | F9 / F10 — quick save overwrites slot 1 |

Single-player accepts a connected gamepad automatically. Local multiplayer
supports two to four players in Deathmatch, Team Deathmatch and Versus; keyboard
and mouse control player one, with pads assigned to the remaining players.
`--gamepad-player-one` assigns pads from player one instead. Network play is
not available.

## Presentation and project status

The resizable window adapts the view to its shape by default. Use `--aspect 4:3`
for a traditional view. Optional CRT emulation and bloom are under
**OPTIONS → VIDEO OPTIONS**, and both are off by default.

This is a work in progress. Campaign and local multiplayer systems are
implemented, with remaining differences recorded in the
[functional audit](docs/FUNCTIONAL-AUDIT.md), [renderer audit](docs/RENDERER-AUDIT.md)
and other subsystem audits. Complete hardware or gameplay parity is not claimed.
The software renderer retains affine texture mapping and 15-bit colour;
enlarging the window does not make it perspective-correct.

## Building and contributing

Requires CMake 3.20+, a C11 compiler and SDL3 for the playable client. To fetch
SDL3 when it is not installed:

```sh
cmake -S . -B build -DQ2PSX_FETCH_SDL3=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Without SDL3, CMake builds the core libraries and tools only. Existing Windows
client builds refresh `.install/`, including the HTML guide, executable, runtime
DLL and launch scripts. See [developer workflows](docs/DEVELOPMENT.md) for the
configured MSVC build, headless tests and inspection tools.

- [Architecture](docs/ARCHITECTURE.md), [formats](docs/FORMATS.md) and
  [fidelity targets](docs/FIDELITY.md)
- [Changelog](CHANGELOG.md) and [release process](docs/RELEASING.md)
- [Visual design reference](docs/DESIGN.md) and [editable sources](docs/design/README.md)

## Licence and credits

The source code is licensed under the **GNU General Public License, version 2**;
see [LICENSE](LICENSE) and [credits and source acknowledgements](CREDITS.md).
You must supply your own legally obtained PlayStation disc. Quake II is a trademark
of id Software LLC. Original game artwork, disc data and local font reconstructions
are not included in the repository or release archives.
