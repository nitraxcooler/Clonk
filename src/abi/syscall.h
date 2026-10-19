#ifndef MYOS_SYSCALL_ABI_H
#define MYOS_SYSCALL_ABI_H

/*
 * int 0x80 ABI: number in RAX, arguments in RDI/RSI, signed result in RAX.
 * All other general-purpose registers are preserved by the entry stub.
 */
#define MYOS_SYS_WRITE 1
#define MYOS_SYS_EXIT  2
#define MYOS_SYS_READ  3
#define MYOS_SYS_CLEAR 4

#endif
