// SPDX-License-Identifier: BSD-3-Clause

#include "osmem.h"
#include <sys/mman.h>
#include <unistd.h>
#include <string.h>
#include "block_meta.h"

#define ALIGNMENT 8
#define MMAP_THRESHOLD (128 * 1024)
#define PREALLOCATE_SIZE (128 * 1024)
#define META_SIZE (sizeof(struct block_meta))

static struct block_meta *heap_start;

size_t align_size(size_t size)
{
	return (size + (ALIGNMENT - 1)) / ALIGNMENT * ALIGNMENT;
}

void check_heap_start(size_t size)
{
	size_t block_size = size + META_SIZE;
	int use_mmap = block_size >= MMAP_THRESHOLD ? 1 : 0;

	if (heap_start == NULL && !use_mmap) {
		heap_start = (struct block_meta *)sbrk(PREALLOCATE_SIZE);
		if (heap_start == (void *)-1)
			return;
		heap_start->size = PREALLOCATE_SIZE - META_SIZE;
		heap_start->status = STATUS_FREE;
		heap_start->prev = NULL;
		heap_start->next = NULL;
	}
}

struct block_meta *find_best_free_block(size_t size)
{
	struct block_meta *best_block = NULL;
	struct block_meta *current_block = heap_start;
	size_t best_size = (size_t)-1;

	while (current_block != NULL) {
		if (current_block->status == STATUS_FREE && current_block->size >= size && current_block->size < best_size) {
			best_block = current_block;
			best_size = current_block->size;
		}
		current_block = current_block->next;
	}

	// if best_block is NULL, check if last block is free and can be expanded
	if (best_block == NULL && size + META_SIZE < MMAP_THRESHOLD) {
		current_block = heap_start;
		while (current_block && current_block->next)
			current_block = current_block->next;

		if (current_block && current_block->status == STATUS_FREE) {
			size_t remaining_size = size - current_block->size;
			struct block_meta *new_block = (struct block_meta *)sbrk(remaining_size);

			if (new_block == (void *)-1)
				return NULL;
			current_block->size += remaining_size;
			current_block->status = STATUS_ALLOC;
			best_block = current_block;
		}
	}

	return best_block;
}

void split_block(struct block_meta *block, size_t size)
{
	size_t remaining_size = block->size - size;

	if (remaining_size >= META_SIZE + ALIGNMENT) {
		struct block_meta *new_block = (struct block_meta *)((char *)block + META_SIZE + size);

		new_block->size = remaining_size - META_SIZE;
		new_block->status = STATUS_FREE;
		new_block->prev = block;
		new_block->next = block->next;

		block->size = size;
		block->next = new_block;

		if (new_block->next != NULL)
			new_block->next->prev = new_block;
	}
	block->status = STATUS_ALLOC;
}

struct block_meta *create_block(size_t size, int use_mmap)
{
	struct block_meta *block;
	size_t block_size = size + META_SIZE;

