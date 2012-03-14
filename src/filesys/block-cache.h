#ifndef FILESYS_BLOCKCACHE_H
#define FILESYS_BLOCKCACHE_H

#include <stdbool.h>
#include <list.h>
#include <hash.h>
#include "devices/block.h"

/* Block cache element states */
enum block_cache_mode
  {
    BCM_READ = 1,                       /* Needs to be read from disk. */
    BCM_EVICTED,                        /* Was evicted. On unused queue. */
    BCM_WRITING,                        /* Writing to disk.  Can be rescued. */
    BCM_UNUSED,                         /* Never been used. On unused queue. */
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
    enum block_cache_mode state;        /* Current state: Evicted, etc. */
    unsigned magic;                     /* Magic number. */
  };

/* Lock to synchronize accesses to cache table. */
struct lock block_cache_lock;

void block_cache_init (void);
void block_cache_synchronize (void);
struct block_cache_elem *buffer_cache_write (struct block *fs_device, block_sector_t sector_idx, const void *buffer);
struct block_cache_elem *buffer_cache_read (struct block *fs_device, block_sector_t sector_idx, void *buffer);
void block_cache_mark_active (struct block_cache_elem * bce, struct lock * block_cache_lock);
struct block_cache_elem *block_cache_add (block_sector_t sector, struct lock * block_cache_lock);
struct block_cache_elem *block_cache_find (block_sector_t sector, struct lock * block_cache_lock);

#endif /* filesys/block_cache.h */
