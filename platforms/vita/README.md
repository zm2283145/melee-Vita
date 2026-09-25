# Building the Vita port

For installation, controls, runtime requirements, and experimental status, see
the [main Vita README](../../README.md).

## Full-game Release build

`build-full.ps1` is the full-game build entry point on Windows and Linux. It
compiles the Melee/HSD sources, the native Vita platform layer, and the THP
decoder, then creates a SELF and a VPK with LiveArea assets.

The VPK bundles five alternate Giga Bowser costume archives from the supplied
mod pack. When the game first needs them, it copies missing archives to
`ux0:data/melee/costumes/`. The normal costume and all other fighter data
continue to load from the user's ISO. Existing files in the costume directory
are kept, so an eboot-only swap can also use a previously installed set.

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
[`version.json`](version.json) sets release version **0.8.13** and Vita's
two-part `APP_VER` value **00.91**. Update both entries for future releases.
The Press Start screen shows `VITA <release> BUILD <run>.<attempt>` in its
bottom-right corner. CI supplies the GitHub Actions run and attempt numbers;
local builds show `BUILD local` unless `-VitaBuildNumber`, or the
`MELEE_VITA_BUILD_NUMBER` environment variable, supplies a value such as
`37.1`. The label is not drawn in menus or gameplay.
Release is the default even if `-Configuration` is omitted. It uses `-O3`,
`NDEBUG`, and `MELEE_VITA_RELEASE`, with no `-g` flags. DebugNet logging,
ordinary OS reports, and the shader diagnostic callback are disabled.
Fatal panics still terminate with an error. VitaDebugger and kubridge are not
linked into the Release executable. The runtime shader compiler and persistent
shader cache are gameplay features and are not disabled.

Hardware test builds can merge shaders learned by the Vita into the built-in
cache before packaging:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8 `
  -WarmCacheVita 10.1.1.93 -WarmCacheFtpPort 1337
```

This downloads `ux0:data/melee/shadercache/warm5.bin`, validates every record,
deduplicates it with `platforms/vita/shadercache/warm5.bin`, and packages the
merged cache. The option fails the build if the requested device cache cannot
be retrieved or validated. It is opt-in so offline and CI builds remain
reproducible and never require a networked Vita. This cache stores compiled
shader programs only; decoded trophy textures and archive data remain
session-local and are still released during scene teardown.

### Opt-in sealed shader cache experiment

Release builds normally compile a missing shader at runtime. A trained-cache
test build can instead seal runtime compilation after the fixed renderer
programs and both writable and packaged warm caches have loaded:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8 `
  -EnableShaderCacheSeal
```

The switch is default-off. Cache hits in the renderer's RAM tables, the
indexed `warm5.bin` caches, and individual `.gxp` files continue to work after
the seal. A genuine miss is rejected and falls back through the existing
renderer path without calling vitaShaRK. `[GXR]` diagnostics are emitted every
300 frames and report RAM, writable/packaged warm-cache and disk hits, runtime
compile attempts/success/time, blocked misses, and renderer fallbacks. Train
and package the cache with sealing disabled before evaluating this mode.
Code with a controlled loading/gameplay boundary can use
`gxr_set_runtime_shader_compilation_enabled(false)` after prewarming and pass
`true` again during a training/loading phase; blocked entries are then retried
rather than treated as permanent compiler failures.

The monolithic warm-cache format is unchanged. At startup each record is
bounds-checked and validated with `sceGxmProgramCheck`, then placed in a
bounded in-memory index. Writable records take deterministic precedence over
packaged records, and the first duplicate within a file wins. Malformed input
is logged with its byte offset and rejected; index-capacity overflow retains
the validated linear-scan fallback for unindexed records.

### Opt-in internal-resolution experiment

`-InternalResolutionScale` accepts `100` (the default), `75`, `60`, or `50`.
Non-native values render the main EFB into an aligned offscreen target and
scale it to the unchanged 960x544 display surface:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8 `
  -InternalResolutionScale 75
```

The 75% setting is 720x408, 60% uses the nearest alignment-safe 576x328
dimensions, and 50% is 480x272. Build-time and runtime guards require widths
aligned to 16 pixels, heights aligned to 8 pixels, and dimensions within
320x240 through 960x544. Presentation, pillarboxing, and the game's 640x480 UI
coordinate mapping remain native.

GX EFB copies/readback-style copies, shadow passes, and THP/YUV frames
automatically force the complete recorded frame back to native resolution
before the render thread begins it. They never return a reduced-resolution
success-shaped result. The periodic `[GXM]` diagnostic reports how many frames
fell back for each reason.

`-EnableScaledShadowFrames` is a separate default-off gameplay experiment that
requires a non-native internal scale. It keeps frames containing the
self-contained offscreen shadow pass at the selected internal resolution.
GX-copy/readback and THP/YUV frames still fall back to native 960x544:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8 `
  -InternalResolutionScale 50 -EnableScaledShadowFrames
```

