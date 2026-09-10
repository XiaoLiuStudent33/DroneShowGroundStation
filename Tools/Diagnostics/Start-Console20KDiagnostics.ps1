$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$backendExe = Join-Path $repoRoot 'ConsoleGCS\x64\Release\DroneGCS.exe'
$backendConfig = Join-Path $repoRoot 'ConsoleGCS\gcs-config-upload.ini'
if (-not (Test-Path -LiteralPath $backendExe)) { throw "Missing backend: $backendExe" }
if (-not (Test-Path -LiteralPath $backendConfig)) { throw "Missing configuration: $backendConfig" }
if (Get-Process -Name DroneGCS -ErrorAction SilentlyContinue) {
    Write-Host 'Close the existing Console backend first. Keep UE and the simulator unchanged.'
    exit 1
}
$runDirectory = Join-Path $repoRoot ('BuildLogs\Scale20K\UserRetest-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $runDirectory | Out-Null
Copy-Item -LiteralPath $backendConfig -Destination (Join-Path $runDirectory 'backend-config.ini')
Get-FileHash -LiteralPath $backendExe | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDirectory 'executable-hash.json') -Encoding UTF8
$priorPerf = $env:DRONEGCS_PERF
$priorLog = $env:DRONEGCS_BACKEND_LOG
try {
    $env:DRONEGCS_PERF = '1'
    $env:DRONEGCS_BACKEND_LOG = Join-Path $runDirectory 'backend.log'
    Write-Host "Diagnostic log: $env:DRONEGCS_BACKEND_LOG"
    Write-Host 'Using existing backend configuration; this script submits no flight or upload commands.'
    Push-Location (Join-Path $repoRoot 'ConsoleGCS')
    try { & $backendExe $backendConfig; $diagnosticExitCode = $LASTEXITCODE }
    finally { Pop-Location }
} finally {
    $env:DRONEGCS_PERF = $priorPerf
    $env:DRONEGCS_BACKEND_LOG = $priorLog
}
exit $diagnosticExitCode
