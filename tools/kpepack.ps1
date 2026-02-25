param(
    [Parameter(Mandatory = $true)]
    [string]$InputRawPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputKpePath,

    [UInt64]$ImageBase = 0x200000,
    [string]$EntryRva = "",
    [UInt32]$EntryOffset = 0,
    [UInt32]$SectionRva = 0x1000,
    [UInt32]$SectionAlignment = 0x1000,
    [UInt32]$FileAlignment = 0x200,
    [ValidateSet("Kernel", "Userland", "SystemAddon")]
    [string]$ImageKind = "Kernel",
    [switch]$DisableAutoReloc
)

function Align-Up {
    param([UInt64]$Value, [UInt64]$Alignment)
    if ($Alignment -eq 0) { return $Value }
    return [UInt64](([Math]::Ceiling($Value / [double]$Alignment)) * $Alignment)
}

function Get-AutoRelocBytes {
    param(
        [byte[]]$Payload,
        [UInt32]$TextSectionRva,
        [UInt64]$ImageBase
    )

    $candidateRvas = New-Object System.Collections.Generic.List[UInt32]
    $payloadSize = [UInt32]$Payload.Length
    if ($payloadSize -lt 8) {
        return [byte[]]@()
    }

    $rvaRangeStart = [UInt64]$TextSectionRva
    $rvaRangeEnd = [UInt64]$TextSectionRva + [UInt64]$payloadSize
    $vaRangeStart = [UInt64]$ImageBase + [UInt64]$TextSectionRva
    $vaRangeEnd = $vaRangeStart + [UInt64]$payloadSize

    for ($offset = 0; $offset -le ($payloadSize - 8); $offset += 8) {
        $value = [BitConverter]::ToUInt64($Payload, $offset)
        $isRvaStyle = ($value -ge $rvaRangeStart -and $value -lt $rvaRangeEnd)
        $isVaStyle = ($value -ge $vaRangeStart -and $value -lt $vaRangeEnd)
        if ($isRvaStyle -or $isVaStyle) {
            $candidateRvas.Add([UInt32]($TextSectionRva + [UInt32]$offset))
        }
    }

    if ($candidateRvas.Count -eq 0) {
        return [byte[]]@()
    }

    $grouped = $candidateRvas | Sort-Object -Unique | Group-Object { $_ -band 0xFFFFF000 }
    $relocStream = New-Object System.IO.MemoryStream
    $relocWriter = New-Object System.IO.BinaryWriter($relocStream)

    foreach ($group in $grouped) {
        [UInt32]$pageRva = [UInt32]$group.Name
        $entries = New-Object System.Collections.Generic.List[UInt16]

        foreach ($rva in ($group.Group | Sort-Object)) {
            [UInt16]$entry = [UInt16](((10 -shl 12) -bor ($rva - $pageRva)))
            $entries.Add($entry)
        }

        [UInt32]$blockSize = [UInt32](8 + ($entries.Count * 2))
        if (($blockSize % 4) -ne 0) {
            $entries.Add([UInt16]0)
            $blockSize = [UInt32]($blockSize + 2)
        }

        $relocWriter.Write([UInt32]$pageRva)
        $relocWriter.Write([UInt32]$blockSize)
        foreach ($entry in $entries) {
            $relocWriter.Write([UInt16]$entry)
        }
    }

    $relocWriter.Flush()
    $bytes = $relocStream.ToArray()
    $relocWriter.Close()
    $relocStream.Close()
    return ,([byte[]]$bytes)
}

$KpeSignature = 0x3045504B
$MachineAur64 = 0xA641
$OptionalMagicAur64 = 0xA641
$DllCharDynamicBase = 0x0040
$DllCharNxCompat = 0x0100
$SubsystemNativeKernel = 1
$SubsystemUserland = 2
$SubsystemSystemAddon = 3
$FileCharExecutable = 0x0002
$FileCharLargeAware = 0x0020
$SectionCharCode = 0x00000020
$SectionCharInitData = 0x00000040
$SectionCharExec = 0x20000000
$SectionCharRead = 0x40000000
$BaseRelocDirectoryIndex = 5