`-EnableScaledGxCopyFrames` is a further default-off gameplay experiment. It
allows supported `GXCopyTex` frames to remain at the selected internal
resolution, scales partial EFB clears into the internal target, and retains a
native fallback for invalid copy bounds or unavailable copy resources:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8 `
  -InternalResolutionScale 50 -EnableScaledShadowFrames `
  -EnableScaledGxCopyFrames
```

Readback and THP/YUV frames remain native even with both gameplay switches.
Keep these switches separate from `-EnableShaderCacheSeal` when measuring each
experiment.

`-GameplayInternalResolutionScale` optionally selects a second internal target
for frames containing supported GX copies or shadow passes. For example, this
keeps ordinary menu frames at approximately 60% (576x328) while rendering
gameplay-class frames at 75% (720x408):

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8 `
  -InternalResolutionScale 60 -GameplayInternalResolutionScale 75 `
  -EnableScaledShadowFrames -EnableScaledGxCopyFrames
```

The setting is default-off (`0`) and must differ from
`-InternalResolutionScale`. If the second target cannot be created, those
gameplay-class frames fall back to native resolution rather than silently using
the menu target.

`-EnableRuntimeResolutionMenu` adds separate **Menu Resolution** and
**Gameplay Resolution** selectors to the hidden Vita Debug menu and to the
normal **Options -> Vita Options** item, which replaces the GameCube-specific
Screen Display label and description on Vita. Vita Options opens a styled
modal resolution panel over the existing Options screen instead of entering
the deflicker screen. While the panel is open, D-pad input is consumed by it:
up/down selects a row, left/right changes its value, and Cross or Circle closes
it. The underlying Melee menu cannot move until the panel closes.

The modal's second page remaps Cross, Circle, Square, Triangle, L, R, Select,
and Start to the GameCube A, B, X, Y, L, R, Z, and Start actions. L/R switches
pages. During a paused match, hold the mapped L and R buttons and press
Start to quit the match; the pause overlay shows this Vita shortcut.
Where a mode supports retry, Select performs Melee's Z action with the default
mapping, and the pause overlay shows a Select icon.
The Display page renders touchable minus and plus buttons beside each
percentage. On the Controls page, select a Melee action with touch or the
D-pad and Cross, then press the Vita button to assign. All physical buttons,
including Cross and Circle, are captured instead of interpreted as Close
while the prompt is active; the touchscreen Cancel button remains available.
Triangle restores defaults when capture is inactive. Assigning an action
already used by another physical button swaps the two assignments, so every
action remains reachable. The mapping applies live and persists in
`ux0:data/melee/control-settings.bin`; malformed files restore defaults.
Reset to Default Controls and Close are part of D-pad row navigation as well
as dedicated touchscreen targets.

On PlayStation TV, the Controls page also captures DualShock 4 L2, R2, L3,
and R3. L1/R1 remain the default GameCube L/R bindings. Version-1 handheld
control settings are migrated in place; the additional DS4 inputs begin
unbound and can replace any existing action through the same capture prompt.
Select+Start cancels capture on PSTV when no touchscreen is available. The
left stick navigates Vita Options as it does other menus; the right stick
remains Melee's C-stick.

The Vita Options overlay uses a 936x520 full-screen-style panel patterned after
the Vita save-editor layout: large Display and Controls tabs, explicit touch
buttons, a full-width mapping list, Reset to Default Controls, and Close.
It remains a SisLib/GXM overlay so it does not introduce a competing VitaGL
rendering context.

Each selector offers Native, 75% (720x408), 60% (576x328), and 50%
(480x272). Changes are applied live after the render thread becomes idle; a
game restart is not required. Targets are allocated on first use and retained
in a four-entry bounded table. If allocation or sync-object creation fails,
the previous setting remains active and the failure is logged. Valid
selections persist in `ux0:data/melee/resolution-settings.bin`; malformed
settings are rejected in favor of the build defaults.

The runtime menu is also default-off and requires both scaled-shadow and
scaled-GX-copy support:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -Jobs 8 `
  -InternalResolutionScale 60 -GameplayInternalResolutionScale 75 `
  -EnableScaledShadowFrames -EnableScaledGxCopyFrames `
  -EnableRuntimeResolutionMenu
```

Readback, THP/YUV, native movie-overlay, and unsupported GX-copy frames
continue to force native 960x544 regardless of the live selections. Native
movies therefore avoid the extra EFB scale pass that can make video fall behind
audio. The Vita Debug menu also renders at native resolution so its late
DevText overlay cannot be hidden by the EFB presentation pass; the selected
scales resume when the debug route closes.

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

## Opt-in VitaShell-style self-updater

See [UPDATER.md](UPDATER.md) for the feasibility result, security model,
release manifest, rollback behavior, and unresolved hardware questions. A
downloaded non-installed process cannot survive Melee exiting, so the opt-in
proof of concept embeds and temporarily promotes a dedicated helper title,
`MLVUPD001`. The updated game removes the helper only after a 300-frame health
acknowledgement.

Updater builds require `libsodium`, `curl-mbedtls`, and `libarchive`, plus a
real Ed25519 release public key:

