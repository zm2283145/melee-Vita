# Graphics settings research (Vita)

Research for user-facing graphics options with three presets: Performance
(steady 60 FPS), Default (45-60 FPS) and Quality (sharper, locked 30
FPS), modeled on mkwiita. No code was changed.

## Sources and citation format

| Short name | Source | Revision read |
| --- | --- | --- |
| (none) | this repo, branch `feature/settings-menu` | working tree |
| `mkwiita:` | https://github.com/boomannotjeff/mkwiita | `d3cd85c954bf3819857fc640492fa5f372d4a40b` (2026-10-17) |
| `vita2d:` | https://github.com/xerpi/libvita2d | `a8f15ab`, the revision CI installs (`tools/setup_vita_ci.sh:42`) |
| `headers:` | https://github.com/vitasdk/vita-headers | `e66ebe9` (HEAD when read) |
| `vitaGL:` | https://github.com/Rinnegatamante/vitaGL | `94c75ce` (HEAD; mkwiita installs vitaGL unpinned through vdpm, `mkwiita:tools/vita/build.py:44-49`) |

Citations are `path:line`. Paths without a prefix are in this repo. Statements
marked (inferred) are my reading of the code and were not tested.

## Summary

- mkwiita has the feature the user described. A Settings screen opens from
  a LiveArea button, not in-game. It offers Preset (Default / Quality /
  Performance / Custom), Resolution 1.0-2.0x, Framerate 60/30, Shadows, Bloom,
  Particles and Object detail. Settings are saved in `ux0:data/mkw/quality.txt`
  as `name=value` lines and take effect at the next launch
  (`mkwiita:src/main.cpp:231-320`).
- mkwiita's 30 FPS mode keeps the simulation at 60 Hz and presents every second
  tick, timed by vblank count (`mkwiita:src/main.cpp:1557-1568`, `1826-1831`,
  `2512-2516`). Its Particles toggle has no effect at the revision read: no
  code reads `particleBudget` except the settings code.
- melee-vita already has most of the infrastructure. The Vita Options panel (under Options)
  has live-applied Menu and Gameplay resolution selectors (Native / 75% / 60% /
  50%), saved to `ux0:data/melee/resolution-settings.bin`. Release CI ships menu
  60% and gameplay 75% (`.github/workflows/vita-release.yml:134-138`).
- Melee's frame loop already runs game logic at 60 Hz independently of
  rendering. A 60 Hz alarm queues pad samples, and each outer loop runs one tick
  per queued sample, then renders once (`src/melee/gm/gmscene.c:322-346`). A
  30 FPS lock can wait for two queued ticks before rendering. The game does not
  slow down.
- melee-vita never sets CPU or GPU clocks. mkwiita sets 444/222/222/166 MHz
  (`mkwiita:platform/vita/src/ctru_sys.cpp:29-35`). This is the cheapest knob to
  try first. Its effect has to be measured.
- Particles have one clean choke point. `psDispParticles` already thins any
  particle list above a hard-coded 512 on Vita, using a stable hash
  (`src/sysdolphin/baselib/psdisp.c:1899-1918`, `1939-1950`). Making the limit a
  runtime value is a small change.
- Shadows and MSAA cannot be toggled today. Shadows are disabled on Vita
  (`src/sysdolphin/baselib/shadow.c:189-197`). MSAA is fixed at none in every
  render target and fragment program. Adding it is a large change that only
  takes effect at boot.
- Release builds have no FPS readout. Logging compiles out
  (`platforms/vita/vita_log.h:3-7`). The first hardware checkpoint therefore
  needs a lightweight FPS and frame-time readout before presets can be tuned.

## mkwiita findings

mkwiita ports MKW-3DS, a 3DS recreation of Mario Kart Wii, through a
libctru/citro3d shim on vitaGL. It does not use raw GXM
(`mkwiita:docs/VITA_PORT.md:6-15`).

### Options exposed

The settings rows are defined at `mkwiita:src/main.cpp:255-256` and drawn at
`mkwiita:src/main.cpp:310-320`. The README documents them at
`mkwiita:README.md:118-136`.

