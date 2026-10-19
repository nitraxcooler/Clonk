# MyOS

> A hobby operating system built from scratch in C for x86-64, booting with Limine.

MyOS is a personal operating system project focused on learning how operating systems work from the hardware up. Instead of building on Linux or another existing kernel, MyOS is developed as its own kernel with custom drivers, memory management, and graphics.

The long-term goal is to grow MyOS from a command-line kernel into a complete operating system with its own filesystem (**MyBread**), graphical desktop (**MyGUI**), and userspace.

---

## Current Features (v0.1.1)

* x86-64 kernel
* Limine bootloader support
* Interrupt Descriptor Table (IDT)
* Interrupt Service Routines (ISR)
* CPU exception handling
* Full-screen Kernel Panic diagnostics
* Physical Memory Manager (PMM)
* Virtual Memory Manager (VMM) — 4-level paging with PMM-allocated page tables
* Isolated user address spaces with supervisor-only shared kernel mappings
* x86-64 ring 3 transitions using a GDT, TSS kernel stack, and `iretq`
* Validated static ELF64 executable loader
* `int 0x80` syscall ABI with validated `write`, blocking keyboard `read`, `clear`, and `exit`
* Limine-loaded persistent Ring 3 `/init` shell (`help`, `echo`, `clear`, `version`, `exit`)
* Page fault diagnostics (CR2 capture + error-code decoding + register dump)
* Kernel heap (`kmalloc` / `kfree` / `kcalloc` / `krealloc`)
* Block device abstraction
* RAM disk driver
* Virtual Filesystem (VFS)
* FAT32 read/write support
* Keyboard driver
* Custom bitmap font renderer
* Command Line Interface (CLI)
* Built and tested in QEMU

### Built-in Commands

| Command    | Description                        |
| ---------- | ---------------------------------- |
| `help`     | Show available commands            |
| `clear`    | Clear the screen                   |
| `version`  | Show MyOS version                  |
| `echo`     | Print text                         |
| `mem`      | Display physical memory statistics |
| `vmem`     | Display virtual memory statistics  |
| `heap`     | Display kernel heap statistics     |
| `selftest` | Re-run all kernel self-tests       |
| `panic`    | Trigger a test Kernel Panic        |
| `pfault`   | Trigger a controlled page fault    |
| `badalloc` | Trigger out-of-memory (heap exhaustion) |
| `blkinfo`  | List block devices                 |
| `ls`       | List directory contents            |
| `cat`      | Print a file                       |
| `touch`    | Create an empty file               |
| `write`    | Write text to a file               |
| `rm`       | Delete a file                      |
| `mkdir`    | Create a directory                 |

### Initial userspace

The boot ISO includes a freestanding `/init` ELF64 executable. It runs in a
fresh lower-half address space; only supervisor-only kernel mappings are
shared. The syscall ABI is defined in `src/abi/syscall.h`: invoke `int 0x80` with the
syscall number in `RAX`, arguments in `RDI` and `RSI`, and read a signed result
from `RAX`; other general-purpose registers are preserved. `MYOS_SYS_WRITE`
takes a user buffer and byte count, emits up to 256 validated bytes to the
framebuffer and COM1, and returns `0` or `-1`. `MYOS_SYS_READ` takes a
writable user buffer and a length of exactly one; it blocks until one translated
keyboard character is available, stores that character (including Enter as
`\n` and Backspace as `\b`), and returns `1`, or returns `-1` for invalid
arguments. The kernel waits with interrupts enabled and `hlt`; this is a
single-process wait, not scheduler-backed blocking. `MYOS_SYS_CLEAR` takes no
arguments, clears the framebuffer, and returns `0` (invalid arguments return
`-1`). `MYOS_SYS_EXIT` accepts an optional status in `RDI` (currently ignored),
does not return, and resumes the kernel shell. Unknown syscall IDs and invalid
buffers return `-1`. Boot initializes the kernel heap, RAM disk, VFS, and FAT32
root mount before starting `/init`.

`/init` is a persistent Ring 3 shell with `help`, `echo TEXT`, `clear`,
`version`, and `exit`. It supports basic character input, shift for letters,
Backspace editing, empty lines, and command lines up to 127 characters;
oversized lines are discarded rather than executed in truncated form. The
keyboard IRQ only translates and queues key events. While `/init` runs, its
`read` syscall consumes those events through a bounded kernel-owned queue; on
exit, queued user input is discarded and the existing kernel shell resumes
receiving new keys. Kernel-shell line editing and command dispatch run outside
the IRQ handler. The queue holds up to 127 characters and drops new events if
full.

The loader currently accepts little-endian x86-64 `ET_EXEC` ELF64 files with
up to 128 program headers and up to 64 MiB of non-overlapping `PT_LOAD`
pages in the user range beginning at `0x400000`. It validates headers and
segments before allocating, rejects writable+executable segments, applies
segment permissions, rejects dynamic/interpreter/TLS and unknown program
segment types, and creates a four-page user stack with an unmapped guard page
below the stack. NX is enabled when the CPU supports it; hardware without NX
cannot enforce non-executable data pages. Boot-time ELF regression tests
exercise permissions, unsupported and malformed headers/segments, stack
guard placement, and address-space reclamation. `/init` also checks invalid,
overflowing, kernel, unmapped, and cross-page syscall buffers. This remains
an initial single-process bring-up, not a scheduler or POSIX environment.
The supervisor-access fault path has also been exercised using a test-only
`MYOS_TEST_SUPERVISOR_FAULT` build of `/init`; normal builds do not trigger it.
QEMU keyboard tests exercised the userspace built-ins, empty input, Backspace,
oversized input rejection, and return to the kernel shell. These checks are not
a complete security proof, and physical hardware and non-NX CPUs have not been
tested.

