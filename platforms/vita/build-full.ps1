#Requires -Version 7.0
param(
    [string]$VitaSdk = $env:VITASDK,
    [string]$BuildDirectory = "$PSScriptRoot/../../build-vita/full",
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 64)]
    [int]$Jobs = 8,
    [string]$VitaDebuggerDirectory = 'D:/Claude/VitaDebugger',
    [string]$KuBridgeLibrary = "$PSScriptRoot/../../build-vita/kubridge-build/libkubridge_stub.a",
    [ValidatePattern('^[0-9]{1,3}(\.[0-9]{1,3}){3}$')]
    [string]$LogHost = '10.1.1.146',
    [ValidateRange(1, 65535)]
    [int]$DebugNetPort = 18197,
    [switch]$EnableDebugger
)

$ErrorActionPreference = 'Stop'
if (-not $VitaSdk) {
    throw 'VITASDK is not set. Pass -VitaSdk or set the VITASDK environment variable.'
}

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$version = Get-Content -Raw (Join-Path $PSScriptRoot 'version.json') | ConvertFrom-Json
if ($version.release -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or
    $version.app -notmatch '^[0-9]{2}\.[0-9]{2}$') {
    throw 'Invalid release or Vita app version in platforms/vita/version.json.'
}
$build = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
if ($EnableDebugger -and $Configuration -ne 'Debug') {
    throw '-EnableDebugger requires -Configuration Debug.'
}
if ($Configuration -eq 'Debug') {
    $VitaDebuggerDirectory = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($VitaDebuggerDirectory)
    $KuBridgeLibrary = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($KuBridgeLibrary)
    foreach ($dependency in @(
        (Join-Path $VitaDebuggerDirectory 'uvdb.h'),
        (Join-Path $VitaDebuggerDirectory 'libuvdb.a'),
        $KuBridgeLibrary
    )) {
        if (-not (Test-Path -LiteralPath $dependency -PathType Leaf)) {
            throw "Missing Vita build dependency: $dependency. See platforms/vita/README.md."
        }
    }
}
$configurationFlags = if ($Configuration -eq 'Release') {
    @('-O2', '-DNDEBUG', '-DMELEE_VITA_RELEASE=1')
} else {
    @('-Og', '-g3')
}
$objects = Join-Path $build 'obj'
New-Item -ItemType Directory -Force -Path $objects | Out-Null

