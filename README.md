# Clonk

> A hobby operating system built from scratch in C for x86-64, booted with Limine.

Clonk is an actively developed learning project for operating-system
development, from boot and hardware interfaces through memory management,
filesystems, and a small Ring 3 userspace. It is not a general-purpose or
production-ready operating system.

The project boots into an interactive Ring 3 `/init` shell and returns to the
kernel shell when `/init` exits. The runtime `version` command reports
**v0.1.1**.
## Current Features

- Higher-half x86-64 kernel and Limine boot configuration
- GDT/TSS setup, IDT, interrupt service routines, and CPU exception handling
- Physical memory manager, 4-level virtual memory manager, and kernel heap
- Kernel panic and page-fault diagnostics
- Separate lower-half address spaces for Ring 3 tasks, with supervisor-only
  shared kernel mappings
- Bounded static ELF64 executable loader and four-page userspace stack with a
  guard page
- `int 0x80` syscall ABI for console output, keyboard input, clearing the
  display, exit, and VFS filesystem access
- Interactive Ring 3 `/init` shell that remains active until it exits
- Eight-slot process table, per-process kernel stacks and filesystem handles,
  parent wait/exit handling, and PIT-driven single-CPU round-robin scheduling
- Filesystem-backed static ELF64 process creation with a bounded 1 MiB image
  limit and parent `WAIT`/exit-status handling
- Ring 3 process tests for task interleaving, memory isolation, process-table
  limits, user-fault isolation, and filesystem executable loading
- Block-device registry, Limine-module-backed RAM disk, VFS, and FAT32
  filesystem operations
- PCI enumeration and an AHCI SATA driver with bounded LBA48 DMA reads,
  writes, and cache flushes for supported 512-byte-sector devices
- PS/2 keyboard input and a custom 8x16 bitmap font for framebuffer text
- QEMU build and test workflow

Clonk provides an early **single-CPU process-management foundation**. Its
syscall ABI and process model are custom; it does not provide POSIX
compatibility, a complete userspace runtime, or a general-purpose process
environment.

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
| `rmdir <path>` | Remove an empty directory |

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
| `rmdir PATH` | Remove an empty directory |
| `exec PATH` | Load a static ELF64 executable from FAT32, run it, and wait |
| `run` | Run bundled process tests and wait for all children |

For `write`, the first whitespace-delimited argument is the path and the
remaining text, including spaces, is written to the file. Other path-taking
commands accept one path argument. The interactive input line is limited to
511 characters; an overlong line is discarded rather than executed.

The userspace syscall ABI is defined in [`src/abi/syscall.h`](src/abi/syscall.h).
The existing syscall IDs remain `WRITE=1`, `EXIT=2`, `READ=3`, and `CLEAR=4`.
Filesystem calls use IDs `FS_OPEN=5`, `FS_READ=6`, `FS_WRITE=7`,
`FS_READDIR=8`, `FS_CLOSE=9`, `FS_MKDIR=10`, `FS_UNLINK=11`, and
`FS_RMDIR=12`. Process calls use `SPAWN=13`, `WAIT=14`, `YIELD=15`, and
`GETPID=16`. The ID is passed in `RAX`, arguments in `RDI`, `RSI`, and `RDX`,
and a signed result is returned in `RAX`.

| Existing call | Arguments | Result |
| --- | --- | --- |
| `WRITE` | `RDI=buffer`, `RSI=count` (maximum 256) | `0`, or `-1` for invalid input |
| `EXIT` | Optional `RDI=status` | Child status is recorded; `/init` returns to the kernel shell |
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
| `FS_RMDIR` | `RDI=path` | `0` on success; non-empty directories are rejected |
| `SPAWN` | `RDI=filesystem path`, `RSI=0`, `RDX=0` | Child PID, or a negative process/path/ELF/filesystem error |
| `WAIT` | `RDI=child PID`, `RSI=optional writable status address`, `RDX=0` | Child PID after exit, or `-1` for invalid PID, parent, or buffer |
| `YIELD` | All arguments zero | `0`; another runnable task runs when available |
| `GETPID` | All arguments zero | Current process ID |

