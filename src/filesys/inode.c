#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include <hash.h>
#include "threads/synch.h"
#include <stdio.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Identifies a block cache element. */
#define BLOCK_CACHE_ELEM_MAGIC 0x32323232

/* Maximum number of sectors allowed in block cache. */
#define MAX_CACHE_SECTORS 64 //!!256

/* Timer interval for periodic dirty cache block writes. */
#define PERIODIC_WRITE_TIME_IN_SECONDS 30

unsigned block_cache_hash (const struct hash_elem *b_, void *aux);
bool block_cache_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux);
struct block_cache_elem *block_cache_add (block_sector_t sector, struct lock * block_cache_lock);
struct block_cache_elem *block_cache_find (block_sector_t sector, struct lock * block_cache_lock);
struct block_cache_elem *block_cache_find_noread (block_sector_t sector, struct lock * block_cache_lock);
void block_cache_evict (struct lock * block_cache_lock);

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[125];               /* Not used. */
  };

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

/* Block cache element states */
//!! Fill in comments
enum block_cache_mode
  {
    BCM_READ = 1,                        /* Panic on failure. */
    BCM_EVICTED,                           /* Zero page contents. */
    BCM_TIMER,                           /* User page. */
    BCM_UNUSED,
    BCM_ACTIVE
  };

/* Block cache. Block must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct block_cache_elem
  {
    struct hash_elem hash_elem;         /* Hash table element. */
    struct list_elem list_elem;         /* Element in block cache list. */
    block_sector_t sector;              /* Sector number of disk location. */
    uint8_t *block;                     /* Block data. */
    bool dirty;                         /* True if dirty. */
    enum block_cache_mode state;                         /* Current state: Evicted, etc. */
    unsigned magic;                     /* Magic number. */
  };

/* Block cache data storage. */
static uint8_t block_cache[MAX_CACHE_SECTORS][BLOCK_SECTOR_SIZE];

/* Block cache elements. */
static struct block_cache_elem block_cache_elems[MAX_CACHE_SECTORS];

/* List of cache blocks with no pending operations. */
struct list block_cache_active_queue;

/* List of empty blocks to be used for caching. */
struct list block_cache_unused_queue;

/* List of cache blocks waiting to be filled with data from disk. */
struct list block_cache_read_queue;

/* List of cache blocks waiting to be written on periodic basis. */
struct list block_cache_timer_queue;

/* List of cache blocks waiting to be written and evicted. */
struct list block_cache_eviction_queue;

/* Block cache table. */
struct hash block_cache_table;

/* Lock to synchronize accesses to cache table. */
struct lock block_cache_lock;

/* Condition when a read completes. */
struct condition cond_read;

/* Condition when a write completes. */
struct condition cond_write;

/* Condition when an eviction completes. */
struct condition cond_evict;

/* Returns a hash value for block b_. */
unsigned
block_cache_hash (const struct hash_elem *b_, void *aux UNUSED)
{
  const struct block_cache_elem *b = hash_entry (b_, struct block_cache_elem, hash_elem);
  return hash_bytes (&b->sector, sizeof b->sector);
}

/* Returns true if block a precedes block b. */
bool
block_cache_less (const struct hash_elem *a_, const struct hash_elem *b_,
           void *aux UNUSED)
{
  const struct block_cache_elem *a = hash_entry (a_, struct block_cache_elem, hash_elem);
  const struct block_cache_elem *b = hash_entry (b_, struct block_cache_elem, hash_elem);

  return a->sector < b->sector;
}

struct block_cache_elem *
block_cache_find (block_sector_t sector, struct lock * block_cache_lock UNUSED)
{
  struct block_cache_elem temp_bce;
  temp_bce.sector = sector;
    
  struct hash_elem * hash_e;
  hash_e = hash_find (&block_cache_table, &temp_bce.hash_elem);
  
  struct block_cache_elem * bce = NULL;
  if (hash_e)
    {
      bce = hash_entry (hash_e, struct block_cache_elem, hash_elem);
    }
    
  /* Reinstate element before it the eviction takes place */
  if (bce && bce->state == BCM_EVICTED)
    {
      list_remove (&bce->list_elem);
      bce->state = BCM_ACTIVE;
      list_push_back (&block_cache_active_queue, &bce->list_elem);
    }
  
  return bce;
}

struct block_cache_elem *
block_cache_find_noread (block_sector_t sector, struct lock * block_cache_lock)
{
  struct block_cache_elem * bce = NULL;
  
  bce = block_cache_find (sector, block_cache_lock);
  if (bce && bce->state == BCM_READ)
    {
      bce = NULL;
    }
    
  return bce;
}

