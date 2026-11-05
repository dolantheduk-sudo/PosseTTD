# PosseTTD local build helper (Windows, x64).
# Usage (from the repo root, in PowerShell):   .\tools\possettd\build.ps1
# Prerequisites: Visual Studio 2022 with "Desktop development with C++", CMake, Ninja, and vcpkg
# (default location C:\Users\<you>\vcpkg, override with the VCPKG_ROOT environment variable).
# Output: build\possettd.exe

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$vcpkg = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { Join-Path $env:USERPROFILE 'vcpkg' }

# Refresh PATH so freshly installed tools (cmake, ninja) are found.
$env:Path = [Environment]::GetEnvironmentVariable('Path','User') + ';' + [Environment]::GetEnvironmentVariable('Path','Machine')

# Load the MSVC compiler environment (what the "x64 Native Tools Command Prompt" does).
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
}

New-Item -ItemType Directory -Force (Join-Path $repo 'build') | Out-Null
Set-Location (Join-Path $repo 'build')

cmake .. -GNinja `
    "-DVCPKG_TARGET_TRIPLET=x64-windows-static" `
    "-DCMAKE_TOOLCHAIN_FILE=$vcpkg\scripts\buildsystems\vcpkg.cmake" `
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo"
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }

cmake --build . --target openttd
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }

Write-Host "Done: $repo\build\possettd.exe"
