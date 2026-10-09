#!/usr/bin/env bash
# Build Melee for PS5 as a native folder title (Linux/WSL host).
#
#   platforms/ps5/build.sh [--clean]
#
# Needs: GCC (x86-64; the decomp relies on scalar_storage_order, which Clang
# lacks), Clang/LLD 18, and the pinned PS5 toolchain:
#   PS5_NATIVE_APP_TEMPLATE  ps5-native-app-boilerplate (with .deps prepared)
#   PS5_OPENGL_PREFIX        ps5-opengl SDK prefix (…/sdk)
#   PS5_OPENGL_SRC           ps5-opengl source tree (native-app/ files)
# Output: build-ps5/app/dist/<TITLE_ID>/ and its ZIP.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
here="$root/platforms/ps5"
out=${MELEE_PS5_BUILD_DIR:-"$root/build-ps5"}
title_id=${MELEE_PS5_TITLE_ID:-PPSA99701}
title_name=${MELEE_PS5_TITLE_NAME:-"Super Smash Bros. Melee"}
jobs=${BUILD_JOBS:-$(nproc)}

export PS5_NATIVE_APP_TEMPLATE=${PS5_NATIVE_APP_TEMPLATE:-/opt/ps5/bp}
export PS5_OPENGL_PREFIX=${PS5_OPENGL_PREFIX:-/opt/ps5gl/ps5-opengl-sdk-1.0.1/sdk}
export PS5_OPENGL_SRC=${PS5_OPENGL_SRC:-/opt/ps5/gl/ps5-opengl}
sdk="$PS5_NATIVE_APP_TEMPLATE/.deps/native/ps5-payload-sdk"
export PS5_PAYLOAD_SDK="$sdk"

[[ ${1:-} == --clean ]] && rm -rf "$out"
mkdir -p "$out/obj"

# GCC with the PS5 SDK's FreeBSD-derived headers.  The SDK's own compiler
# targets x86_64-sie-ps5 (SysV ABI, 2-byte wchar_t); match it.
gcc_ps5=(gcc -nostdinc -isystem "$(gcc -print-file-name=include)" -isystem "$sdk/target/include"
         -U__linux__ -U__linux -U__gnu_linux__ -Ulinux -D__FreeBSD__=9
         -D__FreeBSD_cc_version=900001 -D__PROSPERO__=1 -D__SCE__=1 -fshort-wchar
         -march=znver2 -fPIC -fno-plt -fno-stack-protector -fno-asynchronous-unwind-tables)

notify_define=-DMELEE_PS5_NO_NOTIFY=1
[[ ${MELEE_PS5_NOTIFY:-0} == 1 ]] && notify_define=
# Development log streaming: MELEE_PS5_LOG_HOST=<pc ip> sends the log to
# platforms/ps5/tools/log-listener on TCP 18200 (off by default).
log_defines=()
if [[ -n ${MELEE_PS5_LOG_HOST:-} ]]; then
    log_defines=(-DMELEE_PS5_NET_LOG=1 "-DMELEE_PS5_LOG_HOST=\"$MELEE_PS5_LOG_HOST\"")
fi
defines=(${MELEE_PS5_DEFINES:-} "${log_defines[@]}" $notify_define -DTARGET_PC=1 -DMELEE_PC=1 -DTARGET_VITA=1 -DTARGET_PS5=1
         -DMELEE_VITA_MODERN_DEBUG_MENU=1 -DMELEE_VITA_RUNTIME_RESOLUTION_MENU=1
         -DMELEE_VITA_SCALED_SHADOW_FRAMES=1 -DMELEE_VITA_SCALED_GX_COPY_FRAMES=1
         -DMELEE_VITA_GX_CPU_VERTEX=1 -DNDEBUG)
includes=(-I"$here/shim" -I"$here" -I"$root/platforms/vita/game"
          -I"$root/extern/aurora/include" -I"$root/src" -I"$root/src/sdk_include")
opt=(-O2 -g -fno-omit-frame-pointer)

game_flags=("${gcc_ps5[@]}" -std=gnu11 "${opt[@]}" "${defines[@]}" "${includes[@]}"
            -Wno-all -Wno-extra -Werror=int-conversion -Werror=implicit-function-declaration
            -Werror=incompatible-pointer-types -fno-strict-aliasing -fwrapv -ffp-contract=off
            -fexec-charset=CP932 -Wno-scalar-storage-order -fgnu89-inline
            -include "$root/platforms/vita/vita_compat.h")
platform_flags=("${gcc_ps5[@]}" -std=gnu11 "${opt[@]}" "${defines[@]}" "${includes[@]}"
                -Wall -Wextra -Wno-parentheses -Wno-unused-parameter -Wno-sign-compare
                -Wno-unused-function -fno-strict-aliasing
                -include "$root/platforms/vita/vita_compat.h" -include "$here/shim/ps5_compat.h")
