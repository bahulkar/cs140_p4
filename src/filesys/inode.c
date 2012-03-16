#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/block-cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Length of primary index array of an inode. */
#define INDEX_ARRAY_LENGTH (124)

/* Maximum sector entries that can fit in an single index inode. */
#define MAX_SECTOR_ENTRIES_PER_INODE (BLOCK_SECTOR_SIZE /              \
                                      sizeof (block_sector_t))

/* Number of bytes addressable by a second level inode. */
#define SECONDARY_INODE_BYTES (MAX_SECTOR_ENTRIES_PER_INODE            \
                               * BLOCK_SECTOR_SIZE)

/* Beyond this length Single Index needs to be used. */
#define SINGLE_INDEX_THRESHOLD ((INDEX_ARRAY_LENGTH * BLOCK_SECTOR_SIZE))

/* Beyond this length Double Index needs to be used. */
#define DOUBLE_INDEX_THRESHOLD (SINGLE_INDEX_THRESHOLD +               \
                      (MAX_SECTOR_ENTRIES_PER_INODE * BLOCK_SECTOR_SIZE))

/* Max addressability of 2nd level index. */
#define L2_CAPACITY (MAX_SECTOR_ENTRIES_PER_INODE *                    \
                    (MAX_SECTOR_ENTRIES_PER_INODE * BLOCK_SECTOR_SIZE))

/* Max file size supported by this file system. */
#define MAX_FILE_SIZE (L2_CAPACITY + DOUBLE_INDEX_THRESHOLD)

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    off_t length;                         /* File size in bytes. */
    unsigned magic;                       /* Magic number. */
    block_sector_t index[INDEX_ARRAY_LENGTH]; /* Index array. */
    block_sector_t single_index;          /* Single index. */
    block_sector_t double_index;          /* Double index. */
  };

/* Lock to protect current inode list. */
struct lock cur_inode_list_lock;
/* List of current inodes - used for synchronization. */
struct list cur_inode_list;
/* List element of current inode list. */
struct cur_inode_list_entry
{
  struct list_elem elem;      /* List element. */
  block_sector_t inumber;     /* inumber for the inode. */
  struct condition cond;      /* cond var for synchronization. */
  bool currently_growing;     /* file growth in progress. */
};

/* Helper function prototypes. */
static bool grow_file (struct inode *inode,
                       off_t size,
                       off_t offset,
                       struct cur_inode_list_entry **cur_entry);

static bool grow_l0 (struct inode *inode,
                     block_sector_t start_sector,
                     uint32_t num_sectors);

static bool grow_l1 (struct inode *inode,
                     block_sector_t start_sector,
                     uint32_t num_sectors);

static bool grow_l2 (struct inode *inode,
                     block_sector_t start_sector,
                     uint32_t num_sectors);

static uint32_t calculate_spanned_inodes (struct inode *inode,
                                          block_sector_t start_sector,
                                          uint32_t num_sectors);

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  block_sector_t single_index[MAX_SECTOR_ENTRIES_PER_INODE];
  block_sector_t double_index[MAX_SECTOR_ENTRIES_PER_INODE];
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
      if (pos < SINGLE_INDEX_THRESHOLD) 
        {
          /* Position needs only direct reference. */
          return inode->data.index[pos / BLOCK_SECTOR_SIZE];
        }
      else
        {
          /* Position needs first level of indexing. */
          if (pos < DOUBLE_INDEX_THRESHOLD) 
            {
              block_sector_t first_level_offset;
              first_level_offset = (pos - SINGLE_INDEX_THRESHOLD) / 
                                    BLOCK_SECTOR_SIZE;
              buffer_cache_read (fs_device,
                                 inode->data.single_index,
                                 &single_index);
              return single_index[first_level_offset];
            }
          else
            {
              /* Position needs second level of indexing*/
              block_sector_t offset = pos - DOUBLE_INDEX_THRESHOLD;
              block_sector_t first_level_offset;
              block_sector_t second_level_offset;
              first_level_offset = offset / (SECONDARY_INODE_BYTES);
              buffer_cache_read (fs_device,
                                 inode->data.double_index,
                                 &single_index);
              buffer_cache_read (fs_device,
                                 single_index[first_level_offset],
                                 &double_index);
              second_level_offset = (offset % (SECONDARY_INODE_BYTES)) / 
                                    BLOCK_SECTOR_SIZE;
              return double_index[second_level_offset];
            }
        }
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  lock_init (&cur_inode_list_lock);
  list_init (&cur_inode_list);
  list_init (&open_inodes);
}

