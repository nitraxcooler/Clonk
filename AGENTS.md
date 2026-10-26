# Clonk — AI Development Instructions

## Project

Clonk is a custom hobby operating system written primarily in C/C++ and designed to eventually run on real x86-64 hardware.

The project is developed collaboratively by human programmers, designers, and AI coding agents.

The human developers make the architectural and design decisions. AI agents are collaborators and implementation assistants, not autonomous project owners.

---

## Current State

* Bootloader: Limine
* Architecture: x86-64
* Primary development/test environment: QEMU
* Build system: GNU Make
* Repository: Git/GitHub
* Collaborative editing: VS Code Live Share
* Current interface: CLI
* Current shell commands:

  * `help`
  * `echo`
  * `version`
  * `clear`
* Custom font/rendering code exists.
* Custom keyboard/input code exists.
* Interrupt/IDT code exists.
* A proper kernel panic system is planned.
* A custom filesystem called **ClonkFS** is planned.

Inspect the repository before making assumptions about the current implementation.

---

## Core Rules

### 1. Do not blindly rewrite the project

Do not replace functioning systems simply because another implementation is more conventional.

Preserve existing architecture unless there is a concrete technical reason to change it.

### 2. Do not make major architectural decisions without the human developer

For major changes such as:

* replacing Limine
* changing the build system
* changing architecture
* replacing the rendering system
* introducing a new kernel architecture
* replacing the filesystem architecture
* introducing major external dependencies

first explain the proposed approach and its consequences.

### 3. Inspect before modifying

Before changing code:

1. Inspect the relevant files.
2. Understand the existing implementation.
3. Identify dependencies.
4. Identify build-system requirements.
5. Determine how the change fits into the current architecture.

Never assume a file's contents from its name alone.

### 4. Keep changes focused

Only modify files relevant to the requested task.

Do not perform unrelated cleanup or refactoring during feature work unless explicitly requested.

### 5. Explain unfamiliar low-level concepts

Clonk is also a learning project.

When implementing unfamiliar OS concepts, explain:

* what the mechanism does
* why it is needed
* how it interacts with the existing kernel
* important hardware/architecture considerations
* important limitations

Do not simply dump unexplained kernel code.

---

## C/C++ Kernel Rules

Clonk is a freestanding kernel environment.

Do not assume a normal desktop C/C++ runtime exists.

Be careful with:

* exceptions
* RTTI
* dynamic allocation
* global constructors
* standard library dependencies
* compiler-generated runtime functions
* libc assumptions
* threading assumptions

Before introducing a C++ feature that may require runtime support, verify that Clonk provides the required support.

The existing project may contain C code. Do not automatically rename `.c` files to `.cpp` or convert the entire project to C++.

C-to-C++ migration should be deliberate and incremental.

---

## Limine

Clonk currently uses Limine.

Do not replace Limine unless explicitly instructed.

Treat Limine as third-party/boot infrastructure rather than Clonk application logic.

Do not modify third-party Limine code to solve problems that belong in Clonk unless there is a specific reason.

---

## Architecture

Keep architecture-specific code separated from architecture-independent kernel code.

Current target:

```text
x86_64
```

Architecture-specific code should eventually live under:

```text
src/arch/x86_64/
```

Do not pretend that code is portable when it depends directly on x86-64 hardware.

---

## Drivers

Drivers should eventually be organized independently from the kernel core.

Examples:

```text
src/drivers/
├── keyboard/
├── display/
├── storage/
├── pci/
├── timer/
└── ...
```

Do not introduce drivers as giant collections of unrelated functions.

Keep hardware access separate from higher-level kernel logic where practical.

---

## Graphics and Fonts

Clonk has a custom font system.

The existing font's spacing and proportions are intentional.

Do not replace the font renderer merely because the font looks unusual.

Separate:

```text
font data
font rendering
framebuffer/display hardware
```

as the graphics subsystem grows.

---

## Kernel Panic

A proper Clonk kernel panic system is planned.

The intended panic screen:

* red background
* large centered `KERNEL PANIC`
* human-readable reason
* technical diagnostic information
* no automatic restart
* manual restart required

The panic system should eventually record useful information such as:

* panic reason
* exception number
* error code
* RIP
* RSP
* relevant registers
* CR2 for page faults
* kernel version
* useful diagnostic state

Panic handling must remain as independent and robust as possible because the kernel may already be in a damaged state.

Do not rely on complex subsystems unnecessarily during a panic.

A development shell command called:

```text
panic
```

is planned.

