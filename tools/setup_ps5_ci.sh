#!/usr/bin/env bash
# Fetch the pinned PS5 toolchain used by platforms/ps5/build.sh.
#
#   tools/setup_ps5_ci.sh [prefix]      (default: $RUNNER_TEMP/ps5 or /opt/ps5)
#
# Prints the environment platforms/ps5/build.sh needs (also appended to
# $GITHUB_ENV on GitHub Actions).
set -euo pipefail

prefix=${1:-${RUNNER_TEMP:-/opt}/ps5}
boilerplate_rev=4f531c4b517f80bcb6b1267135848168d2250047
gl_version=1.0.1
gl_url="https://github.com/blackbearreloaded/ps5-opengl/releases/download/v${gl_version}/ps5-opengl-sdk-${gl_version}.tar.gz"
gl_sha=aaa2e8957f55e1fc0b654dcb35a36e585f7e635d63952da40a28be7f64fde741

mkdir -p "$prefix"
cd "$prefix"

# ps5-native-app-boilerplate (packaging + signing tools); its setup script
# fetches the pinned ps5-payload-sdk v0.42 and zlib with hash checks.
if [[ ! -d bp/.git ]]; then
    git clone --quiet https://github.com/blackbearreloaded/ps5-native-app-boilerplate.git bp
fi
git -C bp checkout --quiet "$boilerplate_rev"
(cd bp && bash tools/setup-native-dependencies.sh)

# ps5-opengl SDK (OpenGL 4.6 over AGC) and its bundled source snapshot.
if [[ ! -d ps5-opengl-sdk-${gl_version} ]]; then
    curl -fsSL -o gl.tar.gz "$gl_url"
    echo "$gl_sha  gl.tar.gz" | sha256sum --check --quiet
    tar xzf gl.tar.gz
    rm gl.tar.gz
fi
mkdir -p gl
[[ -d gl/ps5-opengl ]] || tar xf "ps5-opengl-sdk-${gl_version}/sources/ps5-opengl.tar" -C gl

env_lines=(
    "PS5_NATIVE_APP_TEMPLATE=$prefix/bp"
    "PS5_OPENGL_PREFIX=$prefix/ps5-opengl-sdk-${gl_version}/sdk"
    "PS5_OPENGL_SRC=$prefix/gl/ps5-opengl"
)
printf '%s\n' "${env_lines[@]}"
if [[ -n ${GITHUB_ENV:-} ]]; then printf '%s\n' "${env_lines[@]}" >> "$GITHUB_ENV"; fi
