# Kernel for Aster (Aster64)
## This project is under a Temporary Evaluation License.
You may read, build, and run it unmodified. You may not modify or redistribute modified versions. This is not open source.

# ARCHIVED

This repository starts a custom kernel project inspired by:

- NTOSKRNL design layering (`Ke`, `Hal`, `Mm`, `Ob`, `Ps`, `Io`)
- PE-like executable handling (custom headers/sections)
- Linux style early boot pragmatism and simple bring-up paths

The initial milestone implemented here focuses on:

- UEFI bootloader (`boot/uefi/bootx64.c`)
- Custom kernel image format (`include/kpeformat.h`) - not ELF
- Custom architecture contract (`AUR64`)
- HAL bootstrap and low-level I/O (`hal/aur64/*`)
- Hybrid nucleus + executive bootstrap (`kernel/nk/*`, `kernel/ex/*`)

## Layout

- `boot/include/bootproto.h`: bootloader-to-kernel handoff contract
- `boot/uefi/bootx64.c`: UEFI loader for `KPE` kernel image
- `include/kpeformat.h`: `KPE` executable format (PE-inspired custom metadata)
- `hal/inc/hal.h`: HAL interface (NT-style layering)
- `hal/aur64/hal_aur64.c`: AUR64 HAL implementation on x86_64 firmware baseline
- `kernel/ke/init.c`: kernel entry and executive bootstrap skeleton
- `kernel/ex/init.c`: executive module table and staged manager initialization
- `kernel/nk/core.c`: nucleus IPC core for hybrid boundaries
- `kernel/include/ob.h`: object/type/header/handle manager contract
- `kernel/ob/init.c`: object manager skeleton with rights-checked handle lookup
- `kernel/se/init.c`: security manager baseline (`Se`)
- `kernel/lpc/init.c`: local-port manager baseline (`Lpc`)
- `kernel/cc/init.c`: cache manager baseline (`Cc`)
- `kernel/include/mm.h`: memory-manager page allocator contract
- `kernel/mm/init.c`: physical page allocator (`BOOT_INFO` map + reserve ranges)
- `docs/ABI_AUR64.md`: kernel entry ABI and boot handshake contract
- `docs/KPE_FORMAT.md`: format specification
- `docs/ARCHITECTURE.md`: subsystem and architecture notes
- `build/kernel_folders.rsp`: folder manifest for kernel source discovery
- `build/module_folders.rsp`: folder manifest for module source discovery
- `tools/nkcc.ps1`: custom compiler driver (`.c` -> `.obj` -> raw -> `KPE`/`KSYS`)
- `tools/ksyspack.ps1`: system module packer (`.raw` -> `.ksys`)
- `tools/kernel.ld`: linker script for raw kernel/module image output
- `tools/kpepack.ps1`: packs a raw kernel binary into `KPE` format
- `tools/build-bootx64.ps1`: builds UEFI loader binary `BOOTX64.EFI`
- `tools/make-boot-layout.ps1`: emits final boot layout (`EFI\\BOOT\\BOOTX64.EFI` + root kernel image)
- `tools/run-qemu.ps1`: stages EFI boot files + kernel and runs QEMU/OVMF with serial logging
- `tools/stage-esp.ps1`: stages `BOOTX64.EFI` + `ntxkrnl.kpe` into an ESP folder layout
- `tools/run-vbox.ps1`: creates/starts a UEFI VirtualBox VM with serial logging

## Notes

- The target architecture is declared as `AUR64` (`KX_ARCH_AUR64`).
- The target machine ID is `KPE_MACHINE_AUR64`.
- The current bring-up executes on UEFI x64 hardware while maintaining an
  architecture contract that can later be backed by either:
  - native AUR64 silicon/emulator, or
  - a translation layer in early boot.

## First Bring-up Flow

1. Build a freestanding kernel raw binary whose entry is `AsterKernelEntry`.
2. Pack it into `KPE`:
   - `powershell -ExecutionPolicy Bypass -File tools\kpepack.ps1 -InputRawPath .\build\kernel.bin -OutputKpePath .\kernel\ntxkrnl.kpe -EntryRva 0x1000`
