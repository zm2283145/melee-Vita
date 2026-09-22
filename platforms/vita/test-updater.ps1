#Requires -Version 7.0
param(
    [string]$HostCompiler = 'C:/msys64/mingw64/bin/g++.exe',
    [string]$VitaSdk = $env:VITASDK,
    [string]$BuildDirectory = "$PSScriptRoot/../../build-vita/updater-tests"
)

$ErrorActionPreference = 'Stop'
$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = Join-Path $PSScriptRoot 'updater/update_core.cpp'
$sodiumSource = Join-Path $PSScriptRoot 'updater/update_crypto_sodium.cpp'
$test = Join-Path $PSScriptRoot 'tests/updater_core_test.cpp'
$cryptoTest = Join-Path $PSScriptRoot 'tests/updater_crypto_test.py'
$headTest = Join-Path $PSScriptRoot 'tests/updater_head_test.py'
$headGenerator = Join-Path $PSScriptRoot 'updater/make-head-bin.py'
$headTemplate = Join-Path $PSScriptRoot 'updater/head.bin'
$build = [System.IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null

if (-not (Test-Path -LiteralPath $HostCompiler -PathType Leaf)) {
    throw "Host C++ compiler not found: $HostCompiler"
}
$env:PATH = "$(Split-Path -Parent $HostCompiler);$env:PATH"

$hostExe = Join-Path $build 'updater_core_test.exe'
$hostLibrary = Join-Path $build 'update_core_test.dll'
& $HostCompiler -std=c++20 -Wall -Wextra -Werror -pedantic `
    -I (Join-Path $PSScriptRoot 'updater') $source $test -o $hostExe
if ($LASTEXITCODE -ne 0) {
    throw "Host updater test compilation failed with exit code $LASTEXITCODE"
}
& $hostExe
if ($LASTEXITCODE -ne 0) {
    throw "Host updater tests failed with exit code $LASTEXITCODE"
}

$hostLibraryFlags = @(
    '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pedantic', '-shared'
)
if ($IsWindows) {
    $hostLibraryFlags += '-static'
} else {
    $hostLibraryFlags += '-fPIC'
}
& $HostCompiler @hostLibraryFlags `
    -I (Join-Path $PSScriptRoot 'updater') $source -o $hostLibrary
if ($LASTEXITCODE -ne 0) {
    throw "Host updater test library compilation failed with exit code $LASTEXITCODE"
}
python $cryptoTest $hostLibrary
if ($LASTEXITCODE -ne 0) {
    throw "Host Ed25519 tests failed with exit code $LASTEXITCODE"
}
python $headTest $headGenerator $headTemplate
if ($LASTEXITCODE -ne 0) {
    throw "Vita package-head tests failed with exit code $LASTEXITCODE"
}

if (-not $VitaSdk) {
    throw 'VITASDK is not set. Pass -VitaSdk or set VITASDK.'
}
$vitaCompilerName =
    if ($IsWindows) { 'bin/arm-vita-eabi-g++.exe' }
    else { 'bin/arm-vita-eabi-g++' }
$vitaCompiler = Join-Path $VitaSdk $vitaCompilerName
if (-not (Test-Path -LiteralPath $vitaCompiler -PathType Leaf)) {
    throw "Vita compiler not found: $vitaCompiler"
}
$env:PATH = "$(Split-Path -Parent $vitaCompiler);$env:PATH"
$vitaFlags = @(
    '-std=c++20', '-O2', '-DNDEBUG', '-DMELEE_VITA_RELEASE=1',
    '-fno-exceptions', '-fno-rtti', '-Wall', '-Wextra', '-Werror',
    '-fno-short-enums',
    '-I', (Join-Path $PSScriptRoot 'updater'), '-c'
)
& $vitaCompiler @vitaFlags $source -o (Join-Path $build 'update_core.vita.o')
if ($LASTEXITCODE -ne 0) {
    throw "Vita updater core Release compilation failed with exit code $LASTEXITCODE"
}
& $vitaCompiler @vitaFlags $sodiumSource -o (Join-Path $build 'update_crypto_sodium.vita.o')
if ($LASTEXITCODE -ne 0) {
    throw "Vita updater crypto adapter Release compilation failed with exit code $LASTEXITCODE"
}
$vitaUpdaterFlags = @(
    '-DMELEE_VITA_UPDATER=1', '-DMELEE_UPDATE_USE_SODIUM=1',
    '-DCURL_STATICLIB=1', '-DMELEE_VITA_VERSION="0.6.0"',
    '-DMELEE_UPDATE_PUBLIC_KEY_HEX="d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"'
) + $vitaFlags
foreach ($runtimeSource in @(
    (Join-Path $PSScriptRoot 'updater/update_vita.cpp'),
    (Join-Path $PSScriptRoot 'updater/update_runtime.cpp'),
    (Join-Path $PSScriptRoot 'updater/update_helper.cpp')
)) {
    $runtimeObject = Join-Path $build "$([IO.Path]::GetFileNameWithoutExtension($runtimeSource)).vita.o"
    & $vitaCompiler @vitaUpdaterFlags $runtimeSource -o $runtimeObject
    if ($LASTEXITCODE -ne 0) {
        throw "Vita updater runtime Release compilation failed with exit code $LASTEXITCODE"
    }
}

Write-Output "Updater validation artifacts: $build"
