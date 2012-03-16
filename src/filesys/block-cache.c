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
struct block_cache_elem *block_cache_add_internal (block_sector_t sector, struct lock * block_cache_lock, bool read_ahead);
void block_cache_evict (struct lock * block_cache_lock);
void validate_list_element (struct list_elem * le);
void print_buffer_cache_block (struct block_cache_elem * bce);


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
struct condition cond_read_queue_add;

/* Initializes the block cache module. */

/* Thread periodically writes the buffer cache to disk in the background */
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

//!! Read-ahead is currently disabled
/* Thread performs read-aheads in the background */
static void
read_ahead_thread (void *aux UNUSED)
{
  lock_acquire (&block_cache_lock);
  struct list_elem * list_elem = NULL;
  struct block_cache_elem * bce = NULL;
    
  /* Waits for pending reads, and then loads them into the cache */
  while (true)
    {
      // printf ("[");
      cond_wait (&cond_read_queue_add, &block_cache_lock);
      // printf ("read-ahead 1\n");
      // printf ("]");      
      
      while (!list_empty (&block_cache_read_queue))
        {
          
          list_elem = list_pop_front (&block_cache_read_queue);
          bce = list_entry (list_elem, struct block_cache_elem, list_elem);

          ASSERT (bce->state == BCM_READ);
          bce->state = BCM_READING;
          
          lock_release (&block_cache_lock);
          block_read (fs_device, bce->sector, bce->block);
          lock_acquire (&block_cache_lock);

          /* Check that the block hasn't been read and/or written
             by another thread before overwriting with disk contents */
          ASSERT (bce->state == BCM_READING);
          bce->state = BCM_ACTIVE;
              printf ("<ra>");
          list_push_back (&block_cache_active_queue, &bce->list_elem);
          validate_list_element (&bce->list_elem);
          
          cond_broadcast (&cond_read, &block_cache_lock);
        }
        
      // printf ("read-ahead 2\n");
    }
  
  lock_release (&block_cache_lock);
}