ps5_flags=("${gcc_ps5[@]}" -std=gnu11 "${opt[@]}" "${defines[@]}" "${includes[@]}"
           -I"$PS5_OPENGL_PREFIX/include" -DGL_GLEXT_PROTOTYPES=1
           -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -fno-strict-aliasing
           -include "$root/platforms/vita/vita_compat.h")

cpp_flags=(sh "$PS5_NATIVE_APP_TEMPLATE/tooling/prospero-clang18" -x c++ -std=c++20 "${opt[@]}"
           "${defines[@]}" -I"$root/extern/aurora/include" -I"$root/extern/aurora/lib"
           -I"$root/src" -I"$root/src/sdk_include" -fPIC -fno-exceptions -fno-rtti
           -Wno-unknown-attributes)

# ---- source lists -----------------------------------------------------------
mapfile -t game_sources < <(cd "$root" && find src/melee src/sysdolphin -name '*.c' \
    ! -name debugconsole_main.c | sort)
game_sources+=(src/pc/vtxarray.c)
platform_sources=(
    platforms/vita/game/card.c platforms/vita/game/compat.c platforms/vita/game/dvd.c
    platforms/vita/game/audio.c platforms/vita/game/gx.c platforms/vita/game/heap.c
    platforms/vita/game/os.c platforms/vita/game/pad.c platforms/vita/game/thp.c
    platforms/vita/game/vi.c platforms/vita/game/widescreen.c
    platforms/vita/game/opening_audio.c platforms/vita/game/gx_bump.c
    platforms/vita/game/gx_submit.c platforms/vita/game/gxr_shader_cache.c
    platforms/vita/texture_decoder.c src/pc/misc.c
    extern/aurora/lib/dolphin/mtx/mtx.c extern/aurora/lib/dolphin/mtx/mtx44.c
    extern/aurora/lib/dolphin/mtx/mtxstack.c extern/aurora/lib/dolphin/mtx/mtxvec.c
    extern/aurora/lib/dolphin/mtx/quat.c extern/aurora/lib/dolphin/mtx/vec.c
    platforms/vita/game/pc_stubs.c platforms/ps5/ps5_stubs.c)
ps5_sources=(
    platforms/ps5/psp2_shim.c platforms/ps5/ps5_log.c platforms/ps5/ps5_services.c
    platforms/ps5/ps5_memory.c platforms/ps5/ps5_main.c platforms/ps5/gl_game.c
    platforms/ps5/gl_render.c platforms/ps5/ps5_libc_extra.c)

cpp_sources=(extern/aurora/lib/dolphin/thp/THPDec.cpp)

# ---- compile ------------------------------------------------------------------
# Each object depends on its source and on every header (coarse but safe):
# rebuild when the source or any header changed after the object.
newest_header=$(cd "$root" && find src platforms/vita platforms/ps5 extern/aurora/include \
    -name '*.h' -printf '%T@\n' | sort -n | tail -1)

compile_list() {
    local kind=$1; shift
    local -n flags=$kind
    local list=()
    for src in "$@"; do
        local obj="$out/obj/${src//\//_}.o"
        if [[ ! -f $obj || "$root/$src" -nt $obj ||
              $(stat -c %Y "$obj") < ${newest_header%.*} ]]; then
            list+=("$src")
        fi
    done
    [[ ${#list[@]} -eq 0 ]] && return 0
    echo "==> compiling ${#list[@]} ${kind%_flags} sources"
    printf '%s\n' "${list[@]}" | (cd "$root" && xargs -P "$jobs" -I{} sh -c '
        out=$0; src=$1; shift
        obj="$out/obj/$(echo "$src" | tr / _).o"
        "$@" -c "$src" -o "$obj" 2> "$obj.log" || { cat "$obj.log"; echo "FAILED: $src"; exit 255; }
        [ -s "$obj.log" ] || rm -f "$obj.log"' "$out" {} "${flags[@]}")
}

compile_list game_flags "${game_sources[@]}"
compile_list platform_flags "${platform_sources[@]}"
compile_list ps5_flags "${ps5_sources[@]}"
compile_list cpp_flags "${cpp_sources[@]}"

objects=()
for src in "${game_sources[@]}" "${platform_sources[@]}" "${ps5_sources[@]}" "${cpp_sources[@]}"; do
    objects+=("$out/obj/${src//\//_}.o")
done

# ---- link and package -----------------------------------------------------------
extra_libs="$sdk/target/lib/libScePad.so $sdk/target/lib/libSceUserService.so"
extra_libs+=" $sdk/target/lib/libSceAudioOut.so"
if [[ ${MELEE_PS5_NOTIFY:-0} == 1 ]]; then
    extra_libs+=" $sdk/target/lib/libSceNotification.so"
fi
extra_libs+=" $sdk/target/lib/libSceSystemService.so"
PS5_APP_EXTRA_LIBS="$extra_libs" PS5_APP_SCE_SYS="$here/sce_sys" \
    bash "$here/tools/mkapp.sh" "$out/app" "$title_id" "$title_name" "${objects[@]}"
