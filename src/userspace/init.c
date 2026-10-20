#include "abi/syscall.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define INPUT_BUFFER_SIZE 512U

static char input_buffer[INPUT_BUFFER_SIZE];
static char read_validation_buffer;
static char filesystem_validation_buffer[MYOS_FS_MAX_PATH + 1U];
static uint8_t filesystem_boundary_buffer[2U * 4096U] __attribute__((aligned(4096)));

static inline int64_t syscall2(uint64_t number, uint64_t arg1, uint64_t arg2) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    register uint64_t rdx __asm__("rdx") = 0U;
    __asm__ volatile ("int $0x80" : "+a"(rax) :
                      "D"(rdi), "S"(rsi), "d"(rdx) : "memory");
    return (int64_t)rax;
}

static inline int64_t syscall3(uint64_t number, uint64_t arg1, uint64_t arg2,
                               uint64_t arg3) {
    register uint64_t rax __asm__("rax") = number;
    register uint64_t rdi __asm__("rdi") = arg1;
    register uint64_t rsi __asm__("rsi") = arg2;
    register uint64_t rdx __asm__("rdx") = arg3;
    __asm__ volatile ("int $0x80" : "+a"(rax) : "D"(rdi), "S"(rsi), "d"(rdx) : "memory");
    return (int64_t)rax;
}

static int64_t syscall0(uint64_t number) {
    return syscall2(number, 0, 0);
}

