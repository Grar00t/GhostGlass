$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
& dotnet build (Join-Path $root 'src\GhostGlass.Host\GhostGlass.Host.csproj') -c Release --nologo
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& dotnet run --project (Join-Path $root 'tests\GhostGlass.IpcTests\GhostGlass.IpcTests.csproj') --configuration Release
exit $LASTEXITCODE