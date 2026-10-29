$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root 'build\win-x64\src\GhostGlass.Desktop\Release\GhostGlass.exe'
if(!(Test-Path $exe)){Write-Error 'Build first';exit 74}
foreach($mode in @('--self-test-active','--self-test-hidden')){
 $p=$null
 try{
  $p=Start-Process -FilePath $exe -ArgumentList $mode -PassThru
  Start-Sleep -Seconds 1
  $p.Refresh()
  if($p.HasExited){Write-Output "$mode exited prematurely";exit 65}
  $before=$p.TotalProcessorTime.TotalMilliseconds
  Start-Sleep -Seconds 3
  $p.Refresh()
  $cpu=$p.TotalProcessorTime.TotalMilliseconds-$before
  $rss=$p.WorkingSet64
  Write-Output ('MODE='+$mode+' CPU_MS_OVER_3S='+[math]::Round($cpu,2)+' RSS_BYTES='+$rss)
  if(!$p.WaitForExit(5000)){Write-Output "TIMEOUT_$mode";exit 65}
  Write-Output ('MODE_EXIT='+$p.ExitCode)
  if($p.ExitCode -ne 0){exit $p.ExitCode}
 }finally{if($p -and !$p.HasExited){Stop-Process -Id $p.Id -ErrorAction SilentlyContinue}}
}
exit 0
