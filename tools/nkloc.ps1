param(
    [string]$KernelFoldersRsp = "build/kernel_folders.rsp",
    [string]$RepoRoot = "",
    [switch]$ShowFiles,
    [switch]$AllFiles
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-Root {
    if ($RepoRoot -ne "") {
        return (Resolve-Path $RepoRoot).Path
    }
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Read-FolderList {
    param(
        [string]$RspPath,
        [string]$Root
    )

    if (-not (Test-Path $RspPath)) {
        throw "Missing response file: $RspPath"
    }

    $folders = New-Object System.Collections.Generic.List[string]
    Get-Content $RspPath | ForEach-Object {
        $line = $_.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith("#")) {
            return
        }

        $full = Join-Path $Root ($line.Replace("/", "\"))
        if (Test-Path $full) {
            $folders.Add((Resolve-Path $full).Path)
        }
    }

    return $folders
}

function Test-IsIgnoredPath {
    param([string]$FullPath)
    $p = $FullPath.ToLowerInvariant()
    return $p.Contains("\build\") -or
           $p.Contains("\dist\") -or
           $p.Contains("\.git\") -or
           $p.Contains("\.vs\")
}

function Test-IsKernelTextFile {
    param([string]$Name)
    $ext = [System.IO.Path]::GetExtension($Name).ToLowerInvariant()
    if ($AllFiles) {
        return $true
    }

    switch ($ext) {
        ".c" { return $true }
        ".h" { return $true }
        ".cpp" { return $true }
        ".hpp" { return $true }
        ".s" { return $true }
        ".asm" { return $true }
        ".inc" { return $true }
        ".ld" { return $true }
        ".ps1" { return $true }
        ".py" { return $true }
        ".md" { return $true }
        ".txt" { return $true }
        ".rsp" { return $true }
        default { return $false }
    }
}

$root = Resolve-Root
$rspPath = Join-Path $root $KernelFoldersRsp
$foldersFromRsp = Read-FolderList -RspPath $rspPath -Root $root
$folders = New-Object System.Collections.Generic.List[string]
foreach ($f in $foldersFromRsp) {
    $folders.Add($f)
}

# Include shared kernel-facing headers/contracts.
$extraFolders = @("include", "kernel\include", "hal\inc", "boot\include")
foreach ($extra in $extraFolders) {
    $full = Join-Path $root $extra
    if ((Test-Path $full) -and -not ($folders -contains (Resolve-Path $full).Path)) {
        $folders.Add((Resolve-Path $full).Path)
    }
}

$rows = New-Object System.Collections.Generic.List[object]
$total = 0

foreach ($folder in $folders) {
    Get-ChildItem -Path $folder -Recurse -File | ForEach-Object {
        if (Test-IsIgnoredPath $_.FullName) {
            return
        }
        if (-not (Test-IsKernelTextFile $_.Name)) {
            return
        }

        $lineCount = (Get-Content $_.FullName | Measure-Object -Line).Lines
        $total += $lineCount
        $rows.Add([pscustomobject]@{
            Lines = $lineCount
            File  = $_.FullName.Substring($root.Length).TrimStart('\')
        })
    }
}

$rows = $rows | Sort-Object File -Unique

if ($ShowFiles) {
    $rows | Sort-Object Lines -Descending | Format-Table -AutoSize
}

Write-Host "Kernel files counted: $($rows.Count)"
Write-Host "Kernel total lines:   $total"
