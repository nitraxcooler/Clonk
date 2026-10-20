# Implementation Plan — MyOS Subsystems & Roadmap

## Goal

Track incremental development of the existing MyOS x86-64 kernel while preserving and reusing its PMM, VMM, IDT, panic, CLI, and Limine boot architecture. MyOS now has an initial **ring 3 userspace bring-up** and a QEMU-validated **AHCI persistent FAT32 test path** alongside its heap, diagnostics, block-device, VFS, and FAT32 infrastructure. MyBread and a general process/scheduler model remain future work.

## Scope & Context

This is a freestanding C kernel (no libc, no POSIX, no C++ RTTI/exceptions). Its current relevant infrastructure includes:

- **PMM** (`src/kernel/pmm.[ch]`) — bitmap-based physical page allocator, 4 KiB pages, returns physical addresses (`0` on OOM).
- **VMM** (`src/kernel/vmm.[ch]`) — 4-level page tables (PML4→PDPT→PD→PT), page mapping and translation, user-page permissions/range validation, and private lower-half address spaces sharing supervisor-only kernel mappings.
- **Panic** (`src/kernel/panic.[ch]`) — full-screen red Kernel Panic; `kernel_panic_exception()` and `kernel_panic_page_fault()` already exist. Page-fault path reads CR2 and shows fault address, raw error code, RIP, optional RSP — but does **not** decode the error-code bits nor dump general-purpose registers.
- **IDT/ISR** (`src/arch/x86_64/idt.[ch]`, `isr.s`) — exception stubs push RAX–R15 then call `exception_handler_err(stack, vector)`; the CPU error frame sits at `stack + 15`. General registers are on the stack but currently discarded.
- **CLI** — keyboard input is handled in `src/drivers/keyboard/keyboard.c`;
  command dispatch and VFS-backed file commands live in `src/shell/shell.c`.
- **Build** — GNU Make + Clang + custom `linker.lds`; QEMU test via `make clean` / `make` / `make run` (headless serial output currently).
- **Userspace** (`src/kernel/elf.c`, `src/kernel/usermode.c`, `src/arch/x86_64/gdt.c`, `src/arch/x86_64/usermode.s`) — GDT/TSS privilege transition, bounded ELF64 `ET_EXEC` loading, a four-page user stack, and the initial `/init` using `int 0x80`.

## Userspace bring-up — implemented foundation and limits

The kernel installs its own GDT and 64-bit TSS. Ring 3 uses selectors `0x1b`
(data) and `0x23` (code), and privilege transitions land on a dedicated ring
0 stack. IDT vector `0x80` is a DPL-3 interrupt gate. The syscall convention
is defined in `src/abi/syscall.h`: syscall ID in `RAX`, arguments in `RDI`,
`RSI`, and `RDX`, and signed return value in `RAX`. The established syscall
numbers are unchanged: console `WRITE=1`, `EXIT=2`, keyboard `READ=3`, and
`CLEAR=4`. `WRITE` validates up to 256 bytes and writes to the framebuffer and
COM1; `READ` validates a writable one-byte user buffer and blocks until a
queued keyboard character is available; `CLEAR` takes no arguments; `EXIT`
returns from `/init`. The wait enables interrupts and uses `hlt`, so it sleeps
without a scheduler and only supports the single active process.

The added filesystem ABI uses IDs `FS_OPEN=5`, `FS_READ=6`, `FS_WRITE=7`,
`FS_READDIR=8`, `FS_CLOSE=9`, `FS_MKDIR=10`, `FS_UNLINK=11`, and
`FS_RMDIR=12`. It exposes
opaque descriptors and bounded operations only; kernel-owned VFS objects and
pointers never cross the boundary. `FS_READ` and `FS_WRITE` transfer at most
512 bytes per call; reads return zero at EOF, and directory iteration returns
one entry or zero at end. Open flags distinguish read, write, create, and
explicit truncation. Negative error codes are defined in the shared ABI header
for invalid buffers/arguments, paths, missing paths, type mismatches, existing
paths, read-only storage, memory/space exhaustion, invalid handles, limits,
I/O, unsupported operations, and non-empty directories.

