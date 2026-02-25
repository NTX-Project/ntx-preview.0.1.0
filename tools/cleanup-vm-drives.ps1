param(
    [string]$RepoRootOverride = "",
    [switch]$DetachAllAttachedVhds,
    [switch]$DeleteImages,
    [switch]$NoRestartExplorer,
    [switch]$PruneDriveLetters
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    if ($RepoRootOverride -ne "") {
        return (Resolve-Path -LiteralPath $RepoRootOverride).Path
    }
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Get-NkImageCandidates {
    param([string]$RepoRoot)

    $candidates = New-Object System.Collections.Generic.List[string]
    $roots = @(
        (Join-Path $RepoRoot "build\run-vbox"),
        "C:\Temp"
    )

    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }
        Get-ChildItem -LiteralPath $root -File -ErrorAction SilentlyContinue | Where-Object {
            $_.Extension -in @(".vhd", ".vhdx") -and $_.Name -match "^nk-esp"
        } | ForEach-Object {
            $candidates.Add($_.FullName)
        }
    }

    return $candidates | Sort-Object -Unique
}

function Get-AttachedVhdImagePaths {
    $paths = New-Object System.Collections.Generic.List[string]

    try {
        $diskImages = Get-CimInstance -Namespace root/Microsoft/Windows/Storage -ClassName MSFT_DiskImage -ErrorAction Stop
        foreach ($img in $diskImages) {
            if ($img.Attached -and $img.ImagePath) {
                $p = [string]$img.ImagePath
                $pl = $p.ToLowerInvariant()
                if ($pl.EndsWith(".vhd") -or $pl.EndsWith(".vhdx")) {
                    $paths.Add($p)
                }
            }
        }
    }
    catch {
        # fallback: no CIM storage provider access
    }

    return $paths | Sort-Object -Unique
}

function Invoke-DiskPartScript {
    param(
        [string[]]$Lines,
        [string]$WorkDir
    )

    $scriptPath = Join-Path $WorkDir ("diskpart_cleanup_{0}.txt" -f [guid]::NewGuid().ToString("N"))
    $Lines | Set-Content -Path $scriptPath -Encoding ASCII
    try {
        $output = diskpart /s $scriptPath 2>&1
        $output | Out-Host
    }
    finally {
        Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue
    }
}

function Clear-NkVhdMount {
    param(
        [string]$ImagePath,
        [string]$WorkDir
    )

    Write-Host "[cleanup] processing $ImagePath"

    # Remove letter if present (best effort, idempotent), then detach.
    Invoke-DiskPartScript -WorkDir $WorkDir -Lines @(
        "select vdisk file=`"$ImagePath`"",
        "attach vdisk noerr",
        "select partition 1",
        "remove all noerr",
        "detach vdisk noerr",
        "exit"
    )
}

function Remove-NkVhdFile {
    param([string]$ImagePath)

    if (-not (Test-Path -LiteralPath $ImagePath)) {
        return
    }

    try {
        Write-Host "[cleanup] deleting $ImagePath"
        Remove-Item -LiteralPath $ImagePath -Force
    }
    catch {
        Write-Warning "[cleanup] delete failed for $ImagePath : $($_.Exception.Message)"
    }
}

function Remove-SubstDrives {
    $substOut = subst
    foreach ($line in $substOut) {
        if ($line -match '^([A-Za-z]):\\:\s*=>\s*(.+)$') {
            $letter = $matches[1]
            Write-Host "[cleanup] removing SUBST drive $letter`:"
            cmd /c "subst $letter`: /d" | Out-Host
        }
    }
}

function Restart-ExplorerShell {
    Write-Host "[cleanup] restarting explorer shell"
    Get-Process -Name explorer -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 300
    Start-Process explorer.exe
}

function Get-CurrentDriveLetters {
    $letters = New-Object System.Collections.Generic.HashSet[string]

    Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue | ForEach-Object {
        if ($_.Name -match '^[A-Za-z]$') {
            [void]$letters.Add(([string]$_.Name).ToUpperInvariant())
        }
    }

    try {
        Get-Volume -ErrorAction SilentlyContinue | ForEach-Object {
            if ($_.DriveLetter) {
                [void]$letters.Add(([string]$_.DriveLetter).ToUpperInvariant())
            }
        }
    }
    catch {
        # Access to storage APIs may be restricted in some shells.
    }

    return $letters
}

