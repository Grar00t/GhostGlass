$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$exe=Join-Path $root 'src\GhostGlass.Host\bin\Release\net10.0\GhostGlass.Host.exe'
$fixture=Join-Path $root 'tests\evidence-local.txt'
if(!(Test-Path $exe)){Write-Error 'Host must be built';exit 74}
$idOutput=& $exe --index $fixture
if($LASTEXITCODE -ne 0 -or $idOutput -notmatch '^INDEXED_SOURCE_ID=([0-9a-f]{64})$'){Write-Output 'INDEX_TEST_FAIL';exit 65}
$id=$Matches[1]
$id2=& $exe --index $fixture
if($LASTEXITCODE -ne 0 -or $id2 -ne $idOutput){Write-Output 'DEDUP_FAIL';exit 65}
Write-Output 'PASS index and SHA256 dedup'
$result=& $exe --search 'Direct2D'
if($LASTEXITCODE -ne 0 -or ($result -join '') -notmatch $id){Write-Output 'SEARCH_FAIL';exit 65}
Write-Output 'PASS FTS5 search with real source ID'
$remove=& $exe --remove $id
if($LASTEXITCODE -ne 0 -or $remove -ne 'REMOVED_FROM_INDEX'){Write-Output 'REMOVE_FAIL';exit 65}
$result=& $exe --search 'Direct2D'
if($LASTEXITCODE -ne 0 -or ($result -join '') -match $id){Write-Output 'REMOVE_NOT_REFLECTED';exit 65}
Write-Output 'PASS index deletion does not delete original file'
$ErrorActionPreference='Continue'
$negative=& $exe --index (Join-Path $root 'src\GhostGlass.Desktop\main.cpp') 2>&1
$negativeExit=$LASTEXITCODE
$ErrorActionPreference='Stop'
if($negativeExit -ne 65){Write-Output 'UNSUPPORTED_TYPE_TEST_FAIL';exit 65}
Write-Output 'PASS unsupported file extension rejected'
Write-Output 'LOCAL_INDEX_TEST_PASS'
exit 0