`src/kernel/userfs.c` owns a fixed 16-slot descriptor table. Descriptors are
positive integers from 1 through 16, allocated from the first free slot;
invalid and repeated closes fail, and all open objects are closed on process
exit or user exception. `src/kernel/usercopy.c` reuses the VMM's per-page
validation: source buffers must be mapped user pages, destination buffers
must be writable user pages, including every page spanned by a range. Data is
copied through bounded kernel buffers, and paths are copied into a 256-byte
kernel buffer before VFS calls. Paths must be NUL-terminated within that
bound, are normalized relative to `/`, and reject `.`/`..`, empty names,
non-8.3 FAT names, and unsupported characters. Repeated separators are
collapsed. The single-process model prevents concurrent address-space changes
during validation and copying.

`/init` preserves `help`, `echo`, `clear`, `version`, and `exit`, and adds
`ls [PATH]`, `cat PATH`, `touch PATH`, `write PATH TEXT`, `rm PATH`, and
`mkdir PATH`. `ls` defaults to `/`; `touch` creates a missing file without
truncating an existing one; `write` creates if needed, explicitly truncates,
then writes the remaining text while preserving spaces within it; `rm`
removes files but rejects directories; `mkdir` requires an existing parent;
`rmdir` removes only empty directories.
The line buffer allows at most 511 characters. All commands use the syscall
interface and mounted VFS rather than filesystem backend internals.

The default FAT32 root comes from the `ramdisk0` Limine module and remains
writable-in-memory but volatile. The separate `make run-ahci` configuration
bundles a Limine marker and attaches only the generated 64 MiB
`build/ahci-test.img` to QEMU's ICH9 AHCI controller. In this explicit mode,
the kernel selects `sata0` as `/`, validates known sectors, and halts on a
missing/unsupported device or failed test rather than falling back to RAM.
The AHCI driver uses LBA48 DMA, physically allocated page-aligned buffers,
one polled command slot, and ATA cache-flush commands after writes. It accepts
512-byte logical sectors and requires a controller with 64-bit DMA; devices
without a supported cache flush are read-only. The tested setup identified a
64 MiB disk with 131072 sectors of 512 bytes. Its image persists across
repeated `make run-ahci` boots and is recreated after `make clean`.

The FAT32 backend accepts only 8.3 names, has a 32-bit file-size limit, and
provides neither long filenames, permissions, journaling, nor crash recovery.
AHCI writes are flushed individually, but multi-sector FAT updates are not
transactional. The syscall ABI does not yet expose metadata, seek, or rename.

The ELF loader checks ELF64/little-endian/x86-64 `ET_EXEC` headers, program
header bounds, file/memory sizes, alignment, user virtual-address bounds, and
that the entry lies in an executable `PT_LOAD` segment before allocating
process pages. The initial format limit is 128 program headers and 64 MiB of
non-overlapping load pages; writable+executable segments are rejected. It
maps supported segments with user permissions, honors writable/executable
flags, rejects dynamic/interpreter/TLS and unknown program segment types, and
creates a fixed four-page user stack with an unmapped guard page below it.
Boot-time ELF self-tests cover malformed inputs, permissions, guard placement,
and address-space reclamation; `/init` checks invalid syscall pointers in ring
3. A test-only `MYOS_TEST_SUPERVISOR_FAULT` build verifies that a user access
to a supervisor mapping terminates the process and returns to the kernel.
Each process gets a separate PML4 with the kernel's supervisor-only upper-half
mappings copied; private lower-half pages and page tables are reclaimed on
exit. User exceptions terminate `/init` instead of entering the kernel panic
handler.

`src/userspace/init.c` is linked as a static ELF and bundled as a Limine
module. The kernel initializes the heap, block devices, VFS, and FAT32 root
mount before starting `/init`. Boot-time ELF and syscall self-tests cover
malformed ELF input, permissions, guard placement, address-space cleanup,
invalid and cross-page user pointers, read-only destinations, invalid
descriptors, transfer/path limits, and descriptor exhaustion. QEMU command
tests verified filesystem create/list/write/read/remove, empty files and
directories, explicit truncation, error cases, oversized-path rejection, and
the exit handoff marker. A 300-byte file was read across bounded chunks, and
the kernel shell read back a file written by `/init` after the userspace exit.
The kernel-shell `selftest` command passed PMM, VMM, and heap checks; the
`panic` and `pfault` commands also emitted their expected QEMU serial
diagnostics. These checks do not constitute exhaustive security testing. A
test-only `MYOS_TEST_SUPERVISOR_FAULT` build previously exercised termination
on a user access to supervisor memory; normal builds do not trigger it.

