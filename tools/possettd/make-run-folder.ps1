# Creates build\play\ : a ready-to-play copy of the game, laid out exactly like the release zip
# (CPack layout + OpenGFX/OpenSFX/OpenMSX in baseset\). Run build.ps1 first.
# Usage:  .\tools\possettd\make-run-folder.ps1
$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine')
$packs = @(
    'opengfx-releases/8.0/opengfx-8.0',
    'opensfx-releases/1.0.3/opensfx-1.0.3',
    'openmsx-releases/0.4.2/openmsx-0.4.2'
)
$build = Join-Path $repo 'build'
$run   = Join-Path $build 'play'
$cache = Join-Path $build 'pack-cache'
if (Test-Path $run) { Remove-Item -Recurse -Force $run }
if (Test-Path (Join-Path $build 'bundles')) { Remove-Item -Recurse -Force (Join-Path $build 'bundles') }
New-Item -ItemType Directory -Force $run, $cache | Out-Null

Push-Location $build
cpack -G ZIP | Out-Null
Pop-Location
$zip = Get-ChildItem (Join-Path $build 'bundles') -Filter 'possettd-*.zip' | Select-Object -First 1
Expand-Archive $zip.FullName -DestinationPath $run
$exe = Get-ChildItem $run -Recurse -Filter possettd.exe | Select-Object -First 1
# Give the folder a fixed name so it is always the same place to launch from.
Rename-Item $exe.DirectoryName "PosseTTD"
$game = Join-Path $run "PosseTTD"

foreach ($p in $packs) {
    $name = Split-Path $p -Leaf
    $z = Join-Path $cache "$name-all.zip"
    if (-not (Test-Path $z)) { Invoke-WebRequest "https://cdn.openttd.org/$p-all.zip" -OutFile $z }
    $tmp = Join-Path $cache $name
    if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
    Expand-Archive $z -DestinationPath $tmp
    Copy-Item (Join-Path $tmp '*.tar') (Join-Path $game 'baseset')
}
Write-Host "Ready: $game\possettd.exe"
Get-ChildItem (Join-Path $game 'baseset') -Filter *.tar | ForEach-Object Name
