param(
    [Parameter(Mandatory = $true)]
    [string]$InputRawPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputUserImagePath,

    [UInt64]$ImageBase = 0x40000000,
    [UInt32]$EntryRva = 0x1000
)

$target = $OutputUserImagePath
if ([System.IO.Path]::GetExtension($target).ToLowerInvariant() -ne ".kpe") {
    $target = [System.IO.Path]::ChangeExtension($target, ".kpe")
}

& "$PSScriptRoot\kpepack.ps1" `
    -InputRawPath $InputRawPath `
    -OutputKpePath $target `
    -ImageBase $ImageBase `
    -EntryRva $EntryRva `
    -ImageKind Userland

Write-Output "Packed userland KPE image: $target"
