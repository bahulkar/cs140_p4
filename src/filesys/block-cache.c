#include "filesys/block-cache.h"
#include "filesys/filesys.h"
#include "threads/thread.h"
#include "threads/synch.h"
#include "devices/timer.h"
#include <list.h>
#include <debug.h>
#include <string.h>
#include <hash.h>
#include <stdio.h>

/* Identifies a block cache element. */
#define BLOCK_CACHE_ELEM_MAGIC 0x32323232

/* Maximum number of sectors allowed in block cache. */
#define MAX_CACHE_SECTORS 64

/* Timer interval for periodic dirty cache block writes. */
#define PERIODIC_WRITE_TIME_IN_SECONDS 30

unsigned block_cache_hash (const struct hash_elem *b_, void *aux);
bool block_cache_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux);
struct block_cache_elem *block_cache_add_internal (block_sector_t sector, struct lock * block_cache_lock);
void block_cache_evict (struct lock * block_cache_lock);

static thread_func periodic_write_thread; // (void);
static thread_func read_ahead_thread; // (void);

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

/* Condition when a read completes. */
struct condition cond_read;

/* Condition when a write completes. */
struct condition cond_write;

/* Condition when an eviction completes. */
struct condition cond_evict;

/* Initializes the block cache module. */

static void
periodic_write_thread (void *aux UNUSED)
{
  while (1)
    {
      timer_msleep (PERIODIC_WRITE_TIME_IN_SECONDS * 1000);
      printf ("*** synchronize\n");
      block_cache_synchronize ();
    }
}

static void
read_ahead_thread (void *aux UNUSED)
{
  lock_acquire (&block_cache_lock);
  struct list_elem * list_elem = NULL;
  struct block_cache_elem * bce = NULL;
  
  while (1)
    {
      cond_wait (&cond_read, &block_cache_lock);

      while (!list_empty (&block_cache_read_queue))
        {
          // printf ("*** read-ahead\n");
          list_elem = list_pop_front (&block_cache_read_queue);
          bce = list_entry (list_elem, struct block_cache_elem, list_elem);
          
          block_read (fs_device, bce->sector, bce->block);
          list_push_back (&block_cache_active_queue, &bce->list_elem);
        }
    }
  
  lock_release (&block_cache_lock);
}

void
block_cache_init (void) 
{
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
    
    
    thread_create ("read_ahead_thread", PRI_MIN, read_ahead_thread, NULL);
    thread_create ("periodic_write_thread", PRI_MIN + 1, periodic_write_thread, NULL);
}

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

// struct block_cache_elem *
// block_cache_find_noread (block_sector_t sector, struct lock * block_cache_lock)
// {
//   struct block_cache_elem * bce = NULL;
//   
//   bce = block_cache_find (sector, block_cache_lock);
//   if (bce && bce->state == BCM_READ)
//     {
//       bce = NULL;
//     }
//     
//   return bce;
// }

void
block_cache_evict (struct lock * block_cache_lock UNUSED)
{
  struct list_elem * list_elem = NULL;
  struct block_cache_elem * bce = NULL;

  while (!list_elem)
    {
      list_elem = list_pop_front (&block_cache_active_queue);
      if (!list_elem)
        list_elem = list_pop_front (&block_cache_timer_queue);

      if (list_elem)
        break;
      else
        PANIC ("No cache blocks to evict");
          //!! add wait if eviction is another thread is removing blocks
          // cond_wait (&cond_write, block_cache_lock);
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
block_cache_add_internal (block_sector_t sector, struct lock * block_cache_lock)
{
  struct block_cache_elem * bce = block_cache_find (sector, block_cache_lock);
  
  if (!bce)
    {      
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
    
    ASSERT (bce);
    ASSERT (bce->magic == BLOCK_CACHE_ELEM_MAGIC);
      
  return bce;
}

struct block_cache_elem *
block_cache_add (block_sector_t sector, struct lock * block_cache_lock)
{
  struct block_cache_elem * bce = NULL;
  struct block_cache_elem * bce_next = NULL;

  bce = block_cache_add_internal (sector, block_cache_lock);
  bce_next = block_cache_add_internal (sector + 1, block_cache_lock); //!! check for max sector?
  
  /* Read-ahead next sector is not loaded in cache */
  if (bce_next->state == BCM_READ)
    {
      list_remove (&bce_next->list_elem);
      list_push_back (&block_cache_read_queue, &bce_next->list_elem);
      cond_broadcast (&cond_read, block_cache_lock);
    }
  
  return bce;
}

void
block_cache_mark_active (struct block_cache_elem * bce, struct lock * block_cache_lock UNUSED)
{
  list_remove (&bce->list_elem);
  bce->state = BCM_ACTIVE;
  list_push_back (&block_cache_active_queue, &bce->list_elem);
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

struct block_cache_elem *
buffer_cache_write (struct block *fs_device UNUSED, block_sector_t sector_idx, const void *buffer)
{
  lock_acquire (&block_cache_lock);
  struct block_cache_elem * bce = NULL;
  bce = block_cache_add (sector_idx, &block_cache_lock);
  
  // printf ("w: bce=%#x, bce->magic=%#x, bce->state=%d, bce->block=%#x, ", (uint32_t)bce, (uint32_t)bce->magic, bce->state, bce->block);
  // printf ("bounce=%#x\n\t", *bounce);
  // 
  // off_t i;
  // for (i = 0; i < 10; i++)
  //   {
  //     printf ("%#x ", *(bounce + i));
  //   }
  // printf("\n");
  
  bce->dirty = true;
  memcpy (bce->block, buffer, BLOCK_SECTOR_SIZE);      
  block_cache_mark_active (bce, &block_cache_lock);
  lock_release (&block_cache_lock);

  return bce;  
}

struct block_cache_elem *
buffer_cache_read (struct block *fs_device UNUSED, block_sector_t sector_idx, void *buffer_)
{
  lock_acquire (&block_cache_lock);
  struct block_cache_elem * bce = NULL;
  bce = block_cache_add (sector_idx, &block_cache_lock);
  
  uint8_t *buffer = buffer_;
  // lock_release (&block_cache_lock);

  // printf ("r: bce=%#x, bce->magic=%#x, bce->state=%d, bce->block=%#x, ", (uint32_t)bce, (uint32_t)bce->magic, bce->state, bce->block);
  // printf ("bounce=%#x\n\t", *bounce);
  // 
  // off_t i;
  // for (i = 0; i < 10; i++)
  //   {
  //     printf ("%#x ", *(bounce + i));
  //   }
  // printf("\n");

  /* Read sector into the cache, then partially copy
     into caller's buffer. */
  if (bce->state == BCM_READ)
    block_read (fs_device, sector_idx, bce->block);
  
  memcpy (buffer, bce->block, BLOCK_SECTOR_SIZE);
  
  // lock_acquire (&block_cache_lock);
  block_cache_mark_active (bce, &block_cache_lock);
  lock_release (&block_cache_lock);
  
  return bce;
}