	if (use_mmap) {
		block = mmap(NULL, block_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

		if (block == MAP_FAILED)
			return NULL;

		block->status = STATUS_MAPPED;
		block->prev = NULL;
		block->next = NULL;
		block->size = size;
	} else {
		struct block_meta *last_block = heap_start;

		while (last_block && last_block->next)
			last_block = last_block->next;

		block = (struct block_meta *)sbrk(block_size);
		if (block == (void *)-1)
			return NULL;

		block->status = STATUS_ALLOC;

		// add block to the end of the list
		if (last_block)
			last_block->next = block;
		block->prev = last_block;
		block->next = NULL;
		block->size = size;
	}

	return block;
}

void coalesce_blocks(void)
{
	struct block_meta *current_block = heap_start;

	while (current_block && current_block->next) {
		if (current_block->status == STATUS_FREE && current_block->next->status == STATUS_FREE) {
			current_block->size += current_block->next->size + META_SIZE;
			current_block->next = current_block->next->next;

			if (current_block->next)
				current_block->next->prev = current_block;
			// does not move to the next block
		} else {
			current_block = current_block->next;
		}
	}
}

void *os_malloc(size_t size)
{
	if (size == 0)
		return NULL;

	size = align_size(size);
	check_heap_start(size);
	struct block_meta *block = find_best_free_block(size);

	if (block != NULL) {
		split_block(block, size);
	} else {
		int use_mmap = (size + META_SIZE) >= MMAP_THRESHOLD ? 1 : 0;

		block = create_block(size, use_mmap);
		if (block == NULL)
			return NULL;
	}

	return (void *)((char *)block + META_SIZE);
}

void os_free(void *ptr)
{
	if (ptr == NULL)
		return;

	struct block_meta *block = (struct block_meta *)((char *)ptr - META_SIZE);

	if (block->status == STATUS_MAPPED) {
		munmap(block, block->size + META_SIZE);
	} else {
		block->status = STATUS_FREE;
		coalesce_blocks();
	}
}

void *os_calloc(size_t nmemb, size_t size)
{
	if (nmemb == 0 || size == 0)
		return NULL;

	size_t page_size = getpagesize(); // if size is bigger than page size, use mmap
	size_t total_size = nmemb * size;

	total_size = align_size(total_size);
	int use_mmap = (total_size + META_SIZE) >= page_size ? 1 : 0;
	struct block_meta *block = NULL;

	if (!use_mmap) {
		check_heap_start(total_size);
		block = find_best_free_block(total_size);
	}

	if (block != NULL) {
		split_block(block, total_size);
	} else {
		block = create_block(total_size, use_mmap);
		if (block == NULL)
			return NULL;
	}

	void *ptr = (void *)((char *)block + META_SIZE);

	if (ptr != NULL)
		memset(ptr, 0, total_size);

	return ptr;
}

int coalesce_blocks_for_realloc(struct block_meta *block, size_t size, int can_expand)
{
	struct block_meta *current_block = block;

	while (current_block->next && current_block->next->status == STATUS_FREE) {
		current_block->size += current_block->next->size + META_SIZE;
		current_block->next = current_block->next->next;

		if (current_block->next)
			current_block->next->prev = current_block;

		if (current_block->size >= size)
			return 1;
	}

	if (current_block->next == NULL && can_expand) {
		size_t remaining_size = size - current_block->size;
		struct block_meta *new_block = (struct block_meta *)sbrk(remaining_size);

		if (new_block == (void *)-1)
			return 0;

		current_block->size += remaining_size;
		current_block->status = STATUS_ALLOC;
		return 1;
	}

	return 0;
}

void *os_realloc(void *ptr, size_t size)
{
	if (!ptr)
		return os_malloc(size);

	if (size == 0) {
		os_free(ptr);
		return NULL;
	}

	struct block_meta *block = (struct block_meta *)((char *)ptr - META_SIZE);
	size_t old_size = block->size;

	size = align_size(size);

	if (block->status == STATUS_FREE)
		return NULL;

	if (size <= old_size) {
		// if block is mapped and size is smaller than MMAP_THRESHOLD, unmap it and allocate a new block
		if (block->status == STATUS_MAPPED) {
			void *new_ptr = os_malloc(size);

			if (!new_ptr)
				return NULL;

			memcpy(new_ptr, ptr, size);
			munmap(block, old_size + META_SIZE);
			return new_ptr;
		}

		split_block(block, size);
		return ptr;
	}

	if (size + META_SIZE >= MMAP_THRESHOLD) {
		void *new_ptr = os_malloc(size);

		if (!new_ptr)
			return NULL;

		memcpy(new_ptr, ptr, old_size);
		os_free(ptr);
		return new_ptr;
	}

	int can_expand = block->next == NULL ? 1 : 0;
	int expanded = coalesce_blocks_for_realloc(block, size, can_expand);

	if (expanded) {
		split_block(block, size);
		return ptr;
	}
	void *new_ptr = os_malloc(size);

	if (!new_ptr)
		return NULL;

	memcpy(new_ptr, ptr, old_size);
	os_free(ptr);
	return new_ptr;
}
