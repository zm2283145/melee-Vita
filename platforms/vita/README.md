# Building the Vita port

For installation, controls, runtime requirements, and experimental status, see
the [main Vita README](../../README.md).

## Full-game Release build

`build-full.ps1` is the full-game build entry point on Windows and Linux. It
compiles the Melee/HSD sources, the native Vita platform layer, and the THP
decoder, then creates a SELF and a VPK with LiveArea assets.

Required on the build machine:

- PowerShell **7 or newer** (`pwsh`) and Python 3 (`python` on Windows,
  `python3` on Linux).
- [VitaSDK](https://vitasdk.org/) with `VITASDK` set and its `bin` directory on
  `PATH`. The build needs GCC's big-endian `scalar_storage_order` support;
  the CI SDK uses GCC 15.
- `libvita2d`, taiHEN import stubs, pthread support, and the SDK system stubs.
- [SceShaccCgExt](https://github.com/bythos14/SceShaccCgExt) and
  [vitaShaRK](https://github.com/Rinnegatamante/vitaShaRK), installed in
  VitaSDK. Build/install SceShaccCgExt first, then vitaShaRK.

From the repository root:

```powershell
$env:VITASDK = 'C:\vitasdk'
$env:PATH = "$env:VITASDK\bin;$env:PATH"
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8
```

On Linux, set `VITASDK` and add `$VITASDK/bin` to `PATH`, then run:

```sh
pwsh -NoProfile -File platforms/vita/build-full.ps1 -Configuration Release -Jobs 2
```

The output is **`build-vita/full/SmashMeleevita.vpk`**, with the game title
**Smash Melee Vita** and unchanged title ID `MLVITA002`.
[`version.json`](version.json) sets release version **0.6.0** and Vita's
two-part `APP_VER` value **00.60**. Update both entries for future releases.
Release is the default even if `-Configuration` is omitted. It uses `-O2`,
`NDEBUG`, and `MELEE_VITA_RELEASE`, with no `-g` flags. DebugNet logging,
ordinary OS reports, and the shader diagnostic callback are disabled.
Fatal panics still terminate with an error. VitaDebugger and kubridge are not
linked into the Release executable. The runtime shader compiler and persistent
shader cache are gameplay features and are not disabled.

No game image or proprietary shader compiler module is needed at build time.
Neither belongs in source control, CI artifacts, or the VPK.

## Reproducing CI

The workflow uses an Ubuntu 24.04 host with PowerShell 7, Python 3, GNU Make,
CMake, Ninja, curl, Git, bzip2, and xz. From a clean checkout:

```sh
export VITASDK="$HOME/melee-vitasdk"
bash tools/setup_vita_ci.sh
export PATH="$VITASDK/bin:$PATH"
pwsh -NoProfile -File platforms/vita/build-full.ps1 -Configuration Release -Jobs 2
```

`setup_vita_ci.sh` is a Linux x86-64 setup helper, not a Windows SDK installer.
It refuses to replace an existing SDK. It verifies downloaded SDK/library
archives with pinned SHA-256 hashes and builds shader libraries at pinned Git
revisions. Dependencies are downloaded under ignored `build-vita/deps/`.
If the upstream legacy package assets change, checksum verification fails;
review and update the pinned archive/hash pair together instead of bypassing it.

The [workflow](../../.github/workflows/vita-release.yml) uploads the VPK and
creates a draft only for the `vita-port` branch in `zm2283145/melee-Vita`.
It never publishes a release automatically and does not build the PC targets.
`vita-port` must remain the default branch for the built-in Actions token to
create drafts when the Vita workflows differ from those on other branches.

## Opt-in Debug build

Debug builds are for development only and are **not** used by release CI.
They use `-Og -g3`, enable diagnostic logging, and require a built
[VitaDebugger](https://github.com/zm2283145/VitaDebugger) `libuvdb.a` and
[kubridge](https://github.com/zm2283145/kubridge) import library.
The matching kubridge plugin must be installed on the development Vita.

```powershell
.\platforms\vita\build-full.ps1 -Configuration Debug `
  -BuildDirectory .\build-vita\debug `
  -VitaDebuggerDirectory D:\dev\VitaDebugger `
  -KuBridgeLibrary D:\dev\kubridge\build\libkubridge_stub.a `
  -LogHost 192.168.1.100 `
  -DebugNetPort 18194
```

Add `-EnableDebugger` only when you intend to wait for a GDB connection.
It is rejected for Release builds. DebugNet sends logs to the selected host on
the selected UDP port (18194 by default); GDB uses TCP port 1234. Use a trusted
private LAN only. Specify your own paths and log host rather than relying on
the historical local-machine defaults.

Game archives use separate `Release` and `Debug` subdirectories so configuration
changes cannot accidentally reuse the other build's game objects. For a fully
clean build after toolchain/header changes, use a fresh `-BuildDirectory`.

## Source layout and older diagnostics

`game/` contains the native OS, DVD, PAD, audio, memory-card, VI, GX/GXM,
widescreen, and THP implementations. `gx_render.c` generates and caches
shaders; `game/pad.c` is the authoritative input mapping. Four-byte enums
preserve the original ABI. `sjis_literals.py` converts Japanese string
literals into CP932 escapes because the Windows Vita compiler lacks iconv.

The CMake `melee_vita` target and `build-smoke.ps1` are older diagnostic viewers,
not the full game. Their VPK is `melee-vita.vpk`, title ID `MLVITA001`.
The CMake `melee_vita_game` target builds a game-code archive only. These older
paths retain their bring-up/debugger dependencies and are not used for releases;
use `build-full.ps1` for the playable experimental port.
