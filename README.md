# Smash Melee Vita - experimental PS Vita port

An **experimental native PlayStation Vita port of Super Smash Bros. Melee
(NTSC-U 1.02)**, developed on the
[`vita-port` branch](https://github.com/zm2283145/melee-Vita/tree/vita-port).

**Release version: 0.7.1.** Vita's two-part package metadata displays this as
`00.71`; the GitHub release version is `0.7.1`.

The project builds on [melee-pc](https://github.com/999sian/melee-pc),
[doldecomp/melee](https://github.com/doldecomp/melee), and
[aurora](https://github.com/encounter/aurora). Rather than running a GameCube
emulator, it compiles the game code for Vita and replaces the platform services
with Vita-native input, disc access, audio, saves, movie decoding, and a GX-to-GXM
renderer. The renderer generates shaders using vitaShaRK and the Vita shader
compiler.

**This is not a finished or stable release.** Expect slowdowns, visual issues,
audio/movie issues, and possible crashes. Performance and compatibility vary
between scenes, characters, and stages; a locked frame rate is not promised.
Back up your saves before trying a new build.

## Bugs and issue reports

**There will be bugs, possible crashes, and other unexpected issues.**
Please report them in
[GitHub Issues](https://github.com/zm2283145/melee-Vita/issues) so they can be
investigated. Check for an existing report first, then include the Vita release
or commit ID, your Vita model and firmware, relevant plugins, the game mode,
characters and stage, and clear steps to reproduce what happened. Describe
what you expected and whether the problem repeats after revisiting the scene
with a warmed shader cache. Screenshots or a short video can help.

Release builds deliberately do not collect debug logs. Do not attach your disc
image, game assets, proprietary modules, or personal information to a report.

## Shader compilation and stuttering

**There will be stuttering while shaders compile at runtime.** New characters,
stages, menus, and effects can introduce shaders that have not been seen before.
The port caches compiled shaders in `ux0:data/melee/shadercache/`, so repeated
scenes should become smoother as you play and the cache fills.

The cache persists between launches. Keep it when updating unless a release
specifically asks you to clear it. Deleting it causes shaders to be compiled
again. Cache warm-up reduces compilation-related stutters; it does not fix every
performance issue, and new shaders can still cause pauses later.

## Requirements

- A homebrew-enabled PS Vita with HENkaku/taiHEN and **Enable Unsafe Homebrew**
  enabled, plus [VitaShell](https://github.com/TheOfficialFloW/VitaShell) to
  install VPKs and copy files.
- The Vita runtime shader compiler, **`libshacccg.suprx`**, installed at
  **`ur0:data/libshacccg.suprx`**. This proprietary module is not included in the
  repository or release package; obtain it from your own legally acquired Vita
  software using the appropriate homebrew setup instructions.
- Your own legally obtained, **uncompressed Super Smash Bros. Melee USA
  revision 2 / NTSC-U 1.02 disc image (`GALE01`)**, named `GALE01.iso`.
  Other regions/revisions and compressed RVZ/CISO images are not supported by
  this Vita backend. Renaming a compressed image does not decompress it.
- Writable storage on `ux0:` for the image, installed application, saves, and
  shader cache. A full-size disc image alone is 1,459,978,240 bytes
  (about 1.36 GiB); leave additional free space.

No game disc, game assets, or proprietary shader compiler module is distributed
with this project. You do not need a disc image to compile the port.

The normal Release VPK does **not** require VitaDebugger, its kernel companion,
or kubridge for debugging. It contains no DebugNet logger or GDB server.
Development/debug builds have separate requirements.

## Install and run

1. Download `SmashMeleevita.vpk` from a **published Vita release** in
   [Releases](https://github.com/zm2283145/melee-Vita/releases).
2. Install the VPK using VitaShell. It uses title ID **`MLVITA002`** and currently
   appears as **Smash Melee Vita** in LiveArea.
3. Copy your uncompressed disc image to **`ux0:data/melee/GALE01.iso`** and
   confirm the shader compiler module is at the path above.
4. Launch the LiveArea bubble. There is no PC-style disc picker; the Vita build
   reads the fixed disc path.

This is the **full-game** VPK, not the older `MLVITA001` diagnostic viewer.
Do not install the smoke-test VPK expecting it to run the game.

## Controls

The built-in Vita controls act as GameCube controller port 1.

| PS Vita input | GameCube input / use |
| --- | --- |
| Left stick | Main stick: movement and menu navigation |
| Right stick | C-stick |
| D-pad | GameCube D-pad |
| Cross | A: normal attack / confirm |
| Circle | B: special attack / back |
| Square | X: jump |
| Triangle | Y: jump |
| **L shoulder** | **L trigger: shield** |
| **R shoulder** | **R trigger: shield** |
| **Select** | **Z: grab / Z menu shortcuts** |
| Start | Start / pause |

L and R report full digital trigger presses; there is no variable analog
light-shield pressure. Touch controls, rumble, external controllers, and
multiple human controller ports are not implemented by the current Vita input
backend. CPU opponents use the game's normal controls.

### Hidden Debug Tools

The optimized Release includes a hidden, session-only version of Melee's
original developer tools. In **Options**, enter **Up, Up, Down, Down, Left,
Right, Left, Right, Select** on the D-pad to reveal **DEBUG TOOLS**. It becomes
hidden again when the game process is closed and never writes an unlock setting
to the memory card.

Inside Debug Tools, use the D-pad to navigate/change values, Cross to select,
Circle to return, and Start only where a page explicitly supports running a
test. Destructive save-data, memory-card, arbitrary global-data, and unsupported
GameCube hardware operations remain visible but disabled with an explanation.
The hidden menu does not enable DebugNet, GDB, render tracing, or debug symbols.

## Saves

The port stores its virtual memory card in **`ux0:data/melee/save/`**.
Back up that entire directory with the game closed, especially before updates.
The backend also supports Dolphin `.gci` interchange: imports are read from
`save/import/` at boot, and game saves are exported to `save/export/`.
Do not overwrite or remove files while the game is running.

## Current scope

The Vita code includes native GXM rendering, cached runtime-generated GX/TEV
shaders, Vita controls, direct ISO/FST reads, audio mixing, persistent saves,
and THP movie playback with a hardware JPEG path. These are experimental
implementations, not a claim that every game mode is fully working.

Online play/rollback is not implemented. The desktop launcher/settings overlay,
controller remapping UI, custom soundtrack streaming, and PC cheat toggles are
not available in the Vita build. The inherited
[PC README](README-PC.md) and [roadmap](ROADMAP.md) describe the broader upstream
project, not promises of Vita support.

## Building

See [Vita build instructions](platforms/vita/README.md) for the full toolchain
and dependency setup. The normal build uses **Release** configuration:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release
```

The result is `build-vita/full/SmashMeleevita.vpk`, also distributed by CI as
`SmashMeleevita.vpk`.
Release builds use optimization, disable diagnostic logging and debugger
support, and do not include debug symbols. The dormant hidden Debug Tools UI
does not change those guarantees. Runtime shader compilation and its disk cache
remain enabled; those are needed for normal gameplay.

## Credits and licensing

Thanks to the Melee decompilation and PC-port contributors, aurora, VitaSDK,
vita2d, vitaShaRK, SceShaccCgExt, and the Vita homebrew community.
The updated LiveArea artwork was created by Reddit user
[u/cool_pain_6315](https://www.reddit.com/user/cool_pain_6315/).
This project is not affiliated with Nintendo, HAL Laboratory, or Sony.

Read [LICENSE.md](LICENSE.md) and [COPYING](COPYING) before redistributing.
The original port code and third-party components have their own licenses;
the decompiled game code is not covered by the port's GPL license.
