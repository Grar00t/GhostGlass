$ErrorActionPreference = 'Stop'
$exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\win-x64\src\GhostGlass.Desktop\Release\GhostGlass.exe'
if (!(Test-Path $exe)) { Write-Error 'Build first'; exit 74 }
$p = Start-Process -FilePath $exe -ArgumentList '--self-test' -PassThru -Wait
Write-Output ('GHOSTGLASS_SELF_TEST_EXIT=' + $p.ExitCode)
if($p.ExitCode -ne 0){exit $p.ExitCode}
$q=Start-Process -FilePath $exe -ArgumentList '--self-test-panel' -PassThru -Wait
Write-Output ('GHOSTGLASS_PANEL_SELF_TEST_EXIT=' + $q.ExitCode)
if($q.ExitCode -ne 0){exit $q.ExitCode}
foreach($flag in @('--self-test-stress','--self-test-hotkey')){
 $a=Start-Process -FilePath $exe -ArgumentList $flag -PassThru -Wait
 Write-Output ($flag+' EXIT='+$a.ExitCode)
 if($a.ExitCode -ne 0){exit $a.ExitCode}
}
& (Join-Path $PSScriptRoot 'test-ipc.ps1')
if($LASTEXITCODE -ne 0){exit $LASTEXITCODE}
& (Join-Path $PSScriptRoot 'test-index.ps1')
exit $LASTEXITCODE