The AHCI QEMU path additionally verified reads of the FAT32 boot/FSInfo
sectors and a known final-sector marker, invalid-range rejection, one- and
two-sector write/read-back, cache flush, and restoration of reserved test
sectors. A Ring 3 file was read through the kernel shell after creation,
survived a QEMU stop and a new boot with the same image, and its file and
empty-directory removal remained effective after another boot. Booting the
AHCI-marked ISO without a controller halted explicitly without RAM-disk
fallback. These tests validate only the QEMU ICH9 setup; physical hardware,
unsupported-sector devices, and forced timeout/error injection were not
tested.

There is no scheduler, general process creation, dynamic linker, argv/env
setup, libc, seek, or general filesystem descriptor API. NX protection is
enabled when supported by the CPU; processors without NX cannot enforce
non-executable data pages. Physical hardware has not been tested. The older
planning sections below describe other subsystems and future work; this
current-state summary takes precedence where they disagree.

## High-level approach

1. Reuse PMM for physical pages and VMM for mapping — never build a second allocator or page tables from scratch.
2. Reserve a contiguous virtual heap region in an unmapped gap; extend it one page at a time via `pmm_alloc_page()` + `vm_map_page()` (scattered physical pages are fine — VMM maps arbitrary physical → virtual).
3. Extend existing `panic.c`/`idt.c` rather than replacing them; decode x86-64 page-fault error bits and render the already-saved general-purpose registers.
4. Add a thin block-device abstraction and a RAM-backed device for testing.
5. Build a small VFS (filesystem ops vtable + mount table) that does **not** assume FAT32.
6. Implement FAT32 read-only first, then write support, as a clean MyOS-native implementation.
7. Design and implement a minimal MyBread with its own spec (superblock, inode area, free-space bitmap, directory blocks).
8. Add deliberate failure tests and CLI commands; run self-tests on boot via serial so headless QEMU is verifiable.
## Types

The following types are new. C structure/function-pointer conventions are used (this is a C codebase — no C++ classes are introduced).

### 1. Kernel heap (new `src/kernel/heap.h`)

```c
#define HEAP_ALIGN            16UL
#define HEAP_BLOCK_ALIGN      16UL
#define HEAP_MAGIC            0x48454150514D4F4DULL   /* "HEAPQMOM" */
#define HEAP_BLOCK_HEADER_SIZE 16UL   /* size|used + reserved magic area */
#define HEAP_BLOCK_FOOTER_SIZE  8UL   /* boundary tag (size|used) at end */
#define HEAP_MIN_BLOCK         32UL   /* header + mini payload + footer  */

typedef struct heap_block {
    uint64_t size;    /* bit0 = used flag; field holds total block size   */
    uint64_t tag;     /* HEAP_MAGIC when allocated                         */
    /* When free, the next 16 bytes hold prev/next free-list links        */
} heap_block;

typedef struct {
    uint64_t arena_bytes;   /* total reserved virtual arena               */
    uint64_t mapped_bytes;  /* bytes backed by physical pages             */
    uint64_t used_bytes;    /* bytes currently held by allocations        */
    uint64_t free_bytes;    /* bytes available in the free list           */
    uint64_t alloc_count;   /* outstanding allocations                    */
    uint64_t free_count;    /* total kfree() calls served                 */
    uint64_t peak_bytes;    /* high-water mark of used_bytes              */
} heap_stats;

void  heap_init(void);
bool  heap_is_initialized(void);
void *kmalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t new_size);
void  kfree(void *ptr);
void  heap_get_stats(struct heap_stats *out);
void  heap_self_test(void);
```

### 2. CPU register frame / page-fault decode (`src/kernel/panic.h`)

```c
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags, ss;
} cpu_reg_frame;

typedef struct {
    bool present;     /* bit0 of error code: page present?            */
    bool write;       /* bit1: attempted write                        */
    bool user;        /* bit2: user-mode access                       */
    bool reserved;    /* bit3: reserved-bit violation                 */
    bool instruction; /* bit4: instruction fetch (if set by hardware) */
} pf_decode;

void decode_page_error(uint64_t error, pf_decode *out);
```

The page-fault API is extended (the single caller in `idt.c` is updated):

