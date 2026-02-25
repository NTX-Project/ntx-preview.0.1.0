param(
    [Parameter(Mandatory = $true)]
    [string]$InputRawPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputKxPath,

    [UInt64]$ImageBase = 0x200000,
    [UInt32]$EntryRva = 0x1000
)

$target = $OutputKxPath
if ([System.IO.Path]::GetExtension($target).ToLowerInvariant() -eq ".kx") {
    $target = [System.IO.Path]::ChangeExtension($target, ".kpe")
}

& "$PSScriptRoot\kpepack.ps1" `
    -InputRawPath $InputRawPath `
    -OutputKpePath $target `
    -ImageBase $ImageBase `
    -EntryRva $EntryRva

Write-Output "Legacy kxpack wrapper emitted: $target"

