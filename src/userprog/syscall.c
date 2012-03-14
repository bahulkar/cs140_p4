#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include <stdint.h>
#include <inttypes.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "threads/malloc.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include <string.h>
#include <stdarg.h>

typedef int pid_t;

#define PAGE_SIZE 4096
#define MAX_PRINT_SIZE 200         /* Longest string printable to console. */
#define MAX_FILE_NAME_LENGTH 14    /* Current filesystem file name limit. */

/* Number of arguments, indexed by syscall number. */
static int lookup_table[20] = {0, 1, 1, 1, 2, 1, 1, 1, 3, 3, 2, 1, 1, 3, 3, 1, 1, 2, 1, 1};

static void **pull_arguments (void *sp, int num_args);
static int validate_stack (void **args, int num_args);
static int validate_user_ptrs (void **args, int num_ptrs, ...);
static int validate_user_memory_range (void *buffer, int size);
static void syscall_handler (struct intr_frame *);
static void sys_halt (void);
static int sys_exit_helper (void **args);
static int sys_exit (int exit_status);
static tid_t sys_exec_helper (void **args);
tid_t sys_exec (const char *cmd_line);
static int sys_wait_helper (void **args);
static int sys_wait (pid_t pid);
static int sys_create_helper (void **args);
static bool sys_create (const char *file, unsigned initial_size);
static int sys_mkdir_helper (void **args);
static bool sys_mkdir (const char *name);
static int sys_chdir_helper (void **args);
static bool sys_chdir (const char *name);
static int sys_remove_helper (void **args);
static bool sys_remove (const char *file);
static int sys_open_helper (void **args);
static int sys_open (const char *file_name);
static int sys_filesize_helper (void **args);
static int sys_filesize (int fd);
static int sys_read_helper (void **args);
static int sys_read (int fd, void *buffer, unsigned size);
static int sys_write_helper (void **args);
static int sys_write (int fd, const void *buffer, unsigned size);
static void sys_seek_helper (void **args);
static void sys_seek (int fd, unsigned position);
static unsigned sys_tell_helper (void **args);
static unsigned sys_tell (int fd);
static void sys_close_helper (void **args);
static void sys_close (int fd);
static bool sys_isdir_helper (void **args);
static bool sys_isdir (int fd);
static int sys_inumber_helper (void **args);
static int sys_inumber (int fd);
static bool sys_readdir_helper (void **args);
static bool sys_readdir (int fd, char *name);

extern struct lock file_lock; 

/* Register the interrupt to be used to call syscall_handler(). */
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

/* Get system call arguments from user stack. Returns an array of pointers
   to user stack values. */
static void **
pull_arguments (void *sp, int num_args)
{
  if (sp == NULL
      || is_kernel_vaddr (sp)
      || pagedir_get_page (active_pd (), sp) == NULL)
    {
      sys_exit (-1);
      return -1;
    }

  void **args = malloc (sizeof (void *) * (num_args + 1));
  int i;
  for (i = 1; i <= num_args; i++)
    {
      args[i] = (int *) sp + i;
    }

  validate_stack (args, num_args);

  return args;
}

/* Validates the address range of the user stack. */
static int
validate_stack (void **args, int num_args)
{
  int i;
  for (i = 1; i <= num_args; i++) 
    {
      void *ptr = args[i];
      if (ptr == NULL
          || is_kernel_vaddr (ptr)
          || pagedir_get_page (active_pd (), ptr) == NULL)
        {
          sys_exit (-1);
          return -1;
        }
    }
  return 0;
}

/* Verifies that the user memory range where kernel is going to
   read/write data to and from is valid. */
static int 
validate_user_memory_range (void *buffer, int size)
{
    void *cursor = buffer;
    int check_bytes_remaining = size;
    int bytes_to_next_page;

    while (check_bytes_remaining > 0) 
      {
        if (cursor == NULL
            || is_kernel_vaddr (cursor)
            || pagedir_get_page (active_pd (), cursor) == NULL)
          {
            sys_exit (-1);
            return -1;
          }
        bytes_to_next_page = PAGE_SIZE - ((int) cursor % PAGE_SIZE);
        if (check_bytes_remaining > bytes_to_next_page) 
          {
            cursor = (char *) cursor + bytes_to_next_page;
          }
        check_bytes_remaining = check_bytes_remaining - (bytes_to_next_page);
      }
    return 0;
}