```c
void kernel_panic_page_fault(const char *reason, uint8_t vector, const char *name,
                             uint64_t error_code, uint64_t fault_address,
                             uint64_t rip, uint64_t cs, uint64_t rflags,
                             uint64_t rsp, uint64_t ss,
                             const cpu_reg_frame *regs);
```
### 3. Block device (`src/kernel/blockdev.h`)

```c
typedef struct block_device {
    uint64_t block_size;
    uint64_t num_blocks;
    int (*read_blocks)(struct block_device *dev, uint64_t lba, uint32_t count,
                       void *buf);
    int (*write_blocks)(struct block_device *dev, uint64_t lba, uint32_t count,
                        const void *buf);
    void *private_data;
} block_device;

#define BLOCK_OK 0
#define BLOCK_ERR_BOUNDS (-1)   /* LBA+count exceeds device size */
#define BLOCK_ERR_READ    (-2)
#define BLOCK_ERR_WRITE   (-3)

int blockdev_read(struct block_device *dev, uint64_t lba, uint32_t count, void *buf);
int blockdev_write(struct block_device *dev, uint64_t lba, uint32_t count, const void *buf);
```

### 4. RAM-backed block device (`src/drivers/storage/ramdisk.[ch]`)

```c
typedef struct {
    uint8_t *data;
    uint64_t block_size;
    uint64_t num_blocks;
} ramdisk;

int  ramdisk_create(struct block_device *out, uint64_t blocks, uint64_t block_size);
void ramdisk_destroy(struct block_device *dev);
```

### 5. VFS (`src/fs/vfs.h`)

```c
/* A filesystem implementation exposes these operations. */
typedef struct fs_ops {
    int (*open)(void *fs, const char *path, int flags, struct vfs_file *out,
                void **fs_handle);
    int (*close)(void *fs, struct vfs_file *f, void *fs_handle);
    int (*read)(void *fs, struct vfs_file *f, void *buf, size_t len,
                size_t *consumed);
    int (*write)(void *fs, struct vfs_file *f, const void *buf, size_t len,
                 size_t *consumed);
    int (*seek)(void *fs, struct vfs_file *f, uint64_t off);
    int (*create)(void *fs, const char *path);
    int (*unlink)(void *fs, const char *path);
    int (*mkdir)(void *fs, const char *path);
    int (*readdir)(void *fs, const char *dir, struct vfs_entry *entry, int index);
    int (*flush)(void *fs, struct vfs_file *f);
} fs_ops;

typedef struct vfs_mount {
    const char    *mount_path;  /* "/", "/fat32/", "/mnt/"            */
    uint64_t fs_type;           /* FS_FAT32 or FS_MYBREAD             */
    struct block_device *dev;   /* underlying block device            */
    void *fs;                   /* private filesystem context         */
    fs_ops *ops;                /* method table                       */
} vfs_mount;

typedef struct vfs_file {
    struct vfs_mount *mount;
    uint64_t size;
    uint64_t offset;
    void *fs_handle;            /* per-filesystem handle data         */
} vfs_file;

typedef struct vfs_entry {
    char     name[64];
    uint32_t is_dir;
    uint64_t size;
} vfs_entry;

#define FS_FAT32   1
#define FS_MYBREAD 2
```

### 6. FAT32 (`src/fs/fat32/fat32.h`)

```c
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total16;
    uint8_t  media;
    uint16_t fat16_size;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total32;
    uint32_t fat_size32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t boot_backup;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type_str[8];
    /* plus the 64-byte boot-code / 0x55 0xAA at offsets 510-511 */
} fat32_bpb;

typedef struct {
    struct block_device *dev;
    fat32_bpb bpb;
    uint64_t fat_start;       /* first FAT sector                        */
    uint64_t root_cluster;
    uint64_t data_start;      /* first data sector                        */
    uint64_t cluster_bytes;
    uint64_t max_cluster;
    uint64_t fat_copies;
    uint64_t next_index;      /* free-cluster allocation hint (write)    */
} fat32_fs;

typedef struct {
    uint32_t start_cluster;
    uint32_t cur_cluster;
    uint32_t cluster_count;
    uint64_t size;            /* file length                              */
} fat32_handle;
```
### 7. MyBread (`src/fs/mybread/mybread.h`)

