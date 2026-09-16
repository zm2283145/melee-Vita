# Vita bring-up

The `melee_vita` diagnostic target validates the VitaSDK compiler, linker,
display, controller input, SELF conversion, VPK packaging, asset decoding, and
the native GXM renderer. Startup also verifies that
the Vita GCC toolchain implements the big-endian `scalar_storage_order`
attribute required by Melee's on-disc structures; failure exits with code 2.

The separate `melee_vita_game` target compiles the original Melee and
sysdolphin sources using the endian-safe disc representation inherited from the
PC port. It is not linked into the diagnostic application yet. Its purpose is
to expose and resolve source portability gaps while Vita-native GX, DVD, VI,
PAD, OS, audio, card, and movie backends are implemented.

Vita builds use four-byte enums to preserve the original ABI. VitaSDK's GCC
does not provide an iconv implementation for `-fexec-charset=CP932`, so the PC
build's automatic UTF-8-to-Shift-JIS literal conversion cannot be reused.
Japanese runtime strings remain a tracked port item and will be converted in a
generated source step before the complete game executable is linked.

## Native game platform layer

`platforms/vita/game` now contains the first native backends used by the real
game target:

- a Vita entry point that calls the original `melee_main`;
- a 24 MiB MEM1 arena with GameCube-compatible arena allocation;
- GameCube-rate clocks, calendar conversion, interrupt state, and alarms;
- the original 32-byte-aligned Dolphin heap and portable matrix/vector math;
- a direct FST/DVD reader for `ux0:data/melee/GALE01.iso`, including deferred
  asynchronous completions; and
- Vita controls exposed through the original PAD API;
- a Vita-vblank-backed VI frame boundary with original retrace callbacks; and
- a native GX front end that records FIFO, draw synchronization, projection,
  viewport, matrix, vertex, texture, lighting, blend, depth, and TEV state for
  translation into GXM draw batches.

All GX symbols currently referenced by the shared Melee/HSD archive are now
provided by the Vita platform layer. Both big-endian GX display lists and the
immediate-mode stream used by shape animation are decoded into canonical
vertices. Quads, triangle lists, strips, and fans are projected with the
game's current matrices, converted to triangle batches, and submitted to the
GXM-backed Vita frame sink. `GXCopyDisp` now closes and presents that frame.

The live path decodes and caches I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8,
CMPR, C4, C8, and C14X2 textures, including TLUT palettes. GX wrap and filter
state is mapped onto Vita samplers, and the first TEV stage selects the texture
used by each batch. Texture-coordinate generation supports position, normal,
and UV sources with regular and post texture matrices. GX depth testing and
writes map to native GXM state, GX face culling is normalized after the
Y-flipped projection, and line/line-strip/point commands use native Vita
primitives. Mutable texture data safely invalidates the cache at a frame
boundary.

Multi-stage TEV state and constants are recorded. The current practical
approximation selects the first active texture stage and folds later constant
color stages into its tint; a dedicated shader generator will eventually be
needed for exact multi-texture and indirect-TEV behavior.

The disc image remains external and is never added to the VPK. The DVD backend
only supports an uncompressed GALE01 revision 1.02 image at this stage.

Default controls are Cross/A, Circle/B, Square/X, Triangle/Y, Select/Z,
Start/Start, left stick/main stick, right stick/C-stick, and L/R/GameCube
triggers. Rumble is deferred until external-controller support is added.

Configure and build with:

```sh
cmake -S . -B build-vita \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-vita
```

Compile the shared game-code port explicitly with:

```sh
cmake --build build-vita --target melee_vita_game
```

Install `build-vita/platforms/vita/melee-vita.vpk`. It checks
`ux0:data/melee/GALE01.iso`, displays the detected ID, revision, size, and a
validation result, then waits for Start to exit. The current archive milestone
loads `PlSsNr.dat`, relocates its complete pointer graph, obtains the
`PlySamus5K_Share_joint` costume-model root, and validates its joint, material,
texture, and polygon descriptor graphs.

Retail disc images are never packaged. Keep the image outside the repository;
the game target will eventually read it from a user-owned path such as
`ux0:data/melee/GALE01.iso`.

## Optional diagnostics

VitaDebugger and profiler integrations are opt-in. Supply their header and
library locations with the `MELEE_VITA_*` CMake cache variables. Defaults point
at `D:/Claude/VitaDebugger` and `D:/Claude/kuBridge`. The default build has no
dependency on them. VitaDebugger also requires a built
`kuBridge/build-local/libkubridge_stub.a`; the profiler archive is already
available at `VitaDebugger/profiler/build/vita/libvitaprofiler.a`.

The debugger must only be entered after networking has been initialized. It is
therefore wired as a build dependency here but will not be started by the smoke
test. The profiler is kept separate so optimized profiling builds do not inherit
debugger overhead.