/* Initializes buffer cache module. */
void
block_cache_init (void) 
{
  /* Set up hash table and lists used to track buffer cache. */
  list_init (&block_cache_active_queue);
  list_init (&block_cache_unused_queue);
  list_init (&block_cache_read_queue);
  list_init (&block_cache_timer_queue);
  list_init (&block_cache_eviction_queue);
  hash_init (&block_cache_table, block_cache_hash, block_cache_less, NULL);
  lock_init (&block_cache_lock);
  
  /* Set up condition to wake up file I/O thread. */
  cond_init (&cond_read);
  cond_init (&cond_write);
  cond_init (&cond_read_queue_add);
  
  /* Set up buffer cache data structures. */
  memset (block_cache, 0, MAX_CACHE_SECTORS * BLOCK_SECTOR_SIZE);
  int i = 0;
  for (i = 0; i < MAX_CACHE_SECTORS; i++)
    {
      struct block_cache_elem * bce = NULL;
      bce = &block_cache_elems[i];
      bce->block = (uint8_t *)((uint32_t)block_cache + (uint32_t)(i * BLOCK_SECTOR_SIZE));
      bce->state = BCM_UNUSED;
      bce->magic = BLOCK_CACHE_ELEM_MAGIC;
      list_push_back (&block_cache_unused_queue, &bce->list_elem);
    }
    
  /* Start file read-ahead thread & background cache saving thread. */
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

/* Returns the block cache element stored in the cache. */
struct block_cache_elem * 
block_cache_find (block_sector_t sector, struct lock * block_cache_lock)
{
  struct block_cache_elem temp_bce;
  struct hash_elem * hash_e;
  struct block_cache_elem * bce = NULL;

  temp_bce.sector = sector;
  hash_e = hash_find (&block_cache_table, &temp_bce.hash_elem);
  
  /* Let eviction write finish before continuing */
  do
    {
      hash_e = hash_find (&block_cache_table, &temp_bce.hash_elem);
      if (hash_e)
        bce = hash_entry (hash_e, struct block_cache_elem, hash_elem);
//$$$
      // break;
      if (bce && bce->state == BCM_WRITING) {
        // printf ("<");
        cond_wait (&cond_write, block_cache_lock);
        // printf (">");
      }
      //!! not sure if should block on reading if it's read-ahead 
      else if (bce && bce->state == BCM_READING ) {
        // printf ("{");
        cond_wait (&cond_read, block_cache_lock);
        // printf ("}");
      }
      else {
        break;
      }
      // printf ("^");
    } while (true);

  if (bce)
    {
      ASSERT (bce->magic == BLOCK_CACHE_ELEM_MAGIC);
    }
  
  return bce;
}

//!!
/* Evicts a buffer cache element */
void
block_cache_evict (struct lock * block_cache_lock)
{
  struct list_elem * list_elem = NULL;
  struct block_cache_elem * bce = NULL;
  
  /* Find a buffer cache element to evict */
  // FIFO Algorithm
  //!! busy waits!!
  
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);

  
  ASSERT (!list_empty (&block_cache_active_queue));
  printf ("a");
  
  while (!list_elem && !list_empty (&block_cache_active_queue))
    {
      
      // bce = list_entry (e, struct block_cache_elem, list_elem);
      // printf ("b(%X->%X)\n", bce->hash_elem, bce->hash_elem);
      // printf ("b(%X->%X)\n", bce->hash_elem, bce->hash_elem);
      // printf ("b(%X->%X)\n", bce, bce);  
      
      
      /* Pull from active queue, but still in hash so that other threads will block */
      list_elem = list_pop_front (&block_cache_active_queue);
      if (list_elem)
        {
          bce = list_entry (list_elem, struct block_cache_elem, list_elem);
          

          printf ("(%x->%x), ", (uint32_t)(&bce->list_elem), (uint32_t)((bce->list_elem).next));
          
          //!!$$
          if (bce->state == BCM_READING || (bce->state & BCM_PINNED) == BCM_PINNED)
            {
              printf ("|");
              //!! This breaks if cond_wait is below list_push_back
              // cond_wait (&cond_read, block_cache_lock);              
              list_push_back (&block_cache_active_queue, list_elem);
              list_elem = NULL;
              bce = NULL;
            }
          else
            {
              ASSERT (bce->state == BCM_ACTIVE || bce->state == BCM_READ);   
            }
        }
      else 
        {
          // printf (":");
        }
      // printf (";");
      
      //!! Consider panicking if we get back to our start point
    }
    
  // /* Evicts the buffer cache element */
  // bce->state = BCM_WRITING;
  // 
  // /* Write the block back to disk */
  // if (bce->dirty)
  //   {
  //     lock_release (block_cache_lock);
  //     block_write (fs_device, bce->sector, bce->block);
  //     lock_acquire (block_cache_lock);
  //   }
  // 
  // ASSERT (bce->state == BCM_WRITING);
  // //ASSERT (bce doesn't belong to any queue);
  // 
  // /* Once done evicting, place the block on the unused queue */
  // hash_delete (&block_cache_table, &bce->hash_elem);
  // bce->state = BCM_EVICTED;
  // list_push_back (&block_cache_unused_queue, &bce->list_elem);
  // validate_list_element (&bce->list_elem);
  // cond_broadcast (&cond_write, block_cache_lock);
  
    printf ("; s=%d;", bce->state);
  

    ASSERT (bce->state == BCM_ACTIVE || bce->state == BCM_READ);
    
    // /* Evicts the buffer cache element */
    // list_remove (&bce->list_elem);

    /* Write the block back to disk */
    if (bce->dirty)
      {
        bce->state = BCM_WRITING;
        lock_release (block_cache_lock);
        block_write (fs_device, bce->sector, bce->block);
        lock_acquire (block_cache_lock);
        ASSERT (bce->state == BCM_WRITING);
      }

    /* Once done evicting, place the block on the unused queue */
    hash_delete (&block_cache_table, &bce->hash_elem);
    bce->state = BCM_EVICTED;
    list_push_back (&block_cache_unused_queue, &bce->list_elem);
    validate_list_element (&bce->list_elem);
    debug_validate_list (&block_cache_active_queue);
    debug_validate_list (&block_cache_unused_queue);
    
    
    printf ("u(%x->%x)\n", (uint32_t)(&bce->list_elem), (uint32_t)((bce->list_elem).next));
    
    
    cond_broadcast (&cond_write, block_cache_lock);
  
  
}

/* Either 1) adds a buffer cache element into the buffer cache for the
   requested sector if not found, or 2) returns the pre-existing buffer
   cache element */