/* Verifies that the passed user pointer is not a null pointer, a pointer
   to unmapped virtual memory, or a pointer to kernel vertual address
   space (i.e. above PHYS_BASE). In any of these cases, the offending
   process is terminated and its resources are freed, and the function
   returns false. Otherwise, returns true. */
static int
validate_user_ptrs (void **args, int num_ptrs, ...)
{
  va_list to_validate;
  va_start (to_validate, num_ptrs);
  int i;
  for (i = 0; i < num_ptrs; i++)
    {
      void *user_pointer = *((void **) args[va_arg (to_validate, int)]);

      if (user_pointer == NULL
          || is_kernel_vaddr (user_pointer)
          || pagedir_get_page (active_pd (), user_pointer) == NULL)
        {
          sys_exit (-1);
          return -1;
        }
    }
  va_end (to_validate);

  return 0;
}

/* Switches execution to the correct syscall and places any return values
   for the kernel in the eax register. */
static void
syscall_handler (struct intr_frame *f) 
{
  /* Validate user stack pointer. */
  void *sp = f->esp;
  if (sp == NULL
      || is_kernel_vaddr (sp)
      || pagedir_get_page (active_pd (), sp) == NULL)
    {
      sys_exit (-1);
      return -1;
    }

  int return_val = 0;
  int syscall_num = *((int *) sp);
  void **args;
  if (lookup_table[syscall_num]) 
    {
      args = pull_arguments (sp, lookup_table[syscall_num]);
    }

  /* Transfer control to correct system call. */
  switch (syscall_num)
    {
      case SYS_HALT:
        sys_halt();
        return;

      case SYS_EXIT:
        return_val = sys_exit_helper (args);
        break;

      case SYS_EXEC:
        return_val = sys_exec_helper (args);
        break;

      case SYS_WAIT:
        return_val = sys_wait_helper (args);
        break;

      case SYS_CREATE:
        return_val = sys_create_helper (args);
        break;

      case SYS_REMOVE:
        return_val = sys_remove_helper (args);
        break;

      case SYS_OPEN:
        return_val = sys_open_helper (args);
        break;

      case SYS_FILESIZE:
        return_val = sys_filesize_helper (args);
        break;

      case SYS_READ:
        return_val = sys_read_helper (args);
        break;

      case SYS_WRITE:
        return_val = sys_write_helper (args);
        break;

      case SYS_SEEK:
        sys_seek_helper (args);
        break;

      case SYS_TELL:
        return_val = sys_tell_helper (args);
        break;

      case SYS_CLOSE:
        sys_close_helper (args);
        break;

      case SYS_CHDIR:
        return_val = (int) sys_chdir_helper (args);
        break;

      case SYS_READDIR:
        return_val = (int) sys_readdir_helper (args);
        break;

      case SYS_ISDIR:
        return_val = sys_isdir_helper (args);
        break;

      case SYS_INUMBER:
        return_val = sys_inumber_helper (args);
        break;

      case SYS_MKDIR:
        return_val = (int) sys_mkdir_helper (args);
        break;

      default:
        /* Call is unhandled */
        ASSERT (0);
    }

  free (args);
  f->eax = return_val;
}

/* Terminates the kernel. */
static void
sys_halt (void)
{
  shutdown_power_off ();
}

/* Helper function for sys_exit(). Prepares user arguments. */
static int
sys_exit_helper (void **args)
{
  int exit_status = *((int *) args[1]);

  return sys_exit (exit_status);
}

/* Terminates the user program, including freeing resources. */
static int
sys_exit (int exit_status)
{
  thread_current ()->exit_status = exit_status;
  thread_current ()->clean_exit = true;
  thread_exit();

  return exit_status;
}

/* Helper function for sys_exec(). Prepares user arguments. */
static tid_t
sys_exec_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 1) == -1)
    {
      return -1;
    }
  const char *cmd_line = *((char **) args[1]);

  return sys_exec (cmd_line);
}

/* Runs the executable whose name is given in cmd_line, which also
   contains any passed arguments. Returns the new process's program id
   (pid). Returns a pid of -1 only if the executable cannot load or run
   for any reason. */
tid_t
sys_exec (const char *cmd_line)
{
  tid_t return_val;
  struct thread *cur = thread_current ();

  return_val = process_execute (cmd_line);
  if (return_val == -1)
    {
      return -1;
    }
  sema_down (&cur->exec_sema);
  
  /* Check if child loaded successfully. */
  if (!cur->child_loaded)
    {
      return -1;
    }

  cur->child_loaded = false;
  return return_val;
}