```c
#define MYBREAD_MAGIC   0x4D594252454144ULL   /* "MYBREAD" packed */
#define MYBREAD_VERSION 1

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t block_size_log;      /* log2 of data block byte size     */
    uint64_t total_blocks;
    uint64_t superblock_location; /* = 0                                */
    uint64_t bitmap_block;        /* free-space bitmap start block    */
    uint64_t bitmap_blocks;
    uint64_t inode_block;         /* inode area start block            */
    uint64_t inode_blocks;
    uint64_t root_inode;
    uint64_t flags_reserved;
} mybread_superblock;

typedef struct {
    uint32_t used       : 1;
    uint32_t is_dir     : 1;
    uint32_t type       : 5;
    uint32_t size_lo    : 24;
    uint64_t size_hi;
    uint64_t first_block;         /* first data block / dir block list */
    uint32_t block_count;
    uint32_t parent_inode;
    uint64_t ctime;
    /* future: permissions, checksums, journal state */
} mybread_inode;

typedef struct {
    uint32_t name_len;
    char     name[12];    /* packed 8.3-style, hierarchical later   */
    uint32_t inode;
    uint8_t  kind;        /* MB_FILE / MB_DIR                       */
    uint8_t  reserved[3];
} mybread_dir_entry;
```

*(MyBread layout above is the starting point; the exact packed layout is finalized in `docs/mybread.md` before implementation. MyBread uses inode-like metadata so it can grow into large files, hierarchical dirs, permissions, timestamps, and crash-consistency without cloning FAT32.)*

### 8. Self-test

Self-test functions panic on failure, consistent with the existing `pmm_self_test()` / `vm_self_test()` style, and also print a serial heartbeat. Deliberate-failure tests are CLI commands (see Testing).
## Files

### New files

| Path | Purpose |
| ---- | ------- |
| `src/lib/mem.h` | `memcpy`, `memset`, `memcmp` (freestanding helpers) |
| `src/lib/mem.c` | implementations |
| `src/kernel/heap.h` | heap API, block header struct, stats |
| `src/kernel/heap.c` | boundary-tag free-list allocator + PMM/VMM integration + `heap_self_test()` |
| `src/kernel/blockdev.h` | block-device abstraction |
| `src/kernel/blockdev.c` | bounds-checked read/write wrappers |
| `src/drivers/storage/ramdisk.h` | RAM-backed block device |
| `src/drivers/storage/ramdisk.c` | heap-backed RAM block-device read/write |
| `src/fs/vfs.h` | VFS types + public API |
| `src/fs/vfs.c` | mount table, dispatch, path helpers |
| `src/fs/fat32/fat32.h` | FAT32 structures |
| `src/fs/fat32/fat32.c` | FAT32 filesystem implementation |
| `src/fs/mybread/mybread.h` | MyBread on-disk + API types |
| `src/fs/mybread/mybread.c` | MyBread implementation |
| `tools/make_fat32_image.py` | host-side stdlib script to generate a small FAT32 image for embedding |
| `src/boot/fat32_image.h` | generated byte-array from the above tool |
| `docs/memory.md` | memory architecture (PMM/VMM/heap layout) |
| `docs/heap.md` | allocator design, metadata, usage |
| `docs/pagefault.md` | error-code decode + register diagnostics |
| `docs/vfs_blockdev.md` | block-device + VFS design |
| `docs/fat32.md` | FAT32 implementation notes |
| `docs/mybread.md` | MyBread specification |

### Modified files

