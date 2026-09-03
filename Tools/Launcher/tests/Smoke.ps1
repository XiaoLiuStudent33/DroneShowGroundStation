param([string]$RepositoryRoot = (Resolve-Path "$PSScriptRoot/../../..").Path)
$ErrorActionPreference = 'Stop'

function Assert-True($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    Write-Output "PASS: $Message"
}
function Wait-Exit($Process, [int]$Milliseconds = 10000) {
    if (-not $Process.WaitForExit($Milliseconds)) { throw "Process $($Process.Id) did not exit; inspect test logs. No process was killed." }
    $Process.Refresh()
    return $Process.ExitCode
}
function Start-TestProcess([string]$Exe, [string]$Arguments, [string]$WorkingDirectory) {
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $Exe
    $info.Arguments = $Arguments
    $info.WorkingDirectory = $WorkingDirectory
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    return [System.Diagnostics.Process]::Start($info)
}

if (Get-Process -Name DroneGCS,DroneShowPlatform1,DroneGCSLauncher -ErrorAction SilentlyContinue) {
    throw 'Close existing production/test processes normally before smoke testing.'
}
$fixtureRoot = Join-Path $RepositoryRoot ('Tools/Launcher/x64/Smoke-' + [guid]::NewGuid().ToString('N'))
$testLocalAppData = Join-Path $fixtureRoot 'UserData'
$env:LOCALAPPDATA = $testLocalAppData
# Exercise Unicode + spaces + launcher invocation from an unrelated working directory.
$fixture = Join-Path $fixtureRoot ([string][char]0x8054 + [char]0x5408 + ' package test')
$backendDir = Join-Path $fixture 'Backend'
$gameDir = Join-Path $fixture 'Windows/DroneShowPlatform1/Binaries/Win64'
foreach ($directory in @($backendDir, "$backendDir/config", $gameDir,
    "$fixture/Windows/DroneShowPlatform1/Content/Paks", "$fixture/Windows/Engine")) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
Copy-Item -LiteralPath "$RepositoryRoot/ConsoleGCS/x64/Release/DroneGCS.exe" -Destination $backendDir
Copy-Item -LiteralPath "$RepositoryRoot/Tools/Launcher/x64/Release/DroneGCSLauncher.exe" -Destination $fixture
Copy-Item -LiteralPath "$PSScriptRoot/gcs-config.ini.example" -Destination "$backendDir/gcs-config.ini"
Copy-Item -LiteralPath "$PSScriptRoot/DSPGCSTransport.ini" -Destination "$backendDir/config/DSPGCSTransport.ini"
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
$frontendOutput = [System.IO.Path]::GetFullPath("$gameDir/DroneShowPlatform1.exe")
$frontendSource = [System.IO.Path]::GetFullPath("$PSScriptRoot/FakeFrontend.cs")
& $compiler /nologo /target:winexe "/out:$frontendOutput" $frontendSource
if ($LASTEXITCODE -ne 0) { throw 'Failed to compile test-only frontend.' }
$launcherExe = "$fixture/DroneGCSLauncher.exe"
$backendExe = "$backendDir/DroneGCS.exe"

$check = Start-TestProcess $launcherExe '--check' $env:TEMP
Assert-True ((Wait-Exit $check) -eq 0) 'Preflight accepts safe fixture without launching children'
Assert-True (-not (Get-Process -Name DroneGCS,DroneShowPlatform1 -ErrorAction SilentlyContinue)) 'Check mode creates no backend or UE process'

$portOwner = New-Object System.Net.Sockets.UdpClient
try {
    $portOwner.Client.ExclusiveAddressUse = $true
    $portOwner.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, 12001))
    $conflict = Start-TestProcess $launcherExe '--check' $env:TEMP
    Assert-True ((Wait-Exit $conflict) -ne 0) 'Occupied DSP port rejected without stopping its owner'
    Assert-True $portOwner.Client.IsBound 'Foreign socket preserved'
} finally { $portOwner.Dispose() }

$run = Start-TestProcess $launcherExe '--no-dialogs' $env:TEMP
$deadline = [DateTime]::UtcNow.AddSeconds(15)
while (-not (Test-Path -LiteralPath "$fixture/Windows/frontend-result.txt") -and [DateTime]::UtcNow -lt $deadline -and -not $run.HasExited) {
    Start-Sleep -Milliseconds 100
}
Assert-True (Test-Path -LiteralPath "$fixture/Windows/frontend-result.txt") 'Launcher waits for actual backend readiness then starts frontend'
$duplicate = Start-TestProcess $launcherExe '--check' $env:TEMP
Assert-True ((Wait-Exit $duplicate) -ne 0) 'Second launcher refused while first owns the session'
Assert-True ((Wait-Exit $run) -eq 0) 'Frontend exit causes graceful backend exit and launcher success'
Assert-True ((Get-Content "$fixture/Windows/frontend-result.txt" -Raw) -match 'DSP telemetry received') 'Real Console DSP snapshot reaches fake frontend'
Assert-True (-not (Get-Process -Name DroneGCS,DroneShowPlatform1 -ErrorAction SilentlyContinue)) 'No orphan child processes'
$backendLogs = Get-ChildItem "$testLocalAppData/DroneGCS/Logs" -Recurse -Filter backend.log
Assert-True (($backendLogs | ForEach-Object { Get-Content -LiteralPath $_.FullName -Raw }) -match 'releasing network resources') 'Managed stop reaches production cleanup path'

