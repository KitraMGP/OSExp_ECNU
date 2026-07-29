# Repository Guidelines

## Project Overview

This repository incrementally implements an ECNU teaching operating-system kernel from xv6 and the templates in the sibling `../ecnu-oslab-2025-task` repository. Code is cumulative across `lab-n` branches; experiment-specific goals, implementation status, test cases, and observed output belong only in the active branch's `README.md`.

When starting a new experiment:

1. Create `lab-n` from this repository's completed previous lab branch.
2. Switch `../ecnu-oslab-2025-task` to the matching `lab-n` branch before reading instructions or copying templates.
3. Compare the template with inherited code. In the guide, `NEW`/`CHANGE` files are supplied updates; `TODO` marks work to implement. Preserve inherited behavior unless the new guide intentionally changes it.
4. Replace this repository's `README.md` with a new report for that experiment. Include its content and goals, completed work, underlying principles, implemented behavior, test code, and actual test output.
5. Keep diagrams that explain operating-system principles. Do not retain command-line screenshots used only to show expected output; record verified output as text instead.

## Architecture & Data Flow

The stable execution path is:

```text
QEMU virt -> kernel.ld:_entry -> boot/start.c:start (M-mode)
          -> kernel/main.c:main (S-mode initialization)
          -> process/user code (U-mode)
          -> trap trampoline -> trap handler -> syscall/interrupt service
          -> trap return -> U-mode
```

- `src/kernel/boot/entry.S` selects a per-hart boot stack. `boot/start.c` configures delegation, timer/PMP state, and transitions from M-mode to S-mode.
- `src/kernel/main.c` orders boot-hart initialization and per-hart initialization. Preserve barriers and initialization order when adding subsystems.
- User code follows the RISC-V syscall ABI: syscall number in `a7`, arguments in `a0`-`a5`, result in `a0`. Assembly saves state into `trapframe_t`; C handlers dispatch the request and return through the trampoline.
- Kernel state is explicit, not framework-managed: module-static global state, per-CPU state addressed through `tp`, process state, page tables, and lock-protected shared structures. There is no dependency-injection or async framework; concurrency comes from harts, interrupts, traps, and context switches.
- The build compiles `src/user/*.c`, links the user entry at address zero, converts it with `objcopy`/`xxd` into generated `src/user/initcode.h`, then embeds it in the kernel. Kernel C/assembly is linked by `kernel.ld` into `target/kernel/kernel-qemu.elf` and booted by QEMU.

Architecture-sensitive contracts require synchronized changes: `trapframe_t` field offsets and trampoline assembly, CSR/privilege transitions, stack layout, page-table mappings, linker symbols, interrupt claim/complete handling, and lock/interrupt nesting.

## Key Directories

- `src/kernel/`: freestanding kernel source. Stable areas include `arch/` (RISC-V constants/CSR helpers), `boot/`, `lock/`, `lib/`, `mem/`, `proc/`, and `trap/`; later labs may add modules.
- `src/kernel/<module>/`: modules normally expose shared types/constants through `type.h`, public functions through `method.h`, and aggregate dependencies through `mod.h`.
- `src/user/`: user entry/test payload and syscall wrappers. `initcode.h` is generated; edit `initcode.c`, not the generated header.
- `target/`: generated objects, dependency files, user payload, and kernel ELF. Never edit or commit it.
- `picture/` or other README-referenced image directories: lab report assets only; keep conceptual diagrams, not terminal-output screenshots.
- `../ecnu-oslab-2025-task/`: matching-branch experiment guide and template source. Treat it as an input, not as the implementation repository.

## Development Commands

Run commands from the repository root:

```sh
make build   # compile user payload and kernel ELF
make run     # build and boot headless QEMU
make debug   # build, generate .gdbinit, start QEMU halted for remote GDB
make clean   # remove target/ and generated src/user/initcode.h
```

For debugging, attach a RISC-V-capable GDB using the generated `.gdbinit`. The Makefile computes the GDB port dynamically. `.vscode/launch.json` hard-codes a port, and `.vscode/tasks.json` invokes an undefined `make qemu-gdb`; prefer the Makefile's `make debug` flow unless those editor files are corrected.

There is no repository lint or format command.

## Code Conventions & Common Patterns