| Path | Change |
| ---- | ------ |
| `GNUmakefile` | add new `.c` sources to `C_SOURCES`; add `fat32img` target to regenerate the embedded image |
| `src/abi/syscall.h` | preserve syscall IDs 1–4 and define the filesystem ABI, flags, limits, directory entries, and errors |
| `src/kernel/usercopy.c/.h` | validate user mappings and copy user data through bounded kernel buffers |
| `src/kernel/userfs.c/.h` | implement VFS-backed filesystem syscalls and the kernel-owned 16-slot descriptor table |
| `src/kernel/usermode.c` | dispatch filesystem syscalls and close handles during userspace teardown |
| `src/kernel/vmm.c/.h` | validate user-writable ranges across every page-table level |
| `src/fs/vfs.c/.h` | provide truncate/EOF/error semantics and mount-relative path dispatch |
| `src/fs/fat32.c` | implement the required truncate, directory iteration, and file/directory operations |
| `src/userspace/init.c` | add bounded filesystem shell commands and syscall boundary self-tests |
| `src/kernel/main.c` | call `heap_init()` after `vm_init()`; auto-run `heap_self_test()`; register new CLI commands |
| `README.md`, `implementation_plan.md` | document the implemented syscall ABI, commands, limits, storage model, and remaining work |
| `src/kernel/panic.h` | add `cpu_reg_frame` + `pf_decode`; extend `kernel_panic_page_fault` signature |
| `src/kernel/panic.c` | decode error-code bits; render Present/Write/User/Instruction/Reserved + register columns on page-fault screen |
| `src/arch/x86_64/idt.c` | build `cpu_reg_frame` from saved ISR registers (`stack[0..14]`), keep CR2 read, pass frame to `kernel_panic_page_fault` |
| `src/arch/x86_64/isr.s` | capture fault-time RSP (derived = saved stack pointer + 15 GPRs + 32-byte hw frame) if straightforward; otherwise document approximate RSP. Kept minimal to avoid regression |
| `src/drivers/keyboard/keyboard.c` | register/dispatch new CLI commands |
| `src/lib/string.h`/`.c` | add `strcpy`/`strcat`/`strrchr` if path handling needs them |

## Functions

### Heap (`src/kernel/heap.c`)

| Name | File | Purpose |
| ---- | ---- | ------- |
| `void heap_init(void)` | `heap.c` | carve the heap virtual region, seed an empty free list; panic if impossible |
| `void *kmalloc(size_t)` | `heap.c` | allocate >= size bytes, 16-aligned, first-fit with boundary-tag coalescing |
| `void *kcalloc(n, el)` | `heap.c` | `memset(kmalloc(n*el), 0, ..)` with overflow check |
| `void *krealloc(p, ns)` | `heap.c` | grow/shrink; copy old contents; free old |
| `void kfree(void *p)` | `heap.c` | validate magic, coalesce prev/next free blocks, re-insert |
| `void heap_get_stats(out)` | `heap.c` | fill stats struct |
| `void heap_self_test(void)` | `heap.c` | exhaustive checks (see Testing) |

### Page fault (`panic.c`, `idt.c`)
- `void decode_page_error(uint64_t, pf_decode*)` — `panic.c`
- `void kernel_panic_page_fault(...)` — extended existing function
- `exception_handler_noerr/err` — modified to fill `cpu_reg_frame`

### Block device
- `int blockdev_read(...)`, `int blockdev_write(...)` — bounds + delegate
- `int ramdisk_create(...)`, `void ramdisk_destroy(...)` — `ramdisk.c`

### VFS (`vfs.c`)
`vfs_init`, `vfs_mount`, `vfs_open`, `vfs_close`, `vfs_read`, `vfs_write`, `vfs_seek`, `vfs_create`, `vfs_unlink`, `vfs_mkdir`, `vfs_readdir`, `vfs_list`.

### FAT32 (`fat32.c`)
Read-only: `fat32_mount`, `fat32_open`, `fat32_read`, `fat32_seek`, `fat32_readdir`, `fat32_find`, `cluster_to_sector`, `read_chain`, `is_eoc`, `read_fat`. Helpers: `cluster_to_sector`, `read_chain`, `is_eoc`, `read_fat`.
Write later: `fat32_format`, `fat32_write`, `fat32_create`, `fat32_unlink`, `fat32_mkdir`, `fat32_truncate`, `write_fat`.

### MyBread (`mybread.c`)
`mybread_format`, `mybread_mount`, `mybread_create`, `mybread_write`, `mybread_read`, `mybread_list`, `mybread_delete`.

## Classes

This is a C codebase — there are **no C++ classes**. The function-pointer structural analogues are `fs_ops` (VFS vtable), `block_device` (provider interface), and the state structs (`heap_block`, `fat32_fs`, `mybread_superblock`). **Do not rename `.c` files or convert to C++** per `AGENTS.md`. No existing module is removed; internal `fat32_handle`/MyBread layout may be adjusted as specs are finalized.
## Dependencies

