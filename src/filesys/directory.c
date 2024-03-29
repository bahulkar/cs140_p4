#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/free-map.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
    bool is_file;                       /* Is file or directory */
  };

/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  return inode_create (sector, entry_cnt * sizeof (struct dir_entry));
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Checks if DIR is empty. */
static bool
is_directory_empty (const struct dir *dir)
{
  struct dir_entry e;
  size_t ofs;

  ASSERT (dir != NULL);
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    {
      /* . and .. are allowed to be there. */
      if (e.in_use && (strcmp (".", e.name) && strcmp ("..", e.name)))
        {
          return false;
        }
    }
  return true;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (lookup (dir, name, &e, NULL))
  {
    *inode = inode_open (e.inode_sector);
  }
  else
  {
    *inode = NULL;
  }

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, const char *name,
         block_sector_t inode_sector, bool is_file)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.is_file = is_file;
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Dont remove current working directory. */
  if (e.inode_sector == thread_current ()->pwd_sector)
    goto done;

  /* Remove directory only if it is empty. */
  if (!(e.is_file)) 
    {
      struct dir *cur_dir = dir_open (inode_open (e.inode_sector));
      if (!is_directory_empty (cur_dir))
        {
          goto done;
        }
      dir_close (cur_dir);
    }
  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      /* Skip over . and .. */
      if (!strcmp (e.name, ".")) 
        {
          continue;
        }
      if (!strcmp (e.name, "..")) 
        {
          continue;
        }
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}

/* Looks up a file given its complete path (relative/absolute). */
bool
recursive_dir_lookup (const char *name,
            struct inode **return_inode) 
{
  char name_copy[MAX_FULL_PATH];
  char *token = NULL, *save_ptr = NULL, *dir_name = name_copy;
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  struct dir * current_dir = NULL;
  struct dir *parent_dir = NULL;
  block_sector_t sector;
  name_copy[0] = '\0';

  if (strlen (name) > MAX_FULL_PATH)
    {
      printf ("Full directory paths allowed to be only up to %d chars long\n",
               MAX_FULL_PATH);
      return false;
    }

