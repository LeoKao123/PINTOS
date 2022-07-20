#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <stdio.h>
#include <stdlib.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  
  block_sector_t direct_pointer[12];
  block_sector_t indirect_pointer;
  block_sector_t doubly_indirect_pointer;
  enum inode_type type; /* Type */
  uint32_t unused[111]; //not used
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */

  /*cv for file_deny_write*/
  struct condition writer_cv;
  int writers;
  struct lock cv_lock;

  struct lock rwlock;

  struct lock variable_access_lock;
  struct lock resize_lock;
};

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos) {
  ASSERT(inode != NULL);
  struct inode_disk* data = (struct inode_disk*) malloc(sizeof(struct inode_disk));
  ASSERT(data != NULL);
  block_cache_read(fs_device, inode->sector, data);
  if (pos < data->length) {
    if (pos < 12 * BLOCK_SECTOR_SIZE) {
      //handle direct pointers
      block_sector_t answer = data->direct_pointer[pos / BLOCK_SECTOR_SIZE];
      free(data);
      return answer;
    } else if (pos < (12 + 128) * BLOCK_SECTOR_SIZE) {
      //handle indirect pointers
      block_sector_t buffer[128];
      uint32_t extra = pos - 12 * BLOCK_SECTOR_SIZE;
      block_cache_read(fs_device, data->indirect_pointer, buffer);
      free(data);
      return buffer[extra / BLOCK_SECTOR_SIZE];
    } else if (pos < (12 + 128 + 128 * 128) * BLOCK_SECTOR_SIZE) {
      uint32_t extra = pos - (12 + 128) * BLOCK_SECTOR_SIZE;
      block_sector_t buffer[128];
      block_cache_read(fs_device, data->doubly_indirect_pointer, buffer);
      block_sector_t indirect = buffer[extra / BLOCK_SECTOR_SIZE / 128];
      block_cache_read(fs_device, indirect, buffer);
      free(data);
      return buffer[extra / BLOCK_SECTOR_SIZE % 128];
    }
  }
  else {
    free(data);
    return -1;
  }
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
static struct lock open_inodes_lock;
static struct lock free_map_lock;

/* Initializes the inode module. */
void inode_init(void) { 
  list_init(&open_inodes);
  lock_init(&open_inodes_lock);
  lock_init(&free_map_lock);
}

/*Resizes an inode disk to inputted length. 
Returns True if successful 
Returns false if allocation fails inode should stay unchanged. 
Does not write the new length to disk.*/
bool inode_resize(struct inode_disk* disk_inode, off_t length) {
  block_sector_t sector;
  
  //segment for handling direct pointers
  for (int i = 0; i < 12; i++) {
    if (length <= BLOCK_SECTOR_SIZE * i && disk_inode->direct_pointer[i] != 0) {
      lock_acquire(&free_map_lock);
      free_map_release(disk_inode->direct_pointer[i], 1);
      lock_release(&free_map_lock);
      disk_inode->direct_pointer[i] = 0;
    }
    if (length > BLOCK_SECTOR_SIZE * i && disk_inode->direct_pointer[i] == 0) {
      lock_acquire(&free_map_lock);
      bool success = free_map_allocate(1, &sector);
      lock_release(&free_map_lock);
      if (!success) {
        inode_resize(disk_inode, disk_inode->length);
        return false;
      }
      static char zeros[BLOCK_SECTOR_SIZE];
      block_cache_write(fs_device, sector, zeros);
      disk_inode->direct_pointer[i] = sector;
    }
  }
  if (disk_inode->indirect_pointer == 0 && length <= 12 * BLOCK_SECTOR_SIZE) {
    disk_inode->length = length;
    return true;
  }

  //segment for handling indirect pointers
  block_sector_t buffer[128];
  if (disk_inode->indirect_pointer == 0) {
    memset(buffer, 0, BLOCK_SECTOR_SIZE);
    lock_acquire(&free_map_lock);
    bool success = free_map_allocate(1, &sector);
    
    lock_release(&free_map_lock);
    if (!success) {
      inode_resize(disk_inode, disk_inode->length);
      return false;
    }
    static char zeros[BLOCK_SECTOR_SIZE];
    block_cache_write(fs_device, sector, zeros);
    disk_inode->indirect_pointer = sector;
  } else {
    block_cache_read(fs_device, disk_inode->indirect_pointer, buffer);
  }
  for (int i = 0; i < 128; i++) {
    if (length <= (12 + i) * BLOCK_SECTOR_SIZE && buffer[i] != 0) {
      lock_acquire(&free_map_lock);
      free_map_release(buffer[i], 1);
      lock_release(&free_map_lock);
      buffer[i] = 0;
    }
    if (length > (12 + i) * BLOCK_SECTOR_SIZE && buffer[i] == 0) {
      lock_acquire(&free_map_lock);
      bool success = free_map_allocate(1, &sector);
      lock_release(&free_map_lock);
      if (!success) {
        inode_resize(disk_inode, disk_inode->length);
        return false;
      }
      static char zeros[BLOCK_SECTOR_SIZE];
      block_cache_write(fs_device,sector, zeros);
      buffer[i]= sector;
    }
  }
  if (disk_inode->indirect_pointer != 0 && length <= 12 * 512) {
    lock_acquire(&free_map_lock);
    free_map_release(disk_inode->indirect_pointer, 1);
    lock_release(&free_map_lock);
    disk_inode->indirect_pointer = 0;
  } else {
    block_cache_write(fs_device, disk_inode->indirect_pointer, buffer);
  }
  if (disk_inode->doubly_indirect_pointer == 0 
        && length <= (12 + 128) * BLOCK_SECTOR_SIZE) {
    disk_inode->length = length;
    return true;
  }

  //segment for handling doubly indirect pointers
  memset(buffer, 0, BLOCK_SECTOR_SIZE); //reset buffer
  //buffer will now hold the doubly indirect pointer data if there was any
  //either allocate a pointer now or read in the existing pointers data
  if (disk_inode->doubly_indirect_pointer == 0) {
    lock_acquire(&free_map_lock);
    bool success = free_map_allocate(1, &sector);
    lock_release(&free_map_lock);
    if (!success) {
      inode_resize(disk_inode, disk_inode->length);
      return false;
    }
    static char zeros[BLOCK_SECTOR_SIZE];
    block_cache_write(fs_device, sector, zeros);
    disk_inode->doubly_indirect_pointer = sector;
  } else {
    block_cache_read(fs_device, disk_inode->doubly_indirect_pointer, buffer);
  }
  //for loop to go over every entry within the doubly indirect pointer
  for (int i = 0; i < 128; i++) {
    //if the pointer does not exist and the size is smaller than what byte we
    //are currently on, then break out of the loop
    if (buffer[i] == 0 && length <= (12 + 128 + 128 * i) * BLOCK_SECTOR_SIZE) {
      break;
    }
    //allocate special stack buffer just for the indirect pointers within
    block_sector_t inside_buffer[128];
    memset(inside_buffer, 0, BLOCK_SECTOR_SIZE);
    //either read in data, or create a new indirect pointer if there isnt one
    if (buffer[i] == 0) {
      lock_acquire(&free_map_lock);
      bool success = free_map_allocate(1, &sector);
      lock_release(&free_map_lock);
      if (!success) {
        inode_resize(disk_inode, disk_inode->length);
        return false;
      }
      static char zeros[BLOCK_SECTOR_SIZE];
      block_cache_write(fs_device, sector, zeros);
      buffer[i] = sector;
    } else {
      block_cache_read(fs_device, buffer[i], inside_buffer);
    }
    //do indirect pointer loop
    for (int j = 0; j < 128; j++) {
      //if size <= (12 + 128 + 128*i + i) * 512 and pointer exists
      //free that pointer and set to 0 (shrink)
      if (length <= (12 + 128 + 128*i + j) * BLOCK_SECTOR_SIZE && inside_buffer[j] != 0) {
        lock_acquire(&free_map_lock);
        free_map_release(inside_buffer[j], 1);
        lock_release(&free_map_lock);
        inside_buffer[j] = 0;
      }
      //otherwise grow by allocating more space and setting to inside buffer
      if (length > (12 + 128 + 128*i + j) * BLOCK_SECTOR_SIZE && inside_buffer[j] == 0) {
        lock_acquire(&free_map_lock);
        bool success = free_map_allocate(1, &sector);
        lock_release(&free_map_lock);
        if (!success) {
          inode_resize(disk_inode, disk_inode->length);
          return false;
        }
        static char zeros[BLOCK_SECTOR_SIZE];
        block_cache_write(fs_device, sector, zeros);
        inside_buffer[j]= sector;
      }
    }
    //if indirect pointer exists and the size is less, free/zero indirect pointer
    if (buffer[i] != 0 && length <= (12 + 128 + 128 * i) * BLOCK_SECTOR_SIZE) {
      lock_acquire(&free_map_lock);
      free_map_release(buffer[i], 1);
      lock_release(&free_map_lock);
      buffer[i] = 0;
    } else {
      //else put buffer data about direct pointers into indirect pointer(which we
      //can grab from higher level buffer)
      block_cache_write(fs_device, buffer[i], inside_buffer);
    }
  }
  if (disk_inode->doubly_indirect_pointer != 0 && length <= (12 + 128) * BLOCK_SECTOR_SIZE) {
    lock_acquire(&free_map_lock);
    free_map_release(disk_inode->doubly_indirect_pointer, 1);
    lock_release(&free_map_lock);
    disk_inode->doubly_indirect_pointer = 0;
  } else {
    block_cache_write(fs_device, disk_inode->doubly_indirect_pointer, buffer);
  }
  //end everything cleanly
  disk_inode->length = length;
  return true;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length, enum inode_type type) {
  struct inode_disk* disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = 0;
    disk_inode->magic = INODE_MAGIC;
    disk_inode->type = type;
    for (int i = 0; i < 12; i++) {
      disk_inode->direct_pointer[i] = 0; 
    }
    disk_inode->indirect_pointer = 0;
    disk_inode->doubly_indirect_pointer = 0;

    //resize inode to the correct size (maybe? Fill with 0)
    success = inode_resize(disk_inode, length);
    block_cache_write(fs_device, sector, disk_inode);
    
    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
  struct list_elem* e;
  struct inode* inode;

  /* Check whether this inode is already open. */
  lock_acquire(&open_inodes_lock);
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      lock_release(&open_inodes_lock);
      inode_reopen(inode);
      return inode;
    }
  }
  lock_release(&open_inodes_lock);

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  lock_acquire(&open_inodes_lock);
  list_push_front(&open_inodes, &inode->elem);
  lock_release(&open_inodes_lock);

  lock_init(&inode->variable_access_lock);
  lock_init(&inode->resize_lock);
  lock_init(&inode->rwlock);

  cond_init(&inode->writer_cv);
  lock_init(&inode->cv_lock);

  lock_acquire(&inode->resize_lock);
  inode->writers = 0;
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_release(&inode->resize_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL)
    lock_acquire(&inode->resize_lock);
    inode->open_cnt++;
    lock_release(&inode->resize_lock);
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) {
  lock_acquire(&inode->resize_lock);
  int sector = inode->sector; 
  lock_release(&inode->resize_lock);
  return sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire(&inode->resize_lock);
  --inode->open_cnt;
  lock_release(&inode->resize_lock);
  /* Release resources if this was the last opener. */
  if (inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    lock_acquire(&open_inodes_lock);
    list_remove(&inode->elem);
    lock_release(&open_inodes_lock);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      //resize inodedisk to 0 to deallocate all data
      struct inode_disk* disk_inode = (struct inode_disk*) malloc(sizeof(struct inode_disk));
      ASSERT(disk_inode != NULL);
      block_cache_read(fs_device, inode->sector, disk_inode);

      lock_acquire(&inode->resize_lock);
      inode_resize(disk_inode, 0);
      lock_release(&inode->resize_lock);

      //free the actual inode_disk
      lock_acquire(&free_map_lock);
      free_map_release(inode->sector, 1);
      lock_release(&free_map_lock);
      free(disk_inode);
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  lock_acquire(&inode->resize_lock);
  inode->removed = true;
  lock_release(&inode->resize_lock);

}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;

  lock_acquire(&inode->rwlock);
  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      block_cache_read(fs_device, sector_idx, buffer + bytes_read);
    } else {
      /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
      block_cache_read_offset(fs_device, sector_idx, buffer + bytes_read, sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  lock_release(&inode->rwlock);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;
  
  if (inode->deny_write_cnt)
    return 0;

  lock_acquire(&inode->rwlock);
  struct inode_disk* disk_inode = (struct inode_disk*) malloc(sizeof(struct inode_disk));
  if (disk_inode == NULL) {
    bytes_written = 1;
  }
  ASSERT(disk_inode != NULL);
  block_cache_read(fs_device, inode->sector, disk_inode);
  if (disk_inode->length < size + offset) {
    lock_acquire(&inode->resize_lock);
    inode_resize(disk_inode, size + offset);
    block_cache_write(fs_device, inode->sector, disk_inode);
    lock_release(&inode->resize_lock);
  }
  free(disk_inode);

  
  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Write full sector directly to disk. */
      block_cache_write(fs_device, sector_idx, buffer + bytes_written);
    } else {
      /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
      block_cache_write_offset(fs_device, sector_idx, buffer + bytes_written, sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  lock_release(&inode->rwlock);
  
  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  lock_acquire(&inode->resize_lock);
  inode->deny_write_cnt++;
  lock_release(&inode->resize_lock);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  lock_acquire(&inode->resize_lock);
  inode->deny_write_cnt--;
  lock_release(&inode->resize_lock);
  
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) {
  struct inode_disk* disk_inode = (struct inode_disk*) malloc(sizeof(struct inode_disk));
  ASSERT(disk_inode != NULL);
  block_cache_read(fs_device, inode->sector, disk_inode);
  off_t length = disk_inode->length;
  free(disk_inode);
  return length;
}

enum inode_type inode_type(const struct inode* inode) {
  ASSERT(inode != NULL);
  struct inode_disk* disk_inode = (struct inode_disk*) malloc(sizeof(struct inode_disk));
  ASSERT(disk_inode != NULL);
  block_cache_read(fs_device, inode->sector, disk_inode);

  enum inode_type type = disk_inode->type;

  free(disk_inode);
  return type;
}

int inode_open_cnt(const struct inode* inode) {
  return inode->open_cnt;
}

