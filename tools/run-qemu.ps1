param(
    [string]$KernelPath = "dist\\ntxkrnl.kpe",
    [string]$BootEfiPath = "dist\\BOOTX64.EFI",
    [string]$InitPath = "dist\\init.kpe",
    [string]$QemuPath = "",
    [string]$OvmfCodePath = "",
    [string]$OvmfVarsPath = "",
    [string]$RunDir = "build\\run",
    [string]$SerialLogPath = "build\\run\\serial.log",
    [int]$MemoryMb = 512,
    [int]$CpuCount = 2,
    [switch]$NoGraphics = $true
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Resolve-Tool {
    param(
        [string]$Configured,
        [string[]]$Candidates
    )

    if ($Configured -ne "") {
        return $Configured
    }

    foreach ($candidate in $Candidates) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($null -ne $cmd) {
            return $candidate
        }
    }

    throw "QEMU executable not found. Set -QemuPath."
}

function Resolve-FilePath {
    param(
        [string]$Configured,
        [string[]]$Candidates,
        [string]$Label
    )

    if ($Configured -ne "") {
        if (-not (Test-Path $Configured)) {
            throw "$Label not found: $Configured"
        }
        return (Resolve-Path $Configured).Path
    }

    foreach ($candidate in $Candidates) {
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "$Label not found. Set -$Label explicitly."
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
    $resolvedQemu = Resolve-Tool -Configured $QemuPath -Candidates @("qemu-system-x86_64", "qemu-system-x86_64.exe")
    $resolvedKernel = Resolve-FilePath -Configured $KernelPath -Candidates @() -Label "KernelPath"
    $resolvedBootEfi = Resolve-FilePath -Configured $BootEfiPath -Candidates @() -Label "BootEfiPath"
    $resolvedOvmfCode = Resolve-FilePath -Configured $OvmfCodePath -Candidates @(
        "C:\\Program Files\\qemu\\share\\edk2-x86_64-code.fd",
        "C:\\Program Files\\qemu\\share\\OVMF_CODE.fd",
        "C:\\Program Files\\qemu\\share\\OVMF.fd"
    ) -Label "OvmfCodePath"
    $resolvedOvmfVars = Resolve-FilePath -Configured $OvmfVarsPath -Candidates @(
        "C:\\Program Files\\qemu\\share\\edk2-i386-vars.fd",
        "C:\\Program Files\\qemu\\share\\OVMF_VARS.fd"
    ) -Label "OvmfVarsPath"

    $runRoot = Join-Path $repoRoot $RunDir
    $espRoot = Join-Path $runRoot "esp"
    $efiBootDir = Join-Path $espRoot "EFI\\BOOT"
    $kernelDir = Join-Path $espRoot "kernel"
    $systemDir = Join-Path $espRoot "System"
    $varsCopy = Join-Path $runRoot "ovmf_vars.fd"
    $serialLog = Join-Path $repoRoot $SerialLogPath

    if (Test-Path $runRoot) {
        Remove-Item -Recurse -Force $runRoot
    }

    Ensure-Dir $efiBootDir
    Ensure-Dir $kernelDir
    Ensure-Dir $systemDir
    Ensure-Dir ([System.IO.Path]::GetDirectoryName($serialLog))

    Copy-Item -Path $resolvedBootEfi -Destination (Join-Path $efiBootDir "BOOTX64.EFI")
    Copy-Item -Path $resolvedKernel -Destination (Join-Path $kernelDir "ntxkrnl.kpe")
    if (Test-Path $InitPath) {
        $resolvedInit = Resolve-Path $InitPath
        Copy-Item -Path $resolvedInit -Destination (Join-Path $systemDir "init.kpe")
    }
    Copy-Item -Path $resolvedOvmfVars -Destination $varsCopy

    $qemuArgs = @(
        "-machine", "q35,accel=tcg",
        "-m", "$MemoryMb",
        "-smp", "$CpuCount",
        "-drive", "if=pflash,format=raw,readonly=on,file=$resolvedOvmfCode",
        "-drive", "if=pflash,format=raw,file=$varsCopy",
        "-drive", "format=raw,file=fat:rw:$espRoot",
        "-serial", "file:$serialLog",
        "-monitor", "none",
        "-no-reboot"
    )

    if ($NoGraphics) {
        $qemuArgs += "-nographic"
    }

    Write-Host "$resolvedQemu $($qemuArgs -join ' ')"
    & $resolvedQemu @qemuArgs
    if ($LASTEXITCODE -ne 0) {
        throw "QEMU failed with exit code $LASTEXITCODE"
    }

    Write-Host "QEMU run complete. Serial log: $serialLog"
}
finally {
    Pop-Location
}