void
block_cache_evict (struct lock * block_cache_lock UNUSED)
{
  struct list_elem * list_elem = NULL;
  struct block_cache_elem * bce = NULL;

  while (!list_elem)
    {
      list_elem = list_pop_front (&block_cache_active_queue);
      if (!list_elem)
        {
          list_elem = list_pop_front (&block_cache_timer_queue);
        }
    
      if (list_elem)
        {
          break;
        }
      else
        {
          PANIC ("No cache blocks to evict");
          //!! add wait if eviction is another thread is removing blocks
          // cond_wait (&cond_write, block_cache_lock);
        }
    }

  //!! move to eviction queue if needed to not block this thread.  otherwise, write here.

  bce = list_entry (list_elem, struct block_cache_elem, list_elem);
  bce->state = BCM_EVICTED;
  
  //!! if separated, this would be on file thread, would push onto eviction queue instead
  if (bce->dirty)
    {
      // printf ("*** evict: dirty block\n");
      //!! if eviction is this linear, then it would be simpler to return the elem directly.  also remove the eviction list
      block_write (fs_device, bce->sector, bce->block);
    }
  else
    {
      // printf ("*** evict: clean block\n");
    }
    
  hash_delete (&block_cache_table, &bce->hash_elem);
  list_push_back (&block_cache_unused_queue, &bce->list_elem);
}

struct block_cache_elem *
block_cache_add (block_sector_t sector, struct lock * block_cache_lock)
{
  struct block_cache_elem * bce = block_cache_find (sector, block_cache_lock);
  
  if (!bce)
    {
      //!! Until eviction, assert when full
      // ASSERT (!list_empty (&block_cache_unused_queue));
      
      if (list_empty (&block_cache_unused_queue))
        {
          block_cache_evict (block_cache_lock);
        }

      //!! Turn on if evicting in a separate file queue
      // while (list_empty (&block_cache_unused_queue))
      //   {
      //     cond_wait (&cond_write, block_cache_lock);
      //   }
        
      bce = block_cache_find (sector, block_cache_lock);
      if (!bce)
        {
          struct list_elem * list_elem = NULL;      
          list_elem = list_pop_front (&block_cache_unused_queue);
          bce = list_entry (list_elem, struct block_cache_elem, list_elem);
          bce->state = BCM_READ;
          bce->dirty = false;
          bce->sector = sector;
          hash_insert (&block_cache_table, &bce->hash_elem);
          list_push_back (&block_cache_active_queue, &bce->list_elem);
        }
    }
      
  return bce;
}

//!! will block all threads.  consider a less disruptive approach using the periodic_queue
void
block_cache_synchronize ()
{
  struct list_elem * list_elem = NULL;
  struct block_cache_elem * bce = NULL;
  
  lock_acquire (&block_cache_lock);

  do
    {
      if (list_elem)
        {
          list_elem = list_next (list_elem);
          if (list_elem == list_end (&block_cache_active_queue))
            list_elem = NULL;
        }
      else if (!list_empty (&block_cache_active_queue))
        list_elem = list_begin (&block_cache_active_queue);

      // if (!list_elem && !list_empty (&block_cache_timer_queue))
      //   list_elem = list_pop_front (&block_cache_timer_queue);
    
      if (list_elem)
        {
          bce = list_entry (list_elem, struct block_cache_elem, list_elem);
  
          if (bce->dirty)
            block_write (fs_device, bce->sector, bce->block);
            // printf ("*");
        }
        
      // printf ("e=%#x ", list_elem);
    } while (list_elem);
    
  lock_release (&block_cache_lock);
}

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
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
  list_init (&open_inodes);

  list_init (&block_cache_active_queue);
  list_init (&block_cache_unused_queue);
  list_init (&block_cache_read_queue);
  list_init (&block_cache_timer_queue);
  list_init (&block_cache_eviction_queue);
  
  hash_init (&block_cache_table, block_cache_hash, block_cache_less, NULL);
  
  lock_init (&block_cache_lock);
  cond_init (&cond_read);
  cond_init (&cond_write);
  cond_init (&cond_evict);
      
  memset (block_cache, 0, MAX_CACHE_SECTORS * BLOCK_SECTOR_SIZE);
  int i = 0;
  for (i = 0; i < MAX_CACHE_SECTORS; i++)
    {
      struct block_cache_elem * bce = NULL;
      //!! consider using malloc on demand.
      // bce = malloc (sizeof *bce);
      // ASSERT (bce != NULL);
      bce = &block_cache_elems[i];
      bce->block = (uint8_t *)((uint32_t)block_cache + (uint32_t)(i * BLOCK_SECTOR_SIZE));
      bce->state = BCM_UNUSED;
      bce->magic = BLOCK_CACHE_ELEM_MAGIC;
      list_push_back (&block_cache_unused_queue, &bce->list_elem);
    }
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
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if (free_map_allocate (sectors, &disk_inode->start)) 
        {
          block_write (fs_device, sector, disk_inode);
          if (sectors > 0) 
            {
              static char zeros[BLOCK_SECTOR_SIZE];
              size_t i;
              
              for (i = 0; i < sectors; i++) 
                block_write (fs_device, disk_inode->start + i, zeros);
            }
          success = true; 
        } 
      free (disk_inode);
    }
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
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read (fs_device, inode->sector, &inode->data);
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
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); 
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