function Invoke-VitaTool([string]$Name, [string[]]$Arguments) {
    $suffix = if ($IsWindows) { '.exe' } else { '' }
    $tool = Join-Path $VitaSdk "bin/$Name$suffix"
    & $tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

$gameArchive = & (Join-Path $PSScriptRoot 'build-game.ps1') `
    -VitaSdk $VitaSdk -BuildDirectory (Join-Path $build 'game') `
    -Configuration $Configuration -Jobs $Jobs |
    Select-Object -Last 1

$platformSources = @(
    'extern/aurora/lib/dolphin/mtx/mtx.c',
    'extern/aurora/lib/dolphin/mtx/mtx44.c',
    'extern/aurora/lib/dolphin/mtx/mtxstack.c',
    'extern/aurora/lib/dolphin/mtx/mtxvec.c',
    'extern/aurora/lib/dolphin/mtx/quat.c',
    'extern/aurora/lib/dolphin/mtx/vec.c',
    'extern/aurora/lib/dolphin/thp/THPDec.cpp',
    'src/pc/misc.c',
    'platforms/vita/texture_decoder.c',
    'platforms/vita/vita_log.c',
    'platforms/vita/game/audio.c',
    'platforms/vita/game/card.c',
    'platforms/vita/game/compat.c',
    'platforms/vita/game/dvd.c',
    'platforms/vita/game/gx.c',
    'platforms/vita/game/gx_render.c',
    'platforms/vita/game/gxm_game.c',
    'platforms/vita/game/heap.c',
    'platforms/vita/game/main.c',
    'platforms/vita/game/opening_audio.c',
    'platforms/vita/game/opening_movie.c',
    'platforms/vita/game/os.c',
    'platforms/vita/game/pad.c',
    'platforms/vita/game/pc_stubs.c',
    'platforms/vita/game/jpeg_hw.c',
    'platforms/vita/game/thp.c',
    'platforms/vita/game/vi.c',
    'platforms/vita/game/widescreen.c'
)

$common = @(
    '-std=gnu11',
    '-DTARGET_PC=1', '-DTARGET_VITA=1', '-DMELEE_PC=1',
    "-I$(Join-Path $root 'extern/aurora/include')",
    "-I$(Join-Path $root 'src')",
    "-I$(Join-Path $root 'src/sdk_include')",
    "-I$(Join-Path $root 'platforms/vita/game')",
    '-Wall', '-Wextra', '-Werror', '-Wno-parentheses', '-fno-short-enums',
    '-include', (Join-Path $PSScriptRoot 'vita_compat.h'), '-c'
) + $configurationFlags
if ($Configuration -eq 'Debug') {
    $common += @(
        "-I$VitaDebuggerDirectory",
        "-DMELEE_VITA_LOG_HOST=`"$LogHost`"",
        "-DMELEE_VITA_DEBUGNET_PORT=$DebugNetPort"
    )
}

$commonCpp = @(
    '-std=c++20',
    '-DTARGET_PC=1', '-DTARGET_VITA=1', '-DMELEE_PC=1',
    "-I$(Join-Path $root 'extern/aurora/include')",
    "-I$(Join-Path $root 'extern/aurora/lib')",
    "-I$(Join-Path $root 'src')",
    "-I$(Join-Path $root 'src/sdk_include')",
    '-fno-exceptions', '-fno-rtti', '-fno-short-enums', '-c'
) + $configurationFlags

if ($EnableDebugger) { $common = @('-DMELEE_VITA_WAIT_FOR_DEBUGGER=1') + $common }
$platformObjects = foreach ($relative in $platformSources) {
    $source = Join-Path $root $relative
    $name = ($relative -replace '[:\\/]', '__') -replace '\.(c|cpp)$', '.o'
    $object = Join-Path $objects $name
    if ($relative.EndsWith('.cpp')) {
        Invoke-VitaTool 'arm-vita-eabi-g++' ($commonCpp + @($source, '-o', $object))
        $object
        continue
    }
    $sourceFlags = $common
    if ($relative -eq 'platforms/vita/vita_log.c') {
        # libuvdb is a native VitaSDK library and uses the platform's compact
        # enum ABI. Keep this boundary isolated from Melee's four-byte enums.
        $sourceFlags = $sourceFlags + '-fshort-enums'
    }
    Invoke-VitaTool 'arm-vita-eabi-gcc' ($sourceFlags + @($source, '-o', $object))
    $object
}

$elf = Join-Path $build 'melee-full.elf'
$velf = Join-Path $build 'melee-full.velf'
$eboot = Join-Path $build 'eboot.bin'
$sfo = Join-Path $build 'param.sfo'
$vpk = Join-Path $build 'SmashMeleevita.vpk'

$link = @(
    '-fno-short-enums', '-Wl,-q', '-Wl,-z,nocopyreloc',
    '-Wl,--defsym=__sce_headroom=0x1000', '-Wl,--gc-sections'
) + $platformObjects + @(
    '-Wl,--start-group', $gameArchive, '-Wl,--end-group'
)
if ($Configuration -eq 'Debug') {
    $link += @(
        (Join-Path $VitaDebuggerDirectory 'libuvdb.a'),
        $KuBridgeLibrary,
        '-lSceNet_stub', '-lSceNetCtl_stub', '-lSceNetPs_stub'
    )
}
$link += @(
    '-lSceCtrl_stub', '-lSceDisplay_stub', '-lSceAudio_stub', '-lSceJpeg_stub', '-lSceKernelThreadMgr_stub',
    '-lvita2d', '-lSceGxm_stub', '-lSceDisplay_stub', '-lSceAppMgr_stub',
    '-lSceCommonDialog_stub', '-lm', '-lSceProcessmgr_stub',
    '-lSceSysmem_stub', '-lSceLibKernel_stub', '-lSceKernelModulemgr_stub', '-lSceSysmodule_stub',
    '-lvitashark', '-lSceShaccCgExt', '-ltaihen_stub', '-lSceShaccCg_stub_weak',
    '-lturbojpeg', '-lstdc++', '-pthread', '-o', $elf
)

Invoke-VitaTool 'arm-vita-eabi-gcc' $link
if ($Configuration -eq 'Release') {
    Invoke-VitaTool 'arm-vita-eabi-strip' @('--strip-debug', $elf)
}
Invoke-VitaTool 'vita-elf-create' @($elf, $velf)
Invoke-VitaTool 'vita-make-fself' @('-c', $velf, $eboot)
Invoke-VitaTool 'vita-mksfoex' @(
    '-s', 'TITLE_ID=MLVITA002', '-s', "APP_VER=$($version.app)",
    'Smash Melee Vita', $sfo
)
$livearea = Join-Path $PSScriptRoot 'livearea'
Invoke-VitaTool 'vita-pack-vpk' @(
    '-s', $sfo, '-b', $eboot,
    '-a', "$(Join-Path $livearea 'icon0.png')=sce_sys/icon0.png",
    '-a', "$(Join-Path $livearea 'pic0.png')=sce_sys/pic0.png",
    '-a', "$(Join-Path $livearea 'bg.png')=sce_sys/livearea/contents/bg.png",
    '-a', "$(Join-Path $livearea 'startup.png')=sce_sys/livearea/contents/startup.png",
    '-a', "$(Join-Path $livearea 'template.xml')=sce_sys/livearea/contents/template.xml",
    '-a', "$(Join-Path $PSScriptRoot 'shadercache/warm5.bin')=shadercache/warm5.bin",
    $vpk
)

Write-Output $vpk
