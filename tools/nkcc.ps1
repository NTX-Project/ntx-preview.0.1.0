param(
    [string]$KernelFoldersRsp = "build/kernel_folders.rsp",
    [string]$ModuleFoldersRsp = "build/module_folders.rsp",
    [string]$OutDir = "dist",
    [string]$BuildDir = "build/out",
    [string]$KernelName = "ntxkrnl",
    [string]$KernelImageBase = "0x200000",
    [string]$KernelSectionRva = "0x1000",
    [string]$KernelEntryOffset = "0x0",
    [string]$ClangPath = "",
    [string]$LldPath = "",
    [string]$PowerShellPath = "",
    [string]$LldLinkPath = "",
    [switch]$Validate,
    [switch]$BuildKernel = $true,
    [switch]$BuildBoot = $true,
    [switch]$BuildModules = $true,
    [switch]$BuildUser = $true,
    [switch]$Clean,
    [switch]$DryRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Resolve-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function Invoke-Tool {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string[]]$ArgumentList
    )

    $rendered = "$FilePath $($ArgumentList -join ' ')"
    Write-Host $rendered
    if ($DryRun) {
        return
    }

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $rendered"
    }
}

function Resolve-ToolPath {
    param(
        [string]$Configured,
        [string[]]$Candidates,
        [string]$ToolLabel
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

    if ($DryRun -and $Candidates.Count -gt 0) {
        return $Candidates[0]
    }

    throw "Required tool not found: $ToolLabel. Set -$ToolLabel explicitly."
}

function Read-FolderList {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RspPath,
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot
    )

    if (-not (Test-Path $RspPath)) {
        throw "Missing response file: $RspPath"
    }

    $list = New-Object System.Collections.Generic.List[string]
    Get-Content $RspPath | ForEach-Object {
        $line = $_.Trim()
        if ($line.Length -eq 0 -or $line.StartsWith("#")) {
            return
        }
        $normalized = $line.Replace("/", "\")
        $full = Join-Path $RepoRoot $normalized
        if (Test-Path $full) {
            $list.Add($full)
        }
    }

    return $list
}

function Get-CSourceFiles {
    param([string[]]$Folders)

    $files = New-Object System.Collections.Generic.List[string]
    foreach ($folder in $Folders) {
        Get-ChildItem -Path $folder -Recurse -Filter *.c -File | ForEach-Object {
            $files.Add($_.FullName)
        }
    }

    return $files | Sort-Object -Unique
}

function Ensure-Dir {
    param([string]$Path)

    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Path $Path -Force | Out-Null
    }
}

function New-GeneratedLinkerScript {
    param(
        [string]$RepoRoot,
        [string]$BuildRoot,
        [string]$LinkBase
    )

    $templatePath = Join-Path $RepoRoot "tools\\kernel.ld"
    $generatedDir = Join-Path $BuildRoot "generated"
    $generatedPath = Join-Path $generatedDir "kernel.ld"
    Ensure-Dir $generatedDir

    $template = Get-Content $templatePath -Raw
    $content = $template.Replace("__NK_LINK_BASE__", $LinkBase)
    Set-Content -Path $generatedPath -Value $content -Encoding ASCII
    return $generatedPath
}

function Invoke-CompileAndLinkRaw {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Sources,
        [Parameter(Mandatory = $true)]
        [string]$ObjDir,
        [Parameter(Mandatory = $true)]
        [string]$RawOut,
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,
        [Parameter(Mandatory = $true)]
        [string]$LinkerScriptPath
    )

    if ($Sources.Count -eq 0) {
        throw "No source files found to compile."
    }

    Ensure-Dir $ObjDir

    $objects = New-Object System.Collections.Generic.List[string]
    foreach ($src in $Sources) {
        $relative = $src.Substring($RepoRoot.Length).TrimStart('\')
        $objRel = [System.IO.Path]::ChangeExtension($relative, ".obj")
        $objPath = Join-Path $ObjDir $objRel
        Ensure-Dir ([System.IO.Path]::GetDirectoryName($objPath))

        $clangArgs = @(
            "-target", "x86_64-pc-none-elf",
            "-ffreestanding",
            "-fno-stack-protector",
            "-fno-builtin",
            "-mno-red-zone",
            "-mgeneral-regs-only",
            "-Wall",
            "-Wextra"
        ) + $script:GlobalCFlags + @(
            "-I", (Join-Path $RepoRoot "include"),
            "-I", (Join-Path $RepoRoot "boot\include"),
            "-I", (Join-Path $RepoRoot "hal\inc"),
            "-I", (Join-Path $RepoRoot "kernel\include"),
            "-c", $src,
            "-o", $objPath
        )
        Invoke-Tool -FilePath $script:ResolvedClangPath -ArgumentList $clangArgs
        $objects.Add($objPath)
    }

    $linkArgs = @(
        "-nostdlib",
        "-z", "max-page-size=0x1000",
        "-T", $LinkerScriptPath,
        "--oformat", "binary",
        "-o", $RawOut
    ) + $objects

    Invoke-Tool -FilePath $script:ResolvedLldPath -ArgumentList $linkArgs
}