static size_t text_length(const char *text) {
    size_t length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool strings_equal(const char *left, size_t left_length, const char *right) {
    size_t i = 0;
    while (i < left_length && right[i] != '\0' && left[i] == right[i]) {
        ++i;
    }
    return i == left_length && right[i] == '\0';
}

static bool write_text(const char *text) {
    size_t length = text_length(text);
    size_t written = 0;
    while (written < length) {
        size_t amount = length - written;
        if (amount > 256U) {
            amount = 256U;
        }
        if (syscall2(MYOS_SYS_WRITE, (uint64_t)(uintptr_t)(text + written), amount) != 0) {
            return false;
        }
        written += amount;
    }
    return true;
}

static bool write_bytes(const void *buffer, size_t length) {
    size_t written = 0;
    while (written < length) {
        size_t amount = length - written;
        if (amount > 256U) amount = 256U;
        if (syscall2(MYOS_SYS_WRITE,
                     (uint64_t)(uintptr_t)((const uint8_t *)buffer + written),
                     amount) != 0) {
            return false;
        }
        written += amount;
    }
    return true;
}

static bool write_character(char character) {
    return syscall2(MYOS_SYS_WRITE, (uint64_t)(uintptr_t)&character, 1U) == 0;
}

static void write_signed_number(int64_t number) {
    char digits[20];
    size_t count = 0U;
    uint64_t value;
    if (number < 0) {
        (void)write_character('-');
        value = (uint64_t)(-(number + 1)) + 1U;
    } else {
        value = (uint64_t)number;
    }
    do {
        digits[count++] = (char)('0' + value % 10U);
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) (void)write_character(digits[--count]);
}

static int syscall_validation_self_test(void) {
    const uint64_t user_limit = 0x0000800000000000ULL;
    const uint64_t stack_top = 0x00007FFFFFFFE000ULL;
    static const char readable[] = "valid";

    if (syscall2(MYOS_SYS_WRITE, 0xFFFFFFFF80000000ULL, 1U) != -1 ||
        syscall2(MYOS_SYS_WRITE, 0x0000000000500000ULL, 1U) != -1 ||
        syscall2(MYOS_SYS_WRITE, user_limit - 1U, 2U) != -1 ||
        syscall2(MYOS_SYS_WRITE, user_limit - 1U, UINT64_MAX) != -1 ||
        syscall2(MYOS_SYS_WRITE, stack_top - 1U, 2U) != -1 ||
        syscall2(MYOS_SYS_WRITE, (uint64_t)(uintptr_t)readable, 257U) != -1 ||
        syscall2(MYOS_SYS_WRITE, 0x0000000000500000ULL, 0U) != 0 ||
        syscall2(MYOS_SYS_READ, 0x0000000000500000ULL, 1U) != -1 ||
        syscall2(MYOS_SYS_READ, (uint64_t)(uintptr_t)readable, 1U) != -1 ||
        syscall2(MYOS_SYS_READ, (uint64_t)(uintptr_t)&read_validation_buffer, 0U) != -1 ||
        syscall2(MYOS_SYS_CLEAR, 1U, 0U) != -1 ||
        syscall2(0xFFFFFFFFU, 0U, 0U) != -1) {
        return -1;
    }
    return 0;
}

static int64_t fs_open(const char *path, uint32_t flags) {
    return syscall2(MYOS_SYS_FS_OPEN, (uint64_t)(uintptr_t)path, flags);
}

static int64_t fs_read(uint64_t descriptor, void *buffer, size_t length) {
    return syscall3(MYOS_SYS_FS_READ, descriptor,
                   (uint64_t)(uintptr_t)buffer, length);
}

static int64_t fs_write(uint64_t descriptor, const void *buffer, size_t length) {
    return syscall3(MYOS_SYS_FS_WRITE, descriptor,
                    (uint64_t)(uintptr_t)buffer, length);
}

static int64_t fs_readdir(uint64_t descriptor, myos_fs_dirent *entry) {
    return syscall2(MYOS_SYS_FS_READDIR, descriptor,
                    (uint64_t)(uintptr_t)entry);
}

static int64_t fs_close(uint64_t descriptor) {
    return syscall2(MYOS_SYS_FS_CLOSE, descriptor, 0U);
}

static int64_t fs_mkdir(const char *path) {
    return syscall2(MYOS_SYS_FS_MKDIR, (uint64_t)(uintptr_t)path, 0U);
}

static int64_t fs_unlink(const char *path) {
    return syscall2(MYOS_SYS_FS_UNLINK, (uint64_t)(uintptr_t)path, 0U);
}

static int64_t fs_rmdir(const char *path) {
    return syscall2(MYOS_SYS_FS_RMDIR, (uint64_t)(uintptr_t)path, 0U);
}

static int filesystem_syscall_self_test(void) {
    static const char test_path[] = "/USRCHK.TMP";
    static const char missing_path[] = "/NOFILEZZ.TXT";
    static const char traversal_path[] = "/../USRCHK.TMP";
    static const char test_directory[] = "/USRCHK.DIR";
    static const char test_child[] = "/USRCHK.DIR/CHILD.TMP";
    static const char read_only_buffer[] = "ro";
    static const myos_fs_dirent read_only_entry;
    int64_t descriptors[MYOS_FS_HANDLE_LIMIT];
    int64_t descriptor = 0;
    int64_t directory = 0;
    int result = -1;
    for (size_t i = 0; i < MYOS_FS_HANDLE_LIMIT; ++i) descriptors[i] = 0;

    filesystem_validation_buffer[0] = '/';
    for (size_t i = 1; i < MYOS_FS_MAX_PATH; ++i) {
        filesystem_validation_buffer[i] = 'A';
    }
    filesystem_validation_buffer[MYOS_FS_MAX_PATH] = '\0';

    if (fs_open((const char *)(uintptr_t)0xFFFFFFFF80000000ULL,
                MYOS_FS_OPEN_READ) != MYOS_FS_ERR_PATH ||
        fs_open((const char *)(uintptr_t)0x0000000000500000ULL,
                MYOS_FS_OPEN_READ) != MYOS_FS_ERR_PATH ||
        fs_open(filesystem_validation_buffer, MYOS_FS_OPEN_READ) != MYOS_FS_ERR_PATH ||
        fs_open(traversal_path, MYOS_FS_OPEN_READ) != MYOS_FS_ERR_PATH ||
        fs_open("RELATIVE.TXT", MYOS_FS_OPEN_READ) != MYOS_FS_ERR_NOT_FOUND ||
        fs_open(test_path, 0U) != MYOS_FS_ERR_INVALID ||
        fs_open(missing_path, MYOS_FS_OPEN_READ) != MYOS_FS_ERR_NOT_FOUND ||
        fs_read(0U, filesystem_boundary_buffer, 1U) != MYOS_FS_ERR_BAD_HANDLE ||
        fs_write(0U, filesystem_boundary_buffer, 1U) != MYOS_FS_ERR_BAD_HANDLE ||
        fs_readdir(0U, (myos_fs_dirent *)filesystem_boundary_buffer) !=
            MYOS_FS_ERR_BAD_HANDLE ||
        fs_close(0U) != MYOS_FS_ERR_BAD_HANDLE ||
        fs_mkdir((const char *)(uintptr_t)0xFFFFFFFF80000000ULL) != MYOS_FS_ERR_PATH ||
        fs_unlink(missing_path) != MYOS_FS_ERR_NOT_FOUND) {
        return -1;
    }

    descriptor = fs_open(test_path, MYOS_FS_OPEN_WRITE |
                                    MYOS_FS_OPEN_CREATE |
                                    MYOS_FS_OPEN_TRUNCATE);
    if (descriptor <= 0) goto cleanup;
    filesystem_boundary_buffer[4095] = 'A';
    filesystem_boundary_buffer[4096] = 'B';
    if (fs_write((uint64_t)descriptor,
                 (const void *)(uintptr_t)0xFFFFFFFF80000000ULL, 1U) !=
            MYOS_FS_ERR_INVALID ||
        fs_write((uint64_t)descriptor, filesystem_boundary_buffer + 4095U,
                 MYOS_FS_MAX_TRANSFER + 1U) != MYOS_FS_ERR_LIMIT ||
        fs_write((uint64_t)descriptor, filesystem_boundary_buffer + 4095U,
                 2U) != 2 ||
        fs_close((uint64_t)descriptor) != 0) {
        goto cleanup;
    }
    descriptor = 0;

    descriptor = fs_open(test_path, MYOS_FS_OPEN_READ);
    if (descriptor <= 0) goto cleanup;
    if (fs_read((uint64_t)descriptor,
                (void *)(uintptr_t)0xFFFFFFFF80000000ULL, 1U) !=
            MYOS_FS_ERR_INVALID ||
        fs_read((uint64_t)descriptor,
                (void *)(uintptr_t)read_only_buffer, 1U) != MYOS_FS_ERR_INVALID ||
        fs_read((uint64_t)descriptor, filesystem_boundary_buffer,
                MYOS_FS_MAX_TRANSFER + 1U) != MYOS_FS_ERR_LIMIT ||
        fs_readdir((uint64_t)descriptor,
                   (myos_fs_dirent *)filesystem_boundary_buffer) !=
            MYOS_FS_ERR_NOT_DIRECTORY ||
        fs_read((uint64_t)descriptor, filesystem_boundary_buffer + 4095U,
                2U) != 2 ||
        filesystem_boundary_buffer[4095] != 'A' ||
        filesystem_boundary_buffer[4096] != 'B' ||
        fs_read((uint64_t)descriptor, filesystem_boundary_buffer, 2U) != 0 ||
        fs_close((uint64_t)descriptor) != 0) {
        goto cleanup;
    }
    if (fs_close((uint64_t)descriptor) != MYOS_FS_ERR_BAD_HANDLE) goto cleanup;
    descriptor = 0;

    directory = fs_open("///", MYOS_FS_OPEN_READ);
    if (directory <= 0 ||
        fs_readdir((uint64_t)directory,
                   (myos_fs_dirent *)(uintptr_t)&read_only_entry) !=
            MYOS_FS_ERR_INVALID ||
        fs_readdir((uint64_t)directory,
                   (myos_fs_dirent *)(filesystem_boundary_buffer + 4096U -
                                      sizeof(myos_fs_dirent) / 2U)) != 1 ||
        fs_close((uint64_t)directory) != 0) {
        goto cleanup;
    }
    directory = 0;

    for (size_t i = 0; i < MYOS_FS_HANDLE_LIMIT; ++i) {
        descriptors[i] = fs_open(test_path, MYOS_FS_OPEN_READ);
        if (descriptors[i] <= 0) goto cleanup;
    }
    if (fs_open(test_path, MYOS_FS_OPEN_READ) != MYOS_FS_ERR_LIMIT) goto cleanup;
    for (size_t i = 0; i < MYOS_FS_HANDLE_LIMIT; ++i) {
        if (fs_close((uint64_t)descriptors[i]) != 0) goto cleanup;
        descriptors[i] = 0;
    }

    if (fs_unlink(test_path) != 0 ||
        fs_open(test_path, MYOS_FS_OPEN_READ) != MYOS_FS_ERR_NOT_FOUND) {
        goto cleanup;
    }

    if (fs_mkdir(test_directory) != 0) goto cleanup;
    descriptor = fs_open(test_child, MYOS_FS_OPEN_WRITE | MYOS_FS_OPEN_CREATE);
    if (descriptor <= 0 || fs_close((uint64_t)descriptor) != 0) goto cleanup;
    descriptor = 0;
    if (fs_rmdir(test_directory) != MYOS_FS_ERR_NOT_EMPTY ||
        fs_unlink(test_child) != 0 ||
        fs_rmdir(test_directory) != 0) {
        goto cleanup;
    }
    result = 0;

cleanup:
    if (descriptor > 0) (void)fs_close((uint64_t)descriptor);
    if (directory > 0) (void)fs_close((uint64_t)directory);
    for (size_t i = 0; i < MYOS_FS_HANDLE_LIMIT; ++i) {
        if (descriptors[i] > 0) (void)fs_close((uint64_t)descriptors[i]);
    }
    if (result != 0) {
        (void)fs_unlink(test_path);
        (void)fs_unlink(test_child);
        (void)fs_rmdir(test_directory);
    }
    return result;
}

static void print_help(void) {
    static const char help[] =
        "Commands:\n"
        "  help          Show this help\n"
        "  echo TEXT     Print TEXT\n"
        "  clear         Clear the console\n"
        "  version       Show the MyOS version\n"
        "  ls [PATH]     List a directory\n"
        "  cat PATH      Print a file\n"
        "  touch PATH    Create an empty file\n"
        "  write PATH TEXT  Replace a file with TEXT\n"
        "  rm PATH       Remove a file\n"
        "  mkdir PATH    Create a directory\n"
        "  rmdir PATH    Remove an empty directory\n"
        "  run           Run the bundled process scheduler tests\n"
        "  exit          Return to the kernel shell\n";
    (void)write_text(help);
}

static char *next_token(char **cursor) {
    while (**cursor == ' ' || **cursor == '\t') ++*cursor;
    if (**cursor == '\0') return NULL;

    char *token = *cursor;
    while (**cursor != '\0' && **cursor != ' ' && **cursor != '\t') ++*cursor;
    if (**cursor != '\0') {
        *(*cursor)++ = '\0';
        while (**cursor == ' ' || **cursor == '\t') ++*cursor;
    }
    return token;
}

static const char *filesystem_error_text(int64_t error) {
    switch (error) {
        case MYOS_FS_ERR_INVALID: return "invalid arguments or buffer";
        case MYOS_FS_ERR_PATH: return "invalid path";
        case MYOS_FS_ERR_NOT_FOUND: return "path not found";
        case MYOS_FS_ERR_NOT_DIRECTORY: return "not a directory";
        case MYOS_FS_ERR_IS_DIRECTORY: return "is a directory";
        case MYOS_FS_ERR_EXISTS: return "already exists";
        case MYOS_FS_ERR_READ_ONLY: return "read-only filesystem";
        case MYOS_FS_ERR_NO_MEMORY: return "out of memory";
        case MYOS_FS_ERR_NO_SPACE: return "filesystem is full";
        case MYOS_FS_ERR_BAD_HANDLE: return "invalid descriptor";
        case MYOS_FS_ERR_LIMIT: return "resource or transfer limit reached";
        case MYOS_FS_ERR_IO: return "filesystem I/O error";
        case MYOS_FS_ERR_NOT_EMPTY: return "directory is not empty";
        default: return "filesystem operation failed";
    }
}

static void print_filesystem_error(const char *command, int64_t error) {
    (void)write_text(command);
    (void)write_text(": ");
    (void)write_text(filesystem_error_text(error));
    (void)write_text("\n");
}

static void command_ls(const char *path) {
    int64_t descriptor = fs_open(path, MYOS_FS_OPEN_READ);
    if (descriptor <= 0) {
        print_filesystem_error("ls", descriptor);
        return;
    }

    size_t count = 0;
    bool failed = false;
    myos_fs_dirent entry;
    for (;;) {
        int64_t rc = fs_readdir((uint64_t)descriptor, &entry);
        if (rc == 0) break;
        if (rc < 0) {
            print_filesystem_error("ls", rc);
            failed = true;
            break;
        }
        if (rc != 1 || entry.name[0] == '\0' ||
            (entry.type != MYOS_FS_TYPE_FILE && entry.type != MYOS_FS_TYPE_DIR)) {
            print_filesystem_error("ls", MYOS_FS_ERR_OPERATION);
            failed = true;
            break;
        }
        size_t name_length = text_length(entry.name);
        if (strings_equal(entry.name, name_length, ".") ||
            strings_equal(entry.name, name_length, "..")) {
            continue;
        }
        (void)write_text(entry.name);
        (void)write_text(entry.type == MYOS_FS_TYPE_DIR ? "/\n" : "\n");
        ++count;
    }
    if (count == 0U && !failed) (void)write_text("(empty)\n");
    if (fs_close((uint64_t)descriptor) != 0) {
        print_filesystem_error("ls", MYOS_FS_ERR_BAD_HANDLE);
    }
}

static void command_cat(const char *path) {
    int64_t descriptor = fs_open(path, MYOS_FS_OPEN_READ);
    if (descriptor <= 0) {
        print_filesystem_error("cat", descriptor);
        return;
    }

    uint8_t buffer[256];
    for (;;) {
        int64_t rc = fs_read((uint64_t)descriptor, buffer, sizeof(buffer));
        if (rc == 0) break;
        if (rc < 0 || (uint64_t)rc > sizeof(buffer)) {
            print_filesystem_error("cat", rc < 0 ? rc : MYOS_FS_ERR_OPERATION);
            break;
        }
        if (!write_bytes(buffer, (size_t)rc)) {
            (void)write_text("cat: console write failed\n");
            break;
        }
    }
    (void)write_text("\n");
    if (fs_close((uint64_t)descriptor) != 0) {
        print_filesystem_error("cat", MYOS_FS_ERR_BAD_HANDLE);
    }
}

static void command_touch(const char *path) {
    int64_t descriptor = fs_open(path, MYOS_FS_OPEN_WRITE | MYOS_FS_OPEN_CREATE);
    if (descriptor <= 0) {
        print_filesystem_error("touch", descriptor);
        return;
    }
    if (fs_close((uint64_t)descriptor) != 0) {
        print_filesystem_error("touch", MYOS_FS_ERR_BAD_HANDLE);
        return;
    }
    (void)write_text("created ");
    (void)write_text(path);
    (void)write_text("\n");
}

static void command_write(const char *path, const char *text) {
    int64_t descriptor = fs_open(path, MYOS_FS_OPEN_WRITE | MYOS_FS_OPEN_CREATE |
                                      MYOS_FS_OPEN_TRUNCATE);
    if (descriptor <= 0) {
        print_filesystem_error("write", descriptor);
        return;
    }

    size_t length = text_length(text);
    size_t offset = 0;
    bool succeeded = true;
    while (offset < length) {
        size_t amount = length - offset;
        if (amount > MYOS_FS_MAX_TRANSFER) amount = MYOS_FS_MAX_TRANSFER;
        int64_t written = fs_write((uint64_t)descriptor, text + offset, amount);
        if (written <= 0 || (uint64_t)written > amount) {
            print_filesystem_error("write",
                                   written < 0 ? written : MYOS_FS_ERR_OPERATION);
            succeeded = false;
            break;
        }
        offset += (size_t)written;
    }
    if (fs_close((uint64_t)descriptor) != 0) {
        print_filesystem_error("write", MYOS_FS_ERR_BAD_HANDLE);
        succeeded = false;
    }
    if (succeeded) {
        (void)write_text("wrote ");
        (void)write_text(path);
        (void)write_text("\n");
    }
}

static void command_mkdir(const char *path) {
    int64_t rc = fs_mkdir(path);
    if (rc != 0) {
        print_filesystem_error("mkdir", rc);
        return;
    }
    (void)write_text("created directory ");
    (void)write_text(path);
    (void)write_text("\n");
}

static void command_rmdir(const char *path) {
    int64_t rc = fs_rmdir(path);
    if (rc != 0) {
        print_filesystem_error("rmdir", rc);
        return;
    }
    (void)write_text("removed directory ");
    (void)write_text(path);
    (void)write_text("\n");
}

static void command_rm(const char *path) {
    int64_t rc = fs_unlink(path);
    if (rc != 0) {
        print_filesystem_error("rm", rc);
        return;
    }
    (void)write_text("removed ");
    (void)write_text(path);
    (void)write_text("\n");
}

static void command_run(void) {
    static const char *paths[] = {
        "/boot/sched-a.elf",
        "/boot/sched-b.elf",
        "/boot/sched-fault.elf",
        "/boot/sched-a.elf",
        "/boot/sched-b.elf",
        "/boot/sched-a.elf",
        "/boot/sched-b.elf"
    };
    int64_t pids[sizeof(paths) / sizeof(paths[0])];
    size_t count = 0U;
    size_t successful = 0U;

    if (syscall2(MYOS_SYS_SPAWN, (uint64_t)(uintptr_t)"/boot/missing.elf",
                 0U) != -3 ||
        syscall2(MYOS_SYS_SPAWN,
                 (uint64_t)(uintptr_t)"/boot/sched-invalid.elf", 0U) != -1 ||
        syscall2(MYOS_SYS_SPAWN, 0xFFFFFFFF80000000ULL, 0U) != -1 ||
        syscall2(MYOS_SYS_WAIT, UINT64_MAX, 0U) != -1 ||
        syscall2(MYOS_SYS_GETPID, 1U, 0U) != -1 ||
        syscall2(MYOS_SYS_YIELD, 1U, 0U) != -1) {
        (void)write_text("Process syscall validation failed.\n");
        return;
    }

    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); ++i) {
        int64_t pid = syscall2(MYOS_SYS_SPAWN,
                               (uint64_t)(uintptr_t)paths[i], 0U);
        if (pid <= 0) {
            (void)write_text("Process creation failed: ");
            (void)write_text(paths[i]);
            (void)write_text("\n");
            continue;
        }
        pids[count++] = pid;
        ++successful;
        if (i == 0U &&
            syscall2(MYOS_SYS_WAIT, (uint64_t)pid,
                     0xFFFFFFFF80000000ULL) != -1) {
            (void)write_text("Invalid wait buffer check failed.\n");
        }
    }

    if (successful != 7U ||
        syscall2(MYOS_SYS_SPAWN, (uint64_t)(uintptr_t)paths[0], 0U) != -11) {
        (void)write_text("Process table exhaustion check failed.\n");
    } else {
        (void)write_text("Process table limit check passed.\n");
    }

    for (size_t i = 0; i < count; ++i) {
        int64_t status = INT64_MIN;
        int64_t waited = syscall2(MYOS_SYS_WAIT, (uint64_t)pids[i],
                                  (uint64_t)(uintptr_t)&status);
        (void)write_text("wait pid=");
        write_signed_number(pids[i]);
        (void)write_text(" result=");
        write_signed_number(waited);
        (void)write_text(" status=");
        write_signed_number(status);
        (void)write_text("\n");
        if (waited != pids[i] ||
            (i == 2U ? status != -134 : status != 0)) {
            (void)write_text("Process exit check failed for pid ");
            char digits[20];
            size_t digit_count = 0U;
            uint64_t value = (uint64_t)pids[i];
            do {
                digits[digit_count++] = (char)('0' + value % 10U);
                value /= 10U;
            } while (value != 0U);
            while (digit_count != 0U) {
                char character = digits[--digit_count];
                (void)write_bytes(&character, 1U);
            }
            (void)write_text(".\n");
        }
    }
    (void)write_text("Process tests complete.\n");
}