function Remove-DriveLetterMapping {
    param([string]$Letter, [string]$WorkDir)

    if ($Letter -eq "C") {
        return
    }

    Write-Host "[cleanup] removing drive mapping $Letter`:"
    cmd /c "subst $Letter`: /d" | Out-Host
    cmd /c "net use $Letter`: /delete /y" | Out-Host
    cmd /c "mountvol $Letter`: /D" | Out-Host

    $dp = Join-Path $WorkDir ("diskpart_remove_letter_{0}.txt" -f $Letter)
    @(
        "select volume $Letter",
        "remove letter=$Letter noerr",
        "exit"
    ) | Set-Content -Path $dp -Encoding ASCII
    try {
        diskpart /s $dp 2>&1 | Out-Host
    }
    finally {
        Remove-Item -LiteralPath $dp -Force -ErrorAction SilentlyContinue
    }
}

function Invoke-PruneDriveLetters {
    param([string]$WorkDir)

    $allLetters = Get-CurrentDriveLetters
    $current = @()
    foreach ($letter in @('D','E','F','G','H','I','J','K','L','M','N','O','P','Q','R','S','T','U','V','W','X','Y','Z')) {
        if ($allLetters.Contains($letter)) {
            $current += $letter
        }
    }

    if ($current.Count -eq 0) {
        Write-Host "[cleanup] no non-C drive letters detected."
        return
    }

    Write-Host "[cleanup] current non-C drive letters: $($current -join ', ')"
    $keepInput = Read-Host "Enter letters to KEEP besides C (comma-separated, or blank for none)"

    $keep = New-Object System.Collections.Generic.HashSet[string]
    [void]$keep.Add("C")
    if ($keepInput) {
        foreach ($token in ($keepInput -split ',')) {
            $t = $token.Trim().ToUpperInvariant()
            if ($t -match '^[A-Z]$') {
                [void]$keep.Add($t)
            }
        }
    }

    $toRemove = @()
    foreach ($letter in $current) {
        if (-not $keep.Contains($letter)) {
            $toRemove += $letter
        }
    }

    if ($toRemove.Count -eq 0) {
        Write-Host "[cleanup] nothing to remove after keep list."
        return
    }

    Write-Host "[cleanup] letters to remove: $($toRemove -join ', ')"
    Write-Host "[cleanup] this removes drive-letter mappings (SUBST/network/mount points/volume letter)."
    $confirm = Read-Host "Type REMOVE to continue"
    if ($confirm -ne "REMOVE") {
        Write-Host "[cleanup] drive-letter prune cancelled."
        return
    }

    foreach ($letter in $toRemove) {
        Remove-DriveLetterMapping -Letter $letter -WorkDir $WorkDir
    }
}

$repoRoot = Resolve-RepoRoot
Push-Location $repoRoot
try {
    $workDir = Join-Path $repoRoot "build\run-vbox"
    if (-not (Test-Path -LiteralPath $workDir)) {
        New-Item -ItemType Directory -Path $workDir -Force | Out-Null
    }

    $images = Get-NkImageCandidates -RepoRoot $repoRoot
    foreach ($img in $images) {
        try {
            Clear-NkVhdMount -ImagePath $img -WorkDir $workDir
            if ($DeleteImages) {
                Remove-NkVhdFile -ImagePath $img
            }
        }
        catch {
            Write-Warning "[cleanup] failed for $img : $($_.Exception.Message)"
        }
    }

    if ($DetachAllAttachedVhds) {
        Write-Host "[cleanup] detaching any remaining attached VHD/VHDX images (global best-effort)"
        Get-AttachedVhdImagePaths | ForEach-Object {
            $path = $_
            try {
                Write-Host ("[cleanup] dismount {0}" -f $path)
                Dismount-DiskImage -ImagePath $path -ErrorAction Stop
            }
            catch {
                Write-Warning ("[cleanup] dismount failed for {0}: {1}" -f $path, $_.Exception.Message)
            }
        }
    }

    # Remove SUBST drives and stale mount-manager assignments (refresh explorer drive list).
    Remove-SubstDrives
    cmd /c "mountvol /R" | Out-Host

    if ($PruneDriveLetters) {
        Invoke-PruneDriveLetters -WorkDir $workDir
        cmd /c "mountvol /R" | Out-Host
    }

    if (-not $NoRestartExplorer) {
        Restart-ExplorerShell
    }

    Write-Host "[cleanup] done"
}
finally {
    Pop-Location
}