The process table has eight slots, including PID 1 for `/init`. Child
processes have independent user address spaces, four-page user stacks, 16 KiB
kernel stacks, and filesystem descriptor tables. `SPAWN` copies and validates
the user path, opens a regular file through VFS, reads it in bounded chunks,
and passes the temporary image to the existing ELF loader. Temporary image
memory and the VFS handle are released before the syscall returns; loaded
segments reside in the child's private address space. The maximum file-image
size is 1 MiB. `exec PATH` invokes `SPAWN`, waits for the child, and reports its
exit status. For example, `exec /HELLO.ELF` prints a greeting and exits with
status 0, while `exec /EXIT.ELF` exits with status 42.

The loader supports only static, x86-64 ELF64 `ET_EXEC` files in its existing
bounded subset. Dynamic linking, PIE, shared libraries, command-line
arguments, and environment variables are not supported. Paths follow the
filesystem's 8.3 FAT filename rules and reject `.` and `..` components. The
`run` command remains a separate scheduler regression harness; its scheduler
and malformed-image fixtures are now files in the generated FAT32 image
instead of spawnable Limine modules.

Process errors use the existing meanings for invalid ELF/arguments (`-1`),
missing file (`-3`), allocation failure (`-8`), and process-table exhaustion
(`-11`). Invalid paths return `-2`, a directory returns `-4`, VFS read failures
return `-12`, and images larger than 1 MiB return `-14`.

The PIT runs at 100 Hz through the existing 8259 PIC. Timer interrupts
reschedule only from Ring 3 frames using the saved-register/`iretq` format.
The scheduler switches CR3 and updates TSS `RSP0` before restoring the next
task. A timer tick in Ring 0 only requests rescheduling, which is deferred to
a syscall-return boundary; kernel execution is not preemptible. `WAIT` blocks
its caller until the selected child exits. Child exit records a status,
closes process-owned filesystem handles, and destroys the address space only
after switching away. A child's kernel stack remains allocated until its
parent reaps it. A Ring 3 exception terminates only the faulting process;
kernel exceptions still use the panic path.

The `run` command checks invalid process arguments and malformed ELF
rejection, starts independent busy-loop tasks and an invalid-opcode test
process, fills the bounded process table, and waits for all children. It is a
regression command for the bundled programs, not general executable-path
support.

`FS_OPEN` flags are `READ=0x1`, `WRITE=0x2`, `CREATE=0x4`, and
`TRUNCATE=0x8`. At least one access flag is required; create and truncate
require write access. `CREATE` opens an existing file or creates it if
missing; only `TRUNCATE` empties an existing file. File and directory handles
share the fixed descriptor table. Closing an invalid or already-closed
descriptor returns an error.

Filesystem operations use a per-process kernel-owned table of 16 opaque
descriptors; userspace never supplies a kernel VFS pointer. Reads and writes
are limited to 512 bytes per call. Paths must be NUL-terminated within 256
bytes, are
resolved from the root, and are restricted to the current FAT 8.3-compatible
name rules. `.` and `..` components are rejected. User memory is checked for
user access across every mapped page, write permission is required for
kernel-to-user output, and syscall data is copied through kernel buffers.
Filesystem errors are negative values defined by the shared ABI: invalid
arguments/buffers (`-1`), invalid paths (`-2`), not found (`-3`), not a
directory (`-4`), is a directory (`-5`), already exists (`-6`), read-only
(`-7`), out of memory (`-8`), no space (`-9`), invalid descriptor (`-10`),
resource/transfer limit (`-11`), I/O (`-12`), unsupported or failed operation
(`-13`), and non-empty directory (`-14`). Handles close on explicit close,
process exit, or user exception. These checks reduce accidental invalid
accesses; they are not a formal security proof.

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
    -> Limine RAM disk by default, or SATA only in explicit AHCI test mode
