#Requires -Version 7.0
param(
    [string]$VitaSdk = $env:VITASDK,
    [string]$BuildDirectory = "$PSScriptRoot/../../build-vita/game",
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [ValidateRange(1, 64)]
    [int]$Jobs = 8,
    [switch]$EnableDebugMenu,
    [switch]$EnableModernDebugMenu,
    [switch]$EnableDirectSnag,
    [switch]$EnableRenderTrace,
    [switch]$EnableRuntimeResolutionMenu,
    # Slippi Direct play: GameCube-exact floating point (platforms/vita/fp).
    # Needs the contraction plugin that build-full.ps1 builds.
    [switch]$EnableExactMath,
    # Slippi Direct play's hooks in the game code (MELEE_VITA_SLIPPI).
    # build-full.ps1 -EnableSlippi turns this and exact maths on together.
    [switch]$EnableSlippi,
    [string]$FpContractPlugin,
    [string]$VitaReleaseVersion,
    [string]$VitaBuildNumber = $env:MELEE_VITA_BUILD_NUMBER
)

$ErrorActionPreference = 'Stop'
if (-not $VitaSdk) {
    throw 'VITASDK is not set. Pass -VitaSdk or set the VITASDK environment variable.'
}

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$version = Get-Content -Raw (Join-Path $PSScriptRoot 'version.json') | ConvertFrom-Json
if ([string]::IsNullOrWhiteSpace($VitaReleaseVersion)) {
    $VitaReleaseVersion = $version.release
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
if ($VitaReleaseVersion -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or
    $VitaBuildNumber -notmatch '^(local|[1-9][0-9]*\.[1-9][0-9]*)$' -or
    "VITA $VitaReleaseVersion BUILD $VitaBuildNumber".Length -gt 48) {
    throw 'Invalid Vita release version or build number.'
}
$variantParts = @($Configuration)
if ($EnableDebugMenu) { $variantParts += 'DebugMenu' }
if ($EnableModernDebugMenu) { $variantParts += 'ModernDebugMenu' }
if ($EnableDirectSnag) { $variantParts += 'DirectSnag' }
if ($EnableRenderTrace) { $variantParts += 'RenderTrace' }
if ($EnableRuntimeResolutionMenu) { $variantParts += 'RuntimeResolution' }
if ($EnableExactMath) { $variantParts += 'ExactMath' }
if ($EnableSlippi) { $variantParts += 'Slippi' }
$variantParts += "Vita$($VitaReleaseVersion -replace '\.', '_')"
$variantParts += "Build$($VitaBuildNumber -replace '\.', '_')"
$variant = $variantParts -join '-'
$build = Join-Path ($ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($BuildDirectory)) $variant
$objects = Join-Path $build 'obj'
New-Item -ItemType Directory -Force -Path $objects | Out-Null

$suffix = if ($IsWindows) { '.exe' } else { '' }
$compiler = Join-Path $VitaSdk "bin/arm-vita-eabi-gcc$suffix"
$archiver = Join-Path $VitaSdk "bin/arm-vita-eabi-ar$suffix"
$python = (Get-Command $(if ($IsWindows) { 'python' } else { 'python3' }) -ErrorAction Stop).Source
$compat = Join-Path $PSScriptRoot 'vita_compat.h'
$includeAurora = Join-Path $root 'extern/aurora/include'
$includeSrc = Join-Path $root 'src'
$includeSdk = Join-Path $root 'src/sdk_include'
$includeVita = Join-Path $root 'platforms/vita/game'

$sources = @(
    Get-ChildItem (Join-Path $root 'src/melee'), (Join-Path $root 'src/sysdolphin') -Recurse -Filter *.c |
        Where-Object { $_.FullName -notmatch 'baselib[\\/]debugconsole_main\.c$' } |
        Sort-Object FullName |
        Select-Object -ExpandProperty FullName
)
$sources += Join-Path $root 'src/pc/vtxarray.c'

# The console's single-precision maths library (sinf, cosf, tanf, logf,
# frexp) from the decomp.  Only exact builds use it; normal builds keep
# newlib's faster versions.
$mslSources = @()
if ($EnableExactMath) {
    $mslSources = @('math.c', 'math_1.c', 'math_data.c', 'trigf.c') |
        ForEach-Object { Join-Path $root "src/MSL/$_" }
    $sources += $mslSources
}

# Files where the console fuses "field += a*b" inside HSD_JObjAdd*, found by
# the 3DS port's fused-op census (determinism.py JOBJ_FUSED there).
$jobjFusedSources = @(
    'src/melee/ft/kinds/ftCommon/ftCo_JumpAerial.c',
    'src/melee/ty/toy.c',
    'src/melee/mn/mnitemsw.c',
    'src/melee/mn/mnstagesw.c'
)

$configurationFlags = if ($Configuration -eq 'Release' -and $EnableExactMath) {
    # No unsafe maths and no contraction: GCC may not reorder, fuse or
    # vectorize float code, because the contraction plugin and the console
    # maths decide every rounding (platforms/vita/fp, tools/gcc_fp_contract).
    @(
        '-O3', '-ffunction-sections', '-fdata-sections',
        '-march=armv7-a', '-mtune=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard',
        '-fsigned-char',
        '-fno-math-errno', '-ffp-contract=off',
        '-DNDEBUG', '-DMELEE_VITA_RELEASE=1'
    )
} elseif ($Configuration -eq 'Release') {
    @(
        '-O3', '-ffunction-sections', '-fdata-sections',
        '-march=armv7-a', '-mtune=cortex-a9', '-mfpu=neon', '-mfloat-abi=hard',
        '-fsigned-char',
        '-fno-math-errno', '-funsafe-math-optimizations', '-fno-signed-zeros',
        '-ffp-contract=fast',
        '-DNDEBUG', '-DMELEE_VITA_RELEASE=1'
    )
} else {
    @('-Og', '-g3', '-ffp-contract=off')
}
$common = @(
    '-std=c11',
    '-DTARGET_PC=1', '-DTARGET_VITA=1', '-DMELEE_PC=1',
    '-DMELEE_VITA_MODERN_DEBUG_MENU=1',
    "-DMELEE_VITA_RELEASE_VERSION=`"$VitaReleaseVersion`"",
    "-DMELEE_VITA_BUILD_NUMBER=`"$VitaBuildNumber`"",
    "-I$includeAurora", "-I$includeSrc", "-I$includeSdk", "-I$includeVita",
    '-Wno-all', '-Wno-extra',
    '-Werror=int-conversion', '-Werror=implicit-function-declaration',
    '-Werror=incompatible-pointer-types',
    '-fno-strict-aliasing', '-fno-short-enums', '-fwrapv',
    '-fgnu89-inline',
    '-Wno-scalar-storage-order',
    '-include', $compat, '-c'
) + $configurationFlags
if ($EnableDebugMenu) {
    $common += '-DMELEE_VITA_ENABLE_DEBUG_MENU=1'
}
if ($EnableModernDebugMenu) {
    Write-Warning '-EnableModernDebugMenu is deprecated; the modern hidden menu is always included on Vita.'
}
if ($EnableDirectSnag) {
    if (-not $EnableDebugMenu) {
        throw '-EnableDirectSnag requires -EnableDebugMenu.'
    }
    $common += '-DMELEE_VITA_DIRECT_SNAG=1'
}
if ($EnableRenderTrace) {
    $common += '-DMELEE_VITA_RENDER_TRACE=1'
}
if ($EnableRuntimeResolutionMenu) {
    $common += '-DMELEE_VITA_RUNTIME_RESOLUTION_MENU=1'
}
$common += '-DMELEE_VITA_GPU_BUMP_DL=1'
# MU_P, mu_fnmsubs and friends (platforms/vita/fp/mp_fp.h).  The game
# sources carry these console-rounding annotations from the 3DS Slippi port
# (tools/slippi/apply_3ds_edits.py), so every build needs them.
$common += @('-include', (Join-Path $root 'platforms/vita/fp/mp_fp.h'))
if ($EnableSlippi) {
    $common += '-DMELEE_VITA_SLIPPI=1'
}
if ($EnableExactMath) {
    if (-not $FpContractPlugin -or -not (Test-Path -LiteralPath $FpContractPlugin -PathType Leaf)) {
        throw "-EnableExactMath needs -FpContractPlugin pointing at the built plugin (build-full.ps1 builds it)."
    }
    $fpDir = Join-Path $root 'platforms/vita/fp'
    $common += @(
        '-DMELEE_VITA_EXACT_MATH=1',
        # MSL's sqrtf and fmodf (fp_exact.c), not the VFP square root or
        # newlib's exact fmod.  The 3DS Slippi port maps them the same way.
        '-Dsqrtf=mp_sqrtf', '-Dfmodf=mp_fmodf',
        # Without these GCC folds sinf(constant) and friends at compile time
        # with correctly rounded results, which the console never produced.
        '-fno-builtin-sinf', '-fno-builtin-cosf', '-fno-builtin-tanf',
        '-fno-builtin-asinf', '-fno-builtin-acosf', '-fno-builtin-atanf',
        '-fno-builtin-atan2f', '-fno-builtin-expf', '-fno-builtin-logf',
        '-fno-builtin-powf', '-fno-builtin-fmodf', '-fno-builtin-sqrtf',
        "-fplugin=$FpContractPlugin",
        "-fplugin-arg-fp_contract_plugin-no-contract=$(Join-Path $root 'tools/slippi/no_contract.txt')",
        "-fplugin-arg-fp_contract_plugin-stats=$(Join-Path $build 'fp_contract_stats.txt')"
    )
    Remove-Item -LiteralPath (Join-Path $build 'fp_contract_stats.txt') -ErrorAction SilentlyContinue
}
Write-Host "Melee Vita game C compiler flags: $($common -join ' ')"

$sjisTool = Join-Path $PSScriptRoot 'sjis_literals.py'
$sjisDir = Join-Path $build 'sjis'
New-Item -ItemType Directory -Force -Path $sjisDir | Out-Null

$compileItems = foreach ($source in $sources) {
    $relative = [System.IO.Path]::GetRelativePath($root, $source)
    $objectName = ($relative -replace '[:\\/]', '__') -replace '\.c$', '.o'
    # Flags only some files get.
    $fileFlags = @()
    if ($mslSources -contains $source) {
        # MSL's math.h clashes with newlib's; this header stands in for it.
        $fileFlags += @('-include', (Join-Path $root 'platforms/vita/fp/mp_msl_math.h'))
    }
    if ($EnableExactMath -and ($jobjFusedSources -contains $relative.Replace('\', '/'))) {
        # The console inlines HSD_JObjAdd* here and fuses "field += a*b";
        # as macros the plugin sees one expression too (mp_jobj_fused.h).
        $fileFlags += @('-include', (Join-Path $root 'platforms/vita/fp/mp_jobj_fused.h'))
    }
    [pscustomobject]@{
        Source = $source
        Object = Join-Path $objects $objectName
        Sjis = [bool] (Select-String -LiteralPath $source -Pattern '"[^"\n]*[^\x00-\x7F]' -Quiet)
        Flags = $fileFlags
    }
}

$pending = @($compileItems | Where-Object {
    -not (Test-Path $_.Object) -or
    (Get-Item $_.Source).LastWriteTimeUtc -gt (Get-Item $_.Object).LastWriteTimeUtc -or
    (Get-Item $compat).LastWriteTimeUtc -gt (Get-Item $_.Object).LastWriteTimeUtc
})

Write-Output "Compiling $($pending.Count) of $($compileItems.Count) game sources..."
$compileErrors = @()
$pending | ForEach-Object -Parallel {
    $item = $_
    # GCC diagnostics arrive on stderr even when they are non-fatal warnings;
    # do not let PowerShell convert those records into terminating errors.
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    $sourceToCompile = $item.Source
    $extra = @($item.Flags)
    if ($item.Sjis) {
        # Shift-JIS runtime strings: rewrite UTF-8 literals as CP932 escapes
        # (VitaSDK GCC cannot use -fexec-charset=CP932).
        $generated = Join-Path $using:sjisDir ([System.IO.Path]::GetFileName($item.Object) -replace '\.o$', '.c')
        & $using:python $using:sjisTool $item.Source $generated
        if ($LASTEXITCODE -ne 0) { throw "Shift-JIS conversion failed: $($item.Source)" }
        $sourceToCompile = $generated
        $extra += @('-iquote', [System.IO.Path]::GetDirectoryName($item.Source))
    }
    & $using:compiler @using:common @extra $sourceToCompile '-o' $item.Object 2>&1 |
        ForEach-Object { Write-Output $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "Vita compile failed: $($item.Source)"
    }
} -ThrottleLimit $Jobs -ErrorVariable +compileErrors
if ($compileErrors.Count -ne 0) {
    throw "Vita game compilation failed; refusing to archive incomplete or stale objects."
}

$response = Join-Path $build 'game-objects.rsp'
$compileItems.Object |
    ForEach-Object { '"' + ($_.Replace('\', '/')) + '"' } |
    Set-Content -Encoding ascii $response
$archive = Join-Path $build 'libmelee_vita_game.a'
& $archiver 'rcs' $archive "@$response"
if ($LASTEXITCODE -ne 0) {
    throw "arm-vita-eabi-ar failed with exit code $LASTEXITCODE"
}

Write-Output $archive
