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
#include "filesys/inode.h"

/* Identifies a block cache element. */
#define BLOCK_CACHE_ELEM_MAGIC 0x32323232

/* Maximum number of sectors allowed in block cache. */
#define MAX_CACHE_SECTORS 64

/* Timer interval for periodic dirty cache block writes. */
#define PERIODIC_WRITE_TIME_IN_SECONDS 2

unsigned block_cache_hash (const struct hash_elem *b_, void *aux);
bool block_cache_less (const struct hash_elem *a_, const struct hash_elem *b_, void *aux);
struct block_cache_elem *block_cache_add_internal (block_sector_t sector, struct lock * block_cache_lock, bool read_ahead);
void block_cache_evict (struct lock * block_cache_lock);
void validate_list_element (struct list_elem * le);
void print_buffer_cache_block (struct block_cache_elem * bce);
struct list_elem *advance_clock_hand (void);

static thread_func periodic_write_thread; // (void);
static thread_func read_ahead_thread; // (void);

/* Array of all the block_cache_elem's */
static struct block_cache_elem block_cache_elems[MAX_CACHE_SECTORS];

/* Block cache buffer storage. */
static uint8_t block_cache[MAX_CACHE_SECTORS][BLOCK_SECTOR_SIZE];

/* List of cache blocks with no pending operations. */
struct list block_cache_active_queue;

/* List of empty blocks to be used for caching. */
struct list block_cache_unused_queue;

/* List of cache blocks waiting to be filled with data from disk. */
struct list block_cache_read_queue;

/* List of cache blocks used for the eviction algorithm. */
struct list block_cache_clock_elem_queue;

/* Block cache table. */
struct hash block_cache_table;

/* Condition when a read completes. */
struct condition cond_read;

/* Condition when a write completes. */
struct condition cond_write;

/* Condition when an element is ready for read-ahead. */
struct condition cond_read_queue_add;

/* Clock algorithm hand for eviction*/
static struct list_elem * clock_hand_le;

/* Thread periodically writes the buffer cache to disk in the background */
static void
periodic_write_thread (void *aux UNUSED)
{
  while (1)
    {
      timer_msleep (PERIODIC_WRITE_TIME_IN_SECONDS * 1000);
      block_cache_synchronize ();
    }
}

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
      cond_wait (&cond_read_queue_add, &block_cache_lock);
      
      // printf ("#");
      
      while (!list_empty (&block_cache_read_queue))
        {
          // printf ("!");
          
          list_elem = list_pop_front (&block_cache_read_queue);
          bce = list_entry (list_elem, struct block_cache_elem, list_elem);

          ASSERT (bce->state == BCM_READ);
          bce->state = BCM_READING;
          bce->pinned = true;
          
          lock_release (&block_cache_lock);
          block_read (fs_device, bce->sector, bce->block);
          lock_acquire (&block_cache_lock);

          /* Check that the block hasn't been read and/or written
             by another thread before overwriting with disk contents */
          ASSERT (bce->state == BCM_READING && bce->pinned);
          bce->state = BCM_ACTIVE;
          bce->pinned = false;
          list_push_back (&block_cache_active_queue, &bce->list_elem);
          validate_list_element (&bce->list_elem);
          
          cond_broadcast (&cond_read, &block_cache_lock);
        }        
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
  list_init (&block_cache_clock_elem_queue);
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
        
      if (bce && bce->state == BCM_WRITING) {
        cond_wait (&cond_write, block_cache_lock);
      }
      /* Effectively checking for bce && bce->state == BCM_READING */
      else if (bce && bce->pinned) { 
        cond_wait (&cond_read, block_cache_lock);
      }
      else {
        break;
      }
    } while (true);

  if (bce)
    {
      ASSERT (bce->magic == BLOCK_CACHE_ELEM_MAGIC);
    }
  
  return bce;
}

/* Advances the clock hand, or return NULL if the next and current element are the same. */
struct list_elem *
advance_clock_hand ()
{
  struct list_elem * prev_le = NULL;
  
  if (!clock_hand_le && !list_empty (&block_cache_clock_elem_queue))
    clock_hand_le = list_begin (&block_cache_clock_elem_queue);
    
  prev_le = clock_hand_le;
  // printf("(%2x->%2x),", (uint32_t)clock_hand_le,  (uint32_t)(clock_hand_le->next));
  // printf ("(%#2x)", (uint32_t)clock_hand_le);
  
  if (clock_hand_le)
    {
      ASSERT (clock_hand_le);
      ASSERT (!list_empty (&block_cache_clock_elem_queue));

      /* Advance the clock hand */
      if (clock_hand_le == list_back (&block_cache_clock_elem_queue))
        clock_hand_le = list_begin (&block_cache_clock_elem_queue);
      else
        clock_hand_le = list_next (clock_hand_le);
    
      if (clock_hand_le == prev_le)
        clock_hand_le = NULL;
    }
    
    // printf("(%2x->%2x),", (uint32_t)clock_hand_le,  (uint32_t)(clock_hand_le->next));
    
  return clock_hand_le;
}

