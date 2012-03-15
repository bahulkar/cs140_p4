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
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  block_cache_synchronize ();
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size) 
{
  if (name[0] == '\0') 
    {
      return false;
    }
  block_sector_t inode_sector = 0;
  if (strlen (name) > MAX_FULL_PATH) 
    {
      return false;
    }
  struct dir *dir = recursive_dir_open (name);
  /* extract only file name and add its directory entry. */
  char name_copy[MAX_FULL_PATH];
  char *file_name = NULL;
  char *token, *save_ptr;
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
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size)
                  && dir_add (dir, file_name, inode_sector, true));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
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
  if (name[0] == '\0') 
    {
      return NULL;
    }
  if (!strcmp (name, "/"))
    {
      return (struct file *) dir_open (inode_open (ROOT_DIR_SECTOR));
    }
  struct inode *inode = NULL;

  if (!(recursive_dir_lookup (name, &inode))) 
    {
      return NULL;
    }

  if (get_is_file (name)) 
    {
      return file_open (inode);
    }
  else
    {
      return (struct file *) dir_open (inode);
    }
}

bool
is_file (const char *file_name)
{
  return get_is_file (file_name);
}

int
get_inumber (const char *file_name)
{
  struct inode *inode = NULL;
  int inode_number;
  if (!strcmp (file_name, "/"))
    {
      return ROOT_DIR_SECTOR;
    }
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
  if (name[0] == '\0')
    {
      return false;
    }
  //struct dir *dir = dir_open_root ();
  struct dir *dir = recursive_dir_open (name);
  /* extract only file name and remove its directory entry. */
  char name_copy[MAX_FULL_PATH];
  char *file_name = NULL;
  char *token = NULL, *save_ptr = NULL;
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

  bool success = dir != NULL && dir_remove (dir, file_name);
  dir_close (dir); 

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