---

## Screenshots

> Screenshots coming soon.

---

## Project Structure

```text
MyOS/
├── src/
│   ├── arch/          # Architecture-specific code (x86_64)
│   ├── boot/          # Boot protocol headers
│   ├── drivers/       # Hardware drivers (keyboard, storage)
│   ├── fs/            # Filesystems (VFS, FAT32)
│   ├── graphics/      # Rendering and fonts
│   ├── kernel/        # Core kernel (PMM, VMM, heap, panic)
│   ├── userspace/     # Initial user program and ELF link script
│   ├── abi/           # Shared userspace/kernel syscall definitions
│   ├── lib/           # Utility functions (string, memory)
│   └── shell/         # Command-line interface
│
├── iso_root/          # Limine configuration (and other boot stuff)
├── third_party/       # External dependencies (Limine)
├── tools/             # Development tools (FAT32 image generator)
├── build/             # Generated build artifacts
│
├── GNUmakefile
├── linker.lds
└── AGENTS.md
```

---

## Building

### Requirements

* Linux / WSL (recommended) or Windows with MSYS2
* A C compiler and linker that can produce x86-64 ELF freestanding objects:
  * Linux/WSL: `gcc` + GNU `ld` (or `clang` + `ld.lld`)
  * Windows: the MSYS2/clang64 toolchain (`clang` + `ld.lld`)
* GNU Make
* Python 3 (used to generate the FAT32 boot image)
* QEMU
* xorriso
* Limine (included under `third_party/`)

### Build

The build system auto-detects the host:

* On **Windows** it uses the MSYS2/clang64 toolchain (`C:/msys64/clang64/bin`).
* On **Linux/WSL** it uses the system `cc`/`ld`.

You can override the toolchain explicitly, e.g.:

```bash
make CC=clang LD=ld.lld
```

If Python 3 is not on the MSYS2 shell `PATH`, set `PYTHON` to its executable:
`make PYTHON=/c/Python314/python.exe`.

```bash
make clean
make
```

### Run

```bash
make run
```

This builds the kernel, creates a bootable ISO, and launches MyOS in QEMU.

---

## Development Philosophy

MyOS follows a layered development approach.

Rather than implementing everything at once, each subsystem becomes stable before building the next.

```text
Boot
 ↓
Interrupts
 ↓
Kernel Panic
 ↓
Physical Memory
 ↓
Virtual Memory
 ↓
Kernel Heap
 ↓
Block Storage
 ↓
VFS
 ↓
FAT32
 ↓
Userspace
 ↓
GUI
```

This keeps the project easier to debug and helps ensure new features don't break existing functionality.

---

## Roadmap

### Completed

* [x] Limine boot
* [x] Kernel startup
* [x] IDT
* [x] ISR
* [x] Exception handling
* [x] Kernel Panic
* [x] Physical Memory Manager
* [x] Virtual Memory / Paging
* [x] Page fault diagnostics
* [x] Kernel heap (`kmalloc` / `kfree`)
* [x] Block device abstraction
* [x] RAM disk driver
* [x] VFS
* [x] FAT32 read/write
* [x] CLI
* [x] Keyboard input
* [x] Custom font rendering
* [x] GDT/TSS privilege-transition foundation
* [x] Isolated user address spaces and user-page permission checks
* [x] Static ELF64 loading and the initial ring 3 `/init`
* [x] Minimal `write`/`exit` syscall interface

### In Progress

* [ ] MyBread filesystem

### Planned

* [ ] Task scheduling and multiple runnable processes
* [ ] General process creation and lifecycle management
* [ ] Storage drivers (ATA/SATA/NVMe)
* [ ] Networking
* [ ] Audio
* [ ] Framebuffer improvements
* [ ] MyGUI desktop environment
* [ ] Userspace applications

### Userspace limitations

There is no scheduler, dynamic linker, libc, argv/env setup, userspace file
access, or userspace shell yet. `/init` is supplied as a Limine module, prints
a startup line, and exits. User exceptions terminate `/init`; kernel
exceptions retain the existing panic path.

---

## Kernel Panic

One of MyOS's first major debugging tools is its custom Kernel Panic system.

Instead of silently freezing, the kernel displays a dedicated panic screen with diagnostic information, making it easier to identify faults during development.

Example:

```text
KERNEL PANIC

Reason: Manual panic requested

System halted.
Manual restart required.
```

The `panic` command exists specifically for testing this subsystem.

---

## Why "MyOS"?

Because eventually everything started with **My**.

* MyOS
* MyGUI
* MyBread
* (and probably a few other questionable names)

The name stuck.

---

## Contributing

MyOS is currently a learning-focused project.

Contributions, ideas, bug reports, and discussions are welcome, but major architectural decisions remain coordinated with the project's direction.

If you're contributing code:

* Read `AGENTS.md`
* Preserve existing functionality
* Test changes in QEMU
* Avoid unnecessary refactoring

---

## License

MIT License — see `LICENSE` for details.

---

*"Every operating system starts with a bootloader and a dream."*