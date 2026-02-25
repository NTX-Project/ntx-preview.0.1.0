# NK Kernel Plan (NT + Mach Hybrid)

## Vision

Build a modular hybrid kernel with:

- NTOSKRNL-like executive subsystem structure and in-kernel fast paths
- Mach-like first-class IPC via ports/messages and capability transfer
- A custom ABI, custom compiler toolchain, and custom executable formats (`KPE`, `KX`)

Core principle:

- `Object Manager + Handles` is the universal resource model
- `Ports + Messages` are first-class kernel objects, not a side subsystem

## Locked Early Decisions (Spine)

1. ABI + Boot Contract (`aur64`)
- Kernel entry contract is explicit and versioned (`BOOT_INFO`).
- Entry assumptions must be written down in code/docs:
  - CPU mode
  - paging state
  - stack alignment
  - calling convention
- Serial debug output is mandatory in all boot stages.

2. Memory Model
- Layered bring-up order:
  - early physical page allocator (bitmap/buddy)
  - kernel virtual mapping (higher-half)
  - kernel pool/heap allocator (slab/pool for objects)
  - VM features (fault handling, COW later)
- Distinguish page allocation vs object/pool allocation.

3. Object Model
- Everything important is a kernel object with:
  - type descriptor (ops table)
  - refcount
  - access mask
  - optional name
  - optional waitable state
- Required core object types:
  - Process, Thread
  - AddressSpace/VMAR
  - File, Device, Driver
  - Event, Semaphore, Mutex
  - Port, Message, Channel
  - Section (shared memory)

4. Scheduler + Interrupt Model
- Preemptive scheduler: fixed priorities + RR within priority.
- IRQ handling split:
  - hard IRQ top-half (minimal, acknowledge, queue work)
  - bottom-half (DPC/work queue)
- IRQL-like model:
  - per-CPU disable depth
  - software priority threshold

5. Syscall Boundary
- Define syscall table early, even if minimal:
  - `NkCreateProcess`, `NkCreateThread`
  - `NkMapMemory`, `NkUnmapMemory`
  - `NkSend`, `NkReceive`
  - `NkOpen`, `NkRead`, `NkWrite` (later)

6. IPC Performance Contract
- Small message fast path: fixed-size inline payloads.
- Large payload path: section/handle transfer (zero-copy intent).
- Ports are capabilities and transferable in messages.

## Architecture Direction

- Monolithic core for performance and driver simplicity.
- Microkernel-like boundaries where useful:
  - services exposed through `NK_SERVICE_ENDPOINT` and capability-guarded ports.
- Keep drivers in-kernel initially; revisit split only after profiler-guided evidence.

## Practical Build Order (Milestones)

## M0 - Boot Reliability
- Bootloader -> kernel handoff verified on target platform.
- Exception vectors installed.
- Serial logging always available.
- Panic path and halt path deterministic.

## M1 - Memory Foundation
- Physical page allocator operational.
- Basic kernel virtual mappings.
- Kernel pool/slab allocator for objects.
- Allocation stats exposed in debug output.

## M2 - Object + Handle Core
- Object header/type system.
- Handle table implementation with rights checks.
- Reference counting + leak checks in debug builds.
- Waitable object interface baseline.

## M3 - Scheduler + IRQ/DPC
- Timer tick and preemption.
- Thread states and runqueues.
- IRQ top-half and deferred work queue.
- IRQL-like guard rails enforced in assertions.

## M4 - User Boundary + Minimal Syscalls
- User mode entry for one init task.
- Syscall dispatcher + table.
- `NkDebugPrint` and one object syscall working end-to-end.

## M5 - IPC First-Class
- Port creation/connect/send/receive syscalls.
- Capability transfer in message metadata.
- Section transfer for large payload path.
- Basic policy checks in `Se`.

## M6 - Program Loading
- Minimal loader for user binaries (custom format).
- Relocation and segment mapping baseline.
- Start `init` process from loader.

## M7 - VFS + Drivers
- VFS abstraction and root mount.
- Driver object model and registration.
- Device IO path through `Io`.

## Immediate Next Sprint (Do This Now)

1. Write `docs/ABI_AUR64.md`:
- entry mode/state guarantees
- stack and calling convention
- boot info ownership/lifetime

2. Add object header + handle table skeleton:
- new `Ob` headers (`OBJECT_HEADER`, `OBJECT_TYPE`, `HANDLE_TABLE`)
- rights-mask checks on lookup path

3. Implement early physical page allocator in `Mm`:
- memory-map ingestion from `BOOT_INFO`
- reserve kernel/boot ranges
- `MmAllocPage/MmFreePage` primitives

4. Add scheduler bootstrap in `Ke/Ps`:
- idle thread
- timer tick accounting
- single-core round-robin first

5. Upgrade `Lpc` and `Nk` IPC path:
- add message flags and transferable-handle metadata
- enforce access checks through `SeAccessCheck`

6. Add debug observability baseline:
- object IDs and type names
- ring buffer for last N log lines
- panic dump of threads/runqueue/object counts

## Next Natural Steps (Execution Queue)

1. Implement `Mm` page allocator v0:
- choose bitmap or buddy allocator
- ingest `BOOT_INFO` memory map
- reserve kernel/boot regions
- expose `MmAllocPage`/`MmFreePage`

2. Move `Ob` from fixed pools to dynamic allocation:
- replace static type/object arrays with `Mm`-backed allocations
- add per-process handle table scaffolding

3. Add syscall boundary:
- syscall dispatcher and syscall table
- first calls: `NkDebugPrint`, `NkSend`, `NkReceive`

4. Extend `Lpc` IPC with capability transfer:
- message metadata carries transferable handles
- enforce rights checks on transfer and receive paths

5. Validate module path end-to-end:
- add one real module under `modules/<name>/`
- build `.ksys` via `nkcc`
- register module service through `Lpc`

6. Expand kernel diagnostics:
- object/type dump
- handle table dump
- runqueue snapshot
- ring-buffer log dump

7. Add build automation:
- add CI/local automation entrypoint for `tools/nkcc.ps1`
- require `-DryRun` and full build jobs on toolchain-capable runner

## Current Status Snapshot

Already in place:

- Executive module table startup (`Ex`)
- Baseline `Se`, `Lpc`, `Cc` managers
- `NK_SERVICE_ENDPOINT` abstraction over message ports
- Early hybrid binding path (`ex.control` -> `lpc.system`)

Missing for next maturity step:

- real object manager and handle tables
- real page allocator and VM primitives
- scheduler and user-mode boundary
- syscall dispatcher and loader for user programs

## Debuggable-by-Construction Rules

- Every kernel object gets:
  - unique object ID
  - type name
  - refcount visible in debug dump
- Every panic prints:
  - current thread
  - runqueue summary
  - recent log ring entries
- Never add major subsystem code without a dump/trace path.

## Design Guardrails

- Do not overbuild loader/relocation in v0.
- Keep v0 IPC payload format simple and fixed.
- Keep policies out of HAL.
- Keep subsystem boundaries clear (`Ke`, `Mm`, `Ob`, `Ps`, `Io`, `Se`, `Lpc`, `Cc`, `Nk`).
- Prefer measurable progress over architecture churn.
