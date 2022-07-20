#ifndef CACHE_H
#define CACHE_H

#include <stdbool.h>
#include <string.h>
#include "devices/timer.h"
#include "devices/block.h"
#include "threads/synch.h"
#include "threads/malloc.h"

#define MAX_CACHE_SECTORS 64

// This is for each cache block within the cache
struct cache_block {
	bool dirty;
	bool free;
	struct block* block;
	block_sector_t sector;
	char buffer[BLOCK_SECTOR_SIZE];
	
	// NRU Cache
	int64_t last_touched;

	// Used to find next part in the list of blocks
	struct list_elem elem;
};

// This is for the actual cache
struct block_cache {
	// Synchronization
	struct lock lock;

	struct list cache_blocks;
};

/**
 * @brief Initializes the cache
 * 
 */
void block_cache_init(void);

void block_cache_read(struct block* block, block_sector_t sector, void* buffer);

void block_cache_read_offset(struct block* block, block_sector_t sector, void* buffer, int sector_ofs, int chunk_size);

void block_cache_write(struct block* block, block_sector_t sector, const void* buffer);

void block_cache_write_offset(struct block* block, block_sector_t sector, const void* buffer, int sector_ofs, int chunk_size);

/**
 * @brief Full flush of cache. All dirty blocks are written to the disk, this does not clear the cache
 * 
 */
void block_cache_fflush(void);
/**
 * @brief Frees the cache; prevents memory leaks from occuring
 * 
 */
void block_cache_free(void);
#endif