/* Helper function for sys_wait(). Prepares user arguments. */
static int
sys_wait_helper (void **args)
{
  pid_t pid = *((pid_t *) args[1]);

  return sys_wait (pid);
}

/* Waits for a child process pid and retrieves the child's exit status. */
static int
sys_wait (pid_t pid)
{
  return process_wait (pid);
}

/* Helper function for sys_chdir(). Prepares user arguments. */
static int
sys_chdir_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 1) == -1)
    {
      return -1;
    }

  const char *name = *((char **) args[1]);

  return sys_chdir (name);
}

/* Changes working directory. */
static bool
sys_chdir (const char *name)
{
  lock_acquire (&file_lock);
  bool success = change_dir (name);
  lock_release (&file_lock);

  return success;
}

/* Helper function for sys_mkdir(). Prepares user arguments. */
static int
sys_mkdir_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 1) == -1)
    {
      return -1;
    }

  const char *name = *((char **) args[1]);

  return sys_mkdir (name);
}

/* Makes new directory with given name */
static bool
sys_mkdir (const char *name)
{
  lock_acquire (&file_lock);
  bool success = make_new_directory (name);
  lock_release (&file_lock);

  return success;
}

/* Helper function for sys_create(). Prepares user arguments. */
static int
sys_create_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 1) == -1)
    {
      return -1;
    }

  const char *file = *((char **) args[1]);
  unsigned initial_size = *((unsigned *) args[2]);

  return sys_create (file, initial_size);
}

/* Creates a file with the name file of size initial_size. */
static bool
sys_create (const char *file, unsigned initial_size)
{
  lock_acquire (&file_lock);
  bool success = filesys_create (file, initial_size);
  lock_release (&file_lock);

  return success;
}

/* Helper function for sys_remove(). Prepares user arguments. */
static int
sys_remove_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 1) == -1)
    {
      return -1;
    }

  const char *file = *((char **) args[1]);

  return sys_remove (file);
}

/* Removes the file with name file. */
static bool
sys_remove (const char *file)
{
  lock_acquire (&file_lock);
  bool success = filesys_remove (file);
  lock_release (&file_lock);

  return success;
}

/* Helper function for sys_open(). Prepares user arguments. */
static int
sys_open_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 1) == -1)
    {
      return -1;
    }

  const char *file = *((char **) args[1]);

  return sys_open (file);
}

/* Opens the file with name file_name. This will add the file descriptor
   to the calling process's list of open files.*/
static int
sys_open (const char *file_name)
{
  int return_val = -1;

  lock_acquire (&file_lock);
  struct file *file = filesys_open (file_name);
  if (!file) 
    {
      lock_release (&file_lock);
      return -1;
    }
  bool is_file_file = is_file (file_name);
  int inumber = get_inumber (file_name);
  if (file) 
    {
      thread_current ()->fd_counter++;

      struct fd_list_elem *fd_elem;
      fd_elem = (struct fd_list_elem *) malloc (sizeof (struct fd_list_elem));
      fd_elem->fd = thread_current ()->fd_counter;
      fd_elem->file_name = file_name;
      fd_elem->deleted = false;
      fd_elem->file = file;
      fd_elem->is_file = is_file_file;
      fd_elem->inumber = inumber;
      list_push_back (&thread_current ()->open_files, &fd_elem->elem);

      return_val = fd_elem->fd;
    }

  lock_release (&file_lock);
  return return_val;
}

/* Helper function for sys_filesize(). Prepares user arguments. */
static int
sys_filesize_helper (void **args)
{
  int fd = *((int *) args[1]);
  /* It does not make sense to get the filesize of the console. */
  if (fd <= 1)
    {
      sys_exit (-1);
      return -1;
    }

  return sys_filesize (fd);
}

/* Returns the size of the opened file with file descriptor fd. */
static int
sys_filesize (int fd)
{
  int filesize = -1;
  struct thread *cur = thread_current ();
  struct list_elem *e;

  lock_acquire (&file_lock);
  for (e = list_begin (&cur->open_files); e != list_end (&cur->open_files);
       e = list_next (e))
    {
      struct fd_list_elem *fd_elem = list_entry (e, struct fd_list_elem, elem);
      if (fd_elem->fd == fd) 
        {
          filesize = file_length (fd_elem->file);
          break;
        }
    }
  lock_release (&file_lock);

  return filesize;
}


