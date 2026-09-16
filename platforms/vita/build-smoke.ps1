param(
    [string]$VitaSdk = $env:VITASDK,
    [string]$BuildDirectory = "$PSScriptRoot/../../build-vita/platforms/vita"
)

$ErrorActionPreference = 'Stop'
if (-not $VitaSdk) {
    throw 'VITASDK is not set. Pass -VitaSdk or set the VITASDK environment variable.'
}

$source = Join-Path $PSScriptRoot 'smoke_main.c'
$abiSource = Join-Path $PSScriptRoot 'abi_probe.c'
$bringupSource = Join-Path $PSScriptRoot 'bringup_probe.c'
$discSource = Join-Path $PSScriptRoot 'disc_probe.c'
$figatreeSource = Join-Path $PSScriptRoot 'figatree.c'
$gxDecoderSource = Join-Path $PSScriptRoot 'gx_decoder.c'
$gxmProbeSource = Join-Path $PSScriptRoot 'gxm_probe.c'
$runtimeSymbolsSource = Join-Path $PSScriptRoot 'runtime_symbols.c'
$textureDecoderSource = Join-Path $PSScriptRoot 'texture_decoder.c'
$textureAtlasSource = Join-Path $PSScriptRoot 'texture_atlas.c'
$logSource = Join-Path $PSScriptRoot 'vita_log.c'
$debugger = 'D:/Claude/VitaDebugger'
$common = Join-Path $VitaSdk 'share/gcc-arm-vita-eabi/samples/common'
$screenSource = Join-Path $common 'debugScreen.c'
$build = [System.IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Force -Path $build | Out-Null

function Invoke-Tool([string]$Name, [string[]]$Arguments) {
    $tool = Join-Path $VitaSdk "bin/$Name.exe"
    & $tool @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$Name failed with exit code $LASTEXITCODE"
    }
}

$object = Join-Path $build 'smoke_main.o'
$abiObject = Join-Path $build 'abi_probe.o'
$bringupObject = Join-Path $build 'bringup_probe.o'
$discObject = Join-Path $build 'disc_probe.o'
$figatreeObject = Join-Path $build 'figatree.o'
$gxDecoderObject = Join-Path $build 'gx_decoder.o'
$gxmProbeObject = Join-Path $build 'gxm_probe.o'
$runtimeSymbolsObject = Join-Path $build 'runtime_symbols.o'
$textureDecoderObject = Join-Path $build 'texture_decoder.o'
$textureAtlasObject = Join-Path $build 'texture_atlas.o'
$screenObject = Join-Path $build 'debugScreen.o'
$logObject = Join-Path $build 'vita_log.o'
$elf = Join-Path $build 'melee_vita.elf'
$velf = Join-Path $build 'melee_vita.velf'
$eboot = Join-Path $build 'eboot.bin'
$sfo = Join-Path $build 'param.sfo'
$vpk = Join-Path $build 'melee-vita.vpk'

Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror', '-I', $common,
    '-c', $source, '-o', $object
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $abiSource, '-o', $abiObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $bringupSource, '-o', $bringupObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $discSource, '-o', $discObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $figatreeSource, '-o', $figatreeObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $gxDecoderSource, '-o', $gxDecoderObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $gxmProbeSource, '-o', $gxmProbeObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $runtimeSymbolsSource, '-o', $runtimeSymbolsObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $textureDecoderSource, '-o', $textureDecoderObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-c', $textureAtlasSource, '-o', $textureAtlasObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-I', $common,
    '-c', $screenSource, '-o', $screenObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-std=c11', '-O0', '-g3', '-Wall', '-Wextra', '-Werror',
    '-I', $debugger, '-c', $logSource, '-o', $logObject
)
Invoke-Tool 'arm-vita-eabi-gcc' @(
    '-Wl,-q', '-Wl,-z,nocopyreloc', '-Wl,--defsym=__sce_headroom=0x1000',
    $object, $abiObject, $bringupObject, $discObject, $figatreeObject,
    $gxDecoderObject,
    $gxmProbeObject,
    $runtimeSymbolsObject, $textureDecoderObject, $textureAtlasObject,
    $screenObject, $logObject,
    "$debugger/libuvdb.a",
    '-lSceCtrl_stub', '-lSceDisplay_stub', '-lSceKernelThreadMgr_stub',
    '-lvita2d', '-lSceGxm_stub', '-lSceDisplay_stub', '-lSceAppMgr_stub',
    '-lSceCommonDialog_stub', '-lm',
    '-lSceProcessmgr_stub', '-lSceSysmem_stub', '-lSceLibKernel_stub',
    '-lSceNet_stub', '-lSceNetCtl_stub', '-lSceNetPs_stub',
    '-lSceSysmodule_stub', '-pthread',
    '-o', $elf
)
Invoke-Tool 'vita-elf-create' @($elf, $velf)
Invoke-Tool 'vita-make-fself' @('-c', $velf, $eboot)
Invoke-Tool 'vita-mksfoex' @(
    '-s', 'TITLE_ID=MLVITA001', '-s', 'APP_VER=00.01',
    'Melee Vita Bring-up', $sfo
)
Invoke-Tool 'vita-pack-vpk' @('-s', $sfo, '-b', $eboot, $vpk)

Write-Output $vpk
