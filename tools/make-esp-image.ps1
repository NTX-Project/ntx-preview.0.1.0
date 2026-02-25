param(
    [string]$InputDistDir = "dist",
    [string]$OutputImagePath = "build\run-vbox\nk-esp.vhd",
    [int]$SizeMb = 128,
    [string]$TempLayoutDir = "build\esp_image_layout",
    [switch]$Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Ensure-Dir {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Convert-ToFileOpsRoot {
    param([string]$RootPath)

    $normalizedRoot = ([string]$RootPath).Trim().Trim('"').Trim("'")
    if ($normalizedRoot.StartsWith("\\?\Volume{", [System.StringComparison]::OrdinalIgnoreCase)) {
        if (-not $normalizedRoot.EndsWith("\")) {
            $normalizedRoot = $normalizedRoot + "\"
        }

        $name = "NKESP"
        $index = 0
        while ($true) {
            $candidate = if ($index -eq 0) { $name } else { "$name$index" }
            if (-not (Get-PSDrive -Name $candidate -ErrorAction SilentlyContinue)) {
                New-PSDrive -Name $candidate -PSProvider FileSystem -Root $normalizedRoot -Scope Script | Out-Null
                return @{
                    Root = "$candidate`:\"
                    PsDriveName = $candidate
                }
            }
            $index++
            if ($index -gt 99) {
                throw "Failed to allocate temporary PSDrive for ESP volume path."
            }
        }
    }

    return @{
        Root = $normalizedRoot
        PsDriveName = $null
    }
}

function Ensure-VirtualDiskService {
    try {
        $svc = Get-Service -Name "vds" -ErrorAction Stop
    }
    catch {
        return
    }

    if ($svc.Status -ne "Running") {
        try {
            Start-Service -Name "vds" -ErrorAction Stop
        }
        catch {
            Write-Warning "Virtual Disk service (vds) is not running and could not be started."
            Write-Warning "Run this script from an elevated PowerShell session."
        }
    }
}

function Get-SafeLastExitCode {
    $var = Get-Variable -Name LASTEXITCODE -ErrorAction SilentlyContinue
    if ($null -eq $var) {
        if ($?) {
            return 0
        }
        return 1
    }
    return [int]$var.Value
}

function Try-DetachVdiskViaDiskPart {
    param([string]$ImagePath, [string]$WorkDir)

    $dpDetach = Join-Path $WorkDir "diskpart_force_detach.txt"
    @(
        "select vdisk file=`"$ImagePath`"",
        "detach vdisk",
        "exit"
    ) | Set-Content -Path $dpDetach -Encoding ASCII

    $output = diskpart /s $dpDetach 2>&1
    $output | Out-Host
}

function Try-ReleaseViaVBox {
    param([string]$ImagePath)

    $vbox = Get-Command "VBoxManage.exe" -ErrorAction SilentlyContinue
    if ($null -eq $vbox) {
        $vbox = Get-Command "VBoxManage" -ErrorAction SilentlyContinue
    }
    if ($null -eq $vbox) {
        return
    }

    & $vbox.Source closemedium disk $ImagePath 2>&1 | Out-Host
}

function Remove-FileWithUnlock {
    param([string]$ImagePath, [string]$WorkDir)

    if (-not (Test-Path $ImagePath)) {
        return $ImagePath
    }

    $attempt = 0
    while ($attempt -lt 6) {
        $attempt++
        try {
            Remove-Item -Force $ImagePath
            return $ImagePath
        }
        catch {
            Try-ReleaseViaVBox -ImagePath $ImagePath
            Try-DetachVdiskViaDiskPart -ImagePath $ImagePath -WorkDir $WorkDir
            Start-Sleep -Milliseconds 400
        }
    }

    $baseName = [System.IO.Path]::GetFileNameWithoutExtension($ImagePath)
    $ext = [System.IO.Path]::GetExtension($ImagePath)
    $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
    $fallbackPath = Join-Path $WorkDir ("{0}-{1}{2}" -f $baseName, $stamp, $ext)
    Write-Warning "Failed to remove locked image: $ImagePath"
    Write-Warning "Falling back to new image path: $fallbackPath"
    return $fallbackPath
}

function Get-DriveLettersInUse {
    $letters = New-Object System.Collections.Generic.HashSet[string]
    Get-Volume -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.DriveLetter) {
            [void]$letters.Add(([string]$_.DriveLetter).ToUpperInvariant())
        }
    }
    Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Name -match '^[A-Za-z]$') {
            [void]$letters.Add(([string]$_.Name).ToUpperInvariant())
        }
    }
    return $letters
}

function Get-FreeDriveLetterCandidates {
    $inUse = Get-DriveLettersInUse
    $candidates = @('Z','Y','X','W','V','U','T','S','R','Q','P','O','N','M','L','K','J','I','H','G','F','E','D')
    $result = New-Object System.Collections.Generic.List[string]
    foreach ($letter in $candidates) {
        if (-not $inUse.Contains($letter)) {
            $result.Add($letter)
        }
    }
    return $result
}

function Force-FreeDriveLetter {
    param(
        [string]$Letter,
        [string]$WorkDir
    )

    if ($Letter -notmatch '^[A-Za-z]$') {
        return
    }

    $upper = $Letter.ToUpperInvariant()
    try { cmd /c "subst $upper`: /d" *> $null } catch {}
    try { cmd /c "net use $upper`: /delete /y" *> $null } catch {}
    try { cmd /c "mountvol $upper`: /D" *> $null } catch {}

    $dpRemove = Join-Path $WorkDir ("diskpart_remove_letter_{0}.txt" -f $upper)
    @(
        "select volume $upper",
        "remove letter=$upper noerr",
        "exit"
    ) | Set-Content -Path $dpRemove -Encoding ASCII
    $removeOutput = diskpart /s $dpRemove 2>&1
    $removeOutput | Out-Host
}

function Get-EspPartitionAccessPaths {
    param([string]$ImagePath)

    try {
        $img = Get-DiskImage -ImagePath $ImagePath -ErrorAction Stop
        if (-not $img.Attached) {
            return @()
        }
        $disk = $img | Get-Disk -ErrorAction Stop
        $part = $disk | Get-Partition -ErrorAction Stop | Where-Object { $_.PartitionNumber -eq 1 } | Select-Object -First 1
        if ($null -eq $part -or $null -eq $part.AccessPaths) {
            return @()
        }
        return @($part.AccessPaths)
    }
    catch {
        return @()
    }
}

function Mount-VdiskAccessPath {
    param(
        [string]$ImagePath,
        [string]$WorkDir
    )

    $dpProbe = Join-Path $WorkDir "diskpart_probe_letter.txt"
    @(
        "select vdisk file=`"$ImagePath`"",
        "attach vdisk noerr",
        "select partition 1",
        "detail partition",
        "exit"
    ) | Set-Content -Path $dpProbe -Encoding ASCII

    $probeOutput = diskpart /s $dpProbe 2>&1
    $probeOutput | Out-Host

    # Stage through a folder mount on a normal FAT partition (works reliably on Windows).
    $mountBase = "C:\Temp"
    Ensure-Dir $mountBase
    $mountDir = Join-Path $mountBase ("nkesp-mount-" + [Guid]::NewGuid().ToString("N"))
    Ensure-Dir $mountDir

    $dpMount = Join-Path $WorkDir "diskpart_assign_mount.txt"
    @(
        "select vdisk file=`"$ImagePath`"",
        "attach vdisk noerr",
        "select partition 1",
        "remove all noerr",
        "assign mount=`"$mountDir`"",
        "exit"
    ) | Set-Content -Path $dpMount -Encoding ASCII

    $mountOutput = diskpart /s $dpMount 2>&1
    $mountOutput | Out-Host
    $mountJoined = ($mountOutput | ForEach-Object { "$_" }) -join "`n"
    if ($mountJoined -match "successfully assigned the drive letter or mount point" -or
        (Test-Path -LiteralPath $mountDir)) {
        return ($mountDir.TrimEnd('\') + '\')
    }

    if (Test-Path -LiteralPath $mountDir) {
        Remove-Item -LiteralPath $mountDir -Recurse -Force -ErrorAction SilentlyContinue
    }
    throw "Unable to mount VHD partition to folder path for ESP staging"
}

function Convert-PartitionToEspType {
    param(
        [string]$ImagePath,
        [string]$WorkDir
    )

    $dpType = Join-Path $WorkDir "diskpart_set_esp_type.txt"
    @(
        "select vdisk file=`"$ImagePath`"",
        "attach vdisk noerr",
        "select partition 1",
        "set id=c12a7328-f81f-11d2-ba4b-00a0c93ec93b override",
        "exit"
    ) | Set-Content -Path $dpType -Encoding ASCII

    $typeOutput = diskpart /s $dpType 2>&1
    $typeOutput | Out-Host
    $typeJoined = ($typeOutput | ForEach-Object { "$_" }) -join "`n"
    if ($typeJoined -match "successfully set the partition ID" -or $typeJoined -match "already set") {
        return
    }
    throw "Failed to mark partition as EFI System Partition"
}

$repoRoot = Resolve-RepoRoot
Push-Location $repoRoot
try {
    Ensure-VirtualDiskService

    $distRoot = Join-Path $repoRoot $InputDistDir
    if (-not (Test-Path $distRoot)) {
        throw "Input dist directory not found: $distRoot"
    }

    if ([System.IO.Path]::IsPathRooted($OutputImagePath)) {
        $outImage = [System.IO.Path]::GetFullPath($OutputImagePath)
    } else {
        $outImage = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutputImagePath))
    }
    $outDir = Split-Path -Path $outImage -Parent
    Ensure-Dir $outDir

    if ([System.IO.Path]::IsPathRooted($TempLayoutDir)) {
        $layoutRoot = $TempLayoutDir
    } else {
        $layoutRoot = Join-Path $repoRoot $TempLayoutDir
    }
    if ($Clean -and (Test-Path $layoutRoot)) {
        Remove-Item -Recurse -Force $layoutRoot
    }

    & (Join-Path $repoRoot "tools\make-boot-layout.ps1") -InputDistDir $InputDistDir -OutputDir $TempLayoutDir -Clean

    $outImage = Remove-FileWithUnlock -ImagePath $outImage -WorkDir $outDir

    $volumeLabel = ("NKESP{0:X4}" -f (Get-Random -Minimum 0 -Maximum 65535))
    $dpCreate = Join-Path $outDir "diskpart_create_esp.txt"

    @(
        "create vdisk file=`"$outImage`" maximum=$SizeMb type=fixed",
        "select vdisk file=`"$outImage`"",
        "attach vdisk",
        "convert gpt",
        "create partition primary",
        "format fs=fat32 quick label=$volumeLabel",
        "exit"
    ) | Set-Content -Path $dpCreate -Encoding ASCII

    $createOutput = diskpart /s $dpCreate 2>&1
    $createOutput | Out-Host
    $createExit = Get-SafeLastExitCode
    if ($createExit -ne 0) {
        $joined = ($createOutput | ForEach-Object { "$_" }) -join "`n"
        if ($joined -match "virtual disk support provider.*not found") {
            throw "diskpart create/format failed: no virtual disk provider. Run elevated PowerShell and ensure the Virtual Disk service is available on this Windows install."
        }
        throw "diskpart create/format failed"
    }

    $targetRoot = $null
    $fileOpsRoot = $null
    $fileOpsPsDrive = $null
    try {
        $targetRoot = Mount-VdiskAccessPath -ImagePath $outImage -WorkDir $outDir
        $targetInfo = Convert-ToFileOpsRoot -RootPath $targetRoot
        $fileOpsRoot = $targetInfo.Root
        $fileOpsPsDrive = $targetInfo.PsDriveName

        Ensure-Dir (Join-Path $fileOpsRoot "EFI\BOOT")
        Ensure-Dir (Join-Path $fileOpsRoot "kernel")
        Ensure-Dir (Join-Path $fileOpsRoot "System")

        Copy-Item -Path (Join-Path $layoutRoot "EFI\BOOT\BOOTX64.EFI") -Destination (Join-Path $fileOpsRoot "EFI\BOOT\BOOTX64.EFI") -Force
        Copy-Item -Path (Join-Path $layoutRoot "nkxkrnl.kpe") -Destination (Join-Path $fileOpsRoot "kernel\ntxkrnl.kpe") -Force
        if (Test-Path (Join-Path $layoutRoot "System\init.kpe")) {
            Copy-Item -Path (Join-Path $layoutRoot "System\init.kpe") -Destination (Join-Path $fileOpsRoot "System\init.kpe") -Force
        }

        if (-not (Test-Path -LiteralPath (Join-Path $fileOpsRoot "EFI\BOOT\BOOTX64.EFI"))) {
            throw "ESP staging failed: BOOTX64.EFI missing at mounted target"
        }
        if (-not (Test-Path -LiteralPath (Join-Path $fileOpsRoot "kernel\ntxkrnl.kpe"))) {
            throw "ESP staging failed: kernel image missing at mounted target"
        }

        Convert-PartitionToEspType -ImagePath $outImage -WorkDir $outDir
    }
    finally {
        if ($fileOpsPsDrive) {
            Remove-PSDrive -Name $fileOpsPsDrive -Scope Script -Force -ErrorAction SilentlyContinue
        }

        $dpUnmount = Join-Path $outDir "diskpart_unassign_esp.txt"
        @(
            "select vdisk file=`"$outImage`"",
            "attach vdisk noerr",
            "select partition 1",
            "remove all noerr",
            "exit"
        ) | Set-Content -Path $dpUnmount -Encoding ASCII
        $unmountOutput = diskpart /s $dpUnmount 2>&1
        $unmountOutput | Out-Host

        $dpDetach = Join-Path $outDir "diskpart_detach_esp.txt"
        @(
            "select vdisk file=`"$outImage`"",
            "detach vdisk noerr",
            "exit"
        ) | Set-Content -Path $dpDetach -Encoding ASCII

        $detachOutput = diskpart /s $dpDetach 2>&1
        $detachOutput | Out-Host

        if ($targetRoot) {
            $mountFolder = $targetRoot.TrimEnd('\')
            if ($mountFolder.StartsWith("C:\Temp\nkesp-mount-", [System.StringComparison]::OrdinalIgnoreCase) -and
                (Test-Path -LiteralPath $mountFolder)) {
                Remove-Item -LiteralPath $mountFolder -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
    }

    if (-not (Test-Path -LiteralPath $outImage)) {
        throw "ESP image was not created at expected path: $outImage"
    }

    $imageInfo = Get-Item -LiteralPath $outImage
    Write-Host "ESP disk image ready: $($imageInfo.FullName) ($($imageInfo.Length) bytes)"
}
finally {
    Pop-Location
}