off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;
  uint8_t *bounce_check = NULL;

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
      
      lock_acquire (&block_cache_lock);
      
      struct block_cache_elem * bce = NULL;
      bce = block_cache_add (sector_idx, &block_cache_lock);
      ASSERT (bce);
      bounce = bce->block;
      
      // printf ("r: bce=%#x, bce->magic=%#x, bce->state=%d, bce->block=%#x, ", (uint32_t)bce, (uint32_t)bce->magic, bce->state, bce->block);
      // printf ("bounce=%#x\n\t", *bounce);
      // 
      // off_t i;
      // for (i = 0; i < 10; i++)
      //   {
      //     printf ("%#x ", *(bounce + i));
      //   }
      // printf("\n");
      
      // lock_release (&block_cache_lock);

      //!! use for file thread
      // while (bce->state == BCM_READ)
      //   {
      //     cond_wait (&cond_read);
      //   }

      if (bce->state == BCM_READ)
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          block_read (fs_device, sector_idx, bounce);
        }
      else
        {
        
          // /* We need a bounce buffer. */
          // if (bounce_check == NULL) 
          //   {
          //     bounce_check = malloc (BLOCK_SECTOR_SIZE);
          //     if (bounce_check == NULL)
          //       break;
          //   }
          //         
          // block_read (fs_device, sector_idx, bounce_check);
          // ASSERT (memcmp (bounce, bounce_check, BLOCK_SECTOR_SIZE) == 0);          
        }        
        
      memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        
        
      /* Read sector into bounce buffer, then partially copy
         into caller's buffer. */
      // block_read (fs_device, sector_idx, bounce);
      // memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);

        
      // lock_acquire (&block_cache_lock);

      // Consider moving to just the read area (maybe not for saving evicted)
      list_remove (&bce->list_elem);
      bce->state = BCM_ACTIVE;
      list_push_back (&block_cache_active_queue, &bce->list_elem);

      lock_release (&block_cache_lock);
        
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
    
  free (bounce_check);

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
  uint8_t *bounce = NULL;
  uint8_t *bounce_check = NULL;

  if (inode->deny_write_cnt)
    return 0;

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

      lock_acquire (&block_cache_lock);

      struct block_cache_elem * bce = NULL;
      bce = block_cache_add (sector_idx, &block_cache_lock); //!! we are not forcing a read, but it is on the read queue
      ASSERT (bce);
      bounce = bce->block;

      // printf ("w: bce=%#x, bce->magic=%#x, bce->state=%d, bce->block=%#x, ", (uint32_t)bce, (uint32_t)bce->magic, bce->state, bce->block);
      // printf ("bounce=%#x\n\t", *bounce);
      // 
      // off_t i;
      // for (i = 0; i < 10; i++)
      //   {
      //     printf ("%#x ", *(bounce + i));
      //   }
      // printf("\n");
        
      /* If the sector contains data before or after the chunk
         we're writing, then we need to read in the sector
         first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left)
        {
          if (bce->state == BCM_READ)
            {
              // printf ("\tdisk read\n");
              block_read (fs_device, sector_idx, bounce);
            }
        }
      else
        {
          // printf ("\tzeroed\n");
          memset (bounce, 0, BLOCK_SECTOR_SIZE);
        }
      memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
      
      // block_write (fs_device, sector_idx, bounce);
  
      // /* We need a bounce buffer. */
      // if (bounce_check == NULL) 
      //   {
      //     bounce_check = malloc (BLOCK_SECTOR_SIZE);
      //     if (bounce_check == NULL)
      //       break;
      //   }

      // block_read (fs_device, sector_idx, bounce_check);
      // printf ("\t");
      // for (i = 0; i < 10; i++)
      //   {
      //     printf ("%#x ", *(bounce + i));
      //   }
      // printf ("\n");
      
      // ASSERT (memcmp (bounce, bounce_check, BLOCK_SECTOR_SIZE) == 0);
          
      list_remove (&bce->list_elem);
      bce->state = BCM_ACTIVE;
      bce->dirty = true;
      list_push_back (&block_cache_active_queue, &bce->list_elem);
      
      lock_release (&block_cache_lock);
        

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce_check);

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
