# NK Format Policy

## 1. Executable Scope (`.kpe`)

`KPE` is the executable container format for runnable program images in NK.

- Kernel image: `KPE_SUBSYSTEM_NATIVE_KERNEL`
- User program image: `KPE_SUBSYSTEM_USERLAND`
- Kernel addon/module profile: `KPE_SUBSYSTEM_SYSTEM_ADDON` (often packaged as `.ksys`)

Rule: executables supplied/instantiated by the kernel are KPE-profiled images.

## 2. Addon Outlier (`.ksys`)

`.ksys` is an addon packaging identity, not a separate executable structure.
Its payload metadata is KPE and must use `SYSTEM_ADDON` subsystem.

## 3. Non-Executable Data

Assets are not KPE:

- audio
- textures/images
- fonts
- maps/content blobs
- any other runtime data payload

These use their own dedicated data formats.

## 4. Program-Generated Outputs

Anything created by a KPE program is its own format by default, not KPE.

Exception: if the output is itself a runnable program image, it must be emitted
as KPE (with the correct subsystem profile).

## 5. Linker Artifacts

Linker/intermediate formats are distinct from KPE and remain toolchain-defined.
KPE is the final executable image container, not the linker object format.
