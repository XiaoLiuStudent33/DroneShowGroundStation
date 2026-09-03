[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PackageRoot,
    [ValidatePattern('^\d+\.\d+\.\d+(\.\d+)?$')]
    [string]$Version = '1.0.0',
    [string]$OutputDirectory = '',
    [switch]$AllowDevelopment,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $scriptDirectory 'Output'
}

function Require-File([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Missing $Description`: $Path"
    }
}

function Require-Directory([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "Missing $Description`: $Path"
    }
}

function Get-SHA256Hex([string]$Path) {
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}

function Get-NormalizedTextSHA256Hex([string]$Path) {
    $text = [IO.File]::ReadAllText($Path)
    $normalized = $text.Replace("`r`n", "`n").Replace("`r", "`n")
    $bytes = [Text.UTF8Encoding]::new($false).GetBytes($normalized)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($bytes)).Replace('-', '') }
    finally { $algorithm.Dispose() }
}

$resolvedRoot = (Resolve-Path -LiteralPath $PackageRoot).Path
if (Test-Path -LiteralPath (Join-Path $resolvedRoot 'Windows') -PathType Container) {
    $stageWindows = Join-Path $resolvedRoot 'Windows'
} elseif ((Split-Path -Leaf $resolvedRoot) -ieq 'Windows') {
    $stageWindows = $resolvedRoot
} else {
    throw 'PackageRoot must be the UE package output root or its Windows directory.'
}
$stageWindows = [IO.Path]::GetFullPath($stageWindows)
$projectRoot = Join-Path $stageWindows 'DroneShowPlatform1'
$binaryRoot = Join-Path $projectRoot 'Binaries\Win64'
$backendRoot = Join-Path $binaryRoot 'Backend'

Require-Directory (Join-Path $stageWindows 'Engine') 'UE Engine runtime directory'
Require-Directory (Join-Path $projectRoot 'Content\Paks') 'UE Paks directory'
Require-File (Join-Path $projectRoot 'DroneGCSLauncher.exe') 'joint launcher'
Require-File (Join-Path $backendRoot 'DroneGCS.exe') 'Console backend'
Require-File (Join-Path $backendRoot 'gcs-config.ini') 'production configuration'
Require-File (Join-Path $backendRoot 'config\DSPGCSTransport.ini') 'DSP-GCS configuration'
Require-File (Join-Path $backendRoot 'payload-manifest.json') 'payload manifest'
foreach ($runtime in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    Require-File (Join-Path $backendRoot $runtime) "VC143 runtime $runtime"
}

$shippingExe = Join-Path $binaryRoot 'DroneShowPlatform1-Win64-Shipping.exe'
$developmentExe = Join-Path $binaryRoot 'DroneShowPlatform1.exe'
$hasShipping = Test-Path -LiteralPath $shippingExe -PathType Leaf
$hasDevelopment = Test-Path -LiteralPath $developmentExe -PathType Leaf
if ($hasShipping -eq $hasDevelopment) {
    throw 'Exactly one actual Game EXE (Shipping or Development) is required. Package into a new empty directory.'
}
if ($hasDevelopment -and -not $AllowDevelopment) {
    throw 'A release installer requires a Shipping build. Use -AllowDevelopment -ValidateOnly only for structural testing.'
}

$sourceExtensions = @('.cpp', '.c', '.h', '.hpp', '.vcxproj', '.sln', '.uproject')
$sourceFiles = Get-ChildItem -LiteralPath $stageWindows -Recurse -File | Where-Object {
    $sourceExtensions -contains $_.Extension.ToLowerInvariant()
} | Select-Object -First 10
if ($sourceFiles) {
    $names = ($sourceFiles.FullName -join [Environment]::NewLine)
    throw "Source or developer project files were found in the staged package:`n$names"
}