static void run_command(char *line) {
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    if (*line == '\0') {
        return;
    }

    char *command = line;
    size_t command_length = 0;
    while (line[command_length] != '\0' &&
           line[command_length] != ' ' && line[command_length] != '\t') {
        ++command_length;
    }
    char *arguments = line + command_length;
    while (*arguments == ' ' || *arguments == '\t') {
        ++arguments;
    }

    if (strings_equal(command, command_length, "help")) {
        print_help();
    } else if (strings_equal(command, command_length, "echo")) {
        (void)write_text(arguments);
        (void)write_text("\n");
    } else if (strings_equal(command, command_length, "ls")) {
        char *cursor = arguments;
        char *path = next_token(&cursor);
        if (path != NULL && *cursor != '\0') {
            (void)write_text("Usage: ls [PATH]\n");
        } else {
            command_ls(path == NULL ? "/" : path);
        }
    } else if (strings_equal(command, command_length, "cat") ||
               strings_equal(command, command_length, "touch") ||
               strings_equal(command, command_length, "rm") ||
               strings_equal(command, command_length, "mkdir") ||
               strings_equal(command, command_length, "rmdir")) {
        char *cursor = arguments;
        char *path = next_token(&cursor);
        if (path == NULL || *cursor != '\0') {
            (void)write_text("Usage: command PATH\n");
        } else if (strings_equal(command, command_length, "cat")) {
            command_cat(path);
        } else if (strings_equal(command, command_length, "touch")) {
            command_touch(path);
        } else if (strings_equal(command, command_length, "rm")) {
            command_rm(path);
        } else if (strings_equal(command, command_length, "rmdir")) {
            command_rmdir(path);
        } else {
            command_mkdir(path);
        }
    } else if (strings_equal(command, command_length, "write")) {
        char *cursor = arguments;
        char *path = next_token(&cursor);
        if (path == NULL || *cursor == '\0') {
            (void)write_text("Usage: write PATH TEXT\n");
        } else {
            command_write(path, cursor);
        }
    } else if (strings_equal(command, command_length, "run") &&
               *arguments == '\0') {
        command_run();
    } else if (strings_equal(command, command_length, "clear") &&
               *arguments == '\0') {
        (void)syscall0(MYOS_SYS_CLEAR);
    } else if (strings_equal(command, command_length, "version") &&
               *arguments == '\0') {
        (void)write_text("MyOS v0.1.1 (x86_64 Long Mode Bare-Metal)\n");
    } else if (strings_equal(command, command_length, "exit") &&
               *arguments == '\0') {
        (void)syscall0(MYOS_SYS_EXIT);
    } else if (strings_equal(command, command_length, "clear") ||
               strings_equal(command, command_length, "version") ||
               strings_equal(command, command_length, "run") ||
               strings_equal(command, command_length, "exit")) {
        (void)write_text("Usage: command takes no arguments.\n");
    } else {
        (void)write_text("Command not found: ");
        (void)write_text(line);
        (void)write_text("\n");
    }
}