| Setting | Values | Field | Where it takes effect |
| --- | --- | --- | --- |
| Preset | DEFAULT / QUALITY / PERFORMANCE / CUSTOM | derived by `matchPreset` | `mkwiita:src/main.cpp:243-248` |
| Resolution | 1.0-2.0 in 0.1 steps, or typed with the IME keyboard | `resolution` | render-target scale `Scale`, `mkwiita:platform/vita/src/c3d_gpu.cpp:48-55`, `862-868` |
| Framerate | 60 / 30 | `framerate` | race ticks per presented frame, `mkwiita:src/main.cpp:1562-1567` |
| Shadows | on/off | `shadowVolumes` | `mkwiita:src/course_objects.cpp:409` |
| Bloom | OFF / LOW / HIGH (0/1/2 blur rounds) | `bloomBlurRounds` | `mkwiita:src/renderer.cpp:1490-1491`, `1586` |
| Particles | FULL / REDUCED (budget 96) | `particleBudget` | no reader found (see below) |
| Object detail | NEAR / NORMAL / FAR (4000/7000/12000) | `objectLowDistance` | LOD swap, e.g. `mkwiita:src/course_object_kinds.cpp:77` |
| Minimap | WII / OUTLINE | `wiiMap` | `mkwiita:src/race_bottom_screen.cpp:182` |

`quality.txt` also accepts keys that the screen does not show:
`reduced`, `minScreenRatio`, `boxSparkleRatio`, `kartEffectScreens` and
`animationStep` (`mkwiita:src/main.cpp:132-143`). `writeVitaQuality` writes
every field back so hand edits survive (`mkwiita:src/main.cpp:164-172`). The
struct and its defaults are in `mkwiita:include/mkw/render_quality.hpp:15-50`.
The Vita defaults keep full detail apart from culling objects smaller than
about 4 px (`mkwiita:src/main.cpp:116-122`).

`grep -rn particleBudget` over the whole mkwiita tree finds only the struct
(`render_quality.hpp:28,49`) and the settings read/write/UI code in `main.cpp`.
The particle allocator `createParticle` has no budget check
(`mkwiita:src/nw4r_ef.cpp:463-474`). At this revision the Particles toggle
changes the saved value and the preset match, and nothing else.

### Presets

```cpp
// mkwiita:src/main.cpp:234-240
struct GraphicsPreset {const char* name;unsigned framerate;bool shadows;unsigned bloom;bool fullParticles;float distance,resolution;};
static constexpr GraphicsPreset GraphicsPresets[]={
    {"DEFAULT",60,true,2,true,7000.f,DefaultResolution},
    {"QUALITY",30,true,2,true,12000.f,2.f},
    {"PERFORMANCE",60,false,0,false,12000.f,DefaultResolution},
};
```

`DefaultResolution` is 1.5 (`mkwiita:src/main.cpp:154`). Changing any single
row makes `matchPreset` return CUSTOM (`mkwiita:src/main.cpp:243-248`). Cycling
Preset overwrites every graphics field (`mkwiita:src/main.cpp:271-275`).

### Storage, UI and when settings apply

- Settings live in `ux0:data/mkw/quality.txt`, one `name = value` per line, parsed
  with `sscanf(" %63[^= ] = %f")` (`mkwiita:src/main.cpp:126-145`). The
  resolution is parsed a second time by the GPU layer before the display starts
  (`mkwiita:platform/vita/src/c3d_gpu.cpp:862-868`).
- The settings UI opens from a LiveArea `liveitem` with target `psla:-settings`
  (`mkwiita:platform/vita/sce_sys/livearea/contents/template.xml:6-11`).
  `launchedForSettings()` checks `sceAppMgrGetAppParam` for "settings"
  (`mkwiita:src/main.cpp:175-178`). When it matches, `runSettings` draws a
  text menu before the game starts (`mkwiita:src/main.cpp:251-320`, called at
  `637`). There is no in-game menu.
- `applyVitaQuality()` runs once before the first race loads
  (`mkwiita:src/main.cpp:1452-1454`). A resolution change saved with "Save and
  Start" relaunches the eboot with `sceAppMgrLoadExec`
  (`mkwiita:src/main.cpp:296-297`). Nothing is applied live.
- Empty marker files switch on debug features: `gpu-trace.txt`, `gpu-sync.txt`,
  `profile.txt` (`mkwiita:platform/vita/src/c3d_gpu.cpp:414-416`, `859-861`),
  `no-shader-cache.txt` (`mkwiita:platform/vita/src/c3d_shader.cpp:335-338`).