```

The userspace shell parses commands and supplies paths, flags, and data. The
kernel validates and copies syscall inputs, owns the open VFS objects, and
calls the VFS; userspace cannot access filesystem-private pointers or bypass
the mounted namespace.

- **Block devices:** a small registry provides checked sector read/write and
  optional flush interfaces for registered devices.
- **RAM disk:** Limine-loaded modules are exposed as memory-backed block
  devices. The standard boot image's FAT32 image is writable in memory, but
  changes are volatile and lost on reboot.
- **VFS:** mount-based path routing provides open, close, read, write,
  truncate, directory enumeration, metadata lookup, create, unlink, and
  directory creation operations to kernel clients.
- **FAT32:** the current filesystem backend supports file and directory
  operations, but names use the FAT 8.3 format. The on-disk file-size field is
  32-bit; long filenames and filesystem permissions are not implemented.
  The image generator places `HELLO.ELF`, `EXIT.ELF`, scheduler fixtures, and
  negative-test files into ordinary FAT directory entries with allocated
  cluster chains; executable ELF outputs are build dependencies of both
  standard and AHCI image generation.
  The VFS has metadata lookup, but no metadata syscall is provided to
  userspace. `FS_UNLINK` removes files; `FS_RMDIR` removes only empty
  directories.
- **PCI/AHCI:** the driver discovers PCI AHCI controllers, identifies ATA
  disks, and performs bounded LBA48 DMA reads and writes using page-aligned
  physical bounce buffers. It accepts 512-byte logical sectors and requires
  64-bit DMA support. Writes are exposed only when the disk advertises an ATA
  cache-flush command; each completed write is flushed. The driver uses one
  command slot and polling, and does not support NCQ, ATAPI, hot-plug, or
  arbitrary controller/device configurations.

The standard root remains the Limine `ramdisk0` image, so ordinary
`make run` behavior is unchanged and its filesystem changes are volatile.
`make run-ahci` builds a separate boot ISO and uses the dedicated
`build/ahci-test.img` 64 MiB FAT32 image on QEMU's ICH9 AHCI controller. Its
Limine marker makes the kernel select `sata0` as `/`; if that disk is missing,
unsupported, or fails validation, boot halts instead of falling back to the
RAM disk. The AHCI image is created only when absent and is preserved across
repeated `make run-ahci` boots. `make clean` removes it along with the build
directory, so do not clean between persistence-test boots.

The AHCI boot performs known-content reads at the beginning and final LBA,
multi-sector and invalid-range checks, then saves, writes, verifies, and
restores reserved sectors 8-9. The filesystem, VFS, kernel shell, and
userspace syscalls are shared with the RAM-disk path. FAT32 has no journal or
transactional multi-sector updates: cache flushes make completed device
writes persistent, but do not guarantee filesystem consistency after an
unexpected power loss. Names remain limited to 8.3, so the persistence test
uses short names rather than `persist-test`.

## Screenshots

Screenshots coming soon.

## Project Structure

```text
Clonk/
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
│   ├── kernel/             # Startup, memory, ELF, processes, syscalls, panic
│   ├── lib/                # Freestanding memory and string helpers
│   ├── shell/              # Kernel command shell
│   └── userspace/          # `/init`, executable and scheduler test programs
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
- Windows: MSYS2 Git Bash with the `clang64` Clang and `ld.lld` toolchain
- `xorriso` for ISO creation
- QEMU to run the resulting image
- Limine boot files and build tool are included under `third_party/limine/`

On Windows, use MSYS2 Git Bash with GNU Make, Python 3, `xorriso`, QEMU, and
the Clang/LLD toolchain from the MSYS2 `clang64` environment. The Makefile
expects MSYS2 at `C:/msys64` and selects
`C:/msys64/clang64/bin/clang` and `C:/msys64/clang64/bin/ld.lld`. The build
targets `x86_64-elf` with freestanding flags. From Git Bash, the normal
development workflow is:

```bash
make clean && make run
```

The Makefile uses `python3` by default to generate the FAT32 image. If Python
has a different command or path in your Git Bash environment, override it;
for example:

