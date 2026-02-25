param(
    [string]$OutDir = "dist",
    [string]$BuildDir = "build/out_user_init",
    [string]$ClangPath = "",
    [string]$LldPath = "",
    [string]$PowerShellPath = ""
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

    throw "$Label not found."
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
    $clang = Resolve-ToolPath -Configured $ClangPath -Candidates @("clang", "clang.exe") -Label "clang"
    $lld = Resolve-ToolPath -Configured $LldPath -Candidates @("ld.lld", "ld.lld.exe") -Label "ld.lld"
    $pwsh = Resolve-ToolPath -Configured $PowerShellPath -Candidates @("powershell", "pwsh", "powershell.exe", "pwsh.exe") -Label "powershell"

    $buildRoot = Join-Path $repoRoot $BuildDir
    $distRoot = Join-Path $repoRoot $OutDir
    $objDir = Join-Path $buildRoot "obj"
    $rawPath = Join-Path $buildRoot "init.raw"
    $objPath = Join-Path $objDir "init.obj"
    $srcPath = Join-Path $repoRoot "user\init\init.c"
    $ldPath = Join-Path $buildRoot "user.ld"
    $outPath = Join-Path $distRoot "init.kpe"
    [UInt64]$userImageBase = 0x40000000
    [UInt32]$userEntryRva = 0x1000
    $userLinkBase = $userImageBase + [UInt64]$userEntryRva

    Ensure-Dir $buildRoot
    Ensure-Dir $objDir
    Ensure-Dir $distRoot

@'
SECTIONS {
    . = __USER_LINK_BASE__;
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
}
'@.Replace("__USER_LINK_BASE__", ("0x{0:X}" -f $userLinkBase)) | Set-Content -Path $ldPath -Encoding ASCII

    & $clang `
        -target x86_64-pc-none-elf `
        -ffreestanding `
        -fno-stack-protector `
        -fno-builtin `
        -mno-red-zone `
        -Wall -Wextra `
        -c $srcPath `
        -o $objPath
    if ($LASTEXITCODE -ne 0) { throw "clang failed" }

    & $lld `
        -nostdlib `
        -z max-page-size=0x1000 `
        -T $ldPath `
        --oformat binary `
        -o $rawPath `
        $objPath
    if ($LASTEXITCODE -ne 0) { throw "ld.lld failed" }

    & $pwsh -ExecutionPolicy Bypass -File (Join-Path $repoRoot "tools\kuserpack.ps1") `
        -InputRawPath $rawPath `
        -OutputUserImagePath $outPath `
        -ImageBase $userImageBase `
        -EntryRva $userEntryRva
    if ($LASTEXITCODE -ne 0) { throw "kuserpack failed" }

    Write-Host "Built user init image: $outPath"
}
finally {
    Pop-Location
}