/* Grows a file based on requested size and offset.*/
static bool
grow_file (struct inode *inode,
           off_t size,
           off_t offset,
           struct cur_inode_list_entry **cur_entry)
{
  bool success = false;
  uint32_t total_extra_sectors = 0;
  off_t current_length = inode_length (inode);
  off_t final_length = offset + size;
  off_t extra_length = final_length - current_length;
  uint32_t extra_l0_sectors = 0;
  uint32_t extra_l1_sectors = 0;
  uint32_t extra_l2_sectors = 0;
  uint32_t extra_l2_inodes = 0;
  uint32_t start_l1_sector = 0;
  uint32_t start_l2_sector = 0;
  uint32_t current_total_sectors = DIV_ROUND_UP (current_length,
                                                 BLOCK_SECTOR_SIZE);
  uint32_t final_total_sectors = DIV_ROUND_UP (final_length,
                                               BLOCK_SECTOR_SIZE);

  /* Check for and prevent simultaneous growth. */
  struct list_elem *e;
  struct cur_inode_list_entry *cur_inode_list_entry = NULL;
  block_sector_t inumber = inode_get_inumber (inode);
  lock_acquire (&cur_inode_list_lock);
  bool entry_present = false;
  while (1) {
      for (e = list_begin (&cur_inode_list); 
           e != list_end (&cur_inode_list);
           e = list_next (e))
        {
          cur_inode_list_entry = list_entry (e, struct cur_inode_list_entry, elem);
          if (cur_inode_list_entry->inumber == inumber)
            {
              entry_present = true;
              break;
            }
        }
      if (entry_present && cur_inode_list_entry->currently_growing) 
        {
          cond_wait (&cur_inode_list_entry->cond, &cur_inode_list_lock);
        }
      else if (entry_present) 
        {
          *cur_entry = cur_inode_list_entry;
          break;
        }
      else
        {
          struct cur_inode_list_entry *inode_entry = NULL;
          inode_entry = malloc (sizeof (struct cur_inode_list_entry));
          if (inode_entry == NULL)
            {
              lock_release (&cur_inode_list_lock);
              return false;
            }
          inode_entry->inumber = inumber;
          cond_init (&inode_entry->cond);
          inode_entry->currently_growing = true;
          list_push_back (&cur_inode_list, &inode_entry->elem);
          *cur_entry = inode_entry;
          break;
        }
  }
  lock_release (&cur_inode_list_lock);

  /* Calculate number of sectors needed in each zone*/
  if (current_length < SINGLE_INDEX_THRESHOLD) 
    {
      start_l1_sector = (SINGLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE);
      start_l2_sector = (DOUBLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE);
      if (final_length > SINGLE_INDEX_THRESHOLD ) 
        {
          extra_l0_sectors = INDEX_ARRAY_LENGTH - current_total_sectors;
          if (final_length > DOUBLE_INDEX_THRESHOLD) 
            {
              extra_l1_sectors = MAX_SECTOR_ENTRIES_PER_INODE;
              extra_l2_sectors = final_total_sectors -
                                 (DOUBLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE);
            }
          else
            {
              extra_l1_sectors = final_total_sectors - INDEX_ARRAY_LENGTH;
            }
        }
      else
        {
          extra_l0_sectors = final_total_sectors - current_total_sectors;
        }
    }
  else if (current_length < DOUBLE_INDEX_THRESHOLD) 
    {
      start_l1_sector = current_total_sectors;
      start_l2_sector = (DOUBLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE);
      if (final_length > DOUBLE_INDEX_THRESHOLD) 
        {
          extra_l1_sectors = (DOUBLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE) -
                              current_total_sectors;
          extra_l2_sectors = final_total_sectors -
                             (DOUBLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE);
        }
      else
        {
          extra_l1_sectors = final_total_sectors - current_total_sectors;
        }
    }
  else
    {
      start_l2_sector = current_total_sectors;
      extra_l2_sectors = final_total_sectors - current_total_sectors;
    }