```bash
make clean && make run PYTHON=/c/path/to/python.exe
```

`make clean` removes the generated `build/` directory. The run target builds
the kernel and userspace programs, creates the FAT32 image and bootable ISO at
`build/clonk.iso`, then starts QEMU. The Makefile also selects system `cc` and
`ld` on non-Windows hosts; compatible x86-64 ELF tools, Python 3, `xorriso`,
and QEMU are required there.

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

This target runs the explicit persistent-storage test configuration. It
preserves `build/ahci-test.img` between runs; to start with a fresh disposable
volume, remove that specific image or run `make clean` before rebuilding.
No host or other disk is attached by the target; this generated image is the
only virtual SATA disk. The ISO is constructed with Limine BIOS and EFI boot
components, but firmware-specific and physical-hardware behavior should be
tested separately.

## Development Philosophy

Clonk is developed incrementally: build a subsystem, keep its interfaces
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
Bounded process management and round-robin scheduling
 ↓
ClonkFS and ClonkGUI
```

QEMU is the primary development and regression environment. Passing QEMU
tests is useful evidence, not a guarantee of compatibility with physical
machines.

## Recorded Validation

The recorded Windows build completed successfully using the MSYS2/Git Bash
toolchain. QEMU boot output confirmed the ELF, syscall, and filesystem syscall
self-tests. Interactive QEMU tests exercised `/init` directory and file
creation, listing, multiword writes, reads, truncation,
empty files/directories, removal, missing-path and directory-operation
errors, oversized paths, and exit back to the kernel shell. A 300-byte file
was read across chunks, and the kernel shell read content that `/init` had
written to the same VFS mount.

The process regression command was run three times in QEMU. Each cycle filled
the seven child slots, verified that the next spawn failed at the table limit,
and reaped all children before the next cycle. The serial trace showed
interleaved output from independent `SCHED-A` and `SCHED-B` PIDs while they
busy-looped without yielding, demonstrating PIT-driven Ring 3 preemption.
Each test also checked private data and stack counters. The invalid-opcode
child was reported as exception 6 with status `-134`; all other children
returned status 0, and the remaining tasks continued. Missing and malformed
filesystem executables, invalid pointers and paths, directories, empty and
over-limit files, invalid syscall arguments and wait buffers, and process-table
exhaustion were exercised by `run`.

After the handoff, kernel-shell `help`, `ls`, `cat`, `blkinfo`, and
`selftest` were exercised; PMM, VMM, and heap checks passed. `panic` and
`pfault` emitted their expected diagnostics. Invalid userspace pointers,
cross-page buffers, read-only destinations, transfer and path limits,
invalid descriptors, and descriptor exhaustion are also covered by boot-time
or QEMU checks. The standard RAM-disk boot reported no AHCI controller and
completed its userspace filesystem self-test. In the separate AHCI
configuration, QEMU identified the ICH9 controller and 64 MiB `sata0` disk,
verified known sector contents and read/write bounds, and passed single- and
multi-sector write/read-back tests. A Ring 3-created marker file was read
back through the kernel shell, survived a QEMU shutdown and a new boot using
the same image, and its file/directory deletion remained effective after
another boot. A no-controller run halted with an explicit error and did not
launch userspace or fall back to the RAM disk. These results validate only
the tested QEMU configuration; physical hardware, unsupported sector sizes,
injected ATA timeouts, and crash recovery were not tested.

For filesystem-backed execution, the rebuilt RAM-disk image was booted in
QEMU and the interactive shell ran `/HELLO.ELF` twice (status 0) and
`/EXIT.ELF` (status 42). Missing paths, the root directory, an empty file, a
non-ELF file, and a file over the 1 MiB cap were rejected with their expected
errors. The `run` syscall checks also passed for malformed/unterminated paths,
invalid user pointers, malformed ELF, and process-table exhaustion. Two
consecutive scheduler runs reaped and reused all child slots. Boot-time ELF
self-tests cover truncated headers, unsupported architecture, invalid
program-header offsets, segment bounds/overflow, and page/resource cleanup.
The AHCI image was generated with the same executable files, booted in QEMU,
and successfully ran both ELF programs and the scheduler regression suite.
Numerical heap/page-count comparisons and injected block-device read failures
were not performed.

## Roadmap

### Completed

- Limine-based x86-64 boot path and higher-half kernel
- GDT/TSS, IDT/ISR, CPU exceptions, and panic diagnostics
- Physical memory manager, virtual memory manager, and kernel heap
- PS/2 keyboard input and framebuffer text console
- Block-device abstraction and Limine-module-backed RAM disk
- VFS and FAT32 read/write and directory operations
- Kernel shell with memory, diagnostic, storage, and filesystem commands
- Ring 3 transition foundation and isolated lower-half address spaces for
  independent tasks
- Static ELF64 `/init` loader and interactive userspace shell
- Validated console, keyboard, exit, and filesystem syscall paths
- Userspace filesystem commands for listing, reading, creating, writing,
  removing files, and making/removing empty directories
- QEMU AHCI sector writes/flushes and persistent FAT32 testing with a
  dedicated virtual SATA image
- QEMU checks for kernel self-tests, userspace filesystem operations,
  invalid syscall inputs, descriptor limits, and return to the kernel shell
- Bounded process manager and process-owned filesystem handles
- PIT/PIC round-robin preemption, process syscalls, child exit/wait, and safe
  address-space/kernel-stack reclamation
- Filesystem-backed static ELF64 `ET_EXEC` process creation and `/init` `exec`
  command with exit-status reporting
- QEMU evidence for multiple interleaved Ring 3 tasks, isolated data and
  stacks, user-fault isolation, table exhaustion, and repeated spawn/reap
  cycles

### In Progress

- ClonkFS native filesystem design and implementation

### Planned

- Broader storage-driver and hardware support beyond the tested QEMU AHCI path
- Additional userspace programs and runtime support
- Networking and audio
- ClonkGUI graphical desktop environment

### Current limitations

The scheduler is single-CPU and bounded to eight tasks. Timer preemption
occurs only at Ring 3 interrupt frames; Ring 0 work is not preempted. The
existing keyboard `READ` syscall waits inside the kernel, so a task blocked
there can delay other runnable tasks until input arrives. Process creation
loads regular files through VFS, with a 1 MiB image limit and the existing
bounded static ELF64 `ET_EXEC` subset. There is no dynamic linker, PIE, shared
libraries, libc, argv/environment setup, or POSIX process API. FAT32 paths
retain 8.3 filename restrictions. User
exceptions terminate only the affected task; kernel exceptions retain the
kernel panic path. The standard writable root is a volatile FAT32 image.
The optional AHCI root is validated only with QEMU's ICH9 controller and a
512-byte-sector test disk; it uses polling and one command slot, and lacks
NCQ, ATAPI, and hot-plug support. FAT32 uses 8.3 filenames and does not
implement permissions, long filenames, journaling, or crash recovery.
Unexpected power loss may leave filesystem metadata inconsistent despite
device cache flushes. Physical hardware and firmware variants have not been
validated.

## Kernel Panic

The kernel panic path displays a red diagnostic screen and halts for manual
restart. Exception and page-fault diagnostics include the exception or fault
information, error code, instruction pointer, and register state; page faults
also report CR2 and decode relevant error-code bits. The kernel-shell `panic`
and `pfault` commands deliberately exercise these diagnostic paths.

## Why “Clonk”?

The project name inspired a few related names:

- **Clonk** — the operating system
- **ClonkFS** — the planned native filesystem
- **ClonkGUI** — the planned graphical desktop

The names stuck.

## Contributing

Clonk is a collaborative learning project. Read [`AGENTS.md`](AGENTS.md)
before contributing. Preserve the existing architecture, keep changes
focused, and test in QEMU when practical. Coordinate major architectural
decisions with the human maintainers.

## License

Clonk is licensed under the MIT License; see [`LICENSE`](LICENSE).

---

*“Every operating system starts with a bootloader and a dream.”*
" " 
