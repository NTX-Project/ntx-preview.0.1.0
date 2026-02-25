# Architecture and Layering

## AUR64 (custom architecture contract)

`AUR64` is the architecture target exposed by the kernel ABI and image format.
Early bring-up currently runs under UEFI x64 firmware, but all higher layers are
written against the `AUR64` contract and `Hal*` boundary.

## Kernel Shape

The structure follows a hybrid model:

- NT-style executive managers and HAL layering
- PE-inspired custom image metadata (`KPE` headers + sections)
- Linux-inspired staged initialization discipline

Hybrid policy:

- Monolithic fast-path: `Ob`, `Mm`, `Io`, `Ps` managers run in-kernel
- Microkernel-style boundary: `Nk*` message ports define service edges
- NT-style handling: `Ke` orchestrates stage ordering and manager ownership

Current executive manager set:

- Core managers: `Ob`, `Se`, `Mm`, `Cc`, `Io`, `Ps`, `Lpc`
- Hybrid boundary manager: `Ex` endpoint (`ex.control`) over `Nk` service ports

## Bring-up Stages

1. `EARLY`: firmware handoff + `HalInitializePhase0`
2. `CORE`: trap/interrupt baseline, timer baseline, `Nk` IPC core
3. `EXEC`: `Ob`, `Se`, `Mm`, `Cc`, `Io`, `Ps`, `Lpc` initialization
4. `LATE`: policy modules and services (future)

`EXEC` is initialized through a module descriptor table (`EX_MODULE_DESCRIPTOR`)
to keep ordering deterministic while preserving extensibility for future managers.

## HAL Contract

`Hal*` exposes only platform mechanism:

- debug output
- interrupts enable/disable
- stall and time source
- platform topology query
- halt path

Policy remains in nucleus/executive layers.

## Microkernel-style Service Edges

`Nk` now includes service endpoints (`NK_SERVICE_ENDPOINT`) layered on top of
its message ports:

- `lpc.system` endpoint is owned by `Lpc`
- `ex.control` endpoint is owned by `Ex`
- bootstrap binding uses `LPC_OPCODE_BIND_ENDPOINT`

This keeps the monolithic fast path in-kernel while giving explicit message
boundaries for decomposing services over time.