It should eventually trigger the real panic path rather than merely displaying a fake screen.

---

## ClonkFS

ClonkFS is the planned native Clonk filesystem.

Do not implement ClonkFS as a joke-only feature. It must have a real filesystem design.

Potential long-term features:

* superblock
* directories
* files
* metadata
* block allocation
* free-space tracking
* checksums
* crash recovery
* journaling
* fragmentation handling

Start simple.

Do not attempt to implement a production-grade filesystem in one change.

A minimal read-only implementation may be preferable before implementing writes.

ClonkFS may use humorous user-facing health terminology such as:

* Fresh
* Stale
* Moist
* Wet
* Dry
* Moldy
* Burnt
* Crumbly
* Rock Hard

These labels must correspond to real technical conditions and must not replace actual diagnostic error codes.

---

## Testing

QEMU is the primary controlled development environment.

A feature is not considered complete merely because it compiles.

Whenever practical:

1. Build Clonk.
2. Boot it in QEMU.
3. Test the affected functionality.
4. Record failures.
5. Fix the implementation.
6. Report the result.

Do not claim something works without testing it.

Remember that QEMU success does not guarantee physical hardware compatibility.

---

## Physical Hardware

Clonk is eventually intended to run on real x86-64 hardware.

Avoid unnecessary QEMU-specific assumptions.

Future hardware considerations include:

* UEFI
* ACPI
* APIC
* PCI
* memory maps
* storage controllers
* USB
* input devices
* framebuffer/display hardware
* CPU feature differences
* timing differences

Do not optimize for physical hardware at the expense of having a controllable QEMU development environment.

---

## Filesystem and Storage

Separate storage layers conceptually:

```text
hardware/controller
        ↓
block device
        ↓
VFS
        ↓
filesystem
        ↓
files/directories
```

Eventually Clonk may support:

```text
FAT32
exFAT
ClonkFS
```

Do not tightly couple the shell directly to ClonkFS.

---

## Shell

The shell should remain separate from the kernel's internal implementation.

Commands should eventually live under a structure similar to:

```text
src/shell/
└── commands/
```

Commands should call appropriate kernel APIs instead of directly manipulating unrelated kernel internals.

---

## Build System

The existing GNU Make build system is authoritative.

Before changing it:

* inspect the current `GNUmakefile`
* understand compiler flags
* understand linker flags
* understand object generation
* understand ISO generation
* understand Limine integration

Generated files should eventually live under a build directory rather than the repository root.

Do not commit generated `.o` files or ISO images unless the project explicitly decides otherwise.

---

## Git

Do not modify Git history.

Do not run destructive Git commands such as:

```text
git reset --hard
git clean -fd
git push --force
```

unless the human developer explicitly requests the exact operation.

Do not delete branches or tags.

Keep changes reviewable.

Prefer small, focused commits.

---

## Dependencies

Before adding a dependency:

1. Explain what it provides.
2. Explain why Clonk needs it.
3. Determine whether it works in a freestanding kernel environment.
4. Consider whether implementing the required functionality internally would be more appropriate.

Do not add dependencies simply to avoid understanding a small subsystem.

---

## AI Behavior

The AI agent should:

* inspect before modifying
* explain significant decisions
* keep changes focused
* test changes
* report failures honestly
* preserve existing functionality
* ask when requirements are ambiguous
* avoid unnecessary rewrites

The AI agent should NOT:

* blindly generate huge codebases
* invent APIs that do not exist
* claim untested code works
* silently rewrite architecture
* remove working functionality
* hide compiler errors
* modify unrelated files
* treat AI-generated code as automatically correct

The goal is **AI-assisted development**, not blind vibe coding.

---

## Development Priority

Prefer this general progression:

```text
Boot reliability
    ↓
Exceptions / IDT
    ↓
Kernel panic
    ↓
Memory management
    ↓
Interrupt/timer infrastructure
    ↓
Input
    ↓
Processes/threads
    ↓
Scheduler
    ↓
System calls
    ↓
Userspace
    ↓
Block storage
    ↓
VFS
    ↓
ClonkFS
    ↓
Networking
    ↓
Graphics/windowing
    ↓
Larger userspace ecosystem
```

This is a guideline, not a rigid roadmap.

Always prioritize the actual current project state and the human developer's requested task.

---

## Final Rule

**Do not assume. Inspect.
Do not blindly generate. Explain.
Do not claim success. Test.
Do not take control. Collaborate.**

Clonk belongs to its human developers.

AI exists to help build it, understand it, debug it, and improve it.