  if (extra_l0_sectors) 
    {
      success = grow_l0 (inode, current_total_sectors, extra_l0_sectors);
      if (!success)
        return success;
    }
  if (extra_l1_sectors) 
    {
      success = grow_l1 (inode, start_l1_sector, extra_l1_sectors);
      if (!success)
        return success;
    }
  if (extra_l2_sectors) 
    {
      success = grow_l2 (inode, start_l2_sector, extra_l2_sectors);
      if (!success)
        return success;
    }

  /* Write back the inode to disk. */
  inode->data.length = final_length;
  buffer_cache_write (fs_device, inode->sector, &(inode->data));
  return success;
}

/* Grow file in the directly indexed region. */
static bool
grow_l0 (struct inode *inode,
         block_sector_t start_sector,
         uint32_t num_sectors)
{
  bool success = false;
  uint32_t i;
  static char zeros[BLOCK_SECTOR_SIZE];

  /* Allocate new sectors and write 0's to disk. */
  for (i = 0; i < num_sectors; i++) 
    {
      if (free_map_allocate (1, &(inode->data.index[i + start_sector])))
        {
          buffer_cache_write (fs_device,
                              inode->data.index[i + start_sector],
                              zeros);
        }
      else
        goto exit;
    }
  success = true;
exit:
  return success;
}

/* Grow file in the singly indexed region. */
static bool
grow_l1 (struct inode *inode,
         block_sector_t start_sector,
         uint32_t num_sectors)
{
  bool success = false;
  block_sector_t *single_index = NULL;
  uint32_t i;
  static char zeros[BLOCK_SECTOR_SIZE];
  block_sector_t l1_base = start_sector -
                           (SINGLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE);

  single_index = calloc (1, BLOCK_SECTOR_SIZE);
  if (single_index == NULL) 
      return false;

  /* Allocate new inode if needed. */
  if (inode_length (inode) <= SINGLE_INDEX_THRESHOLD) 
    {
      if (!(free_map_allocate (1, &inode->data.single_index)))
          goto exit;
    }
  else
    {
      buffer_cache_read (fs_device, inode->data.single_index, single_index);
    }

  /* Allocate new sectors and write 0's to disk. */
  for (i = 0; i < num_sectors; i++) 
    {
      if (free_map_allocate (1, &(single_index[i + l1_base])))
        {
          buffer_cache_write (fs_device, single_index[i + l1_base], zeros);
        }
      else
        goto exit;
    }
  success = true;

  /* Write back the l1 inode. */
  buffer_cache_write (fs_device, inode->data.single_index, single_index);
exit:
  free (single_index);
  return success;
}

