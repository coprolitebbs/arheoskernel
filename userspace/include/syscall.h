#ifndef SYSCALL_H
#define SYSCALL_H

#define SYS_FILL_SCREEN  2
#define SYS_EXIT 		 3
#define SYS_YIELD 		 4
#define SYS_GET_BOOTINFO 5
#define SYS_FB_DRAW_CHAR 6
#define SYS_GET_FB_INFO  7
#define SYS_ALLOC_PAGE   8
#define SYS_FREE_PAGE    9

#define SYS_OPEN           10
#define SYS_READDIR        11
#define SYS_CLOSE          12
#define SYS_READ           13
#define SYS_WRITE          14
#define SYS_MKDIR          15
#define SYS_UNLINK         16
#define SYS_RMDIR          17
#define SYS_STAT           18
#define SYS_SEEK           19

#define SYS_CLEAR_SCREEN   20
#define SYS_DRAW_CHAR      21

#define SYS_SPAWN          31
#define SYS_WAIT_PID       32
#define SYS_GET_MEM_INFO   33
#define SYS_SET_WALLPAPER  34
#define SYS_SBRK           35

#define SYS_CHDIR          41
#define SYS_GETCWD         42
#define SYS_TELL           43

#define SYS_HALT_OS        99

#define KBD_DRIVER_NOT_READY   -2
#define PROCESS_IS_ACTIVE      -2

#endif

