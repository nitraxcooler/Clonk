#ifndef CLONK_SYSCALL_ABI_H
#define CLONK_SYSCALL_ABI_H

#include <stdint.h>

#define CLONK_SYS_WRITE 1
#define CLONK_SYS_EXIT  2
#define CLONK_SYS_READ  3
#define CLONK_SYS_CLEAR 4

#define CLONK_SYS_FS_OPEN   5
#define CLONK_SYS_FS_READ   6
#define CLONK_SYS_FS_WRITE  7
#define CLONK_SYS_FS_READDIR 8
#define CLONK_SYS_FS_CLOSE  9
#define CLONK_SYS_FS_MKDIR  10
#define CLONK_SYS_FS_UNLINK 11
#define CLONK_SYS_FS_RMDIR  12
#define CLONK_SYS_SPAWN     13
#define CLONK_SYS_WAIT      14
#define CLONK_SYS_YIELD     15
#define CLONK_SYS_GETPID    16

#define CLONK_PROCESS_ERR_INVALID  (-1)
#define CLONK_PROCESS_ERR_NOT_FOUND (-3)
#define CLONK_PROCESS_ERR_NO_MEMORY (-8)
#define CLONK_PROCESS_ERR_LIMIT    (-11)

#define CLONK_FS_OPEN_READ      0x01U
#define CLONK_FS_OPEN_WRITE     0x02U
#define CLONK_FS_OPEN_CREATE    0x04U
#define CLONK_FS_OPEN_TRUNCATE  0x08U

#define CLONK_FS_MAX_PATH       256U
#define CLONK_FS_MAX_TRANSFER  512U
#define CLONK_FS_HANDLE_LIMIT   16U
#define CLONK_FS_DIRENT_NAME_MAX 64U

#define CLONK_FS_TYPE_FILE 0U
#define CLONK_FS_TYPE_DIR  1U

typedef struct clonk_fs_dirent {
    char name[CLONK_FS_DIRENT_NAME_MAX];
    uint32_t type;
} clonk_fs_dirent;

#define CLONK_FS_ERR_INVALID       (-1)
#define CLONK_FS_ERR_PATH          (-2)
#define CLONK_FS_ERR_NOT_FOUND     (-3)
#define CLONK_FS_ERR_NOT_DIRECTORY (-4)
#define CLONK_FS_ERR_IS_DIRECTORY  (-5)
#define CLONK_FS_ERR_EXISTS        (-6)
#define CLONK_FS_ERR_READ_ONLY     (-7)
#define CLONK_FS_ERR_NO_MEMORY     (-8)
#define CLONK_FS_ERR_NO_SPACE      (-9)
#define CLONK_FS_ERR_BAD_HANDLE    (-10)
#define CLONK_FS_ERR_LIMIT         (-11)
#define CLONK_FS_ERR_IO            (-12)
#define CLONK_FS_ERR_OPERATION     (-13)
#define CLONK_FS_ERR_NOT_EMPTY     (-14)

#endif
