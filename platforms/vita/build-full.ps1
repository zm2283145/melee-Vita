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
    [string]$WarmCacheVita = $env:MELEE_VITA_WARM_CACHE_HOST,
    [ValidateRange(1, 65535)]
    [int]$WarmCacheFtpPort = 1337,
    [switch]$EnableDebugger,
    [switch]$EnableDebugMenu,
    [switch]$EnableModernDebugMenu,
    [switch]$EnableDirectSnag,
    [switch]$EnableRenderTrace,
    [switch]$EnableShaderCacheSeal,
    [ValidateSet(100, 75, 60, 50)]
    [int]$InternalResolutionScale = 100,
    [ValidateSet(0, 100, 75, 60, 50)]
    [int]$GameplayInternalResolutionScale = 0,
    [switch]$EnableScaledShadowFrames,
    [switch]$EnableScaledGxCopyFrames,
    [switch]$EnableRuntimeResolutionMenu,
    [switch]$EnableLiveProfiler,
    [string]$ProfilerLibrary,
    [ValidatePattern('^[0-9]{1,3}(\.[0-9]{1,3}){3}$')]
    [string]$ProfilerHost = '10.1.1.146',
    [switch]$UseCpuVertexPath,
    [string]$VitaBuildNumber = $env:MELEE_VITA_BUILD_NUMBER,
    [switch]$EnableUpdater,
    [ValidatePattern('^[0-9A-Fa-f]{64}$')]
    [string]$UpdaterPublicKeyHex
)

$ErrorActionPreference = 'Stop'
if (-not $VitaSdk) {
    throw 'VITASDK is not set. Pass -VitaSdk or set the VITASDK environment variable.'
}

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))

function Merge-WarmShaderCache([string]$Vita, [int]$Port) {
    $cachePath = Join-Path $PSScriptRoot 'shadercache/warm5.bin'
    $downloadPath = Join-Path ([IO.Path]::GetTempPath()) "melee-vita-warm5-$PID.bin"
    try {
        $uri = [Uri] "ftp://${Vita}:$Port/ux0:/data/melee/shadercache/warm5.bin"
        $request = [Net.FtpWebRequest]::Create($uri)
        $request.Method = [Net.WebRequestMethods+Ftp]::DownloadFile
        $request.UseBinary = $true
        $request.KeepAlive = $false
        $response = $request.GetResponse()
        try {
            $input = $response.GetResponseStream()
            $output = [IO.File]::Create($downloadPath)
            try { $input.CopyTo($output) } finally { $output.Dispose(); $input.Dispose() }
        } finally {
            $response.Dispose()
        }

        $records = [Collections.Generic.Dictionary[string, byte[]]]::new()
        foreach ($path in @($cachePath, $downloadPath)) {
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
            $bytes = [IO.File]::ReadAllBytes($path)
            $offset = 0
            while ($offset + 24 -le $bytes.Length) {
                $start = $offset
                $magic = [BitConverter]::ToUInt32($bytes, $offset)
                $kind = [BitConverter]::ToUInt32($bytes, $offset + 4)
                $size = [BitConverter]::ToUInt32($bytes, $offset + 8)
                $hash = [BitConverter]::ToUInt64($bytes, $offset + 16)
                $padded = ($size + 3) -band (-bnot 3)
                $offset += 24
                if ($magic -ne 0x35525847 -or $size -eq 0 -or
                    $size -gt 1MB -or $offset + $padded -gt $bytes.Length) {
                    throw "Invalid warm shader cache record in $path at offset $start."
                }
                $record = [byte[]]::new(24 + $padded)
                [Array]::Copy($bytes, $start, $record, 0, $record.Length)
                $key = "$kind`:$hash"
                if (-not $records.ContainsKey($key)) { $records.Add($key, $record) }
                $offset += $padded
            }
            if ($offset -ne $bytes.Length) {
                throw "Trailing bytes in warm shader cache $path at offset $offset."
            }
        }

        $mergedPath = "$cachePath.tmp"
        $stream = [IO.File]::Create($mergedPath)
        try {
            foreach ($record in $records.Values) {
                $stream.Write($record, 0, $record.Length)
            }
        } finally {
            $stream.Dispose()
        }
        Move-Item -Force -LiteralPath $mergedPath -Destination $cachePath
        Write-Output "Merged $($records.Count) warm shader programs from $Vita into $cachePath"
    } finally {
        Remove-Item -Force -LiteralPath $downloadPath -ErrorAction SilentlyContinue
    }
}

