# MyOS

> A hobby operating system built from scratch in C for x86-64, booted with Limine.

MyOS is a learning project for exploring operating-system development from
boot and hardware interfaces through memory management, filesystems, and a
small Ring 3 userspace. It is an actively developed kernel, not a general
purpose or production-ready operating system.

The project currently boots into a persistent userspace shell and can return
to its kernel shell. Its version command reports **v0.1.1**.

## Current Features

- Higher-half x86-64 kernel and Limine boot configuration
- GDT/TSS setup, IDT, interrupt service routines, and CPU exception handling
- Physical memory manager, 4-level virtual memory manager, and kernel heap
- Kernel panic and page-fault diagnostics
- Separate lower-half address space for the initial Ring 3 program, with
  supervisor-only kernel mappings
- Bounded static ELF64 executable loader and four-page userspace stack with a
  guard page
- `int 0x80` syscall ABI for console output, keyboard input, clearing the
  display, exit, and VFS filesystem access
- Persistent `/init` interactive userspace shell
- Block-device registry, Limine-module-backed RAM disk, VFS, and FAT32
  filesystem operations
- PCI configuration-space enumeration and a read-only AHCI SATA path
- PS/2 keyboard input and a custom 8x16 bitmap font for framebuffer text
- QEMU build and test workflow

This is a **single-process userspace bring-up**. It does not have a scheduler,
dynamic linker, libc, or general process manager.

## Shells and Commands

The kernel shell and `/init` are separate environments with different
command sets. Filesystem commands in `/init` use filesystem syscalls into the
kernel VFS; the kernel remains responsible for filesystem access.

### Kernel shell

| Command | Description |
| --- | --- |
| `help` | Show kernel-shell commands |
| `clear` | Clear the framebuffer console |
| `version` | Print the kernel version |
| `echo <text>` | Print text |
| `mem` | Show physical-memory statistics |
| `vmem` | Show virtual-memory statistics |
| `heap` | Show kernel-heap statistics |
| `selftest` | Re-run PMM, VMM, and heap self-tests |
| `panic` | Trigger the kernel panic path |
| `pfault` | Trigger a controlled page fault |
| `badalloc` | Exercise a failed oversized heap allocation |
| `blkinfo` | List registered block devices |
| `ls [path]` | List a directory; defaults to `/` |
| `cat <path>` | Print a file |
| `touch <path>` | Create an empty file |
| `write <path> <text>` | Create a file if needed and write from its beginning |
| `rm <path>` | Remove a file |
| `mkdir <path>` | Create a directory |

Kernel-shell input supports basic character entry and Backspace editing. Its
command tokenizer splits arguments on whitespace; `write` rejoins text
arguments with single spaces and overwrites from offset zero without
truncating a longer existing file. The keyboard path is a basic PS/2 scan-code
translator, not a full keyboard-layout or Unicode input stack. The framebuffer
text console wraps lines and scrolls when output reaches the bottom of the
display.

### Ring 3 `/init` shell

| Command | Description |
| --- | --- |
| `help` | Show userspace-shell commands |
| `echo TEXT` | Print the remaining text |
| `version` | Print the kernel version |
| `clear` | Clear the framebuffer console |
| `exit` | Close userspace handles and return to the kernel shell |
| `ls [PATH]` | List a directory; defaults to `/` |
| `cat PATH` | Print file contents in bounded chunks |
| `touch PATH` | Create a file; do not truncate an existing file |
| `write PATH TEXT` | Create or truncate a file, then write `TEXT` |
| `rm PATH` | Remove a file; directories are rejected |
| `mkdir PATH` | Create a directory |

For `write`, the first whitespace-delimited argument is the path and the
remaining text, including spaces, is written to the file. Other path-taking
commands accept one path argument. The interactive input line is limited to
511 characters; an overlong line is discarded rather than executed.