```powershell
vdpm install libsodium curl-mbedtls libarchive
.\platforms\vita\build-full.ps1 -Configuration Release -EnableUpdater `
  -UpdaterPublicKeyHex '<64 hex characters>'
```

This path uses VitaShell's unsafe auth ID and internal PAF/promoter APIs. It is
intentionally disabled by default and must be tested on a disposable
HENkaku/taiHEN setup before distribution.

Updater-capable builds also include the Homebrew Update client. After network
initialization, the built-in updater queries the upcoming shell plugin for
title ID `MLVITA002`. Only `HOMEBREW_UPDATE_READY` disables built-in update
discovery; an absent or disabled service, hook errors, malformed responses,
and timeouts retain the existing built-in updater behavior.
Every VPK includes `sce_sys/homebrew_update.ini`, which points the plugin to
the stable `MLVITA002-ver.xml` asset on the latest GitHub release. The release
workflow generates that feed and `MLVITA002-changeinfo.xml` from the final VPK,
its APP_VER, and the current release-notes section.

### Native LiveArea updates with Homebrew Update

Installing and enabling
[VitaHomebrewUpdate](https://github.com/zm2283145/VitaHomebrewUpdate) lets
Melee updates appear through the native LiveArea workflow on its currently
validated retail firmware 3.65 scope. The plugin discovers Melee's packaged
`sce_sys/homebrew_update.ini`, downloads and verifies the release VPK, shows
the update's notification states, and stages it. Activating the notification
or starting Melee installs the staged update and then launches the updated
game.

**Before installing, read the VitaHomebrewUpdate repository's prominent
VitaDB daemon incompatibility warning. Do not enable the two daemon plugins
together unless that warning says the incompatibility has been resolved.**

If the Homebrew Update service is missing, disabled, or not ready, Melee
automatically keeps using its built-in updater.

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
the selected UDP port (18197 by default); GDB uses TCP port 1234. Use a trusted
private LAN only. Specify your own paths and log host rather than relying on
the historical local-machine defaults.

Game archives use separate `Release` and `Debug` subdirectories so configuration
changes cannot accidentally reuse the other build's game objects. For a fully
clean build after toolchain/header changes, use a fresh `-BuildDirectory`.

### Hidden developer tools

The optimized normal Vita build contains a modernized presentation of Melee's
original developer scenes. It remains dormant at boot. In the normal Melee
**Options** screen, enter **Up, Up, Down, Down, Left, Right, Left, Right,
Select** on the D-pad to reveal **DEBUG TOOLS** for the current process.

The unlock is never written to the virtual memory card or another file and is
cleared by fully closing the game. Entering the hidden menu temporarily enables
the original DebugRom state for developer-menu routes; returning to the normal
title or menu restores Master behavior. The old title-screen Cross/Square/
Triangle shortcuts are not enabled in normal Release builds.

The menu retains the original descriptors and safe scene callbacks while using
an opaque backdrop, one fixed-position active page, a visible selection marker,
breadcrumbs, selected-entry help, and functional names. Use D-pad Up/Down to
navigate, D-pad Left/Right to change values, Cross to select, Circle to return,
and Start only where a page supports running a configured test.
Camera Mode uses the Vita controls instead of the original controller-port-4
input. To leave a running debug match, press Start, then hold L and R and press
Cross for Melee's DebugRom no-contest shortcut.

Memory-card formatting/deletion/snapshot tools, saved unlock/record mutations,
language/publicity changes, arbitrary global-data editing, and unsupported
GameCube hardware operations remain visible but disabled before their
initialization or callbacks can run.

See [DEBUG-TOOLS.md](DEBUG-TOOLS.md) for complete Vita menu workflows,
in-match shortcuts, item spawning, debug-camera controls, and limitations.

The legacy `-EnableDebugMenu` switch remains available only as an explicit
developer bypass:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -EnableDebugMenu
```

That non-default variant forces DebugRom at boot and restores the historical
title-screen developer shortcuts. It is not used by release CI.
`-EnableModernDebugMenu` is accepted as a deprecated compatibility no-op
because the modern presentation is now always compiled for Vita.

For direct Snag the Trophies renderer testing without navigating the legacy
debug-menu overlay, add `-EnableDirectSnag`. Triangle on the title screen then
opens Classic character select with route stage 6 selected, so starting the
route enters Snag the Trophies with normal Classic initialization:

```powershell
.\platforms\vita\build-full.ps1 -Configuration Release -EnableDebugMenu -EnableDirectSnag
```

Debug matches load `DbCo.dat` and other original debug-scene resources from the
game filesystem. The port cannot supply missing proprietary assets. Debug Tools
remains intended for development/navigation testing even though dangerous
operations are blocked.

For targeted renderer diagnosis without VitaDebugger, add
`-EnableRenderTrace`. This keeps Release optimization and writes unique GX
draw-state signatures to `ux0:data/melee/render-trace.log`. It is disabled by
default; use it only for a controlled capture because each line is flushed to
storage immediately.

`-UseCpuVertexPath` disables the cached display-list vertex shader path for an
A/B rendering diagnostic while retaining the same texture and TEV backend.
This option is also disabled by default and is not intended for release use.

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
