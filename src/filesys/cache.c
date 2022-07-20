#include "cache.h"

static struct block_cache* BLOCK_CACHE = NULL;

static bool cache_nru_sort(const struct list_elem* a_e, const struct list_elem* b_e, __attribute__((unused)) void* aux) {
	const struct cache_block* a = list_entry(a_e, struct cache_block, elem);
	const struct cache_block* b = list_entry(b_e, struct cache_block, elem);

	return a->last_touched < b->last_touched;
}

static struct cache_block* block_cache_nru_evict(void) {

	// Max gets the largest. If last touched is a small number then it is the oldest
	struct list_elem* e = list_min(&BLOCK_CACHE->cache_blocks, cache_nru_sort, NULL);

	struct cache_block* b = list_entry(e, struct cache_block, elem);

	if (!b->free && b->dirty) block_write(b->block, b->sector, b->buffer);
	b->free = true;
	b->dirty = false;

	return b;
}

void block_cache_init(void) {
	BLOCK_CACHE = malloc(sizeof(struct block_cache));
	lock_init(&BLOCK_CACHE->lock);
	list_init(&BLOCK_CACHE->cache_blocks);
	for (int i = 0; i < MAX_CACHE_SECTORS; i++) {
		struct cache_block* b = (struct cache_block*) malloc(sizeof(struct cache_block));
		b->free = true;
		b->dirty = false;
		b->last_touched = timer_ticks();	// Increases as time goes on

		// Lowest in the list is the oldest
		list_push_back(&BLOCK_CACHE->cache_blocks, &b->elem);
	}
}

void block_cache_read_offset(struct block* block, block_sector_t sector, void* buffer, int sector_ofs, int chunk_size) {
	struct list_elem *e;

	ASSERT(chunk_size + sector_ofs <= BLOCK_SECTOR_SIZE);

	lock_acquire(&BLOCK_CACHE->lock);

	for (
		e = list_begin (&BLOCK_CACHE->cache_blocks);
		e != list_end (&BLOCK_CACHE->cache_blocks);
		e = list_next (e)
	)
	{
		struct cache_block* b = list_entry (e, struct cache_block, elem);
		if (!b->free && b->block == block && b->sector == sector) {
			memcpy(buffer, b->buffer + sector_ofs, chunk_size);
			b->last_touched = timer_ticks();
			
			list_sort(&BLOCK_CACHE->cache_blocks, cache_nru_sort, NULL);
			
			lock_release(&BLOCK_CACHE->lock);
			return;
		}
	}

	// NRU Eviction
	struct cache_block* b = block_cache_nru_evict();
	b->block = block;
	b->sector = sector;
	b->free = false;

	block_read(block, sector, b->buffer);
	memcpy(buffer, b->buffer + sector_ofs, chunk_size);

	b->last_touched = timer_ticks();

	list_sort(&BLOCK_CACHE->cache_blocks, cache_nru_sort, NULL);

	lock_release(&BLOCK_CACHE->lock);
}

void block_cache_read(struct block* block, block_sector_t sector, void* buffer) {
	block_cache_read_offset(block, sector, buffer, 0, BLOCK_SECTOR_SIZE);
}

void block_cache_write_offset(struct block* block, block_sector_t sector, const void* buffer, int sector_ofs, int chunk_size) {
	struct list_elem *e;

	ASSERT(chunk_size + sector_ofs <= BLOCK_SECTOR_SIZE);

	lock_acquire(&BLOCK_CACHE->lock);
	for (
		e = list_begin (&BLOCK_CACHE->cache_blocks);
		e != list_end (&BLOCK_CACHE->cache_blocks);
		e = list_next (e)
	)
	{
		struct cache_block* b = list_entry (e, struct cache_block, elem);
		if (!b->free && b->block == block && b->sector == sector) {
			memcpy(b->buffer + sector_ofs, buffer, chunk_size);
			
			b->dirty = true;
			b->last_touched = timer_ticks();

			list_sort(&BLOCK_CACHE->cache_blocks, cache_nru_sort, NULL);
			lock_release(&BLOCK_CACHE->lock);

			return;
		}
	}

	// NRU Eviction
	struct cache_block* b = block_cache_nru_evict();
	b->block = block;
	b->sector = sector;
	b->free = false;

	//read in data
	block_read(block, sector, b->buffer);

	memcpy(b->buffer + sector_ofs, buffer, chunk_size);
	block_write(block, sector, b->buffer);

	b->last_touched = timer_ticks();

	list_sort(&BLOCK_CACHE->cache_blocks, cache_nru_sort, NULL);

	lock_release(&BLOCK_CACHE->lock);
}

void block_cache_write(struct block* block, block_sector_t sector, const void* buffer) {
	block_cache_write_offset(block, sector, buffer, 0, BLOCK_SECTOR_SIZE);
}


void block_cache_fflush(void) {
	struct list_elem *e;

	lock_acquire(&BLOCK_CACHE->lock);

	for (
		e = list_begin (&BLOCK_CACHE->cache_blocks);
		e != list_end (&BLOCK_CACHE->cache_blocks);
		e = list_next (e)
	)
	{
		struct cache_block* b = list_entry (e, struct cache_block, elem);
		if (!b->free && b->dirty) {
			block_write(b->block, b->sector, b->buffer);
			b->dirty = false;
		}
	}

	lock_release(&BLOCK_CACHE->lock);
}

void block_cache_free(void) {
   while (!list_empty (&BLOCK_CACHE->cache_blocks))
		{
			struct list_elem *e = list_pop_front(&BLOCK_CACHE->cache_blocks);
			struct cache_block* b = list_entry(e, struct cache_block, elem);
			free(b);
		}
		free(BLOCK_CACHE);
}