### Rendering and pacing

- vitaGL is initialized at 960x544 with `SCE_GXM_MULTISAMPLE_NONE` and
  `vglWaitVblankStart(GL_TRUE)` (`mkwiita:platform/vita/src/c3d_gpu.cpp:870-872`).
  In vitaGL that sets `vsync_interval = 1` (`vitaGL:source/vgl.c:558-560`). The
  display callback then calls `sceDisplaySetFrameBuf(..., NEXTFRAME)` and
  `sceDisplayWaitVblankStartMulti(vsync_interval)`
  (`vitaGL:source/gxm.c:265-270`). There is no MSAA option.
- Render targets are `Scale` times the 3DS canvas. The comment
  says 1.5x gives 636x360 and 2x gives 848x480
  (`mkwiita:platform/vita/src/c3d_gpu.cpp:48-52`). `present()` stretches the
  target to the 960x544 framebuffer with a textured quad
  (`mkwiita:platform/vita/src/c3d_gpu.cpp:807-842`).
- In 30 FPS mode the simulation stays at 60 Hz. `PresentationCadence` presents
  once per slot of N ticks and skips the render in between
  (`mkwiita:include/mkw/presentation_cadence.hpp:5-23`;
  `mkwiita:src/main.cpp:2512-2516`). During races, owed ticks are counted in
  whole display refreshes from `sceDisplayGetVcount`, so the tick clock cannot
  drift against the display (`mkwiita:src/main.cpp:1569-1573`, `1826-1831`;
  `mkwiita:platform/vita/src/c3d_gpu.cpp:1178`). Only races use the 30 FPS
  cadence (`if(fullRace)`, `mkwiita:src/main.cpp:2512`).
- `osSetSpeedupEnable(true)` sets ARM 444, bus 222, GPU 222 and
  GPU crossbar 166 MHz (`mkwiita:platform/vita/src/ctru_sys.cpp:29-35`,
  called at `mkwiita:src/main.cpp:564`).
- For FPS measurement, every 300 presented frames, the GPU layer writes
  `c3dvita: %.1f fps, ... draws/frame ...` to `ux0:data/mkw/vita.log`
  (`mkwiita:platform/vita/src/c3d_gpu.cpp:1229-1236`; log opened at `854`).

## melee-vita pipeline

### Display, render resolution and MSAA

- GXM is set up through vita2d: `vita2d_init_advanced(8 MiB)`, then
  `vita2d_set_vblank_wait(0)` (`platforms/vita/game/gxm_game.c:1111-1117`).
  vita2d triple-buffers and allows two pending swaps
  (`vita2d:libvita2d/source/vita2d.c:28-29`, `241`). Its display callback
  calls `sceDisplaySetFrameBuf(..., SCE_DISPLAY_SETBUF_NEXTFRAME)` and waits
  for vblank only when `vblank_wait` is set
  (`vita2d:libvita2d/source/vita2d.c:157-174`). melee-vita turns that wait off.
  Flips still land on a vblank boundary, so there is no tearing, but nothing
  limits how often frames are queued (inferred).
- Output is always 960x544 (`platforms/vita/game/gxm_resolution.h:7-8`).
  Melee's 640x480 EFB coordinates are mapped onto it. Non-widescreen scenes get
  73:60 pillarbox bars drawn at present time
  (`platforms/vita/game/gxm_game.c:1944-1951`, `2028-2030`).
- There are four internal resolutions: 960x544, 720x408, 576x328
  and 480x272 (`platforms/vita/game/gxm_resolution.h:10-42`). A scaled frame
  renders into a vita2d render-target texture with LINEAR filtering
  (`platforms/vita/game/gxm_game.c:749-787`). `exec_present` then draws that
  texture to the 960x544 back buffer before `vita2d_swap_buffers`
  (`platforms/vita/game/gxm_game.c:1959-2006`).
