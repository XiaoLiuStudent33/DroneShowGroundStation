# Developer build hook only. Not distributed and never starts the backend/UE.
param([string]$ProjectDirectory = (Join-Path $PSScriptRoot '../../UnrealGCS'))
$ErrorActionPreference = 'Stop'
function Get-PayloadHash([string]$Path) {
    # UBT may inherit a PSModulePath from another PowerShell installation.
    # Use framework SHA256 directly instead of relying on module autoload.
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}
try {
    $project = (Resolve-Path -LiteralPath $ProjectDirectory).Path
    $repository = Split-Path -Parent $project
    $payload = Join-Path $project 'Intermediate/JointPayload'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $installation) { throw 'VS2022 C++ toolchain not found.' }
    $msbuild = Join-Path $installation 'MSBuild/Current/Bin/MSBuild.exe'
    foreach ($buildProject in @("$repository/ConsoleGCS/DroneGCS.vcxproj", "$PSScriptRoot/DroneGCSLauncher.vcxproj")) {
        & $msbuild $buildProject /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
        if ($LASTEXITCODE -ne 0) { throw "Joint-package build failed: $buildProject" }
    }
    New-Item -ItemType Directory -Path "$payload/config" -Force | Out-Null
    # Strict allow-list. No source, test binaries, developer INIs, or PDB files.
    $files = [ordered]@{
        'DroneGCS.exe' = "$repository/ConsoleGCS/x64/Release/DroneGCS.exe"
        'DroneGCSLauncher.exe' = "$PSScriptRoot/x64/Release/DroneGCSLauncher.exe"
        'gcs-config.ini' = "$PSScriptRoot/config/production.ini"
        'config/DSPGCSTransport.ini' = "$PSScriptRoot/config/DSPGCSTransport.ini"
        'Launcher-README.md' = "$PSScriptRoot/README.md"
    }
    $redistVersion = (Get-Content -LiteralPath "$installation/VC/Auxiliary/Build/Microsoft.VCRedistVersion.default.txt" -Raw).Trim()
    $crt = Join-Path $installation "VC/Redist/MSVC/$redistVersion/x64/Microsoft.VC143.CRT"
    foreach ($dll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $files[$dll] = Join-Path $crt $dll
    }
    $manifest = foreach ($entry in $files.GetEnumerator()) {
        if (-not (Test-Path -LiteralPath $entry.Value -PathType Leaf)) { throw "Missing payload dependency: $($entry.Value)" }
        $destination = Join-Path $payload $entry.Key
        Copy-Item -LiteralPath $entry.Value -Destination $destination -Force
        [ordered]@{ file = $entry.Key; sha256 = (Get-PayloadHash $destination) }
    }
    # Generated build metadata contains relative payload names, never source paths.
    [ordered]@{ schema = 2; builtUtc = [DateTime]::UtcNow.ToString('o'); files = @($manifest) } |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$payload/payload-manifest.json" -Encoding UTF8
    Write-Output "JOINT PAYLOAD READY: $payload (production binaries/config only; no application launched)"
    exit 0
} catch {
    Write-Error $_ -ErrorAction Continue
    exit 1
}
