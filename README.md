# MyOS

A hobby operating system built from scratch in C for x86-64, booting via the Limine bootloader.

MyOS is a personal project focused on understanding how operating systems work from the hardware up. Rather than building on top of Linux or an existing kernel, it is developed as its own kernel with custom drivers, memory management, a filesystem layer, and a command-line interface. The long-term goal is to grow it into a complete system with its own native filesystem (called MyBread) and eventually a graphical desktop.

Version: **MyOS v0.1.1 (x86-64)**

---

## What It Does Today

The kernel boots in QEMU and provides a working interactive shell. The following subsystems are implemented and tested:

**Core kernel**
- x86-64 higher-half kernel linked at `0xffffffff80000000`
- Limine boot protocol (BIOS and UEFI)
- Interrupt Descriptor Table and Interrupt Service Routines
- CPU exception handling with full register dump
- Kernel Panic screen (red background, reason, diagnostic state, manual restart)

**Memory**
- Physical Memory Manager: bitmap-based 4 KiB page allocator
- Virtual Memory Manager: 4-level page table manipulation, arbitrary physical-to-virtual mapping
- Kernel Heap: `kmalloc` / `kfree` / `kcalloc` / `krealloc` using a boundary-tag free list backed by the PMM and VMM

**Storage and filesystems**
- Generic block-device abstraction (`block_device` interface with read/write sector operations)
- RAM disk driver backed by Limine-booted modules
- PCI configuration-space access (legacy 0xCF8 / 0xCFC mechanism)
- AHCI SATA controller driver: PCI enumeration, HBA initialization, port detection, LBA48 DMA sector reads (read-only for now)
- Virtual Filesystem layer: mount table, path-prefix matching, filesystem-agnostic open/read/write/readdir/stat/create/unlink/mkdir
- FAT32 filesystem: BPB parsing, FAT traversal, cluster-to-sector mapping, file read and write, directory creation, 8.3 filename support

**Input and display**
- PS/2 keyboard driver with scancode translation
- Custom bitmap font renderer with full-screen text output

**Shell**
- Interactive command line with line editing (backspace support)
- Commands routed through kernel APIs rather than touching internals directly

---

## Shell Commands

| Command | Description |
|---------|-------------|
| `help` | Show available commands |
| `clear` | Clear the screen |
| `version` | Print the kernel version |
| `echo <text>` | Print text |
| `mem` | Physical memory statistics |
| `vmem` | Virtual memory statistics |
| `heap` | Kernel heap statistics |
| `selftest` | Re-run all kernel self-tests |
| `panic` | Trigger a deliberate kernel panic |
| `pfault` | Trigger a deliberate page fault |
| `badalloc` | Trigger a heap out-of-memory condition |
| `blkinfo` | List registered block devices |
| `ls [path]` | List directory contents |
| `cat <file>` | Print file contents |
| `write <file> <text>` | Write text to a file |
| `rm <file>` | Delete a file |
| `mkdir <path>` | Create a directory |

---

## Project Structure

```text
MyOS/
+-- src/
|   +-- arch/x86_64/      # IDT, ISR stubs, port I/O
|   +-- boot/              # Limine boot protocol headers
|   +-- drivers/
|   |   +-- keyboard/      # PS/2 keyboard
|   |   +-- pci/           # PCI config access
|   |   +-- storage/       # RAM disk, AHCI SATA
|   +-- fs/                # VFS and FAT32
|   +-- graphics/          # Font and framebuffer rendering
|   +-- kernel/            # Main entry, PMM, VMM, heap, panic, blockdev
|   +-- lib/               # memcpy, memset, memcmp, strcmp, strlen
|   +-- shell/             # CLI shell
+-- iso_root/              # Limine config for the bootable ISO
+-- tools/                 # FAT32 test image generator
+-- third_party/           # Limine bootloader binaries
+-- build/                 # Generated artifacts (kernel, ISO, objects)
+-- GNUmakefile
+-- linker.lds
+-- AGENTS.md
+-- README.md
```

---

## Building

### Requirements

- Linux or WSL
- Clang (with `x86_64-elf` target) or GCC cross-compiler
- GNU Make
- NASM or GNU assembler (for the ISR stubs)
- QEMU (for testing)
- xorriso (for ISO generation)
- Python 3 (for the FAT32 test image tool)
- Limine (included under `third_party/`)

### Build and run

```bash
make clean
make
make run
```

This compiles the kernel, links it with `linker.lds`, stages a bootable ISO with Limine, and launches MyOS in QEMU. By default the kernel runs a short self-test battery on boot and prints results to both the framebuffer and the serial port.

### Run with a SATA disk attached

A separate make target attaches a FAT32 test image as a SATA disk on QEMU's ICH9 AHCI controller, exercising the real storage driver path instead of the RAM disk:

```bash
make run-ahci
```

---

## Development Philosophy

MyOS is built one stable layer at a time. Each subsystem is tested in QEMU before the next one is added, which keeps the project debuggable and prevents new features from silently breaking existing ones.

The general progression so far has been:

```text
Boot (Limine)
  -> Interrupts and exception handling
  -> Kernel Panic diagnostics
  -> Physical memory
  -> Virtual memory
  -> Kernel heap
  -> Block devices and storage
  -> VFS and filesystems
  -> Shell and user interaction
```

---

## Roadmap

### Done

- Limine boot (BIOS + UEFI)
- Kernel startup and higher-half linking
- IDT, ISR, exception handling
- Kernel Panic with register and page-fault diagnostics
- Physical Memory Manager
- Virtual Memory Manager
- Kernel Heap
- Keyboard driver
- Custom font rendering
- Block device abstraction
- RAM disk driver
- PCI configuration access
- AHCI SATA driver (read-only DMA)
- VFS layer
- FAT32 read and write
- Interactive CLI shell

### In Progress

- Page-fault diagnostics refinement
- FAT32 robustness and edge-case handling

### Planned

- MyBread native filesystem
- Task scheduling and process management
- System calls and userspace
- Networking
- Audio
- Framebuffer improvements and a graphical desktop
- Userspace applications

---

## Kernel Panic

When something goes wrong that the kernel cannot recover from, it displays a dedicated panic screen instead of freezing silently. The screen has a red background, shows a human-readable reason, and prints technical diagnostic information including the exception number, error code, faulting address (CR2 for page faults), and a general-purpose register dump. The system halts and requires a manual restart.

The `panic` command exists to test this path on purpose.

---

## Why "MyOS"?

Because eventually everything in the project started with "My".

- MyOS
- MyBread (the planned native filesystem)
- MyGUI (the planned desktop)

The name stuck.

---

## Contributing

MyOS is primarily a learning project. Contributions, bug reports, and ideas are welcome, but major architectural decisions are coordinated with the project's overall direction.

If you are contributing code:

- Read `AGENTS.md` first
- Preserve existing functionality
- Test changes in QEMU
- Avoid unrelated refactoring

---

## License

A project license will be added as the project matures.

---

*Every operating system starts with a bootloader and a dream.*