- There are two classes of frame. A frame is "gameplay" if it recorded a GX copy or
  a shadow pass. Otherwise it is "menu"
  (`platforms/vita/game/gxm_resolution.h:69-75`;
  `platforms/vita/game/gxm_game.c:810-814`). Every `GXCopyTex` sets the copy
  reason (`platforms/vita/game/gx.c:4099-4105`). Shadows are off on Vita, so in
  practice a frame is "gameplay" only when Melee issued a texture copy
  (`src/sysdolphin/baselib/tobj.c:1651`, `src/melee/lb/lbrefract.c:487-491`).
  Open question: do ordinary match frames hit this path? The `[GXM]
  ... scaled-copy=` counters (`platforms/vita/game/gxm_game.c:2133-2146`)
  answer it on hardware.
- Readback, THP video, unsupported copies and the Vita debug/options UI force
  native resolution for that frame
  (`platforms/vita/game/gxm_resolution.h:16-23`;
  `platforms/vita/game/gxm_game.c:995-1001`).
- There is no MSAA anywhere. The display surfaces use `vita2d_init_advanced`
  with `SCE_GXM_MULTISAMPLE_NONE` (`vita2d:libvita2d/source/vita2d.c:712-715`).
  Copy targets use `SCE_GXM_MULTISAMPLE_NONE`
  (`platforms/vita/game/gxm_game.c:1353`). Resolution targets come from
  `vita2d_create_empty_texture_rendertarget`, which hard-codes NONE
  (`vita2d:libvita2d/source/vita2d_texture.c:171`, `195-197`). The GX
  fragment programs are patched for NONE
  (`platforms/vita/game/gx_render.c:1147`, `1165`, `1372`).
- The game thread is pinned to core 0, and a render thread on
  core 1 replays the recorded command list
  (`platforms/vita/game/gxm_game.c:670-671`, `881-905`, `1164-1172`).
  `melee_vita_gxm_present` hands one frame to the render thread and blocks
  until the previous frame is done. That wait is "the only game/render sync
  point" (`platforms/vita/game/gxm_game.c:2009-2100`). It is called from
  `GXCopyDisp` (`platforms/vita/game/gx.c:3948`, `4043`).
- No `scePower*ClockFrequency` call exists in `src/`,
  `platforms/` or `extern/` (grep). The game runs at whatever clocks the
  system gives a normal app.

### Game loop and frame pacing

```c
/* src/melee/gm/gmscene.c:322-346 (abridged) */
while ((pad_queue_count = lb_80019894()) == 0) { ... lb_800195D0(); }
...
for (i = 0; i < pad_queue_count; i++) {   /* one 60 Hz game tick per queued sample */
    ... HSD_GObj_RunProcs(); ...
}
... HSD_StartRender(HSD_RP_SCREEN); HSD_GObj_80390FC0(); ... HSD_VICopyXFBAsync(HSD_RP_SCREEN);
```

- The 60 Hz clock is an `OSSetPeriodicAlarm` with a 1/60 s period that pushes
  raw pad samples (`src/melee/lb/lb_0195.c:68-121`). On Vita, alarms fire from
  `OSRestoreInterrupts`/`OSEnableInterrupts` and from `VIWaitForRetrace`
  (`platforms/vita/game/os.c:281-298`, `368-400`;
  `platforms/vita/game/vi.c:62`). OS time comes from
  `sceKernelGetProcessTimeWide` (`platforms/vita/game/os.c:231-236`).
- A late frame delivers every elapsed period, up to a burst of 8
  (`platforms/vita/game/os.c:381-394`). The pad queue holds 8 samples on Vita
  instead of 5, "8 keeps game speed correct down to 7.5 fps"
  (`src/melee/gm/gmmain.c:44-52`). Logic already catches up instead of slowing
  down.
- When the queue is empty, the wait loop at `gmscene.c:322` polls without
  sleeping (inferred: `lb_800195D0` only pumps DVD/audio/card,
  `src/melee/lb/lb_0195.c:49-58`).
- `VIWaitForRetrace` waits for a vblank only when less than 16 ms have passed
  since the previous call (`platforms/vita/game/vi.c:48-60`). HSD calls it while
  an XFB is not free (`src/sysdolphin/baselib/video.c:187-200`, `341-350`).
- There is no frame cap and no render skip. Frame rate is whatever the
  game+render threads achieve. The README says "Varies by scene; not locked"
  (`README.md:20`).

### Particles

- Melee effects draw particles through `efLib_render_callback`, which calls
  `psDispParticles` per render link (`src/melee/ef/eflib.c:719-743`).
