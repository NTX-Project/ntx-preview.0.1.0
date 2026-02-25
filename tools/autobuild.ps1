param(
    [string]$OutDistDir = "dist29",
    [string]$BuildDir = "",
    [string]$VmName = "NK-AUR64-DEBUG",
    [string]$VBoxManagePath = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe",
    [switch]$NoCleanup,
    [switch]$TailSerial
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Push-Location $repoRoot
try {
    if (-not $NoCleanup) {
        powershell -ExecutionPolicy Bypass -File ".\tools\cleanup-vm-drives.ps1"
    }

    $devVmArgs = @(
        "-ExecutionPolicy", "Bypass",
        "-File", ".\tools\dev-vm.ps1",
        "-OutDistDir", $OutDistDir,
        "-VmName", $VmName,
        "-VBoxManagePath", $VBoxManagePath,
        "-FreshImage"
    )
    if ($BuildDir -ne "") {
        $devVmArgs += @("-BuildDir", $BuildDir)
    }
    if ($TailSerial.IsPresent) {
        $devVmArgs += "-TailSerial"
    }
    powershell @devVmArgs
}
finally {
    Pop-Location
}
