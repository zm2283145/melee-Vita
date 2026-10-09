#!/usr/bin/env bash
# Assemble a PS5 native folder title from one relocatable object (or archive
# group) that defines main(), linked against the prebuilt PS5 OpenGL SDK.
# Mirrors ps5-opengl/tools/build-native-test-app.sh (consumer path).
#
# usage: mkapp.sh <out-dir> <title-id> <title-name> <object-or-archive>...
set -euo pipefail

out=$(realpath -m "$1"); title_id=$2; title_name=$3; shift 3
objects=("$@")
template=${PS5_NATIVE_APP_TEMPLATE:-/opt/ps5/bp}
glsrc=${PS5_OPENGL_SRC:-/opt/ps5/gl/ps5-opengl}
prefix=${PS5_OPENGL_PREFIX:-/opt/ps5gl/ps5-opengl-sdk-1.0.1/sdk}
sdk="$template/.deps/native/ps5-payload-sdk"
presentation=${PS5_APP_SCE_SYS:-}

for o in "${objects[@]}"; do test -s "$o"; done
compiler_runtime=$(clang-18 --print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a
static_libraries=(
    "$prefix/lib/libPS5OpenGLCore33.a"
    "$sdk/target/lib/libunwind.a"
    "$sdk/target/lib/libc++abi.a"
    "$sdk/target/lib/libc++.a"
    "$compiler_runtime"
)
for l in "${static_libraries[@]}"; do test -s "$l"; done

app="$out"
rm -rf "$app/src" "$app/include" "$app/vendor" "$app/dist" "$app/build"
mkdir -p "$app"
cp "$template/Makefile" "$app/Makefile"
for d in assets runtime sce_sys tooling tools; do
    mkdir -p "$app/$d"; cp -a "$template/$d/." "$app/$d/"
done
heap_source="$app/tooling/native/sce_module_writer.cpp"
heap_default='write_u64(result.data, result.heap_size, std::numeric_limits<std::uint64_t>::max());'
test "$(grep -Fc "$heap_default" "$heap_source")" = 1
sed -i "s/$heap_default/write_u64(result.data, result.heap_size, 0x10000000ULL);/" "$heap_source"
link_script="$app/tools/build.sh"
test "$(grep -Fc -- '--eh-frame-hdr \' "$link_script")" = 1
sed -i 's/--eh-frame-hdr \\/--eh-frame-hdr --wrap=malloc --wrap=calloc --wrap=realloc --wrap=free --wrap=posix_memalign --wrap=malloc_usable_size \\/' "$link_script"
cp "$template/tooling/native/ps5-pie.ld" "$app/tooling/native/ps5-pie-base.ld"
cp "$glsrc/native-app/ps5-pie.ld" "$app/tooling/native/ps5-pie.ld"
cp "$glsrc/native-app/app-symbols.map" "$app/tooling/native/app-symbols.map"
mkdir -p "$app/src" "$app/include" "$app/vendor"
cp "$glsrc/native-app/runtime_shims.c" "$app/src/runtime_shims.c"
cp "$glsrc/native-app/app_heap.c" "$app/src/app_heap.c"
for h in EGL GL KHR; do cp -a "$prefix/include/$h" "$app/include/"; done

# Identity: start from the OpenGL native-app param.json (memory/page-table
# settings the driver needs) and give it our id and name.
python3 - "$glsrc/native-app/param.json" "$app/sce_sys/param.json" "$title_id" "$title_name" <<'PY'
import json, sys
src, dst, tid, name = sys.argv[1:5]
m = json.load(open(src))
m["titleId"] = tid
m["conceptId"] = tid[4:]
m["contentId"] = f"UP9000-{tid}_00-MELEEPS5DEV00001"
m["localizedParameters"]["en-US"]["titleName"] = name
open(dst, "w").write(json.dumps(m, indent=2) + "\n")
PY
python3 "$glsrc/tools/native-display-metadata.py" "$app/sce_sys/param.json" --fps 60
if [[ -n $presentation && -d $presentation ]]; then
    for a in icon0.png pic0.dds pic1.dds snd0.at9; do
        [[ -f $presentation/$a ]] && install -m 0644 "$presentation/$a" "$app/sce_sys/$a"
    done
fi

{
    printf 'SEARCH_DIR("%s")\n' "$sdk/target/lib"
    printf 'SEARCH_DIR("%s")\n' "$prefix/lib"
    printf 'EXTERN(ps5_agc_gate2_run)\n'
    printf 'GROUP (\n'
    for o in "${objects[@]}"; do printf '  "%s"\n' "$(realpath "$o")"; done
    for l in "${static_libraries[@]}" ${PS5_APP_EXTRA_LIBS:-}; do printf '  "%s"\n' "$l"; done
    printf ')\n'
} > "$app/vendor/libps5_opengl_group.a"
printf 'APP_INCLUDE_PATHS = include\nAPP_STATIC_ARCHIVES = vendor/libps5_opengl_group.a\n' > "$app/.env"

if [[ ! -x "$app/.deps/native/ps5-payload-sdk/bin/prospero-lld" ]]; then
    mkdir -p "$app/.deps"; cp -a "$template/.deps/native" "$app/.deps/native"
fi
app_sdk="$app/.deps/native/ps5-payload-sdk"
mkdir -p "$app/build/native-imports"
cp "$sdk/target/lib/libSceVideoOut.so" "$app_sdk/target/lib/libSceVideoOut.so"
for stub in agc_link_stub:libSceAgc agc_driver_link_stub:libSceAgcDriver; do
    src=${stub%%:*}; lib=${stub##*:}
    PS5_PAYLOAD_SDK="$app_sdk" sh "$app/tooling/prospero-clang18" -std=c11 -O2 -fPIC \
        -ffunction-sections -fdata-sections -c "$glsrc/native-app/$src.c" \
        -o "$app/build/native-imports/$src.o"
    "$app_sdk/bin/prospero-lld" --shared -soname "$lib.prx" -o "$app_sdk/target/lib/$lib.so" \
        "$app/build/native-imports/$src.o"
done

make -C "$app" --no-print-directory -j"$(nproc)" app
dist="$app/dist/$title_id"
test -s "$dist/eboot.bin"
printf 'Native app: %s\n' "$dist"
