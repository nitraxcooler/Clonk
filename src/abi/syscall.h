#ifndef MYOS_SYSCALL_ABI_H
#define MYOS_SYSCALL_ABI_H

#include <stdint.h>

/*
 * int 0x80 ABI: number in RAX, arguments in RDI/RSI/RDX, signed result in
 * RAX. All other general-purpose registers are preserved by the entry stub.
 */
#define MYOS_SYS_WRITE 1
#define MYOS_SYS_EXIT  2
#define MYOS_SYS_READ  3
#define MYOS_SYS_CLEAR 4

#define MYOS_SYS_FS_OPEN   5
#define MYOS_SYS_FS_READ   6
#define MYOS_SYS_FS_WRITE  7
#define MYOS_SYS_FS_READDIR 8
#define MYOS_SYS_FS_CLOSE  9
#define MYOS_SYS_FS_MKDIR  10
#define MYOS_SYS_FS_UNLINK 11

#define MYOS_FS_OPEN_READ      0x01U
#define MYOS_FS_OPEN_WRITE     0x02U
#define MYOS_FS_OPEN_CREATE    0x04U
#define MYOS_FS_OPEN_TRUNCATE  0x08U

#define MYOS_FS_MAX_PATH       256U
#define MYOS_FS_MAX_TRANSFER  512U
#define MYOS_FS_HANDLE_LIMIT   16U
#define MYOS_FS_DIRENT_NAME_MAX 64U

#define MYOS_FS_TYPE_FILE 0U
#define MYOS_FS_TYPE_DIR  1U

typedef struct myos_fs_dirent {
    char name[MYOS_FS_DIRENT_NAME_MAX];
    uint32_t type;
} myos_fs_dirent;

/* Negative filesystem syscall results. */
#define MYOS_FS_ERR_INVALID       (-1)
#define MYOS_FS_ERR_PATH          (-2)
#define MYOS_FS_ERR_NOT_FOUND     (-3)
#define MYOS_FS_ERR_NOT_DIRECTORY (-4)
#define MYOS_FS_ERR_IS_DIRECTORY  (-5)
#define MYOS_FS_ERR_EXISTS        (-6)
#define MYOS_FS_ERR_READ_ONLY     (-7)
#define MYOS_FS_ERR_NO_MEMORY     (-8)
#define MYOS_FS_ERR_NO_SPACE      (-9)
#define MYOS_FS_ERR_BAD_HANDLE    (-10)
#define MYOS_FS_ERR_LIMIT         (-11)
#define MYOS_FS_ERR_IO            (-12)
#define MYOS_FS_ERR_OPERATION     (-13)

/*
 * FS_OPEN(path, flags) returns an opaque positive descriptor. CREATE opens an
 * existing file or creates a missing one; TRUNCATE explicitly empties it.
 * FS_READ/FS_WRITE return transferred bytes, with read returning 0 at EOF.
 * FS_READDIR returns 1 for an entry, 0 at end, or a negative error.
 */

#endif