/* Grow file in the doubly indexed region. */
bool grow_l2 (struct inode *inode,
              block_sector_t start_sector,
              uint32_t num_sectors)
{
  bool success = false;
  bool allocated_new_inodes = false;
  uint32_t new_inodes = 0;
  uint32_t new_sectors_per_inode;
  uint32_t start_new_sector_in_inode;
  uint32_t end_new_sector_in_inode;
  uint32_t start_inode = (start_sector - (DOUBLE_INDEX_THRESHOLD /
                                          BLOCK_SECTOR_SIZE)) /
                                         MAX_SECTOR_ENTRIES_PER_INODE;
  uint32_t spanned_inodes = calculate_spanned_inodes (inode,
                                                      start_sector,
                                                      num_sectors);
  uint32_t i, j;
  static char zeros[BLOCK_SECTOR_SIZE];
  block_sector_t *double_index = calloc (1, BLOCK_SECTOR_SIZE); 
  if (double_index == NULL) 
      return false;
  block_sector_t *new_inode = calloc (1, BLOCK_SECTOR_SIZE); 
  if (new_inode == NULL) 
    {
      free (double_index);
      return false;
    }

  /* First determine if secondary inode needs to be created. */
  if (inode_length (inode) <= DOUBLE_INDEX_THRESHOLD) 
    {
      if (!(free_map_allocate (1, &inode->data.double_index)))
          goto exit;
    }
  else
    {
      buffer_cache_read (fs_device, inode->data.double_index, double_index);
    }
  /* Allocate new inodes if needed. */
  for (i = 0; i < spanned_inodes; i++) 
    {
      /* Determine which sectors in inode need to be allocated. */
      start_new_sector_in_inode = 0;
      if (i == 0) 
        {
          if ((start_sector - (DOUBLE_INDEX_THRESHOLD/BLOCK_SECTOR_SIZE)) % MAX_SECTOR_ENTRIES_PER_INODE) 
            {
              start_new_sector_in_inode = (start_sector - (DOUBLE_INDEX_THRESHOLD/BLOCK_SECTOR_SIZE)) % MAX_SECTOR_ENTRIES_PER_INODE;
              buffer_cache_read (fs_device, double_index[start_inode + i], new_inode);
            }
          else
            {
              allocated_new_inodes = true;
              if (!(free_map_allocate (1, &double_index[i + start_inode])))
                  goto exit;
            }
        }
      else
        {
          allocated_new_inodes = true;
          if (!(free_map_allocate (1, &double_index[i + start_inode])))
            goto exit;
        }
      if (i == (spanned_inodes - 1)) 
        {
          end_new_sector_in_inode = ((start_sector + num_sectors) - (DOUBLE_INDEX_THRESHOLD/BLOCK_SECTOR_SIZE)) % MAX_SECTOR_ENTRIES_PER_INODE;
        }
      else
        {
          end_new_sector_in_inode = MAX_SECTOR_ENTRIES_PER_INODE;
        }

      /* Allocate new sectors and write 0's to disk. */
      for (j = start_new_sector_in_inode; j <= end_new_sector_in_inode; j++) 
        {
          if (!(free_map_allocate (1, &new_inode[j])))
            goto exit;
          buffer_cache_write (fs_device, new_inode[j], zeros);
        }

      /* Write the inodes back to disk*/
      buffer_cache_write (fs_device, double_index[start_inode + i], new_inode);
    }
  success = true;

  /* Write back the l2 inode if needed. */
  if (allocated_new_inodes)
    {
      buffer_cache_write (fs_device, inode->data.double_index, double_index);
    }
exit:
  free (new_inode);
  free (double_index);
  return success;
}

