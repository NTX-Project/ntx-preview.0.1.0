param(
    [string]$OutDistDir = "dist29",
    [string]$BuildDir = "",
    [string]$VmName = "NK-AUR64-DEBUG",
    [string]$VBoxManagePath = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe",
    [string]$ImagePath = "",
    [switch]$FreshImage = $true,
    [switch]$TailSerial
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Invoke-Step {
    param(
        [string]$Name,
        [scriptblock]$Action
    )
    Write-Host ("[dev-vm] " + $Name)
    & $Action
    if ($LASTEXITCODE -ne 0) {
        throw "[dev-vm] step failed with exit code ${LASTEXITCODE}: $Name"
    }
}

$repoRoot = Resolve-RepoRoot
Push-Location $repoRoot
try {
    if ($ImagePath -eq "") {
        if ($FreshImage) {
            $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
            $ImagePath = Join-Path $repoRoot ("build\run-vbox\nk-esp-" + $stamp + ".vhd")
        } else {
            $ImagePath = Join-Path $repoRoot "build\run-vbox\nk-esp.vhd"
        }
    }

    Invoke-Step -Name "Build NK (kernel/user/boot)" -Action {
        $nkccArgs = @(
            "-ExecutionPolicy", "Bypass",
            "-File", ".\tools\nkcc.ps1",
            "-OutDir", $OutDistDir
        )
        if ($BuildDir -ne "") {
            $nkccArgs += @("-BuildDir", $BuildDir)
        }
        powershell @nkccArgs
    }

    Invoke-Step -Name "Release VBox locks" -Action {
        powershell -ExecutionPolicy Bypass -File ".\tools\vbox-release.ps1" `
            -VmName $VmName `
            -VBoxManagePath $VBoxManagePath `
            -ImagePath $ImagePath
    }

    Invoke-Step -Name "Build ESP image" -Action {
        powershell -ExecutionPolicy Bypass -File ".\tools\make-esp-image.ps1" `
            -InputDistDir $OutDistDir `
            -OutputImagePath $ImagePath `
            -Clean
    }

    Invoke-Step -Name "Launch VBox VM" -Action {
        powershell -ExecutionPolicy Bypass -File ".\tools\run-vbox.ps1" `
            -VmName $VmName `
            -EspImagePath $ImagePath `
            -VBoxManagePath $VBoxManagePath `
            -Headless `
            -KeepNetworkOff `
            -ForcePowerOff `
            -Recreate
    }

    Write-Host "[dev-vm] done"
    Write-Host ("[dev-vm] image: " + $ImagePath)
    Write-Host ("[dev-vm] serial: " + (Join-Path $repoRoot "build\run-vbox\serial.log"))

    if ($TailSerial) {
        Get-Content (Join-Path $repoRoot "build\run-vbox\serial.log") -Wait
    }
}
finally {
    Pop-Location
}
