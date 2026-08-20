# MyOS

> A hobby operating system built from scratch in C for x86-64, booting with Limine.



\

MyOS is a personal operating system project focused on learning how operating systems work from the hardware up. Instead of building on Linux or another existing kernel, MyOS is developed as its own kernel with custom drivers, memory management, and graphics.

The long-term goal is to grow MyOS from a command-line kernel into a complete operating system with its own filesystem (**MyBread**), graphical desktop (**MyGUI**), and userspace.

---

## Current Features

* x86-64 kernel
* Limine bootloader support
* Interrupt Descriptor Table (IDT)
* Interrupt Service Routines (ISR)
* CPU exception handling
* Full-screen Kernel Panic diagnostics
* Physical Memory Manager (PMM)
* Keyboard driver
* Custom bitmap font renderer
* Command Line Interface (CLI)
* Built and tested in QEMU

### Built-in Commands

| Command   | Description                        |
| --------- | ---------------------------------- |
| `help`    | Show available commands            |
| `echo`    | Print text                         |
| `version` | Show MyOS version                  |
| `clear`   | Clear the screen                   |
| `panic`   | Trigger a test Kernel Panic        |
| `mem`     | Display physical memory statistics |

---

## Screenshots

> Screenshots coming soon.

---

## Project Structure

```text
MyOS/
├── src/
│   ├── arch/          # Architecture-specific code
│   ├── boot/          # Boot protocol headers
│   ├── drivers/       # Hardware drivers
│   ├── graphics/      # Rendering and fonts
│   ├── kernel/        # Core kernel
│   └── lib/           # Utility functions
│
├── iso_root/          # ISO contents
├── third_party/       # External dependencies (Limine)
├── build/             # Generated build artifacts
│
├── GNUmakefile
├── linker.lds
└── AGENTS.md
```

---

## Building

### Requirements

* Arch Linux or WSL (recommended)
* GCC
* GNU Make
* NASM / GNU assembler
* QEMU
* xorriso
* Limine (included under `third_party/`)

### Build

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
Storage
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
* [x] CLI
* [x] Keyboard input
* [x] Custom font rendering

### In Progress

* [ ] Virtual Memory / Paging
* [ ] Page fault diagnostics
* [ ] Kernel heap (`kmalloc` / `kfree`)

### Planned

* [ ] Task scheduling
* [ ] Process management
* [ ] VFS
* [ ] MyBread filesystem
* [ ] Storage drivers
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

A project license will be added as the project matures.

---

*"Every operating system starts with a bootloader and a dream."*
