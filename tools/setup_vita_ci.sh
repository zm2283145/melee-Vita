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
    https://github.com/vitasdk/autobuilds/releases/download/sdk-snapshot-20260825.612.1/vitasdk-x86_64-linux-gnu-2026-08-25_16-27-41.tar.bz2 \
    49184a5fcc4dd75e9eb233422a56e5801b6a996d6e52b6ca71178788e9559f51 \
    "$deps/vitasdk.tar.bz2"
mkdir -p "$VITASDK"
tar -xjf "$deps/vitasdk.tar.bz2" -C "$VITASDK" --strip-components=1
export PATH="$VITASDK/bin:$PATH"

packages="$root/tools/vita-ci-packages"
install_package() {
    local name=$1 hash=$2 archive="$packages/$1"
    printf '%s  %s\n' "$hash" "$archive" | sha256sum --check --status
    tar -xJf "$archive" -C "$VITASDK" arm-vita-eabi
}

install_package libvita2d-0.0.0.r188.ga8f15ab-1-vita.pkg.tar.xz \
    d604b050e6e445e4019258a75806f24fb325e8abb8dfdc48f6c1fbaa7346fd43
install_package taihen-0.11-1-vita.pkg.tar.xz \
    93d2075b6b61a164224a2aad651d3d6787f31f3a19c3e2bada7c99844f9fae90
install_package libjpeg-turbo-3.2.0-1-vita.pkg.tar.xz \
    8b4b0328c16362b006e8eeae7a6c6400558d5a6de9ccb776bf055d827533eaa7

checkout bythos14/SceShaccCgExt fb0e9d338525b067f3679ab33571323336493cca "$deps/SceShaccCgExt"
cmake -S "$deps/SceShaccCgExt" -B "$deps/SceShaccCgExt/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_STANDARD=11
cmake --build "$deps/SceShaccCgExt/build" --target install --parallel 2

checkout Rinnegatamante/vitaShaRK df24065e65098b2d1ac533760109ad4367573f28 "$deps/vitaShaRK"
make -C "$deps/vitaShaRK" -j2 install \
    CFLAGS='-std=gnu11 -Wl,-q -O2 -ffast-math -mtune=cortex-a9 -mfpu=neon -ftree-vectorize'

arm-vita-eabi-gcc --version