function Invoke-BuildKernel {
    param(
        [string]$KernelRspPath,
        [string]$RepoRoot,
        [string]$BuildRoot,
        [string]$DistRoot
    )

    $folders = Read-FolderList -RspPath $KernelRspPath -RepoRoot $RepoRoot
    $sources = Get-CSourceFiles -Folders $folders

    $kernelObjDir = Join-Path $BuildRoot "kernel\obj"
    $kernelRaw = Join-Path $BuildRoot "kernel\$KernelName.raw"
    $kernelLinkBase = ([UInt64]$KernelImageBase + [UInt64]$KernelSectionRva)
    $kernelLinkBaseHex = ("0x{0:X}" -f $kernelLinkBase)
    $kernelLinkerScript = New-GeneratedLinkerScript -RepoRoot $RepoRoot -BuildRoot $BuildRoot -LinkBase $kernelLinkBaseHex
    $kernelEntryRva = ([UInt32]$KernelSectionRva + [UInt32]$KernelEntryOffset)
    Ensure-Dir (Join-Path $BuildRoot "kernel")

    Invoke-CompileAndLinkRaw -Sources $sources -ObjDir $kernelObjDir -RawOut $kernelRaw -RepoRoot $RepoRoot -LinkerScriptPath $kernelLinkerScript

    $kernelKpe = Join-Path $DistRoot "$KernelName.kpe"
    $packArgs = @(
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\kpepack.ps1"),
        "-InputRawPath", $kernelRaw,
        "-OutputKpePath", $kernelKpe,
        "-ImageBase", $KernelImageBase,
        "-SectionRva", $KernelSectionRva,
        "-EntryRva", ("0x{0:X}" -f $kernelEntryRva)
    )
    Invoke-Tool -FilePath $script:ResolvedPowerShellPath -ArgumentList $packArgs
}

function Invoke-BuildModules {
    param(
        [string]$ModuleRspPath,
        [string]$RepoRoot,
        [string]$BuildRoot,
        [string]$DistRoot
    )

    $moduleRoots = Read-FolderList -RspPath $ModuleRspPath -RepoRoot $RepoRoot
    foreach ($moduleRoot in $moduleRoots) {
        if (-not (Test-Path $moduleRoot)) {
            continue
        }

        $moduleDirs = Get-ChildItem -Path $moduleRoot -Directory
        foreach ($moduleDir in $moduleDirs) {
            $sources = Get-CSourceFiles -Folders @($moduleDir.FullName)
            if ($sources.Count -eq 0) {
                continue
            }

            $moduleName = $moduleDir.Name
            $moduleObjDir = Join-Path $BuildRoot "modules\obj\$moduleName"
            $moduleRaw = Join-Path $BuildRoot "modules\$moduleName.raw"
            $moduleLinkerScript = New-GeneratedLinkerScript -RepoRoot $RepoRoot -BuildRoot $BuildRoot -LinkBase $KernelSectionRva
            Ensure-Dir (Join-Path $BuildRoot "modules")

            Invoke-CompileAndLinkRaw -Sources $sources -ObjDir $moduleObjDir -RawOut $moduleRaw -RepoRoot $RepoRoot -LinkerScriptPath $moduleLinkerScript

            $moduleOut = Join-Path $DistRoot "$moduleName.ksys"
            $packArgs = @(
                "-ExecutionPolicy", "Bypass",
                "-File", (Join-Path $RepoRoot "tools\ksyspack.ps1"),
                "-InputRawPath", $moduleRaw,
                "-OutputSystemModulePath", $moduleOut
            )
            Invoke-Tool -FilePath $script:ResolvedPowerShellPath -ArgumentList $packArgs
        }
    }
}