- C is freestanding GNU C17. Preserve the local four-space/assembly formatting and avoid unrelated mass formatting; no formatter configuration is present.
- Use lowercase `snake_case`; prefix functions by subsystem (`pmem_*`, `vm_*`, `proc_*`, `trap_*`, `spinlock_*`). Use `_t` for typedefs and uppercase names for constants/macros.
- Keep private helpers/state `static`. Implementations normally include their local `mod.h`; update `type.h`, `method.h`, and `mod.h` consistently when changing a module boundary.
- Use the kernel's own `memset`, `memmove`, `printf`, `assert`, and `panic`; the kernel has no hosted C library. Do not introduce libc, heap allocation, threads, exceptions, or runtime facilities without the active lab explicitly implementing them.
- Use `assert(condition, "context")`/`panic("context")` for violated kernel invariants and impossible states. Validate addresses, overflow, alignment, page-table entry type/permissions, trap origin, and interrupt state at subsystem boundaries.
- Protect shared mutable state with the existing spinlock API. `spinlock_acquire()` disables interrupts through nested `push_off()` and rejects recursive acquisition; every path must release the lock and balance interrupt state. Do not sleep or perform unbounded work while holding a spinlock.
- Access user pointers through page-table-aware copy helpers, never by direct kernel dereference. Apply `sfence.vma`/the existing helper when page-table changes require TLB synchronization.
- Treat assembly-visible C layouts and linker symbols as ABIs. If a C structure offset changes, update and verify every assembly consumer in the same change.

## Important Files

- `README.md`: sole authority for the active experiment's requirements, completed work, tests, and results.
- `../ecnu-oslab-2025-task/README.md`: matching experiment guide; switch the sibling checkout to the same branch first.
- `Makefile`: authoritative build graph, generated-file dependencies, QEMU options, and supported targets.
- `common.mk`: cross-toolchain names and freestanding compiler/linker flags.
- `kernel.ld`: kernel entry, address layout, trampoline placement, and allocator boundary symbols.
- `src/kernel/boot/entry.S`, `src/kernel/boot/start.c`, `src/kernel/main.c`: boot and privilege-transition entry path.
- `src/kernel/proc/type.h` and `src/kernel/trap/*.S`: shared C/assembly context and trapframe ABI.
- `.gdbinit.tmpl-riscv`, `registers.xml`, `.vscode/launch.json`: optional debugging support; generated `.gdbinit` and the Makefile port are authoritative.

## Runtime/Tooling Preferences

- Use GNU Make and the toolchain selected by `common.mk` (currently `riscv64-elf-gcc`, `ld`, `objcopy`, and `objdump`). Do not substitute the host compiler.
- Runtime target: 64-bit RISC-V QEMU `virt`, no firmware, 128 MiB RAM, two harts, serial-only output via `qemu-system-riscv64 -nographic`.
- Supporting host tools include `xxd`, `sed`, and a RISC-V-capable GDB such as `gdb-multiarch`.
- This is not a Node/Bun project. There is no package manager, dependency manifest, hosted runtime, CI configuration, or scripting framework.
- Compiler warnings are errors except the configured unused-function exception. Preserve `-ffreestanding`, `-nostdlib`, medany, no-relax, no-PIE/stack-protector, debug-info, and 4096-byte linker-page constraints unless the architecture contract changes.

## Testing & QA

There is no unit-test framework, grading harness, coverage threshold, CI pipeline, or `make test` target in this repository. Validation is end-to-end:

1. Read the active `README.md` for the current branch's acceptance cases.
2. Put the lab's user-mode scenario in `src/user/initcode.c` and add focused kernel checks where appropriate.
3. Run `make build` to catch warnings, dependency, link, and generated-header failures.
4. Run `make run`; exercise the changed path in QEMU and compare serial output with the guide's required behavior. Stop QEMU manually after observing the result.
5. Use `make debug` and GDB for traps, register state, page tables, or multi-hart failures.
6. Record test code and actual textual output in `README.md`; never present planned or expected output as observed output.

Kernel `assert` prints context and then panics; `panic` prints and spins. For shared-state changes, exercise relevant multi-hart/interruption paths. For memory or trap changes, cover valid behavior plus alignment, boundary, permission, overflow, and invalid-state cases required by the active experiment. Numeric coverage is not tracked; the README's experiment-specific cases define required QA.