- On Vita, `psDispParticles` counts the particles in a list. Above
  `display_limit = 512` it draws every Nth particle, chosen by a pointer hash so
  the visible set is stable from frame to frame. Simulation is untouched
  (`src/sysdolphin/baselib/psdisp.c:1899-1918`, `1939-1950`). Replacing the
  constant with a global is the choke point for a Particles knob. Lower limits
  only change scenes that exceed them, such as boss deaths and heavy item
  scenes (per the code comment at `psdisp.c:1900-1903`).
- Effects built from model JObjs (`src/melee/ef/efsync.c`, `efasync.c`) go
  through the normal GObj render path. They have no separate choke point.
  Skipping them would change visible gameplay cues, so I don't recommend it.

### Other expensive features

| Feature | Current state | Toggle? |
| --- | --- | --- |
| EFB texture copies (live screens such as the Pokémon Stadium monitor, refraction) | Non-clearing copies of the same target refresh at most every `MELEE_VITA_COPY_INTERVAL` = 4 frames. "Each copy forces the GPU to finish the scene so far (tens of ms)." (`platforms/vita/game/gx.c:4050-4052`, `4161-4180`). Refraction forces every-frame copies (`src/melee/lb/lbrefract.c:485-488`). | Yes. Make the interval a runtime value (for example 8 / 4 / 1). |
| Shadows | Disabled on Vita because the shadow-sampling stage materials broke stage copies (`src/sysdolphin/baselib/shadow.c:189-197`). R4 shadow-map copies are filled white (`platforms/vita/game/gx.c:4142-4155`). | Not until the bug is fixed. |
| Fog | Part of the generated fragment shader key (`platforms/vita/game/gx_render.c:542`, `658-690`). | Low value. A toggle would create new shader variants and new compile stutter. |
| Texture filtering | Taken from game data (`platforms/vita/game/gxm_game.c:623-627`). The upscale uses LINEAR (`781-783`). | No knob needed. |
| Stage/background animation | Runs in logic (`HSD_GObj_RunProcs`, `gmscene.c:390`). | No. It affects game state. |
| CPU/GPU clocks | Never set (grep). | Yes, see the knob table. |
| MSAA | None (see above). | Hard. It applies at boot only and touches vita2d init, every render target and every fragment program. |

## Settings infrastructure

- The Vita Options panel already ships. In Options, the Screen Display slot opens a
  modal SisLib text panel when built with `MELEE_VITA_RUNTIME_RESOLUTION_MENU`
  (`src/melee/mn/mnmain.c:3141-3148`; panel code `812-1438`). Page 0 (Display)
  has Menu Resolution, Gameplay Resolution and Close
  (`src/melee/mn/mnmain.c:1005-1072`). Page 1 (Controls) holds the remapping
  (`src/melee/mn/mnmain.c:1073-1140`). Left/right and the touch buttons change
  a value, then call `melee_vita_gxm_apply_resolution_options()`
  (`src/melee/mn/mnmain.c:1288-1299`, `1420-1435`). That function waits for the
  render thread to go idle, allocates targets and saves
  (`platforms/vita/game/gxm_game.c:1071-1108`). While the panel is open, its
  input handler consumes input (`src/melee/mn/mnmain.c:3102-3106`).
- Settings files follow one convention: fixed binary structs with magic, version, a temp
  file and rename, and a fallback to defaults when the file is malformed.
  `resolution-settings.bin` is at `platforms/vita/game/gxm_game.c:1006-1069`
  and `control-settings.bin` at `platforms/vita/game/pad.c:15-16`.
- The PS5 build shares this menu through `TARGET_PS5` branches
  (`src/melee/mn/mnmain.c:817-844`, `1407-1418`) and its own
  `melee_vita_gxm_present` (`platforms/ps5/gl_game.c:1417`). New rows have to
  compile, or be hidden, on PS5.
- Debug Tools is hidden and lasts for one session only
  (`platforms/vita/DEBUG-TOOLS.md:45-59`). It is not a place for user settings.
- For the pause menu, `generate_pause_exit_prompt.py` only re-draws the pause art
  with Vita button icons (`platforms/vita/generate_pause_exit_prompt.py:1-15`).
  It is not a settings surface.
