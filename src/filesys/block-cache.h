#ifndef FILESYS_BLOCKCACHE_H
#define FILESYS_BLOCKCACHE_H

#include <stdbool.h>
#include <list.h>
#include <hash.h>
#include "devices/block.h"
#include "filesys/off_t.h"

/* Block cache element states */
enum block_cache_mode
  {
    BCM_UNUSED,               /* Never been used. On unused queue. */
    BCM_EVICTED,              /* Was evicted. On unused queue. */
    BCM_ACTIVE,               /* Active cache data.  Active queue */
    BCM_READ,                 /* Needs to be read from disk. Active queue. */
    BCM_READING,              /* Reading from disk. Active queue. */
    BCM_WRITING,              /* Writing to disk. No queue. */
  };

/* Block cache. Block must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct block_cache_elem
  {
    struct hash_elem hash_elem;         /* Hash table element. */
    struct list_elem list_elem;         /* Element in block cache list. */
    struct list_elem clock_elem;        /* Element for the clock algorithm. */
    block_sector_t sector;              /* Sector number of disk location. */
    uint8_t *block;                     /* Block data. */
    bool dirty;                         /* True if dirty. */
    uint8_t accessed;                   /* One for standara accesses,
                                           two for inode accesses */
    bool pinned;                        /* True when locking element
                                           for thread safety. */
    enum block_cache_mode state;        /* Current state: Evicted, etc. */
    unsigned magic;                     /* Magic number. */
  };

/* Lock to synchronize accesses to cache table. */
struct lock block_cache_lock;

/* Condition when a read completes. */
struct condition cond_read;

/* Condition when a write completes. */
struct condition cond_write;

void block_cache_init (void);

void block_cache_synchronize (void);

struct block_cache_elem *
buffer_cache_write (struct block *fs_device,
                    block_sector_t sector_idx,
                    const void *buffer,
                    struct lock * block_cache_lock);
                    
struct block_cache_elem *
buffer_cache_write_ofs (struct block *fs_device,
                        block_sector_t sector_idx,
                        int sector_ofs,
                        const void *buffer,
                        int chunk_size,
                        struct lock * block_cache_lock);
                        
struct block_cache_elem *
buffer_cache_read (struct block *fs_device,
                   block_sector_t sector_idx,
                   void *buffer,
                   struct lock * block_cache_lock);
                   
struct block_cache_elem *
buffer_cache_read_ofs (struct block *fs_device,
                       block_sector_t sector_idx,
                       int sector_ofs,
                       void *buffer,
                       int chunk_size,
                       struct lock * block_cache_lock);
                                              
void
block_cache_mark_active (struct block_cache_elem * bce,
                         struct lock * block_cache_lock);
                              
struct block_cache_elem *
block_cache_add (block_sector_t sector,
                 struct lock * block_cache_lock);
                                          
struct block_cache_elem *
block_cache_find (block_sector_t sector,
                  struct lock * block_cache_lock);
                                           
struct block_cache_elem *
buffer_cache_read_inode (struct block *fs_device,
                         block_sector_t sector,
                         struct lock * block_cache_lock);
                         
void
  buffer_cache_set_dirty (struct block_cache_elem * bce);
                         

#endif /* filesys/block_cache.h */