struct block_cache_elem *
block_cache_add_internal (block_sector_t sector, struct lock * block_cache_lock, bool read_ahead)
{
  struct block_cache_elem * bce = NULL;
  
  ASSERT (sector < 10000);
  
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
  
  
  while (!(bce = block_cache_find (sector, block_cache_lock))
         && list_empty (&block_cache_unused_queue))
    block_cache_evict (block_cache_lock);
    
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
    
  
  ASSERT (bce->state != BCM_READING || bce->state != BCM_WRITING);
  
  /* Make sure that the block is not in the cache (from before or another thread)*/
  if (bce)
    {
      if (!read_ahead && bce->state == BCM_READ)
        {
          /* Bump items from read_ahead_queue if we are trying to read it now */
          validate_list_element (&bce->list_elem);
          list_remove (&bce->list_elem);
          list_push_back (&block_cache_active_queue, &bce->list_elem);
        }
    }
  else if (!bce)
    {
      struct list_elem * list_elem = NULL;      
      list_elem = list_pop_front (&block_cache_unused_queue);
      bce = list_entry (list_elem, struct block_cache_elem, list_elem);
      bce->dirty = false;
      bce->sector = sector;
      hash_insert (&block_cache_table, &bce->hash_elem);
      
      if (read_ahead)
        {
          bce->state = BCM_READ;
          
          list_push_back (&block_cache_read_queue, &bce->list_elem);
          cond_broadcast (&cond_read_queue_add, block_cache_lock);
        }
      else
        {
          // bce->state = BCM_READ;
          bce->state = BCM_PINNED | BCM_READ;
          list_push_back (&block_cache_active_queue, &bce->list_elem);
        }
    }
    
  /* In case it was found on the read-ahead queue, go ahead and move it
     to the active queue */
  validate_list_element (&bce->list_elem);
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);


  ASSERT (bce);
  ASSERT (bce->magic == BLOCK_CACHE_ELEM_MAGIC);
      
  return bce;
}

void
validate_list_element (struct list_elem * le)
{
  ASSERT (le->next->prev == le);
  ASSERT (le->prev->next == le);
}

/* Returns the new or already existing block cache element for the sector
   and reads in the next sector too */
struct block_cache_elem *
block_cache_add (block_sector_t sector, struct lock * block_cache_lock)
{
  struct block_cache_elem * bce = NULL;
  struct block_cache_elem * bce_next = NULL;
  
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
  
  
  /* Prepares the buffer cache element for the requested sector */
  bce = block_cache_add_internal (sector, block_cache_lock, false);
  
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
  
  debug_print_list (&block_cache_active_queue);
  debug_print_list (&block_cache_unused_queue);
  
  
  bce->state |= BCM_PINNED;
  printf ("bce_le=%#2x (%d), ", (uint32_t)(&bce->list_elem), bce->sector);
  
  /* Starts reading the next sector on a background thread */
  bce_next = block_cache_add_internal (sector + 1, block_cache_lock, true);
  

  // printf ("(%x->%x)", (uint32_t)(&bce->list_elem), (uint32_t)((bce->list_elem).next));
  
  printf ("bce_le=%#2x (%d)\n", (uint32_t)(&bce->list_elem), bce->sector);
  
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
  
  if (bce->sector != sector) {
    debug_print_list (&block_cache_active_queue);
    debug_print_list (&block_cache_unused_queue);
  }
  //!! check sector size
  ASSERT (bce->magic == BLOCK_CACHE_ELEM_MAGIC);
  ASSERT (bce->sector == sector);
  
  if ((bce->state & BCM_PINNED) == BCM_PINNED)
    bce->state &= ~BCM_PINNED;
  
  return bce;
}

/* Moves a block cache element as active */
void
block_cache_mark_active (struct block_cache_elem * bce, struct lock * block_cache_lock UNUSED)
{
  validate_list_element (&bce->list_elem);
  list_remove (&bce->list_elem);
  bce->state = BCM_ACTIVE;
  printf ("<ma>");
  list_push_back (&block_cache_active_queue, &bce->list_elem);  
  validate_list_element (&bce->list_elem);
  
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);

}

//!! update comment if asynch
/* Saves the buffer cache to disk synchronously */
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

      if (list_elem)
        {
          bce = list_entry (list_elem, struct block_cache_elem, list_elem);
          
          /* If another thread moved the list elem off the active queue, then cancel synchronize.
             Otherwise, save the cached block if dirty */
          if (bce->state != BCM_ACTIVE)
            break;
          else if (bce->dirty)
            {
              //!! a local buffer will make extra safe
              //!! async causes one test to fail.  check later
              // lock_release (&block_cache_lock);
              block_write (fs_device, bce->sector, bce->block);
              // cond_broadcast (&cond_write, &block_cache_lock);
              // lock_acquire (&block_cache_lock);
              // printf ("*");
            }
        }
        
      // printf ("e=%#x ", list_elem);
    } while (list_elem);
    
  lock_release (&block_cache_lock);
}