/* Helper function for sys_read(). Prepares user arguments. */
static int
sys_read_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 2) == -1)
    {
      return -1;
    }
  int fd = *((int *) args[1]);
  void *buffer = *((void **) args[2]);
  unsigned size = *((unsigned *) args[3]);
  
  if ((validate_user_memory_range (buffer, size)) == -1)
    {
      return -1;
    }

  return sys_read (fd, buffer, size);
}

/* Reads size bytes from the file open as fd into buffer. Returns the
   number of bytes actually read (0 at end of file), or -1 if the file
   could not be read (due to a condition other than end of file). Fd 0
   reads from the keyboard. */
static int
sys_read (int fd, void *buffer, unsigned size)
{
  int return_val = -1;

  struct thread *cur = thread_current ();
  struct list_elem *e;
  unsigned i;
  uint8_t *cursor = buffer;
  if ((fd == 1) || (fd < 0)) 
    {
      return -1;
    }

  /* Reading from the keyboard. */
  if (fd == 0) 
    {
      for (i = 0; i < size; i++) 
        {
          cursor[i] = input_getc();
        }
      return_val = size;
    }
  else
    {
      /* Reading from a file. */
      for (e = list_begin (&cur->open_files); 
           e != list_end (&cur->open_files);
           e = list_next (e))
        {
          struct fd_list_elem *fd_elem = list_entry (
              e, struct fd_list_elem, elem);
          if (fd_elem->fd == fd) 
            {
              if (!(fd_elem->is_file)) 
                {
                  return -1;
                }
              return_val = file_read (fd_elem->file, buffer, size);
              break;
            }
        }
    }
    return return_val;
}

static bool 
sys_readdir_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 2) == -1)
    {
      return false;
    }
  int fd = *((int *) args[1]);
  void *buffer = *((void **) args[2]);
  if ((validate_user_memory_range (buffer, (NAME_MAX + 1))) == -1)
    {
      return false;
    }
  return sys_readdir (fd, buffer);
}

static bool 
sys_readdir (int fd, char *name)
{
  struct dir *dir;
  struct list_elem *e;
  struct thread *cur = thread_current ();

  lock_acquire (&file_lock);
  for (e = list_begin (&cur->open_files); 
       e != list_end (&cur->open_files);
       e = list_next (e))
    {
      struct fd_list_elem *fd_elem = list_entry (
          e, struct fd_list_elem, elem);
      if (fd_elem->fd == fd) 
        {
          if (fd_elem->is_file) 
            {
              lock_release (&file_lock);
              return false;
            }
          dir = (struct dir *) fd_elem->file;
          break;
        }
    }
  lock_release (&file_lock);

  return dir_readdir (dir, name);
}

/* Helper function for sys_write(). Prepares user arguments. */
static int
sys_write_helper (void **args)
{
  if (validate_user_ptrs (args, 1, 2) == -1)
    {
      return -1;
    }

  int fd = *((int *) args[1]);
  void *buffer = *((void **) args[2]);
  int size = *((int *) args[3]);
  if ((validate_user_memory_range (buffer, size)) == -1)
    {
      return -1;
    }
  return sys_write (fd, buffer, size);
}

/* Writes size bytes from the buffer to the open file fd. Returns the
   number of bytes actually written, which may be less than size if some
   bytes could not be written.  */
static int
sys_write (int fd, const void *buffer, unsigned size)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  if (fd <= 0) 
    {
      return -1;
    }

  /* Writing to console. */
  if (fd == 1)
    {
      int remaining_size = size;
      char *buffer_cursor = (char *) buffer;
      while (remaining_size / MAX_PRINT_SIZE)
        {
          putbuf (buffer_cursor, MAX_PRINT_SIZE);
          buffer_cursor += MAX_PRINT_SIZE;
          remaining_size -= MAX_PRINT_SIZE;
        }
      if (remaining_size)
        {
          putbuf (buffer_cursor, remaining_size);
        }
      return size;
    }
  else
    {
      /* Writing to a file. */
      lock_acquire (&file_lock);
      for (e = list_begin (&cur->open_files); 
           e != list_end (&cur->open_files);
           e = list_next (e))
        {
          struct fd_list_elem *fd_elem = list_entry (
              e, struct fd_list_elem, elem);
          if (fd_elem->fd == fd)
            {
              if (!(fd_elem->is_file)) 
                {
                  lock_release (&file_lock);
                  return -1;
                }
              lock_release (&file_lock);
              return (file_write (fd_elem->file, buffer, size));
            }
        }
      lock_release (&file_lock);
    }
  return -1;
}