# Direct managed invocation proves the backend, not only the launcher, enforces lifecycle.
$token = "${PID}_$([guid]::NewGuid().ToString())"
$ready = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\DroneGCS.Managed.$token.ready")
$stop = [System.Threading.EventWaitHandle]::new($false, 'ManualReset', "Local\DroneGCS.Managed.$token.stop")
try {
    $managed = Start-TestProcess $backendExe "gcs-config.ini --launcher-session $token" $backendDir
    Assert-True ($ready.WaitOne(10000)) 'Managed backend signals readiness independently of dashboard rendering'
    [void]$stop.Set()
    Assert-True ((Wait-Exit $managed) -eq 0) 'Stop event exits managed backend normally'
} finally { [void]$stop.Set(); $ready.Dispose(); $stop.Dispose() }

$invalid = Start-TestProcess $backendExe 'gcs-config.ini --launcher-session malformed' $backendDir
Assert-True ((Wait-Exit $invalid) -ne 0) 'Invalid launcher token rejected before network startup'

Copy-Item -LiteralPath "$PSScriptRoot/gcs-config-control.ini.example" -Destination "$backendDir/control.ini"
$unsafeInfo = New-Object System.Diagnostics.ProcessStartInfo
$unsafeInfo.FileName = $backendExe
$unsafeInfo.Arguments = "control.ini --launcher-session ${PID}_12345678"
$unsafeInfo.WorkingDirectory = $backendDir
$unsafeInfo.UseShellExecute = $false
$unsafeInfo.CreateNoWindow = $true
$unsafeInfo.RedirectStandardError = $true
$unsafeProcess = [System.Diagnostics.Process]::Start($unsafeInfo)
Assert-True ((Wait-Exit $unsafeProcess) -ne 0) 'Control-enabled backend rejects unauthenticated launcher invocation'
Assert-True ($unsafeProcess.StandardError.ReadToEnd() -match 'owner/events unavailable') 'Control flags accepted; missing owner events rejected before network startup'

$ownerOutput = [System.IO.Path]::GetFullPath("$backendDir/FakeOwner.exe")
$ownerSource = [System.IO.Path]::GetFullPath("$PSScriptRoot/FakeOwner.cs")
& $compiler /nologo /target:winexe "/out:$ownerOutput" $ownerSource
if ($LASTEXITCODE -ne 0) { throw 'Failed to compile test-only owner.' }
$owner = Start-TestProcess $ownerOutput '' $backendDir
Assert-True ((Wait-Exit $owner 15000) -eq 0) 'Test owner exits after child readiness, without STOP'
$ownedPid = [int](Get-Content "$backendDir/owner-child-pid.txt")
$owned = Get-Process -Id $ownedPid -ErrorAction SilentlyContinue
if ($owned) { Assert-True ($owned.WaitForExit(10000)) 'Backend detects owner loss and exits without forced kill' }
Assert-True (-not (Get-Process -Name DroneGCS,DroneShowPlatform1 -ErrorAction SilentlyContinue)) 'Owner-loss test leaves no orphan backend'
$recheck = Start-TestProcess $launcherExe '--check' $env:TEMP
Assert-True ((Wait-Exit $recheck) -eq 0) 'Ports and singleton released; safe restart possible'

