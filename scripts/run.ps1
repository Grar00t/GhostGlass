$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root 'build\win-x64\src\GhostGlass.Desktop\Release\GhostGlass.exe'
$hostExe=Join-Path $root 'src\GhostGlass.Host\bin\Release\net10.0\GhostGlass.Host.exe'
if(!(Test-Path $exe)){Write-Error 'Desktop is not built';exit 74}
if(!(Test-Path $hostExe)){Write-Error 'Host is not built';exit 74}
$existing=@(Get-CimInstance Win32_Process -Filter "name = 'GhostGlass.Host.exe'" -ErrorAction SilentlyContinue | Where-Object { $_.ExecutablePath -eq $hostExe })
$owned=$null
if($existing.Count -eq 0){$owned=Start-Process -FilePath $hostExe -WindowStyle Hidden -PassThru}
try {
 & $exe
 $exitCode=$LASTEXITCODE
}finally{
 if($owned -and !$owned.HasExited){
  Stop-Process -Id $owned.Id -ErrorAction SilentlyContinue
 }
}
exit $exitCode