if ($WarmCacheVita) {
    Merge-WarmShaderCache -Vita $WarmCacheVita -Port $WarmCacheFtpPort
}

$version = Get-Content -Raw (Join-Path $PSScriptRoot 'version.json') | ConvertFrom-Json
if ($version.release -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or
    $version.app -notmatch '^[0-9]{2}\.[0-9]{2}$') {
    throw 'Invalid release or Vita app version in platforms/vita/version.json.'
}
if ([string]::IsNullOrWhiteSpace($VitaBuildNumber)) {
    if ($env:GITHUB_RUN_NUMBER -match '^[1-9][0-9]*$') {
        $attempt = if ($env:GITHUB_RUN_ATTEMPT -match '^[1-9][0-9]*$') {
            $env:GITHUB_RUN_ATTEMPT
        } else {
            '1'
        }
        $VitaBuildNumber = "$($env:GITHUB_RUN_NUMBER).$attempt"
    } else {
        $VitaBuildNumber = 'local'
    }
}
if ($VitaBuildNumber -notmatch '^(local|[1-9][0-9]*\.[1-9][0-9]*)$' -or
    "VITA $($version.release) BUILD $VitaBuildNumber".Length -gt 48) {
    throw 'Invalid Vita build number.'
}
$build = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)
$internalWidth = if ($InternalResolutionScale -eq 60) {
    576
} else {
    [int] (960 * $InternalResolutionScale / 100)
}
$internalHeight = if ($InternalResolutionScale -eq 60) {
    328
} else {
    [int] (544 * $InternalResolutionScale / 100)
}
$gameplayInternalWidth = if ($GameplayInternalResolutionScale -eq 0) {
    $internalWidth
} elseif ($GameplayInternalResolutionScale -eq 60) {
    576
} else {
    [int] (960 * $GameplayInternalResolutionScale / 100)
}
$gameplayInternalHeight = if ($GameplayInternalResolutionScale -eq 0) {
    $internalHeight
} elseif ($GameplayInternalResolutionScale -eq 60) {
    328
} else {
    [int] (544 * $GameplayInternalResolutionScale / 100)
}
if ($internalWidth -lt 320 -or $internalWidth -gt 960 -or
    $internalHeight -lt 240 -or $internalHeight -gt 544 -or
    ($internalWidth % 16) -ne 0 -or ($internalHeight % 8) -ne 0) {
    throw "Internal resolution ${internalWidth}x${internalHeight} violates Vita EFB bounds/alignment."
}
if ($gameplayInternalWidth -lt 320 -or $gameplayInternalWidth -gt 960 -or
    $gameplayInternalHeight -lt 240 -or $gameplayInternalHeight -gt 544 -or
    ($gameplayInternalWidth % 16) -ne 0 -or
    ($gameplayInternalHeight % 8) -ne 0) {
    throw "Gameplay internal resolution ${gameplayInternalWidth}x${gameplayInternalHeight} violates Vita EFB bounds/alignment."
}
if ($GameplayInternalResolutionScale -ne 0 -and
    $GameplayInternalResolutionScale -eq $InternalResolutionScale) {
    throw '-GameplayInternalResolutionScale must differ from -InternalResolutionScale or be 0.'
}
if ($GameplayInternalResolutionScale -ne 0 -and
    $InternalResolutionScale -eq 100) {
    throw '-GameplayInternalResolutionScale requires a non-native -InternalResolutionScale.'
}
if ($EnableScaledShadowFrames -and $InternalResolutionScale -eq 100) {
    throw '-EnableScaledShadowFrames requires a non-native -InternalResolutionScale.'
}
if ($EnableScaledGxCopyFrames -and $InternalResolutionScale -eq 100) {
    throw '-EnableScaledGxCopyFrames requires a non-native -InternalResolutionScale.'
}
if ($EnableRuntimeResolutionMenu -and -not $EnableScaledShadowFrames) {
    throw '-EnableRuntimeResolutionMenu requires -EnableScaledShadowFrames.'
}
if ($EnableRuntimeResolutionMenu -and -not $EnableScaledGxCopyFrames) {
    throw '-EnableRuntimeResolutionMenu requires -EnableScaledGxCopyFrames.'
}
if ($EnableDebugger -and $Configuration -ne 'Debug') {
    throw '-EnableDebugger requires -Configuration Debug.'
}
if ($EnableUpdater -and $Configuration -ne 'Release') {
    throw '-EnableUpdater requires -Configuration Release.'
}
if ($EnableUpdater -and -not $UpdaterPublicKeyHex) {
    throw '-EnableUpdater requires a 64-hex-character Ed25519 -UpdaterPublicKeyHex.'
}
if ($EnableUpdater) {
    $headTemplate = Join-Path $PSScriptRoot 'updater/head.bin'
    foreach ($dependency in @(
        (Join-Path $VitaSdk 'arm-vita-eabi/include/curl/curl.h'),
        (Join-Path $VitaSdk 'arm-vita-eabi/include/sodium.h'),
        (Join-Path $VitaSdk 'arm-vita-eabi/include/archive.h'),
        (Join-Path $VitaSdk 'arm-vita-eabi/lib/libcurl.a'),
        (Join-Path $VitaSdk 'arm-vita-eabi/lib/libsodium.a'),
        (Join-Path $VitaSdk 'arm-vita-eabi/lib/libarchive.a'),
        $headTemplate
    )) {
        if (-not (Test-Path -LiteralPath $dependency -PathType Leaf)) {
            throw "Missing updater dependency: $dependency. Install curl-mbedtls, libsodium, and libarchive with vdpm."
        }
    }
    if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
        throw 'Python is required to generate Vita package headers.'
    }
}
if ($EnableDirectSnag -and -not $EnableDebugMenu) {
    throw '-EnableDirectSnag requires -EnableDebugMenu.'
}
if ($EnableLiveProfiler) {
    if (-not $ProfilerLibrary) {
        $ProfilerLibrary = Join-Path $VitaDebuggerDirectory 'profiler/build/vita/libvitaprofiler.a'
    }
    $ProfilerLibrary = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ProfilerLibrary)
    $ProfilerHeader = Join-Path $VitaDebuggerDirectory 'profiler/include/vitaprofiler.h'
    foreach ($dependency in @($ProfilerLibrary, $ProfilerHeader)) {
        if (-not (Test-Path -LiteralPath $dependency -PathType Leaf)) {
            throw "Missing live profiler dependency: $dependency"
        }
    }
    $ProfilerOctets = $ProfilerHost.Split('.') | ForEach-Object { [int]$_ }
    if (@($ProfilerOctets | Where-Object { $_ -lt 0 -or $_ -gt 255 }).Count -ne 0) {
        throw 'Invalid live profiler IPv4 address.'
    }
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
    @(
        '-O3', '-ffunction-sections', '-fdata-sections',
        '-march=armv7-a', '-mtune=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard',
        '-fsigned-char',
        '-fno-math-errno', '-funsafe-math-optimizations', '-fno-signed-zeros',
        '-ffp-contract=fast',
        '-DNDEBUG', '-DMELEE_VITA_RELEASE=1'
    )
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
    -Configuration $Configuration -Jobs $Jobs `
    -EnableDebugMenu:$EnableDebugMenu `
    -EnableModernDebugMenu:$EnableModernDebugMenu `
    -EnableDirectSnag:$EnableDirectSnag `
    -EnableRenderTrace:$EnableRenderTrace `
    -EnableRuntimeResolutionMenu:$EnableRuntimeResolutionMenu `
    -VitaReleaseVersion $version.release `
    -VitaBuildNumber $VitaBuildNumber |
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
    'platforms/vita/game/gx_bump.c',
    'platforms/vita/game/gx_render.c',
    'platforms/vita/game/gxr_shader_cache.c',
    'platforms/vita/game/gxm_game.c',
    'platforms/vita/game/heap.c',
    'platforms/vita/game/main.c',
    'platforms/vita/game/opening_audio.c',
    'platforms/vita/game/opening_movie.c',
    'platforms/vita/game/os.c',
    'platforms/vita/game/pad.c',
    'platforms/vita/game/pc_stubs.c',
    'platforms/vita/game/jpeg_hw.c',
    'platforms/vita/game/profiler_live.c',
    'platforms/vita/game/thp.c',
    'platforms/vita/game/vi.c',
    'platforms/vita/game/widescreen.c'
)
if ($EnableUpdater) {
    $platformSources += @(
        'platforms/vita/updater/update_core.cpp',
        'platforms/vita/updater/update_crypto_sodium.cpp',
        'platforms/vita/updater/update_vita.cpp',
        'platforms/vita/updater/update_runtime.cpp'
    )
}

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
$resolutionOptions = @{ 100 = 0; 75 = 1; 60 = 2; 50 = 3 }
$menuResolutionOption = $resolutionOptions[$InternalResolutionScale]
$gameplayResolutionOption = if ($GameplayInternalResolutionScale -eq 0) {
    $menuResolutionOption
} else {
    $resolutionOptions[$GameplayInternalResolutionScale]
}
$common += "-DMELEE_VITA_DEFAULT_MENU_RESOLUTION_OPTION=$menuResolutionOption"
$common += "-DMELEE_VITA_DEFAULT_GAMEPLAY_RESOLUTION_OPTION=$gameplayResolutionOption"
if ($Configuration -eq 'Debug') {
    $common += @(
        "-I$VitaDebuggerDirectory",
        "-DMELEE_VITA_LOG_HOST=`"$LogHost`"",
        "-DMELEE_VITA_DEBUGNET_PORT=$DebugNetPort"
    )
}
if ($EnableRenderTrace) {
    $common += '-DMELEE_VITA_RENDER_TRACE=1'
}
if ($EnableShaderCacheSeal) {
    $common += '-DMELEE_VITA_SHADER_CACHE_SEAL=1'
}
if ($InternalResolutionScale -ne 100) {
    $common += "-DMELEE_VITA_INTERNAL_WIDTH=$internalWidth"
    $common += "-DMELEE_VITA_INTERNAL_HEIGHT=$internalHeight"
}
if ($GameplayInternalResolutionScale -ne 0) {
    $common += "-DMELEE_VITA_GAMEPLAY_INTERNAL_WIDTH=$gameplayInternalWidth"
    $common += "-DMELEE_VITA_GAMEPLAY_INTERNAL_HEIGHT=$gameplayInternalHeight"
}
if ($EnableScaledShadowFrames) {
    $common += '-DMELEE_VITA_SCALED_SHADOW_FRAMES=1'
}
if ($EnableScaledGxCopyFrames) {
    $common += '-DMELEE_VITA_SCALED_GX_COPY_FRAMES=1'
}
if ($EnableRuntimeResolutionMenu) {
    $common += '-DMELEE_VITA_RUNTIME_RESOLUTION_MENU=1'
}
if ($EnableLiveProfiler) {
    $common += '-DMELEE_VITA_PROFILER=1'
    $common += "-I$(Join-Path $VitaDebuggerDirectory 'profiler/include')"
    for ($i = 0; $i -lt 4; $i++) {
        $common += "-DMELEE_VITA_PROFILER_HOST_$(@('A','B','C','D')[$i])=$($ProfilerOctets[$i])"
    }
}
$common += '-DMELEE_VITA_GPU_BUMP_DL=1'
if ($UseCpuVertexPath) {
    $common += '-DMELEE_VITA_GX_CPU_VERTEX=1'
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
if ($EnableUpdater) {
    $updaterDefines = @(
        '-DMELEE_VITA_UPDATER=1',
        '-DMELEE_UPDATE_USE_SODIUM=1',
        '-DCURL_STATICLIB=1',
        "-DMELEE_VITA_VERSION=`"$($version.release)`"",
        "-DMELEE_UPDATE_PUBLIC_KEY_HEX=`"$($UpdaterPublicKeyHex.ToLowerInvariant())`"",
        "-I$(Join-Path $root 'platforms/vita/updater')"
    )
    $common = $updaterDefines + $common
    $commonCpp = @('-Wall', '-Wextra', '-Werror') + $updaterDefines + $commonCpp
}

if ($EnableDebugger) { $common = @('-DMELEE_VITA_WAIT_FOR_DEBUGGER=1') + $common }
Write-Host "Melee Vita platform C compiler flags: $($common -join ' ')"
Write-Host "Melee Vita platform C++ compiler flags: $($commonCpp -join ' ')"
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
    '-Wl,--defsym=__sce_headroom=0x1000', '-Wl,--gc-sections',
    '-Wl,--wrap=sceGxmBeginScene', '-Wl,--wrap=sceGxmCreateRenderTarget'
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
if ($EnableUpdater) {
    $link += @(
        '-Wl,--start-group',
        '-lcurl', '-lmbedtls', '-lmbedx509', '-lmbedcrypto',
        '-larchive', '-lsodium', '-lbz2', '-lzstd', '-lz',
        '-Wl,--end-group',
        '-lSceNet_stub', '-lSceNetCtl_stub', '-lSceRtc_stub',
        '-lSceIofilemgr_stub', '-lScePromoterUtil_stub', '-lSceAppUtil_stub'
    )
}
if ($EnableLiveProfiler) {
    $link += @($ProfilerLibrary, '-lSceNet_stub', '-lSceNetCtl_stub',
               '-lSceNetPs_stub')
}
$link += @(
    '-lSceCtrl_stub', '-lSceDisplay_stub', '-lSceAudio_stub', '-lSceJpeg_stub', '-lSceKernelThreadMgr_stub',
    '-lvita2d', '-lSceGxm_stub', '-lSceDisplay_stub', '-lSceAppMgr_stub',
    '-lSceTouch_stub',
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
if ($EnableUpdater) {
    Invoke-VitaTool 'vita-make-fself' @(
        '-a', '0x2808000000000000', '-c', $velf, $eboot
    )
} else {
    Invoke-VitaTool 'vita-make-fself' @('-c', $velf, $eboot)
}
Invoke-VitaTool 'vita-mksfoex' @(
    '-s', 'TITLE_ID=MLVITA002', '-s', "APP_VER=$($version.app)",
    'Smash Melee Vita', $sfo
)
$livearea = Join-Path $PSScriptRoot 'livearea'
$vpkArguments = @(
    '-s', $sfo, '-b', $eboot,
    '-a', "$(Join-Path $livearea 'icon0.png')=sce_sys/icon0.png",
    '-a', "$(Join-Path $livearea 'pic0.png')=sce_sys/pic0.png",
    '-a', "$(Join-Path $livearea 'bg.png')=sce_sys/livearea/contents/bg.png",
    '-a', "$(Join-Path $livearea 'startup.png')=sce_sys/livearea/contents/startup.png",
    '-a', "$(Join-Path $livearea 'template.xml')=sce_sys/livearea/contents/template.xml",
    '-a', "$(Join-Path $PSScriptRoot 'shadercache/warm5.bin')=shadercache/warm5.bin"
)
if ($EnableUpdater) {
    $helperDirectory = Join-Path $build 'updater-helper'
    $helperObjectsDirectory = Join-Path $helperDirectory 'obj'
    New-Item -ItemType Directory -Force -Path $helperObjectsDirectory | Out-Null
    $helperSources = @(
        'platforms/vita/updater/update_helper.cpp',
        'platforms/vita/updater/update_core.cpp',
        'platforms/vita/updater/update_crypto_sodium.cpp',
        'platforms/vita/updater/update_vita.cpp'
    )
    $helperCpp = @($commonCpp | Where-Object { $_ -ne '-fno-short-enums' }) +
        '-fshort-enums'
    $helperObjects = foreach ($relative in $helperSources) {
        $source = Join-Path $root $relative
        $object = Join-Path $helperObjectsDirectory (
            (($relative -replace '[:\\/]', '__') -replace '\.cpp$', '.o')
        )
        Invoke-VitaTool 'arm-vita-eabi-g++' ($helperCpp + @($source, '-o', $object))
        $object
    }
    $helperElf = Join-Path $helperDirectory 'updater.elf'
    $helperVelf = Join-Path $helperDirectory 'updater.velf'
    $helperEboot = Join-Path $helperDirectory 'eboot.bin'
    $helperSfo = Join-Path $helperDirectory 'param.sfo'
    $helperHead = Join-Path $helperDirectory 'head.bin'
    Invoke-VitaTool 'arm-vita-eabi-g++' (
        @('-fshort-enums', '-Wl,-q', '-Wl,--gc-sections') + $helperObjects + @(
            '-Wl,--start-group',
            '-larchive', '-lsodium', '-lbz2', '-lzstd', '-lz',
            '-lstdc++',
            '-Wl,--end-group',
            '-lSceAppMgr_stub', '-lScePromoterUtil_stub',
            '-lSceSysmodule_stub', '-lSceProcessmgr_stub',
            '-lSceSysmem_stub', '-lSceLibKernel_stub',
            '-lSceKernelThreadMgr_stub', '-lm',
            '-o', $helperElf
        )
    )
    Invoke-VitaTool 'arm-vita-eabi-strip' @('--strip-debug', $helperElf)
    Invoke-VitaTool 'vita-elf-create' @($helperElf, $helperVelf)
    Invoke-VitaTool 'vita-make-fself' @('-c', $helperVelf, $helperEboot)
    Invoke-VitaTool 'vita-mksfoex' @(
        '-s', 'TITLE_ID=MLVUPD001', '-s', "APP_VER=$($version.app)",
        'Melee Vita Updater', $helperSfo
    )
    $mainHead = Join-Path $build 'head.bin'
    & python (Join-Path $PSScriptRoot 'updater/make-head-bin.py') `
        --template $headTemplate --title-id MLVITA002 --output $mainHead
    if ($LASTEXITCODE -ne 0) {
        throw "Generating the Melee package head failed with exit code $LASTEXITCODE"
    }
    & python (Join-Path $PSScriptRoot 'updater/make-head-bin.py') `
        --template $headTemplate --title-id MLVUPD001 --output $helperHead
    if ($LASTEXITCODE -ne 0) {
        throw "Generating the updater package head failed with exit code $LASTEXITCODE"
    }
    $vpkArguments += @(
        '-a', "$mainHead=sce_sys/package/head.bin",
        '-a', "$helperEboot=updater/eboot.bin",
        '-a', "$helperSfo=updater/sce_sys/param.sfo",
        '-a', "$helperHead=updater/sce_sys/package/head.bin"
    )
}
$vpkArguments += $vpk
Invoke-VitaTool 'vita-pack-vpk' $vpkArguments

Write-Output $vpk