/* Writes the provided buffer into the buffer cache */
struct block_cache_elem *
buffer_cache_write_ofs (struct block *fs_device, block_sector_t sector_idx, int sector_ofs, const void *buffer, int chunk_size)
{
  lock_acquire (&block_cache_lock);
  
  ASSERT (sector_idx < 10000)
  
  struct block_cache_elem * bce = NULL;
  bce = block_cache_add (sector_idx, &block_cache_lock);
  
  
  ASSERT (bce->state == BCM_READ || bce->state == BCM_ACTIVE);
  
  /* If the sector contains data before or after the chunk
     we're writing, then we need to read in the sector
     first.  Otherwise we start with a sector of all zeros. */
  if (sector_ofs > 0 || chunk_size < BLOCK_SECTOR_SIZE - sector_ofs)
    {
      if (bce->state == BCM_READ)
        {
          lock_release (&block_cache_lock);
          buffer_cache_read (fs_device, sector_idx, bce->block);
          lock_acquire (&block_cache_lock);
        }
    }
  else
    {
      memset (bce->block, 0, BLOCK_SECTOR_SIZE);
    }
    
  memcpy (bce->block + sector_ofs, buffer, chunk_size);      
  
  bce->dirty = true;

  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
  validate_list_element (&bce->list_elem);
  list_remove (&bce->list_elem);
  bce->state = BCM_ACTIVE;
  printf ("<wo>");
  list_push_back (&block_cache_active_queue, &bce->list_elem);  
  validate_list_element (&bce->list_elem);
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);

  lock_release (&block_cache_lock);
  
  return bce;
}

/* Writes the provided buffer into the buffer cache */
struct block_cache_elem *
buffer_cache_write (struct block *fs_device UNUSED, block_sector_t sector_idx, const void *buffer)
{
  return buffer_cache_write_ofs (fs_device, sector_idx, 0, buffer, BLOCK_SECTOR_SIZE);  
}

//!! don't return bce
/* Reads the sector from the buffer cache into the caller's buffer */
struct block_cache_elem *
buffer_cache_read_ofs (struct block *fs_device, block_sector_t sector_idx, int sector_ofs, void *buffer_, int chunk_size)
{
  lock_acquire (&block_cache_lock);
  
  ASSERT (sector_idx < 10000);

  struct block_cache_elem * bce = NULL;
  uint8_t *buffer = buffer_;
  
  bce = block_cache_add (sector_idx, &block_cache_lock);
    
  ASSERT (bce->state == BCM_READ || bce->state == BCM_ACTIVE);

  /* Read sector into the cache, then partially copy
     into caller's buffer. */
  if (bce->state == BCM_READ)
    {
      debug_validate_list (&block_cache_active_queue);
      debug_validate_list (&block_cache_unused_queue);
      
      //!! want to take it off the current queue (but must check that it's on one)
      validate_list_element (&bce->list_elem);
      list_remove (&bce->list_elem);
      
      bce->state = BCM_READING;
      
      lock_release (&block_cache_lock);
      block_read (fs_device, bce->sector, bce->block);
      lock_acquire (&block_cache_lock);

      /* Check that the block hasn't been read and/or written
         by another thread before overwriting with disk contents */
      ASSERT (bce->state == BCM_READING);
      
    printf ("<ro> <bce_le=%#2x (%d)> ", (uint32_t)(&bce->list_elem), bce->sector);
      bce->state = BCM_ACTIVE;
      list_push_back (&block_cache_active_queue, &bce->list_elem);  
      validate_list_element (&bce->list_elem);   
      
      debug_validate_list (&block_cache_active_queue);
      debug_validate_list (&block_cache_unused_queue);
           
    }  

  memcpy (buffer, bce->block + sector_ofs, chunk_size);
  cond_broadcast (&cond_read, &block_cache_lock);
  lock_release (&block_cache_lock);
  
  return bce;
}


struct block_cache_elem *
buffer_cache_read (struct block *fs_device, block_sector_t sector_idx, void *buffer)
{
  return buffer_cache_read_ofs (fs_device, sector_idx, 0, buffer, BLOCK_SECTOR_SIZE);
}

void 
print_buffer_cache_block (struct block_cache_elem * bce)
{
  printf ("w: bce=%#x, bce->magic=%#x, bce->state=%d, bce->block=%#x, ", (uint32_t)bce, (uint32_t)bce->magic, bce->state, (uint32_t)bce->block);
  printf ("block=%#x\n\t", *bce->block);
  
  off_t i;
  for (i = 0; i < 10; i++)
    {
      printf ("%#x ", *(bce->block + i));
    }
  printf("\n");  
}