static void run_shell(void) {
    for (;;) {
        size_t length = 0;
        bool too_long = false;
        input_buffer[0] = '\0';
        (void)write_text("MyOS$ ");

        for (;;) {
            char character;
            if (syscall2(MYOS_SYS_READ, (uint64_t)(uintptr_t)&character, 1U) != 1) {
                (void)write_text("\nInput unavailable; returning to the kernel shell.\n");
                (void)syscall0(MYOS_SYS_EXIT);
                for (;;) __asm__ volatile ("pause");
            }

            if (character == '\n') {
                (void)write_text("\n");
                if (too_long) {
                    (void)write_text("Command too long; input discarded.\n");
                } else {
                    input_buffer[length] = '\0';
                    run_command(input_buffer);
                }
                break;
            }
            if (character == '\b') {
                if (too_long) {
                    too_long = false;
                    length = 0;
                    input_buffer[0] = '\0';
                    (void)write_text("\nMyOS$ ");
                } else if (length > 0U) {
                    input_buffer[--length] = '\0';
                    (void)write_character('\b');
                }
                continue;
            }
            if (character >= ' ' && character <= '~' && !too_long) {
                if (length < INPUT_BUFFER_SIZE - 1U) {
                    input_buffer[length++] = character;
                    input_buffer[length] = '\0';
                    (void)write_character(character);
                } else {
                    too_long = true;
                }
            }
        }
    }
}

void _start(void) {
    static const char validation_failed[] = "USER: syscall validation FAILED\n";
    static const char validation_passed[] = "USER: syscall validation passed.\n";
    static const char filesystem_validation_failed[] =
        "USER: filesystem syscall validation FAILED\n";
    static const char filesystem_validation_passed[] =
        "USER: filesystem syscall validation passed.\n";
    static const char banner[] =
        "MyOS userspace shell\n"
        "Type help for available commands.\n";

    if (syscall_validation_self_test() != 0) {
        (void)write_text(validation_failed);
    } else {
        (void)write_text(validation_passed);
    }
    if (filesystem_syscall_self_test() != 0) {
        (void)write_text(filesystem_validation_failed);
    } else {
        (void)write_text(filesystem_validation_passed);
    }
#ifdef MYOS_TEST_SUPERVISOR_FAULT
    (void)*(volatile uint64_t *)(uintptr_t)0xFFFFFFFF80000000ULL;
#endif
    (void)write_text(banner);
    run_shell();
}
