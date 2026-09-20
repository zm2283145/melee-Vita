include("${VITASDK}/share/vita.cmake")

set(VITA_APP_NAME "Melee Vita Bring-up")
set(VITA_TITLE_ID "MLVITA001")
set(VITA_VERSION "00.01")

option(MELEE_VITA_DEBUGGER "Enable the optional VitaDebugger integration" OFF)
option(MELEE_VITA_PROFILER "Enable the optional Vita profiler integration" OFF)
set(MELEE_VITA_DEBUGGER_DIR "D:/Claude/VitaDebugger" CACHE PATH "VitaDebugger checkout")
set(MELEE_VITA_KUBRIDGE_DIR "D:/Claude/kuBridge" CACHE PATH "kuBridge checkout")
set(MELEE_VITA_PROFILER_LIBRARY
    "${MELEE_VITA_DEBUGGER_DIR}/profiler/build/vita/libvitaprofiler.a"
    CACHE FILEPATH "Vita profiler library archive")

# Compile the original Melee and HSD sources as a separate archive while the
# native Vita implementations of the Dolphin APIs are brought online.  This is
# deliberately not linked into the diagnostic viewer yet: building the target
# gives us a precise compatibility checklist without regressing the known-good
# renderer used to validate assets and GXM behavior.
file(GLOB_RECURSE MELEE_VITA_GAME_SOURCES CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/src/melee/*.c"
    "${PROJECT_SOURCE_DIR}/src/sysdolphin/*.c"
)
list(FILTER MELEE_VITA_GAME_SOURCES EXCLUDE REGEX "baselib/debugconsole_main\\.c$")
list(APPEND MELEE_VITA_GAME_SOURCES "${PROJECT_SOURCE_DIR}/src/pc/vtxarray.c")

add_library(melee_vita_game STATIC EXCLUDE_FROM_ALL ${MELEE_VITA_GAME_SOURCES})
target_include_directories(melee_vita_game PUBLIC
    "${PROJECT_SOURCE_DIR}/extern/aurora/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/sdk_include"
    "${CMAKE_CURRENT_SOURCE_DIR}/game"
)
target_compile_features(melee_vita_game PRIVATE c_std_11)
target_compile_definitions(melee_vita_game PUBLIC
    TARGET_PC=1
    TARGET_VITA=1
    MELEE_PC=1
    MELEE_VITA_MODERN_DEBUG_MENU=1
)
target_compile_options(melee_vita_game PRIVATE
    -Wno-all
    -Wno-extra
    -Werror=int-conversion
    -Werror=implicit-function-declaration
    -Werror=incompatible-pointer-types
    -fno-strict-aliasing
    # ARM EABI otherwise stores small enums in one byte. Melee's runtime and
    # on-disc layouts require the GameCube/PC four-byte enum ABI.
    -fno-short-enums
    # The decomp uses plain C `inline` helpers in implementation files. GCC's
    # GNU inline linkage matches the original compiler and emits fallbacks at
    # debug optimization levels.
    -fgnu89-inline
    -fwrapv
    -ffp-contract=off
    -Wno-scalar-storage-order
    -include "${CMAKE_CURRENT_SOURCE_DIR}/vita_compat.h"
)

add_library(melee_vita_platform STATIC EXCLUDE_FROM_ALL
    "${PROJECT_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtx.c"
    "${PROJECT_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtx44.c"
    "${PROJECT_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtxstack.c"
    "${PROJECT_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/mtxvec.c"
    "${PROJECT_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/quat.c"
    "${PROJECT_SOURCE_DIR}/extern/aurora/lib/dolphin/mtx/vec.c"
    "${PROJECT_SOURCE_DIR}/src/pc/misc.c"
    texture_decoder.c
    vita_log.c
    game/card.c
    game/compat.c
    game/dvd.c
    game/audio.c
    game/gx.c
    game/gxm_game.c
    game/heap.c
    game/main.c
    game/jpeg_hw.c
    game/opening_audio.c
    game/opening_movie.c
    game/os.c
    game/pad.c
    game/thp.c
    game/vi.c
    game/widescreen.c
)
target_include_directories(melee_vita_platform PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/game"
    "${PROJECT_SOURCE_DIR}/extern/aurora/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/src/sdk_include"
    "${MELEE_VITA_DEBUGGER_DIR}"
)
target_compile_features(melee_vita_platform PRIVATE c_std_11)
target_compile_definitions(melee_vita_platform PUBLIC
    TARGET_PC=1
    TARGET_VITA=1
    MELEE_PC=1
)
target_compile_options(melee_vita_platform PRIVATE
    -Wall
    -Wextra
    -Werror
    -Wno-parentheses
    -fno-short-enums
    -include "${CMAKE_CURRENT_SOURCE_DIR}/vita_compat.h"
)
set_source_files_properties(vita_log.c PROPERTIES COMPILE_OPTIONS "-fshort-enums")
target_link_libraries(melee_vita_platform PUBLIC vitadebugger_logging)
target_link_libraries(melee_vita_platform PUBLIC turbojpeg)

add_executable(melee_vita
    abi_probe.c
    bringup_probe.c
    disc_probe.c
    figatree.c
    gx_decoder.c
    gxm_probe.c
    runtime_symbols.c
    smoke_main.c
    texture_decoder.c
    texture_atlas.c
    vita_log.c
    "${VITASDK}/share/gcc-arm-vita-eabi/samples/common/debugScreen.c"
)

target_include_directories(melee_vita PRIVATE
    "${VITASDK}/share/gcc-arm-vita-eabi/samples/common"
)

target_compile_options(melee_vita PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_link_libraries(melee_vita PRIVATE
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    SceAppMgr_stub
    SceCommonDialog_stub
    SceKernelThreadMgr_stub
    SceLibKernel_stub
    SceProcessmgr_stub
    SceNet_stub
    SceNetCtl_stub
    SceNetPs_stub
    SceSysmodule_stub
    SceSysmem_stub
    pthread
    vita2d
    m
)

add_library(vitadebugger_logging STATIC IMPORTED GLOBAL)
set_target_properties(vitadebugger_logging PROPERTIES
    IMPORTED_LOCATION "${MELEE_VITA_DEBUGGER_DIR}/libuvdb.a"
    INTERFACE_INCLUDE_DIRECTORIES "${MELEE_VITA_DEBUGGER_DIR}"
)
target_link_libraries(melee_vita PRIVATE vitadebugger_logging)

if (MELEE_VITA_DEBUGGER)
    if (NOT EXISTS "${MELEE_VITA_DEBUGGER_DIR}/uvdb.h" OR
        NOT EXISTS "${MELEE_VITA_DEBUGGER_DIR}/libuvdb.a")
        message(FATAL_ERROR "VitaDebugger headers or libuvdb.a were not found")
    endif ()
    if (NOT EXISTS "${MELEE_VITA_KUBRIDGE_DIR}/build-local/libkubridge_stub.a")
        message(FATAL_ERROR "VitaDebugger requires a built kuBridge stub archive")
    endif ()
    add_library(vitadebugger STATIC IMPORTED GLOBAL)
    set_target_properties(vitadebugger PROPERTIES
        IMPORTED_LOCATION "${MELEE_VITA_DEBUGGER_DIR}/libuvdb.a"
        INTERFACE_INCLUDE_DIRECTORIES "${MELEE_VITA_DEBUGGER_DIR};${MELEE_VITA_KUBRIDGE_DIR}"
    )
    add_library(kubridge_stub STATIC IMPORTED GLOBAL)
    set_target_properties(kubridge_stub PROPERTIES
        IMPORTED_LOCATION "${MELEE_VITA_KUBRIDGE_DIR}/build-local/libkubridge_stub.a"
    )
    target_compile_definitions(melee_vita PRIVATE MELEE_VITA_DEBUGGER=1)
    target_compile_options(melee_vita PRIVATE -Og -g3)
    target_link_libraries(melee_vita PRIVATE
        vitadebugger
        kubridge_stub
        SceNet_stub
        SceNetPs_stub
        pthread
    )
endif ()

if (MELEE_VITA_PROFILER)
    if (NOT EXISTS "${MELEE_VITA_DEBUGGER_DIR}/profiler/include/vitaprofiler.h" OR
        NOT EXISTS "${MELEE_VITA_PROFILER_LIBRARY}")
        message(FATAL_ERROR "The Vita profiler headers or archive were not found")
    endif ()
    add_library(vitaprofiler STATIC IMPORTED GLOBAL)
    set_target_properties(vitaprofiler PROPERTIES
        IMPORTED_LOCATION "${MELEE_VITA_PROFILER_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MELEE_VITA_DEBUGGER_DIR}/profiler/include"
    )
    target_compile_definitions(melee_vita PRIVATE MELEE_VITA_PROFILER=1)
    target_link_libraries(melee_vita PRIVATE vitaprofiler)
endif ()

vita_create_self(eboot.bin melee_vita)
vita_create_vpk(melee-vita.vpk ${VITA_TITLE_ID} eboot.bin
    VERSION ${VITA_VERSION}
    NAME ${VITA_APP_NAME}
)