//!! Update notes
/* Evicts a buffer cache element */
void
block_cache_evict (struct lock * block_cache_lock)
{
  
  struct block_cache_elem * bce = NULL;
  struct list_elem * e = NULL;
  
        // printf("(%2x->%2x),", (uint32_t)clock_hand_le,  (uint32_t)((*clock_hand_le).next));

  // printf ("(%#2x)", (uint32_t)clock_hand_le);
  //!! add eviction lock to prevent multiple threads from trying to evict (on cond_wait)
  //!! if global == null, assign e advance_ch
  // if (!clock_hand_le)
  //   {
      e = advance_clock_hand ();
    //   printf ("tick\n");
    // }
    // else printf ("tock\n");
      // printf ("(%#2x)", (uint32_t)clock_hand_le);

  while (true)
    { 
      //!! cond_wait if we get back to the same place (NULL?)
      // printf ("@");
      // printf ("(%#2x)", (uint32_t)clock_hand_le);
      // 
      // debug_validate_list (&block_cache_clock_elem_queue);
      // debug_print_list (&block_cache_clock_elem_queue);
      
      if (!e)
        {
          ASSERT (e);
          //!!!!!! This will break.  Allows another thread to move clock handle
          // printf ("w");
          cond_wait (&cond_read, block_cache_lock);
          e = advance_clock_hand ();
          continue;
        }
      else
        {
          bce = list_entry (e, struct block_cache_elem, clock_elem);

          ASSERT (bce->state != BCM_EVICTED && bce->state != BCM_UNUSED)
          if (bce->pinned)
            {
              // printf ("p");
              e = advance_clock_hand ();
              continue;
            }
        
          if (bce->accessed)
            {
              // printf ("a");
              bce->accessed = false;
              e = advance_clock_hand ();
              continue;
            }
      
          // printf ("x");
          // printf ("[%#2x {p=%#2x}] (%d)\n", (uint32_t)(&bce->list_elem), (uint32_t)((bce->list_elem).next), bce->sector);
      
      
        /* Advance the hand once more before editing the list */
        e = advance_clock_hand ();
      
        if (!(bce->state == BCM_ACTIVE || bce->state == BCM_READ))
          printf ("bce->state = %#2x\n", bce->state); 
        ASSERT (bce->state == BCM_ACTIVE || bce->state == BCM_READ);
      
      
        inode_clear_data (bce->sector);
        
        /* Evicts the buffer cache element */
        list_remove (&bce->list_elem);
        list_remove (&bce->clock_elem);
  
        /* Write the block back to disk */
        if (bce->dirty)
          {
            bce->pinned = true;
            bce->state = BCM_WRITING;
            lock_release (block_cache_lock);
            block_write (fs_device, bce->sector, bce->block);
            lock_acquire (block_cache_lock);
            ASSERT (bce->state == BCM_WRITING);
            bce->pinned = false;
          }
        
          printf ("@");
        
  
        /* Once done evicting, place the block on the unused queue */
        hash_delete (&block_cache_table, &bce->hash_elem);
        bce->state = BCM_EVICTED;
        list_push_back (&block_cache_unused_queue, &bce->list_elem);
        validate_list_element (&bce->list_elem);
      
        debug_validate_list (&block_cache_active_queue);
        debug_validate_list (&block_cache_unused_queue);
      
            
        break;
      }
    }
    
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
      
  /* Make sure that the block is not in the cache (from before or another thread) */
  if (bce)
    {
      /* In case it was found on the read-ahead queue, go ahead and move it
         to the active queue */
      if (!read_ahead && bce->state == BCM_READ && !bce->pinned)
        {
          validate_list_element (&bce->list_elem);
          list_remove (&bce->list_elem);
          list_push_back (&block_cache_active_queue, &bce->list_elem);
        }
      /* Otherwise simply return the pre-existing element for the sector */
    }
  else if (!bce)
    {
      /* Set up new buffer cache element */
      struct list_elem * list_elem = NULL;      
      list_elem = list_pop_front (&block_cache_unused_queue);
      bce = list_entry (list_elem, struct block_cache_elem, list_elem);
      bce->dirty = false;
      bce->sector = sector;
      bce->pinned = false;
      hash_insert (&block_cache_table, &bce->hash_elem);
      
      if (read_ahead)
        {
          bce->state = BCM_READ;
          list_push_back (&block_cache_read_queue, &bce->list_elem);
          debug_validate_list (&block_cache_read_queue);
          cond_broadcast (&cond_read_queue_add, block_cache_lock);
        }
      else
        {
          bce->state = BCM_READ;
          list_push_back (&block_cache_active_queue, &bce->list_elem);
        }
        
        list_push_back (&block_cache_clock_elem_queue, &bce->clock_elem);
    }
    
  validate_list_element (&bce->list_elem);
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);


  ASSERT (bce);
  ASSERT (bce->magic == BLOCK_CACHE_ELEM_MAGIC);
      
  return bce;
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
  
  /* Pins the buffer cache element before possibly losing the lock when
     adding the next sector into the read-ahead queue */
  bool did_pin = false;
  if (!bce->pinned)
    {
      did_pin = true;
      bce->pinned = true;
    }
  
  /* Starts reading the next sector on a background thread */
  if (valid_sector (fs_device, sector + 1))
    bce_next = block_cache_add_internal (sector + 1, block_cache_lock, true);  
  
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
  
  if (bce->sector != sector) {
    debug_print_list (&block_cache_active_queue);
    debug_print_list (&block_cache_unused_queue);
    debug_print_list (&block_cache_read_queue);
  }

  ASSERT (bce->magic == BLOCK_CACHE_ELEM_MAGIC);
  ASSERT (bce->sector == sector);

  if (did_pin)
    bce->pinned = false;
    
  bce->accessed = true;
  inode_update_data (bce->sector, bce);
  
  
  return bce;
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
  debug_validate_list (&block_cache_read_queue);

  validate_list_element (&bce->list_elem);
  list_remove (&bce->list_elem);
  bce->state = BCM_ACTIVE;
  list_push_back (&block_cache_active_queue, &bce->list_elem);  
  validate_list_element (&bce->list_elem);
  debug_validate_list (&block_cache_active_queue);
  debug_validate_list (&block_cache_unused_queue);
  debug_validate_list (&block_cache_read_queue);
  
  lock_release (&block_cache_lock);
  
  return bce;
}

