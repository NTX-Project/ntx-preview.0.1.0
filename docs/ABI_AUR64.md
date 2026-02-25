# AUR64 Kernel Entry ABI (v0)

## Purpose

Define the exact boot-to-kernel handshake for early reliability and toolchain
consistency. This applies to `AsterKernelEntry` and the `BOOT_INFO` contract.

## Entry Symbol

- Entry routine: `AsterKernelEntry(BOOT_INFO *BootInfo)`
- Calling side: UEFI loader (`boot/uefi/bootx64.c`)
- Ownership: loader transfers control permanently to kernel entry

## CPU/Execution State on Entry

Required state on entry to `AsterKernelEntry`:

- 64-bit mode active (UEFI x64 baseline in current bring-up)
- Interrupts disabled before early kernel init
- A valid writable stack is active
- Paging state is implementation-defined in v0, but must remain stable until
  `HalInitializePhase0` completes

Future `AUR64` native targets may change setup internals, but must preserve the
logical contract above.

## Calling Convention + Stack

v0 policy:

- Use the platform C ABI selected by the kernel toolchain for x64 UEFI bring-up
  (do not mix ABIs inside entry path).
- Stack alignment at entry must be at least 16-byte aligned.
- Entry code must avoid assumptions about red-zone usage unless toolchain flags
  explicitly guarantee behavior.

If a custom `aur64` ABI is introduced, this document must be version-bumped and
the loader/kernel pair must be updated atomically.

## BOOT_INFO Contract

`BootInfo` must satisfy:

- non-null pointer
- `Magic == BOOTINFO_MAGIC`
- `Version == BOOTINFO_VERSION` (or explicitly accepted compatibility range)
- struct size is large enough for fields consumed by kernel

Minimum fields consumed today:

- firmware identity (`FirmwareType`)
- kernel image placement (`KernelBase`, `KernelSize`, `KernelEntry`)
- optional init image handoff (`InitImageBase`, `InitImageSize`, `BOOTINFO_FLAG_INIT_IMAGE_PRESENT`)
- memory map pointer/count/entry-size
- basic video information

## BOOT_INFO Ownership + Lifetime

- Loader provides the memory backing `BOOT_INFO` and memory map.
- Kernel treats it as read-only input during early boot.
- HAL/NK may cache pointer references in v0.
- In later milestones, kernel should copy critical boot metadata into owned
  kernel memory before reclaiming loader-owned pages.

## Early Console Requirement

- Serial output is mandatory from `HalInitializePhase0` onward.
- Any early boot fatal path must emit at least one diagnostic line before halt.

## Compatibility Rules

- `BOOT_INFO` changes require incrementing `BOOTINFO_VERSION`.
- New fields should append to the struct where possible.
- Kernel must fail fast with a clear message on ABI mismatch.

## Validation Checklist (Boot Path)

1. Loader sets `BOOT_INFO` fields and transfers control to kernel entry.
2. Kernel validates `BootInfo`, `Magic`, and expected version.
3. HAL phase 0 initializes serial and baseline platform data.
4. Nucleus and executive init proceed only after ABI checks pass.

## Syscall ABI (v0)

Current entry mechanism:

- software interrupt vector `0x80` (`int 0x80`)
- IDT gate is configured with DPL3 to allow future user-mode invocation

Register contract:

- `RAX`: syscall number
- `RDI`, `RSI`, `RDX`, `R10`, `R8`, `R9`: arguments 0..5
- return value in `RAX`

Current syscall table:

- `0`: `NkDebugPrint`
- `1`: `NkYield`
- `2`: `NkSend`
- `3`: `NkReceive`
- `4`: `NkMapMemory`
- `5`: `NkExitThread`
- `6`: `NkCreatePort`
- `7`: `NkCloseHandle`
- `8`: `NkCreateSection`
- `9`: `NkMapSection`
- `10`: `NkCreateProcessFromBuffer`
- `11`: `NkOpen`
- `12`: `NkRead`
- `13`: `NkClose`
- `14`: `NkCreateProcessFromFileHandle`
- `15`: `NkReadConsole`

Error convention:

- negative `KSTATUS` values are returned in `RAX` as signed 64-bit values.