$payload = [System.IO.File]::ReadAllBytes($InputRawPath)
$payloadSize = [UInt32]$payload.Length
$rawSize = [UInt32](Align-Up -Value $payloadSize -Alignment $FileAlignment)
$sectionVA = [UInt32]$SectionRva

$effectiveEntryRva = 0
if ($EntryRva -eq "") {
    $effectiveEntryRva = [UInt32]($sectionVA + $EntryOffset)
} else {
    $effectiveEntryRva = [UInt32]$EntryRva
}

if ($effectiveEntryRva -lt $sectionVA -or $effectiveEntryRva -ge ([UInt32]($sectionVA + $payloadSize))) {
    throw "EntryRva must point inside the primary section payload."
}

$subsystemValue = switch ($ImageKind) {
    "Userland" { $SubsystemUserland }
    "SystemAddon" { $SubsystemSystemAddon }
    default { $SubsystemNativeKernel }
}

[byte[]]$relocPayload = @()
if (-not $DisableAutoReloc) {
    $relocPayload = [byte[]](Get-AutoRelocBytes -Payload $payload -TextSectionRva $sectionVA -ImageBase $ImageBase)
}
$relocPayloadSize = [UInt32]$relocPayload.Length
$hasRelocSection = $relocPayloadSize -gt 0

$dosHeaderSize = 64
$peOffset = 0x80
$fileHeaderSize = 20
$optionalHeaderSize = 240
$sectionHeaderSize = 40
$sectionCount = if ($hasRelocSection) { 2 } else { 1 }

$headersEnd = $peOffset + 4 + $fileHeaderSize + $optionalHeaderSize + ($sectionHeaderSize * $sectionCount)
$sizeOfHeaders = [UInt32](Align-Up -Value $headersEnd -Alignment $FileAlignment)
$pointerToRawData = $sizeOfHeaders

$relocSectionVA = [UInt32](Align-Up -Value ([UInt64]$sectionVA + [UInt64]$payloadSize) -Alignment $SectionAlignment)
$relocRawSize = [UInt32](Align-Up -Value $relocPayloadSize -Alignment $FileAlignment)
$relocPointerToRaw = [UInt32](Align-Up -Value ([UInt64]$pointerToRawData + [UInt64]$rawSize) -Alignment $FileAlignment)

if ($hasRelocSection) {
    $sizeOfImage = [UInt32](Align-Up -Value ([UInt64]$relocSectionVA + [UInt64]$relocPayloadSize) -Alignment $SectionAlignment)
} else {
    $sizeOfImage = [UInt32](Align-Up -Value ([UInt64]$sectionVA + [UInt64]$payloadSize) -Alignment $SectionAlignment)
}

$sizeOfInitializedData = 0
if ($hasRelocSection) {
    $sizeOfInitializedData = $relocRawSize
}