function Invoke-BuildBoot {
    param(
        [string]$RepoRoot,
        [string]$BuildRoot,
        [string]$DistRoot
    )

    $args = @(
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\\build-bootx64.ps1"),
        "-BuildDir", $BuildDir,
        "-OutDir", $OutDir,
        "-ClangPath", $script:ResolvedClangPath
    )

    if ($script:ResolvedLldLinkPath -ne "") {
        $args += @("-LldLinkPath", $script:ResolvedLldLinkPath)
    }

    Invoke-Tool -FilePath $script:ResolvedPowerShellPath -ArgumentList $args
}

function Invoke-BuildUser {
    param(
        [string]$RepoRoot,
        [string]$BuildRoot,
        [string]$DistRoot
    )

    $args = @(
        "-ExecutionPolicy", "Bypass",
        "-File", (Join-Path $RepoRoot "tools\\build-user-init.ps1"),
        "-BuildDir", (Join-Path $BuildDir "user"),
        "-OutDir", $OutDir,
        "-ClangPath", $script:ResolvedClangPath,
        "-LldPath", $script:ResolvedLldPath,
        "-PowerShellPath", $script:ResolvedPowerShellPath
    )

    Invoke-Tool -FilePath $script:ResolvedPowerShellPath -ArgumentList $args
}

$repoRoot = Resolve-RepoRoot
Push-Location $repoRoot
try {
    $kernelRspFull = Join-Path $repoRoot $KernelFoldersRsp
    $moduleRspFull = Join-Path $repoRoot $ModuleFoldersRsp
    $buildRoot = Join-Path $repoRoot $BuildDir
    $distRoot = Join-Path $repoRoot $OutDir
    $script:ResolvedClangPath = Resolve-ToolPath -Configured $ClangPath -Candidates @("clang", "clang.exe") -ToolLabel "ClangPath"
    $script:ResolvedLldPath = Resolve-ToolPath -Configured $LldPath -Candidates @("ld.lld", "ld.lld.exe") -ToolLabel "LldPath"
    $script:ResolvedLldLinkPath = ""
    if ($BuildBoot) {
        $script:ResolvedLldLinkPath = Resolve-ToolPath -Configured $LldLinkPath -Candidates @("lld-link", "lld-link.exe") -ToolLabel "LldLinkPath"
    }
    $script:ResolvedPowerShellPath = Resolve-ToolPath -Configured $PowerShellPath -Candidates @("powershell", "pwsh", "powershell.exe", "pwsh.exe") -ToolLabel "PowerShellPath"
    $script:GlobalCFlags = @()
    if ($Validate) {
        $script:GlobalCFlags += "-DNK_VALIDATE=1"
    }

    if ($Clean) {
        if (Test-Path $buildRoot) {
            Remove-Item -Recurse -Force $buildRoot
        }
        if (Test-Path $distRoot) {
            Remove-Item -Recurse -Force $distRoot
        }
    }

    Ensure-Dir $buildRoot
    Ensure-Dir $distRoot

    if ($BuildKernel) {
        Invoke-BuildKernel -KernelRspPath $kernelRspFull -RepoRoot $repoRoot -BuildRoot $buildRoot -DistRoot $distRoot
    }

    if ($BuildModules) {
        Invoke-BuildModules -ModuleRspPath $moduleRspFull -RepoRoot $repoRoot -BuildRoot $buildRoot -DistRoot $distRoot
    }

    if ($BuildUser) {
        Invoke-BuildUser -RepoRoot $repoRoot -BuildRoot $buildRoot -DistRoot $distRoot
    }

    if ($BuildBoot) {
        Invoke-BuildBoot -RepoRoot $repoRoot -BuildRoot $buildRoot -DistRoot $distRoot
    }

    Write-Host "NKCC build complete."
    Write-Host "Kernel output: $(Join-Path $distRoot "$KernelName.kpe")"
    Write-Host "Init output: $(Join-Path $distRoot "init.kpe")"
    Write-Host "Boot output: $(Join-Path $distRoot "BOOTX64.EFI")"
    Write-Host "Module output dir: $distRoot"
}
finally {
    Pop-Location
}