$manifest = Get-Content -LiteralPath (Join-Path $backendRoot 'payload-manifest.json') -Raw | ConvertFrom-Json
$expectedPayload = @(
    'DroneGCS.exe', 'DroneGCSLauncher.exe', 'gcs-config.ini',
    'config/DSPGCSTransport.ini', 'Launcher-README.md',
    'msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll'
)
$declaredPayload = @($manifest.files.file)
if ($manifest.schema -ne 2 -or $declaredPayload.Count -ne $expectedPayload.Count -or
    @($expectedPayload | Where-Object { $_ -notin $declaredPayload }).Count -ne 0) {
    throw 'payload-manifest.json does not contain the exact release payload allow-list.'
}
foreach ($entry in $manifest.files) {
    $relative = [string]$entry.file
    if ($relative -in @('DroneGCSLauncher.exe', 'Launcher-README.md')) {
        $payloadFile = Join-Path $projectRoot $relative
    } else {
        $payloadFile = Join-Path $backendRoot ($relative -replace '/', '\')
    }
    Require-File $payloadFile "manifest payload $relative"
    $actualHash = Get-SHA256Hex $payloadFile
    if ($actualHash -ine [string]$entry.sha256) {
        $textOnlyDifference = $relative -match '\.(ini|md)$' -and
            (Get-NormalizedTextSHA256Hex $payloadFile) -ieq [string]$entry.sha256
        if ($textOnlyDifference) {
            Write-Verbose "Accepted staged newline normalization: $relative"
        } elseif ($AllowDevelopment -and $ValidateOnly) {
            Write-Warning "Development validation: staged payload hash differs from its build manifest: $relative"
        } else {
            throw "Staged payload hash mismatch: $relative"
        }
    }
}

$buildKind = if ($hasShipping) { 'Shipping' } else { 'Development (validation only)' }
Write-Host "Validation passed: $buildKind"
Write-Host "Staged Windows directory: $stageWindows"
if ($ValidateOnly) {
    Write-Host 'Validate-only mode: no installer was created and the staged package was not modified.'
    return
}

$compilerCandidates = @()
if ($env:NSIS_COMPILER) { $compilerCandidates += $env:NSIS_COMPILER }
$command = Get-Command makensis.exe -ErrorAction SilentlyContinue
if ($command) { $compilerCandidates += $command.Source }
$compilerCandidates += @(
    'D:\NSIS\makensis.exe',
    (Join-Path ${env:ProgramFiles(x86)} 'NSIS\makensis.exe'),
    (Join-Path $env:ProgramFiles 'NSIS\makensis.exe')
)
$makensis = $compilerCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
if (-not $makensis) {
    throw 'NSIS compiler makensis.exe was not found. Install NSIS 3 or set NSIS_COMPILER.'
}

$resolvedOutput = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $resolvedOutput -Force | Out-Null
$script = Join-Path $scriptDirectory 'DroneGCS.nsi'
$versionParts = @($Version.Split('.'))
while ($versionParts.Count -lt 4) { $versionParts += '0' }
$versionInfoVersion = $versionParts -join '.'
$arguments = @(
    '/V3',
    "/DStageWindows=$stageWindows",
    "/DAppVersion=$Version",
    "/DVersionInfoVersion=$versionInfoVersion",
    "/DOutputDir=$resolvedOutput",
    $script
)
& $makensis @arguments
if ($LASTEXITCODE -ne 0) { throw "NSIS compilation failed with exit code $LASTEXITCODE." }

$installer = Join-Path $resolvedOutput "DroneGCS-Setup-$Version-x64.exe"
Require-File $installer 'installer output'
$hash = (Get-SHA256Hex $installer).ToLowerInvariant()
$hashFile = "$installer.sha256"
Set-Content -LiteralPath $hashFile -Encoding ascii -Value "$hash  $([IO.Path]::GetFileName($installer))"
Write-Host "Installer: $installer"
Write-Host "SHA-256: $hash"
Write-Host "Checksum file: $hashFile"