/* Writes the provided buffer into the buffer cache */
struct block_cache_elem *
buffer_cache_write (struct block *fs_device UNUSED, block_sector_t sector_idx, const void *buffer)
{
  return buffer_cache_write_ofs (fs_device, sector_idx, 0, buffer, BLOCK_SECTOR_SIZE);  
}

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
            
      bce->state = BCM_READING;
      bce->pinned = true;
      
      lock_release (&block_cache_lock);
      block_read (fs_device, bce->sector, bce->block);
      lock_acquire (&block_cache_lock);

      /* Check that the block hasn't been read and/or written
         by another thread before overwriting with disk contents */
      ASSERT (bce->state == BCM_READING && bce->pinned);
      
      validate_list_element (&bce->list_elem);
      /* Removed from active/read_ahead queue and moved to the active */
      list_remove (&bce->list_elem);
      bce->state = BCM_ACTIVE;
      bce->pinned = false;
      list_push_back (&block_cache_active_queue, &bce->list_elem);  
      validate_list_element (&bce->list_elem);   
      
      debug_validate_list (&block_cache_active_queue);
      debug_validate_list (&block_cache_unused_queue);
           
    }  

  if (buffer)
    memcpy (buffer, bce->block + sector_ofs, chunk_size);
    
  cond_broadcast (&cond_read, &block_cache_lock);
  lock_release (&block_cache_lock);
  
  return bce;
}

/* Reads the sector from the buffer cache into the caller's buffer */
struct block_cache_elem *
buffer_cache_read (struct block *fs_device, block_sector_t sector_idx, void *buffer)
{
  return buffer_cache_read_ofs (fs_device, sector_idx, 0, buffer, BLOCK_SECTOR_SIZE);
}

/* Saves the buffer cache to disk asynchronously */
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
          else if (bce->dirty && !bce->pinned)
            {
              
              /* Write the block back to disk */
              bce->state = BCM_WRITING;
              bce->pinned = true;
              lock_release (&block_cache_lock);
              block_write (fs_device, bce->sector, bce->block);
              lock_acquire (&block_cache_lock);
              ASSERT (bce->state == BCM_WRITING);
              bce->pinned = false;
              bce->state = BCM_ACTIVE;
            
              debug_validate_list (&block_cache_active_queue);
              debug_validate_list (&block_cache_unused_queue);
              debug_validate_list (&block_cache_read_queue);              
              validate_list_element (&bce->list_elem);
            }
        }
        
    } while (list_elem);
    
  lock_release (&block_cache_lock);
}

struct block_cache_elem *
buffer_cache_read_inode (struct block *fs_device, block_sector_t sector)
{
  // struct block_cache_elem * bce = NULL;
  // void *block = NULL;
  // 
  // bce = buffer_cache_read (fs_device, sector, NULL);
  // if (bce) 
  //   {
  //     block = bce->block;
  //     // inode_validate_data (sector);
  //   }
  //   
  //   
  // return block;
  struct block_cache_elem * bce = NULL;
  void *block = NULL;
  
  bce = buffer_cache_read (fs_device, sector, NULL);  
  ASSERT (bce);
    
  return bce;

}


/* Debug function to display information about a buffer cache element */
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

/* Ensures the element is in a list */
void
validate_list_element (struct list_elem * le)
{
  ASSERT (le->next->prev == le);
  ASSERT (le->prev->next == le);
}
