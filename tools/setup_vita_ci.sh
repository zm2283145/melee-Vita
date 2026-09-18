#!/usr/bin/env bash
set -euo pipefail

# Pinned SDK and library archives; no retail game data or Sony modules are fetched.
: "${VITASDK:?Set VITASDK to a new SDK installation directory}"
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
deps="$root/build-vita/deps"
mkdir -p "$deps"

download() {
    local url=$1 hash=$2 destination=$3
    curl --fail --location --retry 3 --silent --show-error "$url" -o "$destination"
    printf '%s  %s\n' "$hash" "$destination" | sha256sum --check --status
}

checkout() {
    local repository=$1 revision=$2 destination=$3
    git init --quiet "$destination"
    git -C "$destination" fetch --quiet --depth 1 "https://github.com/$repository.git" "$revision"
    git -C "$destination" checkout --quiet --detach FETCH_HEAD
}

if [[ -e "$VITASDK" ]]; then
    echo "Refusing to replace existing SDK: $VITASDK" >&2
    exit 1
fi
download \
    https://github.com/vitasdk/autobuilds/releases/download/sdk-snapshot-20260914.739.1/vitasdk-x86_64-linux-gnu-2026-09-14_08-52-15.tar.bz2 \
    4ed76056eea7cfeb090524a10cb69dcb036b94fb27f0ec22a103fcc0ee6db21f \
    "$deps/vitasdk.tar.bz2"
mkdir -p "$VITASDK"
tar -xjf "$deps/vitasdk.tar.bz2" -C "$VITASDK" --strip-components=1
export PATH="$VITASDK/bin:$PATH"

download https://github.com/vitasdk/packages/releases/download/master/libvita2d.tar.xz \
    f2688386ca15e030450223812c65beea4d1ce930a79b8a99d04f99b55ab1a455 \
    "$deps/libvita2d.tar.xz"
download https://github.com/vitasdk/packages/releases/download/master/taihen.tar.xz \
    afced213d9d308a665ef91809fd728614a92ec31b2fcf61cf1f6a3e98bec4014 \
    "$deps/taihen.tar.xz"
tar -xJf "$deps/libvita2d.tar.xz" -C "$VITASDK/arm-vita-eabi"
tar -xJf "$deps/taihen.tar.xz" -C "$VITASDK/arm-vita-eabi"

checkout bythos14/SceShaccCgExt fb0e9d338525b067f3679ab33571323336493cca "$deps/SceShaccCgExt"
cmake -S "$deps/SceShaccCgExt" -B "$deps/SceShaccCgExt/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build "$deps/SceShaccCgExt/build" --target install --parallel 2

checkout Rinnegatamante/vitaShaRK df24065e65098b2d1ac533760109ad4367573f28 "$deps/vitaShaRK"
make -C "$deps/vitaShaRK" -j2 install

arm-vita-eabi-gcc --version
