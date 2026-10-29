$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\win-x64'
& cmake -S $root -B $build -G 'Visual Studio 18 2026' -A x64 '-DCMAKE_SYSTEM_VERSION=10.0.26100.0'
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $build --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$exe = Join-Path $build 'src\GhostGlass.Desktop\Release\GhostGlass.exe'
if (!(Test-Path $exe)) { Write-Error 'GhostGlass.exe not produced'; exit 74 }
& certutil.exe -hashfile $exe SHA256
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& (Join-Path $PSScriptRoot 'build-host.ps1')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
exit 0
