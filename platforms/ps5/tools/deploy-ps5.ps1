param(
  [Parameter(Mandatory)][string]$Zip,
  [Parameter(Mandatory)][string]$Ps5,
  [switch]$EbootOnly
)
# Upload a built title ZIP (build-ps5/app/dist/PPSA99701.zip) to
# /data/homebrew/<TITLE_ID> over the FTP payload on port 2121. eboot.bin and
# param.json go last so ShadowMountPlus never sees a half-copied title.
# -EbootOnly replaces just eboot.bin (close the game first).
$ErrorActionPreference = 'Stop'
$tmp = Join-Path $env:TEMP ("ps5app-" + [guid]::NewGuid())
Expand-Archive $Zip -DestinationPath $tmp
$root = Get-ChildItem $tmp -Directory | Select-Object -First 1
$tid = $root.Name
$base = "ftp://${Ps5}:2121/data/homebrew/$tid"
$files = Get-ChildItem $root.FullName -Recurse -File |
  Where-Object { -not $EbootOnly -or $_.Name -eq 'eboot.bin' } |
  Sort-Object { if ($_.Name -in 'eboot.bin','param.json') {1} else {0} }
foreach ($f in $files) {
  $rel = $f.FullName.Substring($root.FullName.Length + 1).Replace('\','/')
  curl.exe -s --max-time 300 --ftp-create-dirs -T $f.FullName "$base/$rel"
  if ($LASTEXITCODE) { throw "upload failed: $rel (curl $LASTEXITCODE; 25 usually means the game is running)" }
}
Remove-Item $tmp -Recurse -Force
"deployed $tid ($($files.Count) files)"
