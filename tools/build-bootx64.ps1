param(
    [string]$OutDir = "dist",
    [string]$BuildDir = "build\\out",
    [string]$ClangPath = "",
    [string]$LldLinkPath = ""
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Resolve-ToolPath {
    param(
        [string]$Configured,
        [string[]]$Candidates,
        [string]$Label
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

    throw "Required tool not found: $Label. Set -$Label explicitly."
}

function Ensure-Dir {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function Invoke-Tool {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList
    )

    Write-Host "$FilePath $($ArgumentList -join ' ')"
    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($ArgumentList -join ' ')"
    }
}

$repoRoot = Resolve-RepoRoot
Push-Location $repoRoot
try {
    $clang = Resolve-ToolPath -Configured $ClangPath -Candidates @("clang", "clang.exe") -Label "ClangPath"
    $lldLink = Resolve-ToolPath -Configured $LldLinkPath -Candidates @("lld-link", "lld-link.exe") -Label "LldLinkPath"

    $buildRoot = Join-Path $repoRoot $BuildDir
    $outRoot = Join-Path $repoRoot $OutDir
    $bootObjDir = Join-Path $buildRoot "boot\\obj"
    $bootOut = Join-Path $outRoot "BOOTX64.EFI"

    Ensure-Dir $bootObjDir
    Ensure-Dir $outRoot

    $sources = @(
        (Join-Path $repoRoot "boot\\uefi\\bootx64.c"),
        (Join-Path $repoRoot "boot\\uefi\\efistub.c")
    )

    $objects = New-Object System.Collections.Generic.List[string]
    foreach ($src in $sources) {
        $obj = Join-Path $bootObjDir ([System.IO.Path]::GetFileNameWithoutExtension($src) + ".obj")
        $args = @(
            "--target=x86_64-pc-windows-msvc",
            "-ffreestanding",
            "-fshort-wchar",
            "-fno-stack-protector",
            "-fno-builtin",
            "-Wall",
            "-Wextra",
            "-I", (Join-Path $repoRoot "boot\\include"),
            "-I", (Join-Path $repoRoot "include"),
            "-c", $src,
            "-o", $obj
        )
        Invoke-Tool -FilePath $clang -ArgumentList $args
        $objects.Add($obj)
    }

    $linkArgs = @(
        "/nologo",
        "/nodefaultlib",
        "/subsystem:efi_application",
        "/entry:efi_main",
        "/machine:x64",
        "/out:$bootOut"
    ) + $objects
    Invoke-Tool -FilePath $lldLink -ArgumentList $linkArgs

    Write-Host "Built UEFI loader: $bootOut"
}
finally {
    Pop-Location
}