$dir = Split-Path -Path $OutputKpePath -Parent
if ($dir -and -not (Test-Path $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$stream = [System.IO.File]::Open($OutputKpePath, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write)
$writer = New-Object System.IO.BinaryWriter($stream)

try {
    $writer.Write([UInt16]0x5A4D)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    1..4 | ForEach-Object { $writer.Write([UInt16]0) }
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    1..10 | ForEach-Object { $writer.Write([UInt16]0) }
    $writer.Write([UInt32]$peOffset)

    while ($stream.Position -lt $peOffset) {
        $writer.Write([byte]0)
    }

    $writer.Write([UInt32]$KpeSignature)
    $writer.Write([UInt16]$MachineAur64)
    $writer.Write([UInt16]$sectionCount)
    $writer.Write([UInt32][DateTimeOffset]::UtcNow.ToUnixTimeSeconds())
    $writer.Write([UInt32]0)
    $writer.Write([UInt32]0)
    $writer.Write([UInt16]$optionalHeaderSize)
    $writer.Write([UInt16]($FileCharExecutable -bor $FileCharLargeAware))

    $writer.Write([UInt16]$OptionalMagicAur64)
    $writer.Write([byte]1)
    $writer.Write([byte]0)
    $writer.Write([UInt32]$rawSize)
    $writer.Write([UInt32]$sizeOfInitializedData)
    $writer.Write([UInt32]0)
    $writer.Write([UInt32]$effectiveEntryRva)
    $writer.Write([UInt32]$sectionVA)
    $writer.Write([UInt64]$ImageBase)
    $writer.Write([UInt32]$SectionAlignment)
    $writer.Write([UInt32]$FileAlignment)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]1)
    $writer.Write([UInt16]0)
    $writer.Write([UInt32]0)
    $writer.Write([UInt32]$sizeOfImage)
    $writer.Write([UInt32]$sizeOfHeaders)
    $writer.Write([UInt32]0)
    $writer.Write([UInt16]$subsystemValue)
    $writer.Write([UInt16]($DllCharDynamicBase -bor $DllCharNxCompat))
    $writer.Write([UInt64]0x200000)
    $writer.Write([UInt64]0x20000)
    $writer.Write([UInt64]0x200000)
    $writer.Write([UInt64]0x20000)
    $writer.Write([UInt32]0)
    $writer.Write([UInt32]16)

    for ($dirIndex = 0; $dirIndex -lt 16; $dirIndex++) {
        if ($hasRelocSection -and $dirIndex -eq $BaseRelocDirectoryIndex) {
            $writer.Write([UInt32]$relocSectionVA)
            $writer.Write([UInt32]$relocPayloadSize)
        } else {
            $writer.Write([UInt32]0)
            $writer.Write([UInt32]0)
        }
    }

    $textNameBytes = [System.Text.Encoding]::ASCII.GetBytes(".text")
    $textNameField = New-Object byte[] 8
    [Array]::Copy($textNameBytes, $textNameField, $textNameBytes.Length)
    $writer.Write($textNameField)
    $writer.Write([UInt32]$payloadSize)
    $writer.Write([UInt32]$sectionVA)
    $writer.Write([UInt32]$rawSize)
    $writer.Write([UInt32]$pointerToRawData)
    $writer.Write([UInt32]0)
    $writer.Write([UInt32]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt16]0)
    $writer.Write([UInt32]($SectionCharCode -bor $SectionCharExec -bor $SectionCharRead))

    if ($hasRelocSection) {
        $relNameBytes = [System.Text.Encoding]::ASCII.GetBytes(".reloc")
        $relNameField = New-Object byte[] 8
        [Array]::Copy($relNameBytes, $relNameField, $relNameBytes.Length)
        $writer.Write($relNameField)
        $writer.Write([UInt32]$relocPayloadSize)
        $writer.Write([UInt32]$relocSectionVA)
        $writer.Write([UInt32]$relocRawSize)
        $writer.Write([UInt32]$relocPointerToRaw)
        $writer.Write([UInt32]0)
        $writer.Write([UInt32]0)
        $writer.Write([UInt16]0)
        $writer.Write([UInt16]0)
        $writer.Write([UInt32]($SectionCharInitData -bor $SectionCharRead))
    }

    while ($stream.Position -lt $sizeOfHeaders) {
        $writer.Write([byte]0)
    }

    $writer.Write($payload)
    while (($stream.Position - $pointerToRawData) -lt $rawSize) {
        $writer.Write([byte]0)
    }

    if ($hasRelocSection) {
        while ($stream.Position -lt $relocPointerToRaw) {
            $writer.Write([byte]0)
        }

        $writer.Write($relocPayload)
        while (($stream.Position - $relocPointerToRaw) -lt $relocRawSize) {
            $writer.Write([byte]0)
        }
    }
}
finally {
    $writer.Close()
    $stream.Close()
}

Write-Output "Packed KPE image: $OutputKpePath"
if ($hasRelocSection) {
    Write-Output "Embedded base relocation data: $relocPayloadSize bytes"
}
