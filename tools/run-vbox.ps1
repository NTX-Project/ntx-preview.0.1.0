param(
    [string]$VmName = "NK-AUR64-DEBUG",
    [string]$EspImagePath = "",
    [string]$VBoxManagePath = "",
    [string]$SerialLogPath = "build\\run-vbox\\serial.log",
    [string]$SerialPipeName = "\\.\\pipe\\nkdbg",
    [int]$MemoryMb = 1024,
    [int]$CpuCount = 1,
    [switch]$Recreate,
    [string]$Headless = "true",
    [switch]$UseSerialPipe,
    [switch]$KeepNetworkOff = $true,
    [switch]$ForcePowerOff
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"
if (Get-Variable -Name PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Resolve-VBoxManage {
    param([string]$Configured)

    if ($Configured -ne "") {
        $normalized = Normalize-PathText -Raw $Configured
        if ($normalized -eq "") {
            throw "Invalid -VBoxManagePath."
        }
        if (Test-Path -LiteralPath $normalized) {
            return (Resolve-Path -LiteralPath $normalized).Path
        }
        $cmd = Get-Command $normalized -ErrorAction SilentlyContinue
        if ($null -ne $cmd) {
            return $cmd.Source
        }
        throw "VBoxManage not found at: $normalized"
    }

    $candidates = @(
        "VBoxManage",
        "VBoxManage.exe",
        "C:\\Program Files\\Oracle\\VirtualBox\\VBoxManage.exe"
    )

    foreach ($candidate in $candidates) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($null -ne $cmd) {
            return $cmd.Source
        }
        if (Test-Path $candidate) {
            return (Resolve-Path $candidate).Path
        }
    }

    throw "VBoxManage not found. Set -VBoxManagePath."
}

function Invoke-VBox {
    param(
        [string]$VBoxManage,
        [string[]]$ArgumentList,
        [int]$RetryCount = 40
    )

    $rendered = "$VBoxManage $($ArgumentList -join ' ')"
    $attempt = 0
    while ($attempt -lt $RetryCount) {
        $attempt++
        Write-Host $rendered
        $prevEA = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        try {
            $output = & $VBoxManage @ArgumentList 2>&1
        }
        finally {
            $ErrorActionPreference = $prevEA
        }
        if ($output) {
            $output | Out-Host
        }
        if ($LASTEXITCODE -eq 0) {
            return
        }

        $joined = ($output | ForEach-Object { "$_" }) -join "`n"
        $isLockState = Test-VBoxLockError -Text $joined
        if (-not $isLockState -or $attempt -ge $RetryCount) {
            throw "VBoxManage failed ($LASTEXITCODE): $rendered"
        }
        Start-Sleep -Milliseconds 750
    }
}

function Invoke-VBoxOptional {
    param(
        [string]$VBoxManage,
        [string[]]$ArgumentList
    )

    $rendered = "$VBoxManage $($ArgumentList -join ' ')"
    Write-Host $rendered
    $prevEA = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $output = & $VBoxManage @ArgumentList 2>&1
    }
    finally {
        $ErrorActionPreference = $prevEA
    }
    if ($output) {
        $output | Out-Host
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[warn] optional VBox setting failed, continuing: $($ArgumentList -join ' ')"
    }
}

function Test-VmExists {
    param(
        [string]$VBoxManage,
        [string]$VmName
    )

    $output = & $VBoxManage list vms
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to list VMs."
    }

    foreach ($line in $output) {
        if ($line -match '^"(.+)"\s+\{') {
            if ($matches[1] -eq $VmName) {
                return $true
            }
        }
    }
    return $false
}

