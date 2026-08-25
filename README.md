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

### In Progress

* [ ] MyBread filesystem

### Planned

* [ ] Task scheduling
* [ ] Process management
* [ ] Storage drivers (ATA/SATA/NVMe)
* [ ] Networking
* [ ] Audio
* [ ] Framebuffer improvements
* [ ] MyGUI desktop environment
* [ ] Userspace applications

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