  /* Check if directory is relative or absolute. */
  if (name[0] == '/') 
    {
      inode = inode_open (ROOT_DIR_SECTOR);
    }
  else
  {
      inode = inode_open (thread_current ()->pwd_sector);
  }
  /* Traverse through the path and fail if any part not present. */
  strlcpy (name_copy, name, strlen (name) + 1);
  for (token = strtok_r (name_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      dir_name = token;
      dir = dir_open (inode);
      parent_dir = dir;
      if (dir_name[0] == '\0') 
        {
          dir_close (dir);
          return false;
        }
      if (!(dir_lookup (dir, token, &inode)))
        {
          dir_close (dir);
          return false;
        }
      dir_close (dir);
    }
  *return_inode = inode;
  return true;
}

/* Makes a new directory. Path may be absolute/relative. */
bool
make_new_directory (const char *name)
{
  char name_copy[MAX_FULL_PATH];
  char *token = NULL, *save_ptr = NULL, *dir_name = name_copy;
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  struct dir * current_dir = NULL;
  struct dir *parent_dir = NULL;
  block_sector_t sector;
  struct inode *parent_inode = NULL;
  bool final_dir = false;
  name_copy[0] = '\0';
  struct dir_entry e;

  /* Null name not allowed. */
  if (name[0] == '\0')
    {
      return false;
    }
  if (strlen (name) > MAX_FULL_PATH)
    {
      printf ("Full directory paths allowed to be only upto  %d chars long\n",
               MAX_FULL_PATH);
      return false;
    }

  /* Check if directory is relative or absolute. */
  if (name[0] == '/')
    {
      inode = inode_open (ROOT_DIR_SECTOR);
    }
  else
  {
      inode = inode_open (thread_current ()->pwd_sector);
  }
  parent_inode = inode;
  /* Traverse through the given path */
  strlcpy (name_copy, name, strlen (name) + 1);
  for (token = strtok_r (name_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      if (final_dir) 
        {
          break;
        }
      dir_name = token;
      dir = dir_open (inode);
      parent_dir = dir;
      if (token[0] == '\0')
        {
          dir_close (dir);
          return false;
        }
      if (!(dir_lookup (dir, token, &inode)))
        {
          final_dir = true;
          continue;
        }
      /* Check that looked up entry is a directory (and not a file). */
      lookup (dir, token, &e, NULL);
      if (e.is_file)
        {
          dir_close (dir);
          return false;
        }
      parent_inode = inode;
      dir_close (dir);
    }
  /* Check to see if path is valid. */
  if (token) 
    {
      ASSERT (dir);
      dir_close (dir);
      return false;
    }

  /* Allocate sector for new directory. */
  if (!(free_map_allocate (1, &sector)))
    {
      return false;
    }
  /*Create directory with 2 entries - for . and .. */
  if (!dir_create (sector, 2))
      return false; 

  /* Create . and .. entries. */
  current_dir = dir_open (inode_open (sector));
  dir_add (current_dir, ".", sector, false);
  dir_add (current_dir, "..", inode_get_inumber (parent_dir->inode), false);
  dir_close (current_dir);

  /* Add directory entry to parent directory. */
  if (!dir_add (parent_dir, dir_name, sector, false))
    {
      dir_close (parent_dir);
      return false;
    }
  dir_close (parent_dir);
  return true;
}

/* Changes current working directory. */
bool
change_dir (const char *name)
{
  char name_copy[MAX_FULL_PATH];
  char *token = NULL, *save_ptr = NULL, *dir_name = name_copy;
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  struct dir * current_dir = NULL;
  struct dir *parent_dir = NULL;
  block_sector_t sector = thread_current ()->pwd_sector;
  name_copy[0] = '\0';

  if (strlen (name) > MAX_FULL_PATH)
    {
      printf ("Full directory paths allowed to be only upto  %d chars long\n",
              MAX_FULL_PATH);
      return false;
    }

  /* Check if directory is relative or absolute. */
  if (name[0] == '/') 
    {
      inode = inode_open (ROOT_DIR_SECTOR);
    }
  else
  {
      inode = inode_open (thread_current ()->pwd_sector);
  }
  /* Traverse path and fail if any intermediate part is absent. */
  strlcpy (name_copy, name, strlen (name) + 1);
  for (token = strtok_r (name_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      dir_name = token;
      dir = dir_open (inode);
      parent_dir = dir;
      if (!(dir_lookup (dir, token, &inode)))
        {
          dir_close (dir);
          return false;
        }
      sector = inode_get_inumber (inode);
      dir_close (dir);
    }
  inode_close (inode);

  /* Assign inumber of directory as new current working directory. */
  thread_current ()->pwd_sector = sector;
  return true;
}

/* Returns the parent directory for a given file/directory. */
/* File creation and file/directory removal use this. */
struct dir *
recursive_dir_open (const char *name)
{

  char name_copy[MAX_FULL_PATH];
  char *token = NULL, *save_ptr = NULL, *dir_name = name_copy;
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  struct dir * current_dir = NULL;
  struct dir *parent_dir = NULL;
  block_sector_t sector;
  struct inode *parent_inode = NULL;
  bool final_dir = false;
  name_copy[0] = '\0';
  block_sector_t parent_inode_sector;

  if (strlen (name) > MAX_FULL_PATH)
    {
      printf ("Full directory paths allowed to be only upto  %d chars long\n",
              MAX_FULL_PATH);
      return NULL;
    }

  /* Check if directory is relative or absolute. */
  if (name[0] == '/') 
    {
      inode = inode_open (ROOT_DIR_SECTOR);
      parent_inode_sector = ROOT_DIR_SECTOR;
    }
  else
  {
      inode = inode_open (thread_current ()->pwd_sector);
      parent_inode_sector = thread_current ()->pwd_sector;
  }
  parent_inode = inode;
  /* Traverse through the path */
  strlcpy (name_copy, name, strlen (name) + 1);
  for (token = strtok_r (name_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      if (final_dir) 
        {
          break;
        }
      dir_name = token;
      dir = dir_open (inode);
      parent_dir = dir;
      parent_inode_sector = inode_get_inumber (inode);
      /* Only look for directories, not files. */
      if (!(dir_lookup (dir, token, &inode)))
        {
          /* Only last component of name is allowed to be asbent as
             it will be the new file created. */
          inode = NULL;
          final_dir = true;
          dir_close (dir);
          continue;
        }
      parent_inode = inode;
      dir_close (dir);
    }
  /* Check to see if path is valid. */
  if (token) 
    {
      ASSERT (dir);
      dir_close (dir);
      return false;
    }
  if (inode)
    {
      inode_close (inode);
    }
  return dir_open (inode_open (parent_inode_sector));
}

/* Returns whether name is a file or a directory. */
bool get_is_file (const char *name)
{
  char name_copy[MAX_FULL_PATH];
  char *token = NULL, *save_ptr = NULL, *dir_name = name_copy;
  struct dir *dir = NULL;
  struct inode *inode = NULL;
  struct dir * current_dir = NULL;
  struct dir *parent_dir = NULL;
  block_sector_t sector;
  name_copy[0] = '\0';
  struct dir_entry dir_entry;

  /* Root directory is special case. */
  if (!strcmp (name, "/"))
    {
      return false;
    }

  if (strlen (name) > MAX_FULL_PATH)
    {
      printf ("Full directory paths allowed to be only upto  %d chars long\n",
              MAX_FULL_PATH);
      return false;
    }

  /* Check if directory is relative or absolute. */
  if (name[0] == '/') 
    {
      inode = inode_open (ROOT_DIR_SECTOR);
    }
  else
  {
      inode = inode_open (thread_current ()->pwd_sector);
  }
  /* Traverse through the path. */
  strlcpy (name_copy, name, strlen (name) + 1);
  for (token = strtok_r (name_copy, "/", &save_ptr); token != NULL;
       token = strtok_r (NULL, "/", &save_ptr))
    {
      dir_name = token;
      dir = dir_open (inode);
      parent_dir = dir;
      if (!(dir_lookup (dir, token, &inode)))
        {
          ASSERT (0);
          dir_close (dir);
          return false;
        }
      lookup (dir, token, &dir_entry, NULL);
      dir_close (dir);
    }
  inode_close (inode);
  return dir_entry.is_file;
}