The userspace syscall ABI is defined in [`src/abi/syscall.h`](src/abi/syscall.h).
The existing syscall IDs remain `WRITE=1`, `EXIT=2`, `READ=3`, and `CLEAR=4`.
Filesystem calls use IDs `FS_OPEN=5`, `FS_READ=6`, `FS_WRITE=7`,
`FS_READDIR=8`, `FS_CLOSE=9`, `FS_MKDIR=10`, and `FS_UNLINK=11`. The ID is
passed in `RAX`, arguments in `RDI`, `RSI`, and `RDX`, and a signed result is
returned in `RAX`.

| Existing call | Arguments | Result |
| --- | --- | --- |
| `WRITE` | `RDI=buffer`, `RSI=count` (maximum 256) | `0`, or `-1` for invalid input |
| `EXIT` | Optional `RDI=status` (ignored) | Does not return; resumes kernel shell |
| `READ` | `RDI=buffer`, `RSI=1` | `1` character, or `-1` |
| `CLEAR` | No arguments | `0`, or `-1` for invalid arguments |

| Call | Arguments | Result |
| --- | --- | --- |
| `FS_OPEN` | `RDI=path`, `RSI=flags` | Descriptor `1`–`16`, or a negative error |
| `FS_READ` | `RDI=fd`, `RSI=buffer`, `RDX=count` | Bytes read; `0` at EOF |
| `FS_WRITE` | `RDI=fd`, `RSI=buffer`, `RDX=count` | Bytes written |
| `FS_READDIR` | `RDI=fd`, `RSI=dirent*` | `1` for entry; `0` at end |
| `FS_CLOSE` | `RDI=fd` | `0` on success |
| `FS_MKDIR` | `RDI=path` | `0` on success |
| `FS_UNLINK` | `RDI=path` | `0` on success |

`FS_OPEN` flags are `READ=0x1`, `WRITE=0x2`, `CREATE=0x4`, and
`TRUNCATE=0x8`. At least one access flag is required; create and truncate
require write access. `CREATE` opens an existing file or creates it if
missing; only `TRUNCATE` empties an existing file. File and directory handles
share the fixed descriptor table. Closing an invalid or already-closed
descriptor returns an error.

Filesystem operations use a kernel-owned table of 16 opaque descriptors;
userspace never supplies a kernel VFS pointer. Reads and writes are limited
to 512 bytes per call. Paths must be NUL-terminated within 256 bytes, are
resolved from the root, and are restricted to the current FAT 8.3-compatible
name rules. `.` and `..` components are rejected. User memory is checked for
user access across every mapped page, write permission is required for
kernel-to-user output, and syscall data is copied through kernel buffers.
Filesystem errors are negative values defined by the shared ABI: invalid
arguments/buffers (`-1`), invalid paths (`-2`), not found (`-3`), not a
directory (`-4`), is a directory (`-5`), already exists (`-6`), read-only
(`-7`), out of memory (`-8`), no space (`-9`), invalid descriptor (`-10`),
resource/transfer limit (`-11`), I/O (`-12`), and unsupported or failed
operation (`-13`). Handles close on explicit close, userspace exit, or user
exception. These checks reduce accidental invalid accesses; they are not a
formal security proof.

The kernel routes keyboard characters to `/init` through a bounded
kernel-owned queue while userspace is active. The current input translator
handles basic US-style scan-code characters, Enter, Backspace, and Shift for
alphabetic case; it is not a complete keyboard layout implementation. The
userspace shell supports basic line editing and empty lines.

## Storage and Filesystems

The filesystem access path is:

```text
Ring 3 shell
    -> int 0x80 filesystem syscall
    -> user-memory and path validation
    -> kernel user-filesystem descriptor layer
    -> VFS
    -> mounted FAT32 filesystem
    -> block-device layer
    -> Limine RAM-disk image
```

The userspace shell parses commands and supplies paths, flags, and data. The
kernel validates and copies syscall inputs, owns the open VFS objects, and
calls the VFS; userspace cannot access filesystem-private pointers or bypass
the mounted namespace.

- **Block devices:** a small registry provides checked sector read/write
  interfaces for registered devices.
- **RAM disk:** Limine-loaded modules are exposed as memory-backed block
  devices. The standard boot image's FAT32 image is writable in memory, but
  changes are volatile and lost on reboot.
- **VFS:** mount-based path routing provides open, close, read, write,
  truncate, directory enumeration, metadata lookup, create, unlink, and
  directory creation operations to kernel clients.
