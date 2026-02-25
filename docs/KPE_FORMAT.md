# KPE Kernel Image Format (custom, PE-inspired, non-ELF)

`KPE` is a custom kernel executable format that borrows structural ideas from PE:

- DOS-style prefix (`MZ`, `e_lfanew`)
- NT-style main signature and file header
- Optional header describing image base, alignments, entrypoint
- Section table with RVA/raw mapping

This is a kernel-native format (`KPE_SIGNATURE = "KPE0"`), not Windows PE.

See also: `docs/FORMAT_POLICY.md` for project-wide executable vs data format rules.

## Image Kinds (same `.kpe` container)

`KPE` is used for kernel-instantiated/supplied executable images. They are distinguished by
`OptionalHeader.Subsystem`:

- `KPE_SUBSYSTEM_NATIVE_KERNEL` (`1`): kernel/system image
- `KPE_SUBSYSTEM_USERLAND` (`2`): userland image
- `KPE_SUBSYSTEM_SYSTEM_ADDON` (`3`): kernel addon/module payload profile

`KSYS` is treated as an addon packaging outlier: the file extension is `.ksys`,
but payload metadata is KPE with `Subsystem = SYSTEM_ADDON`.

Current UEFI boot loader only accepts `NATIVE_KERNEL` KPE images.

## Core Structures

1. `KPE_DOS_HEADER`
2. `KPE_NT_HEADERS64`
3. `KPE_SECTION_HEADER[]`

`include/kpeformat.h` defines all on-disk structures and constants.

## Machine and ABI

- `KPE_MACHINE_AUR64`: required machine id
- `KPE_OPT_MAGIC_AUR64`: required optional-header magic

## Loader Rules

UEFI loader (`boot/uefi/bootx64.c`) performs:

1. Open `\kernel\ntxkrnl.kpe`
2. Validate DOS header, signature, machine id, optional header
3. Allocate `SizeOfImage` at preferred `ImageBase` (or relocate if dynamic base)
4. Zero image memory, copy headers, map each section by RVA
5. Transfer control to `ImageBase + AddressOfEntryPoint`

## Relocation Policy

If preferred base allocation fails:

- Allowed only when `KPE_DLLCHAR_DYNAMIC_BASE` is set
- Loader applies `KPE_DIR_BASERELOC` blocks (`DIR64` entries) using base delta
- If relocation data is missing/invalid for a rebased load, image load fails
