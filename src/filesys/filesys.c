#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/synch.h"
#include "filesys/block-cache.h"

/* Partition that contains the file system. */
struct block *fs_device;

struct lock cur_name_list_lock; /* Lock to protect list of file names. */
struct list cur_name_list; /* List of file names being created/removed. */

/* List entry for ist of file names. */
struct cur_name_list_entry
{
  struct list_elem elem; /* Hash table element. */
  char file_name [NAME_MAX + 1];     /* file/directory name. */
  block_sector_t parent_dir_sector; /* inumber of parent dir. */
};

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  block_cache_init ();
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
  lock_init (&cur_name_list_lock);
  list_init (&cur_name_list);
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  block_cache_synchronize ();  
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  char name_copy[MAX_FULL_PATH];
  char *file_name = NULL;
  char *token, *save_ptr;
  block_sector_t inode_sector = 0;

  /* Null file name not allowed. */
  if (name[0] == '\0') 
    {
      return false;
    }
  if (strlen (name) > MAX_FULL_PATH) 
    {
      return false;
    }

  /* Open parent directory. */
  struct dir *dir = recursive_dir_open (name);
  if (!dir) 
    {
      return false;
    }
  /* extract only file name from entire path. */
  strlcpy (name_copy, name, strlen (name) + 1);
  for (token = strtok_r (name_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      file_name = token;
    }
  if (file_name[0] == '\0') 
    {
      dir_close (dir);
      return false;
    }
  if (strlen (file_name) > NAME_MAX) 
    {
      dir_close (dir);
      return false;
    }

  /* Check for and prevent simultaneous accesses. */
  struct list_elem *e;
  block_sector_t parent_dir_sector = inode_get_inumber (dir_get_inode (dir));
  lock_acquire (&cur_name_list_lock);
  for (e = list_begin (&cur_name_list); 
       e != list_end (&cur_name_list);
       e = list_next (e))
    {
      struct cur_name_list_entry *cur_name_list_entry = NULL;
      cur_name_list_entry = list_entry (e, struct cur_name_list_entry, elem);
      if ((cur_name_list_entry->parent_dir_sector == parent_dir_sector) && (!strcmp (file_name, cur_name_list_entry->file_name)))
        {
          dir_close (dir);
          return false;
        }
    }
  struct cur_name_list_entry *name_entry = malloc (sizeof (struct cur_name_list_entry));
  if (name_entry == NULL)
    {
      dir_close (dir);
      lock_release (&cur_name_list_lock);
      return false;
    }
  strlcpy (name_entry->file_name, file_name, strlen (file_name) + 1);
  name_entry->parent_dir_sector = parent_dir_sector;
  list_push_back (&cur_name_list, &name_entry->elem);
  lock_release (&cur_name_list_lock);

  /* Create file. and add directory entry. */
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, file_name, inode_sector, true));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);

  lock_acquire (&cur_name_list_lock);
  list_remove (&name_entry->elem);
  lock_release (&cur_name_list_lock);
  free (name_entry);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct inode *inode = NULL;

  /* Null file name not allowed. */
  if (name[0] == '\0') 
    {
      return NULL;
    }

  /* Root directory is special case. */
  if (!strcmp (name, "/"))
    {
      return (struct file *) dir_open (inode_open (ROOT_DIR_SECTOR));
    }

  /* Lookup file name and get its inode. */
  if (!(recursive_dir_lookup (name, &inode))) 
    {
      return NULL;
    }

  /* Check if it is file or directory and open accordingly. */
  if (get_is_file (name)) 
    {
      return file_open (inode);
    }
  else
    {
      return (struct file *) dir_open (inode);
    }
}

/* Returns if file is a file or directory. */
bool
is_file (const char *file_name)
{
  return get_is_file (file_name);
}

/* Returns inumber for file. */
int
get_inumber (const char *file_name)
{
  struct inode *inode = NULL;
  int inode_number;

  /* Root directory is special case. */
  if (!strcmp (file_name, "/"))
    {
      return ROOT_DIR_SECTOR;
    }
  
  /* Look up file and get its inode. */
  ASSERT (recursive_dir_lookup (file_name, &inode));
  inode_number = inode_get_inumber (inode);
  inode_close (inode);
  return inode_number;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  char name_copy[MAX_FULL_PATH];
  char *file_name = NULL;
  char *token = NULL, *save_ptr = NULL;
  struct dir *dir = NULL;

  /* Null name not allowed. */
  if (name[0] == '\0')
    {
      return false;
    }

  /* Open parent directory. */
  dir = recursive_dir_open (name);

  /* extract only file name from full path. */
  strlcpy (name_copy, name, strlen (name) + 1);
  for (token = strtok_r (name_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      file_name = token;
    }
  if (!file_name)
    {
      dir_close (dir);
      return false;
    }

  /* Check for and prevent simultaneous accesses. */
  struct list_elem *e;
  block_sector_t parent_dir_sector = inode_get_inumber (dir_get_inode (dir));
  lock_acquire (&cur_name_list_lock);
  for (e = list_begin (&cur_name_list); 
       e != list_end (&cur_name_list);
       e = list_next (e))
    {
      struct cur_name_list_entry *cur_name_list_entry = NULL;
      cur_name_list_entry = list_entry (e, struct cur_name_list_entry, elem);
      if ((cur_name_list_entry->parent_dir_sector == parent_dir_sector) && (!strcmp (file_name, cur_name_list_entry->file_name)))
        {
          dir_close (dir);
          return false;
        }
    }
  struct cur_name_list_entry *name_entry = malloc (sizeof (struct cur_name_list_entry));
  if (name_entry == NULL)
    {
      dir_close (dir);
      lock_release (&cur_name_list_lock);
      return false;
    }
  strlcpy (name_entry->file_name, file_name, strlen (file_name) + 1);
  name_entry->parent_dir_sector = parent_dir_sector;
  list_push_back (&cur_name_list, &name_entry->elem);
  lock_release (&cur_name_list_lock);

  /* Remove dirctory entry of file. */
  bool success = dir != NULL && dir_remove (dir, file_name);
  dir_close (dir); 
  lock_acquire (&cur_name_list_lock);
  list_remove (&name_entry->elem);
  lock_release (&cur_name_list_lock);
  free (name_entry);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  struct dir *current_dir;
  printf ("Formatting file system...");
  free_map_create ();
  /*Create directory with 2 entries - for . and .. */
  if (!dir_create (ROOT_DIR_SECTOR, 2))
    PANIC ("root directory creation failed");
  free_map_close ();
  /* Create . and .. entries. */
  current_dir = dir_open_root ();
  dir_add (current_dir, ".", ROOT_DIR_SECTOR, false);
  dir_add (current_dir, "..", ROOT_DIR_SECTOR, false);
  dir_close (current_dir);
  printf ("done.\n");
}
