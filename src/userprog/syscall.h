#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "threads/thread.h"

void syscall_init (void);
tid_t sys_exec (const char *cmd_line);

#endif /* userprog/syscall.h */
