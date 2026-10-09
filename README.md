# Smash Melee — experimental PS Vita and PS5 ports

Native ports of **Super Smash Bros. Melee (NTSC-U 1.02)** to the **PlayStation
Vita** and the **PlayStation 5**, developed on the
[`vita-port` branch](https://github.com/zm2283145/melee-Vita/tree/vita-port).

These are not emulators. The game code from
[doldecomp/melee](https://github.com/doldecomp/melee) and
[melee-pc](https://github.com/999sian/melee-pc) is compiled for each console,
and the GameCube platform services are replaced with native input, disc
access, audio, saves, movie playback and a GX renderer.

**Both ports are experimental.** Expect slowdowns, visual or audio issues and
possible crashes. Back up your saves before trying a new build.

| | PS Vita | PS5 |
| --- | --- | --- |
| Status | Releases on GitHub (current: **0.8.15**) | New; builds from CI |
| Renderer | GX → GXM, shaders built with vitaShaRK | GX → OpenGL 4.6 ([ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl), Mesa over AGC) |
| Performance | Varies by scene; not locked | 60 fps menus; matches near 60 |
| Controllers | Built-in controls, 1 player | Up to 4 DualSense pads |
| Resolution | Scaled internal resolution, 960×544 output | Up to 4K render and 4K output |
| Docs | [PS Vita](#ps-vita) below | [platforms/ps5/README.md](platforms/ps5/README.md) |

You need your own legally obtained, **uncompressed** Melee NTSC-U 1.02 disc
image (`GALE01`), named `GALE01.iso`, on either console. Other regions,
revisions and compressed RVZ/CISO images are not supported. No game data or
proprietary console module is distributed with this project, and you do not
need a disc image to compile.

---

## PS Vita

### Requirements

- A homebrew-enabled PS Vita with HENkaku/taiHEN and **Enable Unsafe
  Homebrew** turned on, plus [VitaShell](https://github.com/TheOfficialFloW/VitaShell)
  to install VPKs and copy files.
- The Vita runtime shader compiler **`libshacccg.suprx`** at
  **`ur0:data/libshacccg.suprx`**. It is not included. Get it from your own
  Vita software using the usual homebrew setup guides.
- Your `GALE01.iso`, plus free space on `ux0:` for the image (1,459,978,240
  bytes, about 1.36 GiB), the app, saves and the shader cache.

The Release VPK does **not** need VitaDebugger, its kernel companion or
kubridge, and contains no DebugNet logger or GDB server.

### Install and run

1. Download `SmashMeleevita-<version>.vpk` from a **published release** in
   [Releases](https://github.com/zm2283145/melee-Vita/releases).
2. Install it with VitaShell. It uses title ID **`MLVITA002`** and appears as
   **Smash Melee Vita** in LiveArea.
3. Copy your disc image to **`ux0:data/melee/GALE01.iso`**. Check that the
   shader compiler is in place.
4. Launch the LiveArea bubble. The game reads that fixed path; there is no disc
   picker.

This is the full-game VPK, not the older `MLVITA001` diagnostic viewer.

### Controls

The built-in controls act as GameCube controller port 1.

| PS Vita | GameCube / use |
| --- | --- |
| Left stick | Main stick |
| Right stick | C-stick |
| D-pad | D-pad |
| Cross / Circle | A / B |
| Square / Triangle | X / Y (jump) |
| L / R | L / R triggers (shield; full press only) |
| Select | Z (grab) |
| Start | Start / pause |

Touch controls, rumble, external controllers and more than one human player
are not supported on Vita. Open **Options → Vita Options** to change the menu
and gameplay render resolution or remap buttons. The settings persist.

### Shader compilation and stutter

New characters, stages, menus and effects compile shaders the first time they
appear, which causes stutter. Compiled shaders are cached in
`ux0:data/melee/shadercache/`, so scenes get smoother as you play. Keep the
cache when updating unless release notes say otherwise.

### Saves

The virtual memory card lives in **`ux0:data/melee/save/`**. Back up that
folder with the game closed, especially before updating. Dolphin `.gci` files
in `save/import/` are imported at boot, and game saves are exported to
`save/export/`. Don't change those files while the game is running.

### Hidden Debug Tools

Release builds include a hidden, session-only version of Melee's developer
tools. In **Options**, press **Up, Up, Down, Down, Left, Right, Left, Right,
Select** on the D-pad to reveal **DEBUG TOOLS**. It disappears again when the
game closes and never writes an unlock to the memory card.

Inside, use the D-pad to move and change values, Cross to select and Circle to
go back. Save-data, memory-card, raw memory and unsupported GameCube hardware
options are shown but disabled. To leave a debug match, press Start, then hold
L and R and press Cross. See the
[Debug Tools Guide](platforms/vita/DEBUG-TOOLS.md) for the item spawner, debug
camera, collision overlays and match shortcuts.

### Building

See the [Vita build instructions](platforms/vita/README.md) for the toolchain
and dependencies. The normal build is:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release
```

The output is `build-vita/full/SmashMeleevita.vpk`. Release builds are
optimized and have no logging, debugger support or debug symbols. The
**Vita build and draft release** workflow builds every push to `vita-port`
and drafts a release, with both the Vita VPK and the PS5 build, when the shared version in `platforms/vita/version.json` is bumped.

---

## PS5

A native homebrew folder title for jailbroken PS5 consoles. It has been
tested on firmware 13.60 with kstuff, etaHEN and ShadowMountPlus.

- **What you need:** a PS5 that runs homebrew folder titles, an FTP payload
  (port 2121) and your `GALE01.iso`.
- **Install:** copy the `PPSA99701` folder to `/data/homebrew/PPSA99701`, and
  put your disc image at `/data/homebrew/PPSA99701/GALE01.iso`.
- **Get a build:** download `SmashMeleePS5-<version>.zip` from a published
  release, use a CI artifact from the Actions tab, or build it yourself on
  Linux/WSL. PS5 and Vita releases share one version number.
- **Features:** up to 4 controllers (each signed in to a PS5 user), a **PS5
  Options** menu with button remapping, render resolution up to 4K and video
  output at 1080p/1440p/4K, and the same hidden debug menu. On PS5, Select is
  the left half of the touch pad.

Full requirements, controls, install steps and build instructions are in
**[platforms/ps5/README.md](platforms/ps5/README.md)**.

---

## Bugs and issue reports

Please report problems in
[GitHub Issues](https://github.com/zm2283145/melee-Vita/issues). Check for an
existing report first, then include:

- the console (Vita or PS5), the release or commit ID, and the firmware and
  plugins/payloads you use;
- the game mode, characters and stage;
- clear steps to reproduce, what you expected, and whether it happens again
  once shaders are cached.

Screenshots or a short video help. Never attach your disc image, game assets,
proprietary modules or personal information.

## Scope

Online play and rollback are not implemented on either console. The desktop
launcher, settings overlay, custom soundtrack streaming and PC cheat toggles
are PC-only. The inherited [PC README](README-PC.md) and [roadmap](ROADMAP.md)
describe the upstream PC project, not promises for these ports.

## Credits and licensing

Thanks to the Melee decompilation and melee-pc contributors, aurora, VitaSDK,
vita2d, vitaShaRK, SceShaccCgExt, the Vita homebrew community, and the
ps5-payload-sdk, ps5-native-app-boilerplate and ps5-opengl projects. The
updated LiveArea artwork is by Reddit user
[u/cool_pain_6315](https://www.reddit.com/user/cool_pain_6315/). The PS5
home-screen art and music are original to this port. The Vita VPK includes
five alternate Giga Bowser costume archives from the supplied mod pack.

This project is not affiliated with Nintendo, HAL Laboratory or Sony. Read
[LICENSE.md](LICENSE.md) and [COPYING](COPYING) before redistributing. The
port code and third-party components have their own licenses; the decompiled
game code is not covered by the port's GPL license.