/* Calculate number of doubly reference inodes spanned in file growth. */
static uint32_t
calculate_spanned_inodes (struct inode *inode,
                          block_sector_t start_sector,
                          uint32_t num_sectors)
{
  uint32_t count = 1;
  block_sector_t next_inode_boundary = (DIV_ROUND_UP((start_sector -
                                                      DOUBLE_INDEX_THRESHOLD),
                                                     MAX_SECTOR_ENTRIES_PER_INODE)) *
      MAX_SECTOR_ENTRIES_PER_INODE;
  if ((start_sector + num_sectors) > next_inode_boundary) 
    {
      count += DIV_ROUND_UP (((start_sector + num_sectors) -
                              next_inode_boundary),
                             MAX_SECTOR_ENTRIES_PER_INODE);
    }
  return count;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  block_sector_t *single_index = NULL;
  block_sector_t *double_index = NULL;
  bool success = false;
  uint32_t secondary_inode_cnt = 0;
  uint32_t primary_sector_cnt = 0;
  uint32_t i, j;
  block_sector_t **secondary_ptr = NULL;

  ASSERT (length >= 0);
  ASSERT (length < MAX_FILE_SIZE);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode == NULL) 
    {
      return false;
    }
  single_index = calloc (1, BLOCK_SECTOR_SIZE);
  if (single_index == NULL) 
    {
      free (disk_inode);
      return false;
    }
  double_index = calloc (1, BLOCK_SECTOR_SIZE);
  if (double_index == NULL) 
    {
      free (disk_inode);
      free (double_index);
      return false;
    }
  /* Allocate disk space for inodes. */
  if (length > SINGLE_INDEX_THRESHOLD) 
    {
      /* Single level index needed. */
      if (!(free_map_allocate (1, &disk_inode->single_index)))
        {
          goto exit;
        }
      if (length > DOUBLE_INDEX_THRESHOLD) 
        {
          /* Double level index needed. */
          primary_sector_cnt = MAX_SECTOR_ENTRIES_PER_INODE;
          if (!(free_map_allocate (1, &disk_inode->double_index)))
            {
              goto exit;
            }
          secondary_inode_cnt = DIV_ROUND_UP ((length - DOUBLE_INDEX_THRESHOLD), SECONDARY_INODE_BYTES);

          secondary_ptr = calloc (1, (sizeof (struct inode_disk *) * secondary_inode_cnt));
          if (secondary_ptr == NULL) 
            {
              goto exit;
            }
          for (i = 0; i < secondary_inode_cnt; i ++) 
            {
              if (!(free_map_allocate (1, &double_index[i])))
                {
                  goto exit;
                }
              secondary_ptr[i] = calloc (1, sizeof (struct inode_disk *) *
                                            secondary_inode_cnt);
            }
        }
      else
        {
          primary_sector_cnt = DIV_ROUND_UP ((length - SINGLE_INDEX_THRESHOLD),
                                             BLOCK_SECTOR_SIZE);
        }
    }
  size_t sectors = bytes_to_sectors (length);
  disk_inode->length = length;
  disk_inode->magic = INODE_MAGIC;

  /* Allocate sectors for actual file and write 0's. */
  static char zeros[BLOCK_SECTOR_SIZE];
  /* Direct indexed sectors. */
  for (i = 0; i < sectors && i < INDEX_ARRAY_LENGTH; i++)
    {
      if (free_map_allocate (1, &disk_inode->index[i]))
        {
          buffer_cache_write (fs_device, disk_inode->index[i], zeros);
        }
      else
        goto exit;
    }

  if (sectors > INDEX_ARRAY_LENGTH)
  {
    /* Single level indexed sectors.  */
    if (i < (DOUBLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE)) 
      {
        for (i = 0; i < primary_sector_cnt; i++) 
          {
            if (free_map_allocate (1, &single_index[i]))
              {
                buffer_cache_write (fs_device, single_index[i], zeros);
              }
            else
              goto exit;
          }
      }
    /* Double level indexed sectors. */
    if (sectors > (DOUBLE_INDEX_THRESHOLD / BLOCK_SECTOR_SIZE)) 
      {
        for (i = 0; i < secondary_inode_cnt; i++)
          {
            uint32_t secondary_sector_cnt;
            if (i == (secondary_inode_cnt - 1))
              {
                secondary_sector_cnt = (sectors -
                                        INDEX_ARRAY_LENGTH -
                                        MAX_SECTOR_ENTRIES_PER_INODE) % MAX_SECTOR_ENTRIES_PER_INODE;
              }
            else
              {
                secondary_sector_cnt = MAX_SECTOR_ENTRIES_PER_INODE;
              }
            for (j = 0; j < secondary_sector_cnt; j++)
              {
                if (free_map_allocate (1, &secondary_ptr[i][j]))
                  {
                    buffer_cache_write (fs_device, secondary_ptr[i][j], zeros);
                  }
                else
                  goto exit;
              }
          }
      }
  }
  /* Write back the inodes to disk. */
  buffer_cache_write (fs_device, sector, disk_inode);

  if (length > SINGLE_INDEX_THRESHOLD) 
    {
      buffer_cache_write (fs_device, disk_inode->single_index, single_index);
    }
  if (length > DOUBLE_INDEX_THRESHOLD) 
    {
      buffer_cache_write (fs_device, disk_inode->double_index, double_index);
      for (i = 0; i < secondary_inode_cnt; i++)
        {
          buffer_cache_write (fs_device, double_index[i], secondary_ptr[i]);
        }
    }
  success = true;