- **No external libraries or packages.** The kernel stays freestanding.
- New helper `src/lib/mem.[ch]` implements `memcpy`/`memset`/`memcmp` because libc is unavailable.
- `tools/make_fat32_image.py` uses only the Python `struct` stdlib; its committed output array (`src/boot/fat32_image.h`) needs no runtime library.
- Reuses existing PMM / VMM / IDT / panic / string subsystems entirely. No new build toolchains.

## Testing

Build/run cycle after each milestone:

```text
make clean
make
make run            # QEMU: boot runs self-tests, prints to serial + framebuffer
```

Automated boot self-tests (headless-friendly serial heartbeat):
- `heap_self_test()` runs automatically after `vm_self_test()` in `main.c`.
- Coverage: small alloc, large alloc, many allocs, free + reuse, deterministic random pattern, `kmalloc(0)` → NULL, 16-byte alignment, `kcalloc` zero init, `krealloc` preserves data, OOM returns NULL without corrupting.
- `selftest` CLI command re-runs all; results printed to framebuffer and serial.

Deliberate-fault tests (manual CLI, must panic visibly):
- `pfault` — dereference an unmapped virtual address → verify page-fault screen shows fault address, CR2-derived address, decoded `Present: No / Write: No / User: No / Reserved: No / Instruction: No`, and register dump.
- `pfaultw` — write to a read-only mapping → `Write: Yes`.
- `badmalloc` — huge overflow allocation → `kmalloc` returns NULL gracefully (no corruption).

FAT32:
- `mount /` against embedded pre-generated FAT32 image; `ls` lists root; `cat` reads a known file; `vfs_list` returns expected sizes.
- `mountbad` loads a corrupted image (broken FAT chain / bad BPB tag) and verifies a diagnostic panic rather than silent corruption.
- RAM-disk write test: format, create file, write, read back, `ls`, delete → verify byte round-trip.

MyBread:
- `mbfmt` (format RAM disk), `mbmount`, `mkdir`, `create`, `write`, `read`, `list`, `delete`; corrupt a superblock magic to verify clean mount failure.
- `fill` — full-disk exhaustion → graceful error, no corruption.

QEMU specifics: the standard ISO uses the Limine RAM disk. A separate
AHCI-marked ISO attaches only the generated disposable FAT32 test image to an
ICH9 controller, selects it as the root, and refuses RAM-disk fallback.
Physical SATA hardware and other controllers remain unvalidated.

## Implementation Order

1. `src/lib/mem.[ch]` (`memcpy`/`memset`/`memcmp`) — pure helpers.
2. Heap subsystem (`heap.h`/`heap.c`): boundary-tag free list + PMM/VMM integration; build.
3. `heap_self_test` + auto-run on boot + `heap`/`mem`/`selftest` CLI wiring; build + QEMU.
4. Page-fault diagnostics: `panic.c`/`.h` decode + register frame; `idt.c` supplies registers; `isr.s` RSP capture (if safe); `pfault`/`pfaultw`/`badalloc` commands; build + QEMU.
5. Block-device abstraction + RAM disk; build + minimal serial self-test.
6. VFS (`vfs.c`) layered on block devices; dispatcher; build.
7. FAT32 read-only: BPB parse, validation, FAT read, cluster→sector, root traversal, 8.3 lookup, file read, readdir; QEMU check against embedded image (`mount`, `ls`, `cat`).
8. FAT32 write: format, free-cluster alloc, create, write, truncate, delete, mkdir (root + one-level dirs); QEMU round-trip.
9. MyBread spec (`docs/mybread.md`) finalized; implement format/mount/create/write/read/list/delete; corrupt-input tests.
10. Integration pass: `make clean && make`; boot; run full self-test battery + CLI suites; update `docs/*` and `README.md`; commit in focused reviewable stages.

## Deferred / Honest limitations

- Long filenames: optional, only if time permits; default to clean 8.3 that can be extended (`docs/fat32.md`).
- Other physical storage/controller drivers and configurations beyond the
  tested QEMU ICH9 AHCI setup remain deferred and unvalidated.
- FAT32 multi-FAT mirroring write policy: reads always checked across copies; writes may update only a single copy in the first pass (documented).
- Heap fragmentation: first-fit + forward/backward coalescing mitigates; a dedicated bump region is not implemented.
- RSP on the page-fault screen is derived from the saved ISR frame (approximate); labels will be explicit so the value is not misleadingly presented as exact.
- All hardening is validated only in QEMU; physical-hardware behavior is deferred.