- **FAT32:** the current filesystem backend supports file and directory
  operations, but names use the FAT 8.3 format. The on-disk file-size field is
  32-bit; long filenames and filesystem permissions are not implemented.
  The VFS has metadata lookup, but no metadata syscall is provided to
  userspace; `FS_UNLINK` removes files and rejects directories.
- **PCI/AHCI:** PCI configuration-space enumeration and an AHCI SATA driver
  with DMA sector reads are present. The AHCI block device is read-only. The
  optional `make run-ahci` target attaches a FAT32 test image through QEMU's
  ICH9 AHCI controller; this is a test path, not writable persistent storage.

The standard root mount prefers the Limine `ramdisk0` image. If a SATA device
is available, boot also attempts to mount it separately at `/sata` for
verification. Userspace and kernel-shell filesystem commands use the mounted
VFS rather than calling the FAT32 implementation directly.

## Screenshots

No screenshot assets are currently included in the repository.

## Project Structure

```text
MyOS/
├── src/
│   ├── abi/                # Shared syscall ABI
│   ├── arch/x86_64/        # GDT, IDT, interrupt and privilege-transition code
│   ├── boot/               # Limine protocol definitions
│   ├── drivers/
│   │   ├── keyboard/       # PS/2 keyboard input
│   │   ├── pci/            # PCI configuration-space access
│   │   └── storage/        # RAM disk and AHCI
│   ├── fs/                 # VFS and FAT32
│   ├── graphics/           # Bitmap font
│   ├── kernel/             # Startup, memory, ELF, syscalls, panic, block devices
│   ├── lib/                # Freestanding memory and string helpers
│   ├── shell/              # Kernel command shell
│   └── userspace/          # `/init` shell and its linker script
├── iso_root/               # Limine boot configuration
├── third_party/limine/     # Included Limine files/tools
├── tools/                  # FAT32 image generation
├── GNUmakefile
├── implementation_plan.md
├── linker.lds
└── LICENSE
```

The `build/` directory contains generated objects, images, and the ISO; it is
created by the build and should not be treated as source.

## Building

### Requirements

- GNU Make
- Python 3 (generates the FAT32 image)
- The project C/assembly toolchain and linker:
  - Windows: MSYS2 Git Bash with the `clang64` Clang and `ld.lld` toolchain
  - Linux/WSL: `cc` and `ld` are selected by the Makefile
- `xorriso` for ISO creation
- QEMU to run the resulting image
- Limine boot files and build tool are included under `third_party/limine/`

The build targets `x86_64-elf` with freestanding flags. On Windows, the
Makefile selects `C:/msys64/clang64/bin/clang` and
`C:/msys64/clang64/bin/ld.lld`. From Git Bash, use:

```bash
make clean
make -j4 PYTHON=/c/Python314/python.exe
```

Replace the `PYTHON` value with the Python executable available in your
MSYS2 environment if needed. The clean command removes generated build
artifacts; the build compiles the kernel and `/init`, generates the FAT32
image, and creates a bootable ISO at `build/myos.iso`.

The Makefile also selects system `cc` and `ld` on non-Windows hosts. Linux
builds require compatible x86-64 ELF tools in addition to Make, Python, and
`xorriso`.

## Running in QEMU

```bash
make run
```

This builds the ISO if necessary and starts QEMU with the standard boot image.
The Makefile selects a graphical display when a desktop display environment
is available, otherwise QEMU uses no graphical window; serial output defaults
to the terminal.

To attach the FAT32 test image as a SATA disk through QEMU's ICH9 AHCI
controller:

```bash
make run-ahci
```

This target exercises the optional read-only AHCI storage path. Availability
and successful operation depend on the QEMU environment; no physical-hardware
compatibility is implied. The ISO is constructed with Limine BIOS and EFI
boot components, but firmware-specific and physical-hardware behavior should
be tested separately.

## Development Philosophy

MyOS is developed incrementally: build a subsystem, keep its interfaces
small, then validate it in a controlled environment before relying on it for
the next layer.

