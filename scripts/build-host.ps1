$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& dotnet build (Join-Path $root 'src\GhostGlass.Host\GhostGlass.Host.csproj') -c Release --nologo
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
exit 0
