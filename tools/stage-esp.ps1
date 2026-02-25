param(
    [string]$KernelPath = "dist\\ntxkrnl.kpe",
    [string]$BootEfiPath = "dist\\BOOTX64.EFI",
    [string]$InitPath = "dist\\init.kpe",
    [string]$StageDir = "build\\esp_stage"
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
    $kernelFull = Resolve-Path $KernelPath
    $bootEfiFull = Resolve-Path $BootEfiPath
    $stageRoot = Join-Path $repoRoot $StageDir
    $efiBoot = Join-Path $stageRoot "EFI\\BOOT"
    $kernelDir = Join-Path $stageRoot "kernel"
    $systemDir = Join-Path $stageRoot "System"

    if (Test-Path $stageRoot) {
        Remove-Item -Recurse -Force $stageRoot
    }

    Ensure-Dir $efiBoot
    Ensure-Dir $kernelDir
    Ensure-Dir $systemDir
    Copy-Item -Path $bootEfiFull -Destination (Join-Path $efiBoot "BOOTX64.EFI")
    Copy-Item -Path $kernelFull -Destination (Join-Path $kernelDir "ntxkrnl.kpe")
    if (Test-Path $InitPath) {
        $initFull = Resolve-Path $InitPath
        Copy-Item -Path $initFull -Destination (Join-Path $systemDir "init.kpe")
    }

    Write-Host "ESP staging complete: $stageRoot"
}
finally {
    Pop-Location
}