- The LiveArea template (`platforms/vita/livearea/template.xml:1-9`) has no liveitem. An
  mkwiita-style settings launcher is possible but not needed, because an
  in-game panel already exists and applies resolution live.

Recommendation: add a third Graphics page, or extra rows on the Display
page, to the existing Vita Options panel. Store everything in one versioned
file, for example `ux0:data/melee/graphics-settings.bin`, using the same
magic/version/tmp-rename pattern. Migrate the two values from
`resolution-settings.bin` on first load. A text file like mkwiita's is easier
to edit by hand but breaks the repo's existing convention. That choice is for
the user.

## 30 FPS / frame pacing

**Logic must keep ticking at 60 Hz.** Melee's simulation is per tick (see
the "Deterministic 60 Hz Simulation" tenet, `ROADMAP.md:11`). The loop already
separates ticks from renders: `pad_queue_count` ticks per render
(`src/melee/gm/gmscene.c:346`). Slowing the game is wrong and is not needed.

Two parts, used together:

1. Gate rendering on two ticks. In 30 FPS mode, have the wait at
   `src/melee/gm/gmscene.c:322` loop until `lb_80019894() >= 2` instead of
   `== 0`. Each render then follows exactly two logic ticks unless the frame
   ran late, in which case the existing catch-up applies. Input is still
   sampled at 60 Hz by the alarm (`src/melee/lb/lb_0195.c:61-66`, `118-119`).
   Average display latency rises by about one 60 Hz frame (inferred). This
   is the same model as mkwiita's `PresentationCadence`.
2. Align flips to every second vblank. vita2d only supports a wait of
   0 or 1 vblank, inside a static callback
   (`vita2d:libvita2d/source/vita2d.c:157-174`, `1014-1017`). To get an even
   33.3 ms cadence, register a display callback that does what vitaGL does
   with an interval: `sceDisplaySetFrameBuf(NEXTFRAME)` followed by
   `sceDisplayWaitVblankStartMulti(2)` (`vitaGL:source/gxm.c:265-270`; API at
   `headers:include/psp2/display.h:88-93`). The callback is chosen in
   `SceGxmInitializeParams.displayQueueCallback`
   (`headers:include/psp2/gxm.h:60-66`), which vita2d fills at
   `vita2d:libvita2d/source/vita2d.c:238-245`. The build already uses
   `-Wl,--wrap` for GXM entry points (`platforms/vita/build-full.ps1:446`), so a
   `--wrap=sceGxmInitialize` that swaps the callback, and keeps vita2d's
   `{void* address}` callback data, is one option (inferred, untested).
   With at most two pending swaps (`vita2d.c:29`), the blocking callback backs
   up the render thread, which then backs up the game thread at
   `gxm_game.c:2092-2098` (inferred).

Part 1 alone gives about 30 FPS but can judder. The alarm runs on the process
clock (`os.c:231-236`), not the display clock, so a frame can sometimes cover 1
or 3 vblanks (inferred). mkwiita avoids this in races by counting vblanks
(`mkwiita:src/main.cpp:1826-1831`). Measure the refresh rate with
`sceDisplayGetRefreshRate` (`headers:include/psp2/display.h:46-54`; the header
says it "returns a theoretical value") before deciding whether Part 2 needs a
vcount-based tick source.

Scope decision (open question): mkwiita locks only races. For Melee, locking
every scene is simplest. Locking only in-match frames needs a scene check such
as `gm_804D6720->scene_kind`, which the profiler code already reads
(`src/melee/gm/gmscene.c:442-443`). THP movies already force native resolution
(`gxm_game.c:1901`). Their audio sync at 30 FPS needs a test.

The Performance "steady 60" target needs no new pacing code. It needs the
frame to fit in 16.7 ms. The `vi.c:56` heuristic and `NEXTFRAME` flips already
avoid tearing.

## Knob mapping table

Perf impact is my expectation only. None of it has been measured.