exit:
  for (i = 0; i < secondary_inode_cnt; i ++) 
    {
      free (secondary_ptr[i]);
    }
  free (secondary_ptr);
  free (disk_inode);
  free (single_index);
  free (double_index);

  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    return NULL;
  }

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  buffer_cache_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  uint32_t i;
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          free_map_release (inode->sector, 1);
          for (i = 0; i < bytes_to_sectors (inode->data.length); i++)
            {
              free_map_release (inode->data.index[i], 1);
            }
        }
      free (inode); 
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  ASSERT ((size + offset) < MAX_FILE_SIZE);

  /* Dont read a file while it is growing. */
  struct list_elem *e;
  struct cur_inode_list_entry *cur_inode_list_entry = NULL;
  block_sector_t inumber = inode_get_inumber (inode);
  lock_acquire (&cur_inode_list_lock);
  bool entry_present = false;
  while (1) {
      for (e = list_begin (&cur_inode_list); 
           e != list_end (&cur_inode_list);
           e = list_next (e))
        {
          cur_inode_list_entry = list_entry (e, struct cur_inode_list_entry, elem);
          if (cur_inode_list_entry->inumber == inumber)
            {
              entry_present = true;
              break;
            }
        }
      if (entry_present && cur_inode_list_entry->currently_growing) 
        {
          cond_wait (&cur_inode_list_entry->cond, &cur_inode_list_lock);
        }
      else
        {
          break;
        }
  }
  lock_release (&cur_inode_list_lock);

  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Read from the buffer cache */
      struct block_cache_elem * bce = NULL;
      bce = buffer_cache_read_ofs (fs_device, sector_idx, sector_ofs, buffer + bytes_read, chunk_size);
            
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  bool file_growth_needed = false;
  const off_t current_upper_length = (DIV_ROUND_UP(inode_length (inode),
                                                   BLOCK_SECTOR_SIZE)) *
                                                   BLOCK_SECTOR_SIZE;
  struct cur_inode_list_entry *inode_entry = NULL;

  ASSERT ((size + offset) < MAX_FILE_SIZE);

  if (inode->deny_write_cnt)
    return 0;

  /* Check if file growth is needed and accordingly grow it. */
  if (((offset + size) > inode_length (inode)) &&
      ((offset + size) > current_upper_length)) 
    {
      file_growth_needed = true;
      if (!grow_file (inode, size, offset, &inode_entry)) 
        {
          printf ("Error growing file\n");
          return 0;
        }
    }
  else if (((offset + size) > inode_length (inode)) && 
           ((offset + size) <= current_upper_length))
    {
      inode->data.length = offset + size;
      buffer_cache_write (fs_device, inode->sector, &(inode->data));
    }

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      /* Write to the buffer cache */
      struct block_cache_elem * bce = NULL;
      bce = buffer_cache_write_ofs (fs_device,
                                    sector_idx,
                                    sector_ofs,
                                    buffer + bytes_written,
                                    chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  if (file_growth_needed)
    {
      lock_acquire (&cur_inode_list_lock);
      inode_entry->currently_growing = false;
      cond_signal (&inode_entry->cond, &cur_inode_list_lock);
      lock_release (&cur_inode_list_lock);
    }
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}