3. Place `ntxkrnl.kpe` on the EFI boot volume at `\kernel\ntxkrnl.kpe`.
4. Build/run the UEFI loader (`boot/uefi/bootx64.c`) as your boot app.

## Build Pipeline (`NKCC`)

Use the custom compiler driver:

- `powershell -ExecutionPolicy Bypass -File tools\nkcc.ps1`

Behavior:

- Compiles all `.c` files in folders listed by `build/kernel_folders.rsp` into `.obj`.
- Links objects into raw kernel image.
- Packs raw kernel into custom `KPE` output at `dist\ntxkrnl.kpe`.
- Builds UEFI loader output at `dist\BOOTX64.EFI`.
- Embeds base-relocation blocks and supports non-fixed load addresses.
- Compiles each module directory found under folders in `build/module_folders.rsp`.
- Packs each module as `NT`-style system image (`.ksys`) in `dist\`.

To include new source files automatically:

- Add `.c` files anywhere under existing folders in the `.rsp` manifests.

To include new folder trees:

- Add the folder path to `build/kernel_folders.rsp` or `build/module_folders.rsp`.

Useful flags:

- `-DryRun`: show planned compile/link/pack commands without executing.
- `-Clean`: remove build and dist outputs before building.
- `-Validate`: compile with `NK_VALIDATE=1` and run kernel validation suite at boot.
- `-BuildBoot`: build the UEFI boot app (`BOOTX64.EFI`) as part of `nkcc`.
- `-ClangPath`, `-LldPath`, `-PowerShellPath`: override tool executable paths.
- `-LldLinkPath`: override `lld-link` path for UEFI app linking.
- `-KernelSectionRva`: set kernel link/section RVA (no fixed `0x1000` assumption).
- `-KernelEntryOffset`: set entry offset inside the primary section.

## Runtime Validation (QEMU)

After building the kernel and UEFI boot app:

- `powershell -ExecutionPolicy Bypass -File tools\run-qemu.ps1 -KernelPath dist\ntxkrnl.kpe -BootEfiPath <path-to-BOOTX64.EFI>`

The run script:

- stages `BOOTX64.EFI` and `kernel\ntxkrnl.kpe` into a FAT EFI system partition directory
- boots via OVMF pflash firmware
- writes serial output to `build\run\serial.log`

## Runtime Validation (VirtualBox)

1. Stage ESP files:

- `powershell -ExecutionPolicy Bypass -File tools\stage-esp.ps1 -KernelPath dist\ntxkrnl.kpe -BootEfiPath <path-to-BOOTX64.EFI>`

2. Build/prepare an EFI disk image from `build\esp_stage` (FAT image) using your preferred image tool.

3. Start VM:

- `powershell -ExecutionPolicy Bypass -File tools\run-vbox.ps1 -EspImagePath <path-to-efi-disk-image>`

Notes:

- `run-vbox.ps1` expects an existing disk image containing:
  - `EFI\BOOT\BOOTX64.EFI`
  - `kernel\ntxkrnl.kpe`
- Serial output is logged to `build\run-vbox\serial.log`.

Expected validation markers in serial output include:

- `[KEVT] validation mode enabled`
- `[KEVT] run syscall` / `pass syscall`
- `[KEVT] run scheduler` / worker iteration lines / `pass scheduler`
- `[KEVT] touching lazy-mapped page`
- `[KEVT] run user_smoke` / `skip user_smoke` (until ring3 transition is implemented)
- `[MMFLT]` fault trace lines
- `[KE] validation suite passed`

## Final Boot Layout Command

To create deployment layout with:

- `\EFI\BOOT\BOOTX64.EFI`
- `\nkxkrnl.kpe`

Run:

- `powershell -ExecutionPolicy Bypass -File tools\make-boot-layout.ps1 -InputDistDir dist -OutputDir build\layout -KernelInputName ntxkrnl.kpe -KernelOutputName nkxkrnl.kpe -Clean`
