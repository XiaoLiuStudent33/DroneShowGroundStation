param(
    [Parameter(Mandatory = $true)][string]$PackageRoot,
    [switch]$SkipBuild
)
$ErrorActionPreference = 'Stop'
$repository = (Resolve-Path "$PSScriptRoot/../..").Path
$package = (Resolve-Path -LiteralPath $PackageRoot).Path
if (-not (Test-Path -LiteralPath "$package/Windows/DroneShowPlatform1/Binaries/Win64/DroneShowPlatform1.exe") -or
    -not (Test-Path -LiteralPath "$package/Backend/gcs-config.ini")) {
    throw 'Expected an existing paired Windows/ and Backend/ deployment. No files changed.'
}
if (Get-Process -Name DroneGCS,DroneShowPlatform1,DroneGCSLauncher -ErrorAction SilentlyContinue) {
    throw 'Close existing UE, Console and launcher normally before deployment. No processes will be stopped.'
}
if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $installation) { throw 'Visual Studio MSBuild not found.' }
    $msbuild = Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe'
    & $msbuild "$repository/ConsoleGCS/DroneGCS.vcxproj" /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Backend build failed; package unchanged.' }
    & $msbuild "$PSScriptRoot/DroneGCSLauncher.vcxproj" /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Launcher build failed; package unchanged.' }
}
$backendSource = "$repository/ConsoleGCS/x64/Release/DroneGCS.exe"
$launcherSource = "$PSScriptRoot/x64/Release/DroneGCSLauncher.exe"
foreach ($file in @($backendSource, $launcherSource)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing build output: $file" }
}
# Freeze hashes before staging. Saved files are included because the apps are stopped.
$windowsBefore = @(Get-ChildItem -LiteralPath "$package/Windows" -Recurse -File | Get-FileHash -Algorithm SHA256)
$configBefore = @(Get-ChildItem -LiteralPath "$package/Backend" -Recurse -File -Filter '*.ini' | Get-FileHash -Algorithm SHA256)
$backup = Join-Path $package ('Backups/BeforeLauncher-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0,8))
New-Item -ItemType Directory -Path $backup | Out-Null
Copy-Item -LiteralPath "$package/Backend" -Destination "$backup/Backend" -Recurse
foreach ($item in @('DroneGCSLauncher.exe', 'Launcher-README.md')) {
    if (Test-Path -LiteralPath "$package/$item") { Copy-Item -LiteralPath "$package/$item" -Destination $backup }
}
foreach ($original in (Get-ChildItem -LiteralPath "$package/Backend" -Recurse -File)) {
    $relative = $original.FullName.Substring(("$package\Backend").Length).TrimStart('\')
    if ((Get-FileHash -LiteralPath $original.FullName).Hash -ne (Get-FileHash -LiteralPath "$backup/Backend/$relative").Hash) {
        throw 'Backup verification failed; package binaries unchanged.'
    }
}
Copy-Item -LiteralPath $backendSource -Destination "$package/Backend/DroneGCS.exe" -Force
Copy-Item -LiteralPath $launcherSource -Destination "$package/DroneGCSLauncher.exe" -Force
Copy-Item -LiteralPath "$PSScriptRoot/README.md" -Destination "$package/Launcher-README.md" -Force
foreach ($original in @($windowsBefore) + @($configBefore)) {
    if ((Get-FileHash -LiteralPath $original.Path).Hash -ne $original.Hash) {
        throw "Unexpected package/config change: $($original.Path). Backup: $backup"
    }
}
$check = Start-Process -FilePath "$package/DroneGCSLauncher.exe" -ArgumentList '--check' -WorkingDirectory $env:TEMP -PassThru -WindowStyle Hidden
if (-not $check.WaitForExit(10000)) { throw 'Preflight did not exit. Inspect Logs; no process was killed.' }
$check.Refresh()
if ($check.ExitCode -ne 0) { throw "Preflight failed. Inspect $package/Logs. Original backend retained at: $backup" }
Write-Output "DEPLOYED: $package/DroneGCSLauncher.exe"
Write-Output "BACKUP: $backup"
Write-Output "VERIFIED: $($windowsBefore.Count) UE files and $($configBefore.Count) INI files unchanged; preflight passed."