| Knob | Where | Difficulty | Expected impact | Live? |
| --- | --- | --- | --- | --- |
| Menu / gameplay internal resolution | Exists: `gxm_resolution.h:10-42`, `gxm_game.c:1071-1108`, `mnmain.c:1288-1299` | Done; presets only set the two options | GPU fill and bandwidth. Largest GPU-side lever. | Yes |
| Frame cap 60/30 | `gmscene.c:322` (tick gate) plus a display callback with vblank interval 2 (see above) | Medium | Quality: frees about 2x frame time for higher resolution | Gate: yes. Callback interval: runtime variable read by the callback (inferred) |
| CPU/GPU clock boost | New call at boot, e.g. in `melee_vita_platform_init` (`platforms/vita/game/os.c:89-96`), using `scePowerSet{Arm,Bus,Gpu,GpuXbar}ClockFrequency` (`headers:include/psp2/power.h:191-224`) | Easy | Possibly large on both CPU-bound logic and GPU. Battery and heat cost. Check return codes. | Yes (the calls can be made at any time) |
| Particle display limit | `psdisp.c:1906`: replace the constant with a global | Easy | Only in particle-heavy scenes | Yes |
| EFB copy refresh interval | `gx.c:4050-4052`, `4176-4180`: replace the macro with a global | Easy | Stages with live screens; each copy costs "tens of ms" per the comment at `gx.c:4163` | Yes |
| Shadows | `shadow.c:189-197` | Hard (rendering bug first) | Adds cost; Quality-only if ever fixed | n/a |
| MSAA 2x/4x | vita2d init, `gxm_game.c:1353`, `gx_render.c:1147,1165,1372`, resolution targets | Hard | Costs GPU; mainly useful at native 30 FPS | Boot only |
| Fog / filtering / stage animation | see table above | n/a | Not recommended | n/a |

## Proposed presets

The values below are a starting point for hardware tuning, not measured
results.

| Setting | Performance | Default | Quality |
| --- | --- | --- | --- |
| Frame cap | 60 (uncapped, as today) | 60 (uncapped, as today) | 30 (tick gate + 2-vblank flip) |
| Menu resolution | 60% (576x328) | 60% (576x328), current CI default | Native (960x544) |
| Gameplay resolution | 60% (576x328), or 50% if 60 FPS is not held | 75% (720x408), current CI default | Native (960x544) |
| Particle display limit | 256 | 512 (current) | 2048, or no limit |
| EFB copy interval | 8 | 4 (current) | 1 (every frame) |
| Clock boost (444/222/222/166) | On | On only if Phase 1 shows a gain, else Off (current behaviour) | On |
| Custom | Shown when any row differs from the preset, as in `mkwiita:src/main.cpp:243-248` | | |

Default stays equal to today's release build, apart from the clock decision,
so choosing it is never a regression.

## Phased plan with hardware test checkpoints

Each checkpoint means: build a Release VPK
(`.github/workflows/vita-release.yml:134-138` flags), install it on real
hardware and record results. Use the same test scenes every time: title menu,
CSS, Final Destination 1v1, Pokémon Stadium 4-player with items, Fountain of
Dreams, Giga Bowser defeat (particle burst), the opening movie.

**Phase 0: FPS readout.** Release builds log nothing
(`platforms/vita/vita_log.h:3-7`). The only fps numbers today are the
`[PERF] display_fps / game_fps` line (`platforms/vita/game/gx.c:3975`), which
reaches a log only in a Debug build over DebugNet or with `-EnableRenderTrace`
(`platforms/vita/vita_log.c:3-24`), and the network live profiler
(`-EnableLiveProfiler`, `platforms/vita/build-full.ps1:31-34`, `223-235`).
Render trace flushes every line, which may perturb timing. Add a small
release-safe readout instead, either an on-screen counter toggled from the
panel, or a once-per-5-seconds append to `ux0:data/melee/perf.log`. It should
report display fps, game fps (ticks/s), average and max frame ms, and
ticks/frame.
*Checkpoint 0:* baseline numbers for every test scene at the current CI
defaults. Confirm game_fps stays at 60 when display_fps drops.

**Phase 1: settings file, clocks and resolution presets.** Add
`graphics-settings.bin` (versioned, migrated from `resolution-settings.bin`).
Add a clock-boost flag applied at boot and on change. Log the
`scePowerGet*ClockFrequency` results.
*Checkpoint 1:* settings persist across a restart. A malformed or truncated
file falls back to defaults. A clocks-on vs clocks-off A/B per scene gives fps,
frame-time max and battery drain over 15 minutes. Confirm the clock calls
return 0 in a normal (non-kernel) app. Read
`[GXM] ... scaled-copy=` to see which frames use the gameplay resolution.

