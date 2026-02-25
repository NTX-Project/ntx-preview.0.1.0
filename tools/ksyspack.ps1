param(
    [Parameter(Mandatory = $true)]
    [string]$InputRawPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputSystemModulePath,

    [UInt64]$ImageBase = 0x400000,
    [UInt32]$EntryRva = 0x1000
)

$target = $OutputSystemModulePath
$ext = [System.IO.Path]::GetExtension($target).ToLowerInvariant()
if ($ext -ne ".ksys") {
    $target = [System.IO.Path]::ChangeExtension($target, ".ksys")
}

$kpeTarget = [System.IO.Path]::ChangeExtension($target, ".kpe")
& "$PSScriptRoot\kpepack.ps1" `
    -InputRawPath $InputRawPath `
    -OutputKpePath $kpeTarget `
    -ImageBase $ImageBase `
    -EntryRva $EntryRva `
    -ImageKind SystemAddon

if (Test-Path $target) {
    Remove-Item -Force $target
}
Copy-Item -Path $kpeTarget -Destination $target
Remove-Item -Force $kpeTarget

Write-Output "Packed KSYS module image: $target"