/* Helper function for sys_seek(). Prepares user arguments. */
static void
sys_seek_helper (void **args)
{
  int fd = *((int *) args[1]);
  unsigned position = *((unsigned *) args[2]);

  sys_seek (fd, position);
}

/* Changes the next byte to be read or written in open file fd to
   position, expressed in bytes from the beginning of the file. (Thus, a
   position of 0 is the file's start.)

   A seek past the current end of a file is not an error. A later read
   obtains 0 bytes, indicating end of file. A later write extends the file,
   filling any unwritten gap with zeros. */
static void
sys_seek (int fd, unsigned position)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  lock_acquire (&file_lock);
  for (e = list_begin (&cur->open_files); 
       e != list_end (&cur->open_files);
       e = list_next (e))
    {
      struct fd_list_elem *fd_elem = list_entry (
          e, struct fd_list_elem, elem);
      if (fd_elem->fd == fd) 
        {
          file_seek (fd_elem->file, position);
          break;
        }
    }
  lock_release (&file_lock);
}

/* Helper function for sys_tell(). Prepares user arguments. */
static unsigned
sys_tell_helper (void **args)
{
  int fd = *((int *) args[1]);

  return sys_tell (fd);
}

/* Returns the position of the next byte to be read or written in open
   file fd, expressed in bytes from the beginning of the file. */
static unsigned
sys_tell (int fd)
{
  unsigned return_val = 0;
  struct thread *cur = thread_current ();
  struct list_elem *e;

  if (fd <= 1) 
  {
    return 0;
  }

  for (e = list_begin (&cur->open_files); 
       e != list_end (&cur->open_files);
       e = list_next (e))
    {
      struct fd_list_elem *fd_elem = list_entry (
          e, struct fd_list_elem, elem);
      if (fd_elem->fd == fd) 
        {
          return_val = file_tell (fd_elem->file);
          break;
        }
    }
  return return_val;
}

static int
sys_inumber_helper (void **args)
{
  int fd = *((int *) args[1]);

  return sys_inumber (fd);
}

static int
sys_inumber (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  int inumber;

  lock_acquire (&file_lock);
  for (e = list_begin (&cur->open_files); 
       e != list_end (&cur->open_files);
       e = list_next (e))
    {
      struct fd_list_elem *fd_elem = list_entry (
          e, struct fd_list_elem, elem);
      if (fd_elem->fd == fd) 
        {
          inumber = fd_elem->inumber;
          break;
        }
    }
  lock_release (&file_lock);
return inumber;
}

static bool
sys_isdir_helper (void **args)
{
  int fd = *((int *) args[1]);

  return sys_isdir (fd);
}

static bool
sys_isdir (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  bool is_dir = false;

  lock_acquire (&file_lock);
  for (e = list_begin (&cur->open_files); 
       e != list_end (&cur->open_files);
       e = list_next (e))
    {
      struct fd_list_elem *fd_elem = list_entry (
          e, struct fd_list_elem, elem);
      if (fd_elem->fd == fd) 
        {
          is_dir = !fd_elem->is_file;
          break;
        }
    }
  lock_release (&file_lock);
return is_dir;
}

/* Helper function for sys_close(). Prepares user arguments. */
static void
sys_close_helper (void **args)
{
  int fd = *((int *) args[1]);

  sys_close (fd);
}

/* Closes file descriptor fd. Exiting or terminating a process implicitly
   closes all its open file descriptors, as if by calling this function
   for each one. */
static void
sys_close (int fd)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;

  lock_acquire (&file_lock);
  for (e = list_begin (&cur->open_files); 
       e != list_end (&cur->open_files);
       e = list_next (e))
    {
      struct fd_list_elem *fd_elem = list_entry (
          e, struct fd_list_elem, elem);
      if (fd_elem->fd == fd) 
        {
          if (fd_elem->is_file)
            {
              file_close (fd_elem->file);
            }
          else
            {
              dir_close ((struct dir *) fd_elem->file);
            }
          list_remove (e);
          free (fd_elem);
          break;
        }
    }
  lock_release (&file_lock);
}
