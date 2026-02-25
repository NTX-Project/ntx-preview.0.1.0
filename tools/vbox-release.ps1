param(
    [string]$VmName = "NK-AUR64-DEBUG",
    [string]$VBoxManagePath = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe",
    [string]$ImagePath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-VBoxManage {
    param([string]$Configured)

    if ($Configured -and (Test-Path -LiteralPath $Configured)) {
        return (Resolve-Path -LiteralPath $Configured).Path
    }

    $candidates = @("VBoxManage.exe", "VBoxManage", $Configured)
    foreach ($candidate in $candidates) {
        if (-not $candidate) { continue }
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) {
            return $cmd.Source
        }
    }

    throw "VBoxManage not found. Set -VBoxManagePath."
}

function Invoke-VBoxOptional {
    param(
        [string]$VBoxManage,
        [string[]]$Args
    )

    $rendered = "$VBoxManage $($Args -join ' ')"
    Write-Host $rendered
    $output = & $VBoxManage @Args 2>&1
    if ($output) {
        $output | Out-Host
    }
}

function Test-VmExists {
    param([string]$VBoxManage, [string]$VmName)

    $out = & $VBoxManage list vms 2>&1
    if ($LASTEXITCODE -ne 0) {
        return $false
    }
    foreach ($line in $out) {
        if ($line -match '^"(.+)"\s+\{') {
            if ($matches[1] -eq $VmName) {
                return $true
            }
        }
    }
    return $false
}

function Get-VmState {
    param([string]$VBoxManage, [string]$VmName)

    $info = & $VBoxManage showvminfo $VmName --machinereadable 2>$null
    if ($LASTEXITCODE -ne 0) {
        return "notfound"
    }
    foreach ($line in $info) {
        if ($line -match '^VMState="([^"]+)"') {
            return $matches[1]
        }
    }
    return "unknown"
}

function Wait-VmPoweroff {
    param([string]$VBoxManage, [string]$VmName, [int]$TimeoutMs = 10000)

    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $state = Get-VmState -VBoxManage $VBoxManage -VmName $VmName
        if ($state -eq "poweroff" -or $state -eq "saved" -or $state -eq "notfound") {
            return
        }
        Start-Sleep -Milliseconds 250
    }
}

$vbox = Resolve-VBoxManage -Configured $VBoxManagePath

if (Test-VmExists -VBoxManage $vbox -VmName $VmName) {
    $state = Get-VmState -VBoxManage $vbox -VmName $VmName
    if ($state -eq "running" -or $state -eq "paused" -or $state -eq "stuck") {
        Invoke-VBoxOptional -VBoxManage $vbox -Args @("controlvm", $VmName, "poweroff")
        Wait-VmPoweroff -VBoxManage $vbox -VmName $VmName
    }

    foreach ($port in 0..5) {
        Invoke-VBoxOptional -VBoxManage $vbox -Args @(
            "storageattach", $VmName,
            "--storagectl", "SATA",
            "--port", "$port",
            "--device", "0",
            "--medium", "none"
        )
    }
}

if ($ImagePath -and (Test-Path -LiteralPath $ImagePath)) {
    $fullImage = (Resolve-Path -LiteralPath $ImagePath).Path
    Invoke-VBoxOptional -VBoxManage $vbox -Args @("closemedium", "disk", $fullImage)
}

Write-Host "[vbox-release] done"
