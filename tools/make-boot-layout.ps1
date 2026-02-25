param(
    [string]$InputDistDir = "dist",
    [string]$OutputDir = "build\\layout",
    [string]$BootFileName = "BOOTX64.EFI",
    [string]$KernelInputName = "ntxkrnl.kpe",
    [string]$KernelOutputName = "nkxkrnl.kpe",
    [string]$InitInputName = "init.kpe",
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Ensure-Dir {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

$repoRoot = Resolve-RepoRoot
Push-Location $repoRoot
try {
    $distRoot = Join-Path $repoRoot $InputDistDir
    if (-not (Test-Path $distRoot)) {
        throw "Input dist directory not found: $distRoot"
    }

    $bootSrc = Join-Path $distRoot $BootFileName
    $kernelSrc = Join-Path $distRoot $KernelInputName
    if (-not (Test-Path $bootSrc)) {
        throw "Boot file not found: $bootSrc"
    }
    if (-not (Test-Path $kernelSrc)) {
        throw "Kernel file not found: $kernelSrc"
    }

    $layoutRoot = Join-Path $repoRoot $OutputDir
    if ($Clean -and (Test-Path $layoutRoot)) {
        Remove-Item -Recurse -Force $layoutRoot
    }

    $efiBootDir = Join-Path $layoutRoot "EFI\\BOOT"
    $systemDir = Join-Path $layoutRoot "System"
    Ensure-Dir $efiBootDir
    Ensure-Dir $systemDir

    $bootDst = Join-Path $efiBootDir "BOOTX64.EFI"
    $kernelDst = Join-Path $layoutRoot $KernelOutputName

    Copy-Item -Path $bootSrc -Destination $bootDst -Force
    Copy-Item -Path $kernelSrc -Destination $kernelDst -Force
    $initSrc = Join-Path $distRoot $InitInputName
    if (Test-Path $initSrc) {
        Copy-Item -Path $initSrc -Destination (Join-Path $systemDir "init.kpe") -Force
    }

    Write-Host "Layout ready: $layoutRoot"
    Write-Host " - \\EFI\\BOOT\\BOOTX64.EFI"
    Write-Host " - \\$KernelOutputName"
    if (Test-Path (Join-Path $systemDir "init.kpe")) {
        Write-Host " - \\System\\init.kpe"
    }
}
finally {
    Pop-Location
}