function Get-VmState {
    param(
        [string]$VBoxManage,
        [string]$VmName
    )

    try {
        $info = & $VBoxManage showvminfo $VmName --machinereadable 2>$null
    }
    catch {
        return "notfound"
    }
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

function Ensure-Dir {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Close-VBoxMediumOptional {
    param(
        [string]$VBoxManage,
        [string]$MediumRef
    )

    if ([string]::IsNullOrWhiteSpace($MediumRef)) {
        return
    }

    Invoke-VBoxOptional -VBoxManage $VBoxManage -ArgumentList @(
        "closemedium", "disk", $MediumRef
    )
}

function Wait-ForVmPoweroff {
    param(
        [string]$VBoxManage,
        [string]$VmName,
        [int]$TimeoutMs = 8000
    )

    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline) {
        $state = Get-VmState -VBoxManage $VBoxManage -VmName $VmName
        if ($state -eq "poweroff" -or $state -eq "saved" -or $state -eq "notfound") {
            return
        }
        Start-Sleep -Milliseconds 200
    }
}

function Test-VBoxLockError {
    param([string]$Text)

    if ($null -eq $Text) {
        return $false
    }

    return ($Text -match "already locked for a session" -or
            $Text -match "being unlocked" -or
            $Text -match "while it is locked" -or
            $Text -match "VBOX_E_INVALID_OBJECT_STATE" -or
            $Text -match "VBOX_E_OBJECT_IN_USE")
}

function Unregister-VmWithRetry {
    param(
        [string]$VBoxManage,
        [string]$VmName,
        [int]$Attempts = 40
    )

    $attempt = 0
    while ($attempt -lt $Attempts) {
        $attempt++
        $stdoutPath = Join-Path $env:TEMP ("nk_vbox_unreg_out_{0}.log" -f [guid]::NewGuid().ToString("N"))
        $stderrPath = Join-Path $env:TEMP ("nk_vbox_unreg_err_{0}.log" -f [guid]::NewGuid().ToString("N"))

        $proc = Start-Process -FilePath $VBoxManage `
                              -ArgumentList @("unregistervm", $VmName, "--delete") `
                              -NoNewWindow `
                              -PassThru `
                              -Wait `
                              -RedirectStandardOutput $stdoutPath `
                              -RedirectStandardError $stderrPath

        $output = @()
        if (Test-Path $stdoutPath) {
            $output += Get-Content -LiteralPath $stdoutPath -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $stdoutPath -Force -ErrorAction SilentlyContinue
        }
        if (Test-Path $stderrPath) {
            $output += Get-Content -LiteralPath $stderrPath -ErrorAction SilentlyContinue
            Remove-Item -LiteralPath $stderrPath -Force -ErrorAction SilentlyContinue
        }

        if ($proc.ExitCode -eq 0) {
            return $true
        }

        $joined = ($output | ForEach-Object { "$_" }) -join "`n"
        if (-not (Test-VBoxLockError -Text $joined)) {
            throw "VBoxManage failed ($($proc.ExitCode)): $VBoxManage unregistervm $VmName --delete"
        }
        Start-Sleep -Milliseconds 750
    }

    return $false
}

function Normalize-PathText {
    param([string]$Raw)

    if ($null -eq $Raw) {
        return ""
    }

    $value = $Raw.Trim()
    $value = $value.Trim('"')
    $value = $value.Trim("'")
    $value = $value -replace "[\r\n\t]", ""
    return $value
}

function Parse-BoolArg {
    param(
        [string]$Value,
        [bool]$Default = $false
    )

    if ($null -eq $Value) {
        return $Default
    }

    $text = $Value.Trim().ToLowerInvariant()
    if ($text -eq "") {
        return $Default
    }

    switch -Regex ($text) {
        '^(1|true|\$true|yes|on)$'  { return $true }
        '^(0|false|\$false|no|off)$' { return $false }
        default { throw "Invalid boolean value '$Value'. Use true/false or 1/0." }
    }
}

$repoRoot = Resolve-RepoRoot
Push-Location $repoRoot
try {
    if ($EspImagePath -eq "") {
        throw "Set -EspImagePath to an EFI disk image containing EFI\\BOOT\\BOOTX64.EFI and kernel\\ntxkrnl.kpe."
    }

    $vbox = Resolve-VBoxManage -Configured $VBoxManagePath
    $normalizedEsp = Normalize-PathText -Raw $EspImagePath
    if ($normalizedEsp -eq "") {
        throw "Set -EspImagePath to a valid EFI disk image path."
    }
    if (-not (Test-Path $normalizedEsp)) {
        $resolvedMissing = if ([System.IO.Path]::IsPathRooted($normalizedEsp)) {
            $normalizedEsp
        } else {
            Join-Path $repoRoot $normalizedEsp
        }

        $candidateDir = Split-Path -Path $resolvedMissing -Parent
        if ($candidateDir -and (Test-Path -LiteralPath $candidateDir)) {
            $fallback = Get-ChildItem -LiteralPath $candidateDir -File -Filter "*.vhd" -ErrorAction SilentlyContinue |
                Where-Object { $_.Name -like "nk-esp*" } |
                Sort-Object LastWriteTime -Descending |
                Select-Object -First 1
            if ($null -ne $fallback) {
                Write-Warning "ESP image not found at requested path: $normalizedEsp"
                Write-Warning "Using newest available ESP image instead: $($fallback.FullName)"
                $normalizedEsp = $fallback.FullName
            } else {
                throw "ESP image not found: $normalizedEsp"
            }
        } else {
            throw "ESP image not found: $normalizedEsp"
        }
    }
    $espImage = (Resolve-Path -LiteralPath $normalizedEsp).Path
    $serialLog = Join-Path $repoRoot $SerialLogPath
    Ensure-Dir ([System.IO.Path]::GetDirectoryName($serialLog))

    if ((Test-VmExists -VBoxManage $vbox -VmName $VmName) -and $Recreate) {
        $state = Get-VmState -VBoxManage $vbox -VmName $VmName
        if ($state -eq "running" -or $state -eq "paused" -or $state -eq "stuck") {
            Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("controlvm", $VmName, "poweroff")
            Wait-ForVmPoweroff -VBoxManage $vbox -VmName $VmName
        }
        $removed = Unregister-VmWithRetry -VBoxManage $vbox -VmName $VmName
        if (-not $removed) {
            Write-Warning "Failed to unregister VM '$VmName' after retries (still locked). Continuing without recreate."
            Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("controlvm", $VmName, "poweroff")
            Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("discardstate", $VmName)
            Wait-ForVmPoweroff -VBoxManage $vbox -VmName $VmName
        }
    }

    if (-not (Test-VmExists -VBoxManage $vbox -VmName $VmName)) {
        Invoke-VBox -VBoxManage $vbox -ArgumentList @(
            "createvm",
            "--name", $VmName,
            "--platform-architecture", "x86",
            "--ostype", "Other_64",
            "--register"
        )
    }

    $state = Get-VmState -VBoxManage $vbox -VmName $VmName
    if (($state -eq "running" -or $state -eq "paused" -or $state -eq "stuck") -and $ForcePowerOff) {
        Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("controlvm", $VmName, "poweroff")
        Wait-ForVmPoweroff -VBoxManage $vbox -VmName $VmName
    }

    if ($UseSerialPipe) {
        $uartMode = @("--uart-mode1", "server", $SerialPipeName)
    } else {
        $uartMode = @("--uart-mode1", "file", $serialLog)
    }

    # Core deterministic debug profile.
    Invoke-VBox -VBoxManage $vbox -ArgumentList @(
        "modifyvm", $VmName,
        "--firmware", "efi",
        "--firmware-apic", "apic",
        "--firmware-logo-fade-in", "off",
        "--firmware-logo-fade-out", "off",
        "--firmware-logo-display-time", "1",
        "--firmware-boot-menu", "disabled",
        "--chipset", "ich9",
        "--memory", "$MemoryMb",
        "--cpus", "$CpuCount",
        "--apic", "on",
        "--ioapic", "on",
        "--x86-hpet", "on",
        "--rtc-use-utc", "on",
        "--paravirt-provider", "none",
        "--nested-paging", "off",
        "--large-pages", "off",
        "--x86-vtx-vpid", "off",
        "--x86-x2apic", "on",
        "--boot1", "disk",
        "--boot2", "none",
        "--boot3", "none",
        "--boot4", "none",
        "--mouse", "ps2",
        "--keyboard", "ps2",
        "--clipboard-mode", "disabled",
        "--drag-and-drop", "disabled",
        "--uart1", "0x3F8", "4"
    )

    Invoke-VBox -VBoxManage $vbox -ArgumentList (@("modifyvm", $VmName) + $uartMode)

    # Optional noise reduction knobs (VBox version dependent).
    Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("modifyvm", $VmName, "--audio-enabled", "off")
    Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("modifyvm", $VmName, "--usb-ohci", "off")
    Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("modifyvm", $VmName, "--usb-ehci", "off")
    Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("modifyvm", $VmName, "--usb-xhci", "off")
    Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("modifyvm", $VmName, "--triple-fault-reset", "off")

    if ($KeepNetworkOff) {
        Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @("modifyvm", $VmName, "--nic1", "none")
    }

    $controllers = & $vbox showvminfo $VmName --machinereadable
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to query VM info."
    }
    if (-not ($controllers -match 'storagecontrollername0="SATA"')) {
        Invoke-VBox -VBoxManage $vbox -ArgumentList @(
            "storagectl", $VmName,
            "--name", "SATA",
            "--add", "sata",
            "--controller", "IntelAhci"
        )
    }

    # Detach any previous medium on SATA:0:0 first.
    Invoke-VBoxOptional -VBoxManage $vbox -ArgumentList @(
        "storageattach", $VmName,
        "--storagectl", "SATA",
        "--port", "0",
        "--device", "0",
        "--medium", "none"
    )

    # Clear stale media-registry entries for this VHD path before reattach.
    Close-VBoxMediumOptional -VBoxManage $vbox -MediumRef $espImage

    Invoke-VBox -VBoxManage $vbox -ArgumentList @(
        "storageattach", $VmName,
        "--storagectl", "SATA",
        "--port", "0",
        "--device", "0",
        "--type", "hdd",
        "--medium", $espImage
    )

    $state = Get-VmState -VBoxManage $vbox -VmName $VmName
    if ($state -eq "running" -or $state -eq "paused") {
        throw "VM '$VmName' is already running. Stop it first or use -ForcePowerOff."
    }

    $headlessMode = Parse-BoolArg -Value $Headless -Default $true
    $startType = if ($headlessMode) { "headless" } else { "gui" }
    Invoke-VBox -VBoxManage $vbox -ArgumentList @("startvm", $VmName, "--type", $startType)

    Write-Host "VirtualBox debug VM started: $VmName"
    if ($UseSerialPipe) {
        Write-Host "Serial pipe (server): $SerialPipeName"
    } else {
        Write-Host "Serial log: $serialLog"
    }
    Write-Host "ESP image: $espImage"
}
finally {
    Pop-Location
}