# Exercise UAT's actual project layout with full production flags on LOOPBACK.
# No aircraft datagrams or command requests are ever sent by these fixtures.
$stagedRoot = Join-Path $fixtureRoot 'Staged Windows'
$project = Join-Path $stagedRoot 'DroneShowPlatform1'
$stagedBackend = Join-Path $project 'Binaries/Win64/Backend'
foreach ($directory in @($stagedBackend, "$stagedBackend/config", "$project/Content/Paks", "$stagedRoot/Engine")) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
Copy-Item -LiteralPath $backendExe -Destination "$stagedBackend/DroneGCS.exe"
Copy-Item -LiteralPath $launcherExe -Destination "$project/DroneGCSLauncher.exe"
New-Item -ItemType File -Path "$project/installed-layout.marker" -Force | Out-Null
Copy-Item -LiteralPath $frontendOutput -Destination "$project/Binaries/Win64/DroneShowPlatform1-Win64-Shipping.exe"
Copy-Item -LiteralPath "$PSScriptRoot/gcs-config-control.ini.example" -Destination "$stagedBackend/gcs-config.ini"
Copy-Item -LiteralPath "$PSScriptRoot/DSPGCSTransport.ini" -Destination "$stagedBackend/config/DSPGCSTransport.ini"
$stagedLauncher = "$project/DroneGCSLauncher.exe"
$stopOutput = [IO.Path]::GetFullPath("$fixtureRoot/ConsoleStop.exe")
$stopSource = [IO.Path]::GetFullPath("$PSScriptRoot/ConsoleStop.cs")
& $compiler /nologo /target:winexe "/out:$stopOutput" $stopSource
if ($LASTEXITCODE -ne 0) { throw 'Failed to compile test-only Ctrl+C helper.' }
$controlRun = Start-TestProcess $stagedLauncher '--no-dialogs' $env:TEMP
$deadline = [DateTime]::UtcNow.AddSeconds(15)
$controlBackend = $null
$stopControl = $null
try {
    while ([DateTime]::UtcNow -lt $deadline -and -not $controlRun.HasExited) {
        $candidate = Get-CimInstance Win32_Process -Filter "Name='DroneGCS.exe'" | Where-Object { $_.ExecutablePath -eq [IO.Path]::GetFullPath("$stagedBackend/DroneGCS.exe") } | Select-Object -First 1
        if ($candidate -and $candidate.CommandLine -match '--launcher-session ([0-9A-Fa-f_-]+)') {
            $stopControl = [Threading.EventWaitHandle]::OpenExisting("Local\DroneGCS.Managed.$($Matches[1]).stop")
            $controlBackend = Get-Process -Id $candidate.ProcessId
            [void]$controlBackend.Handle # Retain a native handle for ExitCode after owner exit.
            break
        }
        Start-Sleep -Milliseconds 100
    }
    Assert-True ($null -ne $stopControl) 'Full-control joint launch creates an owned visible backend'
    Assert-True ((Wait-Exit $controlRun 15000) -eq 0) 'Shipping-layout fake UE exits without stopping full-control backend'
    Assert-True (-not $controlBackend.HasExited) 'Control backend survives UE and launcher exit'
    $controlLog = (Get-ChildItem "$testLocalAppData/DroneGCS/Logs" -Recurse -Filter backend.log |
        Sort-Object LastWriteTime | Select-Object -Last 1).FullName
    $text = Get-Content -LiteralPath $controlLog -Raw
    Assert-True (Test-Path "$testLocalAppData/DroneGCS/Config/gcs-config.ini") 'Installed layout creates persistent per-user configuration'
    Assert-True (Test-Path "$testLocalAppData/DroneGCS/Config/config/DSPGCSTransport.ini") 'Installed layout creates persistent DSP transport configuration'
    Assert-True ($text -match 'Visible Console attached') 'Backend has real Console stdout, not a hidden redirected console'
    Assert-True (($text -match 'MAVLinkControlEnabled=true') -and ($text -match 'ROUTE_UPLOAD_ENABLED=true')) 'Production control and route-upload gates are enabled in isolated fixture'
    $frontendText = Get-Content -LiteralPath "$stagedRoot/frontend-result.txt" -Raw
    Assert-True ($frontendText -match 'DSP telemetry received') 'Full-control backend still publishes DSP telemetry'
    $sharedRoot = ($frontendText -split 'ArtifactWorkspace=')[1].Trim()
    $backendRoot = ([regex]::Match($text, 'ArtifactWorkspace=([^\r\n]+)')).Groups[1].Value
    Assert-True ([IO.Path]::GetFullPath($backendRoot) -eq [IO.Path]::GetFullPath($sharedRoot)) 'Both processes inherit identical per-user artifact workspace'
    $blocked = Start-TestProcess $stagedLauncher '--check' $env:TEMP
    Assert-True ((Wait-Exit $blocked) -ne 0) 'Surviving backend blocks duplicate start without being killed'
    $consoleStop = Start-TestProcess $stopOutput ([string]$controlBackend.Id) $fixtureRoot
    Assert-True ((Wait-Exit $consoleStop) -eq 0) 'Operator Ctrl+C delivered to isolated backend Console only'
    $controlExit = Wait-Exit $controlBackend
    Assert-True ($controlExit -eq 0) "Control backend releases resources normally on operator Ctrl+C (exit=$controlExit)"
} finally {
    if ($stopControl) {
        [void]$stopControl.Set() # Explicit test-owner shutdown; no real aircraft present.
        if ($controlBackend) { [void](Wait-Exit $controlBackend) }
        $stopControl.Dispose()
    }
}
Assert-True (-not (Get-Process -Name DroneGCS,DroneShowPlatform1,DroneShowPlatform1-Win64-Shipping -ErrorAction SilentlyContinue)) 'All isolated test children exit after explicit stop'
Write-Output "Test artifacts retained at: $fixture"
