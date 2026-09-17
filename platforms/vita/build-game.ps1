param(
    [string]$VitaSdk = $env:VITASDK,
    [string]$BuildDirectory = "$PSScriptRoot/../../build-vita/game",
    [int]$Jobs = 8
)

$ErrorActionPreference = 'Stop'
if (-not $VitaSdk) {
    throw 'VITASDK is not set. Pass -VitaSdk or set the VITASDK environment variable.'
}

$root = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$build = [System.IO.Path]::GetFullPath($BuildDirectory)
$objects = Join-Path $build 'obj'
New-Item -ItemType Directory -Force -Path $objects | Out-Null

$compiler = Join-Path $VitaSdk 'bin/arm-vita-eabi-gcc.exe'
$archiver = Join-Path $VitaSdk 'bin/arm-vita-eabi-ar.exe'
$compat = Join-Path $PSScriptRoot 'vita_compat.h'
$includeAurora = Join-Path $root 'extern/aurora/include'
$includeSrc = Join-Path $root 'src'
$includeSdk = Join-Path $root 'src/sdk_include'

$sources = @(
    Get-ChildItem (Join-Path $root 'src/melee'), (Join-Path $root 'src/sysdolphin') -Recurse -Filter *.c |
        Where-Object { $_.FullName -notmatch 'baselib\\debugconsole_main\.c$' } |
        Sort-Object FullName |
        Select-Object -ExpandProperty FullName
)
$sources += Join-Path $root 'src/pc/vtxarray.c'

$common = @(
    '-std=c11', '-O2', '-g3',
    '-DTARGET_PC=1', '-DTARGET_VITA=1', '-DMELEE_PC=1', '-DMELEE_VITA_SKIP_OPENING_MOVIE=1', '-DMELEE_VITA_DISABLE_ATTRACT_DEMO=1',
    "-I$includeAurora", "-I$includeSrc", "-I$includeSdk",
    '-Wno-all', '-Wno-extra',
    '-Werror=int-conversion', '-Werror=implicit-function-declaration',
    '-Werror=incompatible-pointer-types',
    '-fno-strict-aliasing', '-fno-short-enums', '-fwrapv',
    '-fgnu89-inline',
    '-ffp-contract=off', '-Wno-scalar-storage-order',
    '-include', $compat, '-c'
)

$sjisTool = Join-Path $PSScriptRoot 'sjis_literals.py'
$sjisDir = Join-Path $build 'sjis'
New-Item -ItemType Directory -Force -Path $sjisDir | Out-Null

$compileItems = foreach ($source in $sources) {
    $relative = [System.IO.Path]::GetRelativePath($root, $source)
    $objectName = ($relative -replace '[:\\/]', '__') -replace '\.c$', '.o'
    [pscustomobject]@{
        Source = $source
        Object = Join-Path $objects $objectName
        Sjis = [bool] (Select-String -LiteralPath $source -Pattern '"[^"\n]*[^\x00-\x7F]' -Quiet)
    }
}

$pending = @($compileItems | Where-Object {
    -not (Test-Path $_.Object) -or
    (Get-Item $_.Source).LastWriteTimeUtc -gt (Get-Item $_.Object).LastWriteTimeUtc -or
    (Get-Item $compat).LastWriteTimeUtc -gt (Get-Item $_.Object).LastWriteTimeUtc
})

Write-Output "Compiling $($pending.Count) of $($compileItems.Count) game sources..."
$pending | ForEach-Object -Parallel {
    $item = $_
    # GCC diagnostics arrive on stderr even when they are non-fatal warnings;
    # do not let PowerShell convert those records into terminating errors.
    $ErrorActionPreference = 'Continue'
    $PSNativeCommandUseErrorActionPreference = $false
    $sourceToCompile = $item.Source
    $extra = @()
    if ($item.Sjis) {
        # Shift-JIS runtime strings: rewrite UTF-8 literals as CP932 escapes
        # (VitaSDK GCC cannot use -fexec-charset=CP932).
        $generated = Join-Path $using:sjisDir ([System.IO.Path]::GetFileName($item.Object) -replace '\.o$', '.c')
        & py -3 $using:sjisTool $item.Source $generated
        if ($LASTEXITCODE -ne 0) { throw "Shift-JIS conversion failed: $($item.Source)" }
        $sourceToCompile = $generated
        $extra = @('-iquote', [System.IO.Path]::GetDirectoryName($item.Source))
    }
    & $using:compiler @using:common @extra $sourceToCompile '-o' $item.Object 2>&1 |
        ForEach-Object { Write-Output $_ }
    if ($LASTEXITCODE -ne 0) {
        throw "Vita compile failed: $($item.Source)"
    }
} -ThrottleLimit $Jobs

$response = Join-Path $build 'game-objects.rsp'
$compileItems.Object |
    ForEach-Object { '"' + ($_.Replace('\\', '/')) + '"' } |
    Set-Content -Encoding ascii $response
$archive = Join-Path $build 'libmelee_vita_game.a'
& $archiver 'rcs' $archive "@$response"
if ($LASTEXITCODE -ne 0) {
    throw "arm-vita-eabi-ar failed with exit code $LASTEXITCODE"
}

Write-Output $archive