```text
Boot
 ↓
Interrupts and diagnostics
 ↓
Physical and virtual memory
 ↓
Kernel heap
 ↓
Block devices
 ↓
VFS and FAT32
 ↓
Ring 3 `/init` and syscalls
 ↓
Scheduling and additional userspace
 ↓
MyBread and MyGUI
```

QEMU is the primary development and regression environment. Passing QEMU
tests is useful evidence, not a guarantee of compatibility with physical
machines.

## Validation

The documented Windows build command completed successfully in the
MSYS2/Git Bash environment. QEMU boot output confirmed the ELF, syscall, and
filesystem syscall self-tests. Interactive QEMU tests exercised `/init`
directory and file creation, listing, multiword writes, reads, truncation,
empty files/directories, removal, missing-path and directory-operation
errors, oversized paths, and exit back to the kernel shell. A 300-byte file
was read across chunks, and the kernel shell read content that `/init` had
written to the same VFS mount.

After the handoff, kernel-shell `help`, `ls`, `cat`, `blkinfo`, and
`selftest` were exercised; PMM, VMM, and heap checks passed. `panic` and
`pfault` emitted their expected diagnostics. Invalid userspace pointers,
cross-page buffers, read-only destinations, transfer and path limits,
invalid descriptors, and descriptor exhaustion are also covered by boot-time
or QEMU checks. The standard QEMU run reported no AHCI controller; AHCI and
physical hardware were not validated in this test run.

## Roadmap

### Completed

- Limine-based x86-64 boot path and higher-half kernel
- GDT/TSS, IDT/ISR, CPU exceptions, and panic diagnostics
- Physical memory manager, virtual memory manager, and kernel heap
- PS/2 keyboard input and framebuffer text console
- Block-device abstraction and Limine-module-backed RAM disk
- VFS and FAT32 read/write and directory operations
- Kernel shell with memory, diagnostic, storage, and filesystem commands
- Ring 3 transition foundation and isolated lower-half address space for the
  initial program
- Static ELF64 `/init` loader and persistent userspace shell
- Validated console, keyboard, exit, and filesystem syscall paths
- Userspace filesystem commands for listing, reading, creating, writing,
  removing files, and making directories
- QEMU checks for kernel self-tests, userspace filesystem operations,
  invalid syscall inputs, descriptor limits, and return to the kernel shell

### In Progress

- MyBread native filesystem design and implementation
- Generalizing storage and userspace facilities beyond the initial shell

### Planned

- Scheduler and multiple runnable processes
- General process creation and lifecycle management
- Writable persistent storage drivers and broader hardware support
- Additional userspace programs and runtime support
- Networking and audio
- MyGUI graphical desktop environment

### Current limitations

`/init` is the single Ring 3 process; there is no scheduler, dynamic linker,
libc, or argv/environment setup. The ELF loader supports a bounded static
ELF64 `ET_EXEC` subset, not dynamically linked executables. User exceptions
terminate `/init`; kernel exceptions retain the kernel panic path. The
standard writable root is a volatile FAT32 image loaded as a Limine module.
AHCI currently provides a read-only block path. FAT32 uses 8.3 filenames and
does not implement permissions or long filenames. Physical hardware and
firmware variants have not been fully validated.

## Kernel Panic

The kernel panic path displays a red diagnostic screen and halts for manual
restart. Exception and page-fault diagnostics include the exception or fault
information, error code, instruction pointer, and register state; page faults
also report CR2 and decode relevant error-code bits. The kernel-shell `panic`
and `pfault` commands deliberately exercise these diagnostic paths.

## Why “MyOS”?

The project name inspired a few related names:

- **MyOS** — the operating system
- **MyBread** — the planned native filesystem
- **MyGUI** — the planned graphical desktop

The names stuck.

## Contributing

MyOS is a collaborative learning project. Read [`AGENTS.md`](AGENTS.md)
before contributing. Preserve the existing architecture, keep changes
focused, and test in QEMU when practical. Coordinate major architectural
decisions with the human maintainers.

## License

MyOS is licensed under the MIT License; see [`LICENSE`](LICENSE).

---

*“Every operating system starts with a bootloader and a dream.”*