**Phase 2: 30 FPS lock.** Add the tick gate at `gmscene.c:322` and a
2-vblank display callback, both behind the frame-cap setting.
*Checkpoint 2:* timing and pacing.
- An in-match 3:00 timer against a stopwatch shows no drift.
- Frame-time histogram: share of frames at exactly 33.3 ms, and any 16.7 ms or
  50 ms outliers.
- The ticks/frame average is 2.00.
- Audio and music do not drift, and THP movies stay in sync.
- Menus feel acceptable at 30 FPS. If not, restrict the lock to matches.
- Switching between 60 and 30 live works without a restart.

**Phase 3: particles and EFB copy interval.** Turn both constants into
settings.
*Checkpoint 3:* fps on Giga Bowser defeat, a heavy item match and Pokémon
Stadium at each preset value. Check visuals: no flicker of the live screen at
interval 8 and no visibly missing effects at limit 256.

**Phase 4: Graphics page and presets.** Add Preset / Frame cap / Clock boost /
Particles / Live screens rows, Custom detection and Reset. Keep the PS5 build
compiling.
*Checkpoint 4:* D-pad, stick and touch navigation. Each preset reproduces the
Phase 1-3 numbers. Check the settings file after each change. Check PSTV.
Then run the Quality preset for 30 minutes and record thermals and battery.

## Leads from the vita-port skill

The coordinator passed on notes from a third-party skill
(`~/.claude/skills/vita-port/references/*.md`). I checked each one against
primary sources.

| Lead (skill file) | Status |
| --- | --- |
| mkwiita reads `quality.txt` as `name=value` with keys such as `framerate`, `resolution`, `shadowVolumes`, `bloomBlurRounds`, and uses empty files as debug switches (`api-emulation.md:122-133`, `port-types.md:284`) | **Confirmed.** Full key list and parser are under mkwiita findings. Marker files are confirmed at `c3d_gpu.cpp:414-416,859-861` and `c3d_shader.cpp:335-338`. The particle budget key has no reader. |
| A 30 FPS cap that covers exactly two vblanks with vsync on is steadier than a busy-wait target; VitaKotor2 `loader/frame_pace.c`; PCSX_AwO sleep-then-spin pacer (`runtime.md:336-358`) | **Mechanism confirmed** in the SDK header (`display.h:88-93`) and in vitaGL (`gxm.c:265-270`). **VitaKotor2 and PCSX_AwO were not checked.** Their measured claims are unverified. Applicability to melee-vita is covered in "30 FPS / frame pacing". |
| Common clocks 444/222/222/166 (`runtime.md:312-327`) | **Confirmed** in mkwiita (`ctru_sys.cpp:29-35`). melee-vita sets none. The skill's allowed-value list, the 333 MHz default and the 500 MHz note are **unverified**. The headers document only "frequency in MHz" (`power.h:191-224`). |
| Halo CE Vita overlay (Select+Start) with resolution scale default 75%, frame limit, detail level and dynamic-resolution presets; another port with 100/75/50% scale and MSAA 0/2/4 (`port-types.md:129,135`; `runtime.md:621`) | **Unverified.** Repos not checked. Listed only as precedent. melee-vita already uses the same 960x544/720x408/480x272 steps (`gxm_resolution.h:31-36`). |

## Open questions

1. Which in-match frames count as "gameplay" for the resolution split, given
   that shadows are off and only GX copies set the flag? Measure this on
   hardware (Checkpoint 1).
2. What clocks does the system give melee-vita today, and do the
   `scePowerSet*` calls succeed and help? Measure (Checkpoint 1).
3. Is the panel's display refresh exactly 60 Hz? This decides whether the
   process-clock tick source can produce judder against 2-vblank flips.
4. Should the 30 FPS lock cover menus and movies, or only matches?
5. Is a native-resolution 30 FPS Quality preset GPU-bound or CPU-bound in
   4-player matches? If it is CPU-bound, 30 FPS gives the CPU more headroom
   and MSAA could become worth the work later.
6. Should the settings file be binary (repo convention) or text (mkwiita,
   hand-editable)?
7. Does the existing busy wait at `gmscene.c:322` cost battery? At 30 FPS it
   spins for about half of every frame (inferred). A short
   `sceKernelDelayThread` there may be worth testing.
