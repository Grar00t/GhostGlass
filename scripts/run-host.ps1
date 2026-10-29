$ErrorActionPreference='Stop'
$exe=Join-Path (Split-Path -Parent $PSScriptRoot) 'src\GhostGlass.Host\bin\Release\net10.0\GhostGlass.Host.exe'
if(!(Test-Path $exe)){Write-Error 'Build Host first';exit 74}
& $exe
exit $LASTEXITCODE
