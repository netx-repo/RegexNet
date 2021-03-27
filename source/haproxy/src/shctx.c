/*
 * shctx.c - shared context management functions for SSL
 *
 * Copyright (C) 2011-2012 EXCELIANCE
 *
 * Author: Emeric Brun - emeric@exceliance.fr
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <sys/mman.h>
#include <arpa/inet.h>
#include <ebmbtree.h>
#include <types/global.h>
#include <common/mini-clist.h>
#include "proto/shctx.h"

#if !defined (USE_PRIVATE_CACHE)

int use_shared_mem = 0;

#endif

/*
 * Reserve a row, put it in the hotlist, set the refcount to 1
 *
 * Reserve blocks in the avail list and put them in the hot list
 * Return the first block put in the hot list or NULL if not enough blocks available
 */
struct shared_block *shctx_row_reserve_hot(struct shared_context *shctx, int data_len)
{
	struct shared_block *block, *sblock, *ret = NULL, *first;
	int enough = 0;
	int freed = 0;

	/* not enough usable blocks */
	if (data_len > shctx->nbav * shctx->block_size)
		goto out;

	while (!enough && !LIST_ISEMPTY(&shctx->avail)) {
		int count = 0;
		int first_count = 0, first_len = 0;

		first = block = LIST_NEXT(&shctx->avail, struct shared_block *, list);
		if (ret == NULL)
			ret = first;

		first_count = first->block_count;
		first_len = first->len;
		/*
		Should never been set to 0.
		if (first->block_count == 0)
		first->block_count = 1;
		*/

		list_for_each_entry_safe_from(block, sblock, &shctx->avail, list) {

			/* release callback */
			if (first_len && shctx->free_block)
				shctx->free_block(first, block);

			block->block_count = 1;
			block->len = 0;

			freed++;
			data_len -= shctx->block_size;

			if (data_len > 0)
				shctx_block_set_hot(shctx, block);

			if (data_len <= 0 && !enough) {
				shctx_block_set_hot(shctx, block);
				ret->block_count = freed;
				ret->refcount = 1;
				enough = 1;
			}

			count++;
			if (count >= first_count)
				break;
		}
	}

out:
	return ret;
}

/*
 * if the refcount is 0 move the row to the hot list. Increment the refcount
 */
void shctx_row_inc_hot(struct shared_context *shctx, struct shared_block *first)
{
	struct shared_block *block, *sblock;
	int count = 0;

	if (first->refcount <= 0) {

		block = first;

		list_for_each_entry_safe_from(block, sblock, &shctx->avail, list) {

			shctx_block_set_hot(shctx, block);

			count++;
			if (count >= first->block_count)
				break;
		}
	}

	first->refcount++;
}

/*
 * decrement the refcount and move the row at the end of the avail list if it reaches 0.
 */
void shctx_row_dec_hot(struct shared_context *shctx, struct shared_block *first)
{
	struct shared_block *block, *sblock;
	int count = 0;

	first->refcount--;

	if (first->refcount <= 0) {

		block = first;

		list_for_each_entry_safe_from(block, sblock, &shctx->hot, list) {

			shctx_block_set_avail(shctx, block);

			count++;
			if (count >= first->block_count)
				break;
		}
	}

}


/*
 * Append data in the row if there is enough space.
 * The row should be in the hot list
 *
 * Return the amount of appended data if ret >= 0
 * or how much more space it needs to contains the data if < 0.
 */
int shctx_row_data_append(struct shared_context *shctx, struct shared_block *first, unsigned char *data, int len)
{
	int remain, start;
	int count = 0;
	struct shared_block *block;


	/* return -<len> needed to work */
	if (len > first->block_count * shctx->block_size - first->len)
		return (first->block_count * shctx->block_size - first->len) - len;

	/* skipping full buffers, stop at the first buffer with remaining space */
	block = first;
	list_for_each_entry_from(block, &shctx->hot, list) {
		count++;


		/* break if there is not enough blocks */
		if (count > first->block_count)
			break;

		/* end of copy */
		if (len <= 0)
			break;

		/* skip full buffers */
		if (count * shctx->block_size <= first->len)
			continue;

		/* remaining space in the current block which is not full */
		remain = (shctx->block_size * count - first->len) % shctx->block_size;
		/* if remain == 0, previous buffer are full, or first->len == 0 */
		remain = remain ? remain : shctx->block_size;

		/* start must be calculated before remain is modified */
		start = shctx->block_size - remain;

		/* must not try to copy more than len */
		remain = MIN(remain, len);

		memcpy(block->data + start, data, remain);
		data += remain;
		len -= remain;
		first->len += remain; /* update len in the head of the row */
	}

	return len;
}

/*
 * Copy <len> data from a row of blocks, return the remaining data to copy
 * If 0 is returned, the full data has successfuly be copied
 *
 * The row should be in the hot list
 */
int shctx_row_data_get(struct shared_context *shctx, struct shared_block *first,
                       unsigned char *dst, int offset, int len)
{
	int count = 0, size = 0, start = -1;
	struct shared_block *block;

	/* can't copy more */
	if (len > first->len)
		len = first->len;

	block = first;
	count = 0;
	/* Pass through the blocks to copy them */
	list_for_each_entry_from(block, &shctx->hot, list) {
		if (count >= first->block_count  || len <= 0)
			break;

		count++;
		/* continue until we are in right block
		   corresponding to the offset */
		if (count < offset / shctx->block_size + 1)
			continue;

		/* on the first block, data won't possibly began at offset 0 */
		if (start == -1)
			start = offset - (count - 1) * shctx->block_size;

		/* size can be lower than a block when copying the last block */
		size = MIN(shctx->block_size - start, len);

		memcpy(dst, block->data + start, size);
		dst += size;
		len -= size;
		start = 0;
	}
	return len;
}

/* Allocate shared memory context.
 * <maxblocks> is maximum blocks.
 * If <maxblocks> is set to less or equal to 0, ssl cache is disabled.
 * Returns: -1 on alloc failure, <maxblocks> if it performs context alloc,
 * and 0 if cache is already allocated.
 */
int shctx_init(struct shared_context **orig_shctx, int maxblocks, int blocksize, int extra, int shared)
{
	int i;
	struct shared_context *shctx;
	int ret;
#ifndef USE_PRIVATE_CACHE
#ifdef USE_PTHREAD_PSHARED
	pthread_mutexattr_t attr;
#endif
#endif
	void *cur;
	int maptype = MAP_PRIVATE;

	if (maxblocks <= 0)
		return 0;

#ifndef USE_PRIVATE_CACHE
	if (shared)
		maptype = MAP_SHARED;
#endif

	shctx = (struct shared_context *)mmap(NULL, sizeof(struct shared_context) + extra + (maxblocks * (sizeof(struct shared_block) + blocksize)),
	                                      PROT_READ | PROT_WRITE, maptype | MAP_ANON, -1, 0);
	if (!shctx || shctx == MAP_FAILED) {
		shctx = NULL;
		ret = SHCTX_E_ALLOC_CACHE;
		goto err;
	}

	shctx->nbav = 0;

#ifndef USE_PRIVATE_CACHE
	if (maptype == MAP_SHARED) {
#ifdef USE_PTHREAD_PSHARED
		if (pthread_mutexattr_init(&attr)) {
			munmap(shctx, sizeof(struct shared_context) + extra + (maxblocks * (sizeof(struct shared_block) + blocksize)));
			shctx = NULL;
			ret = SHCTX_E_INIT_LOCK;
			goto err;
		}

		if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)) {
			pthread_mutexattr_destroy(&attr);
			munmap(shctx, sizeof(struct shared_context) + extra + (maxblocks * (sizeof(struct shared_block) + blocksize)));
			shctx = NULL;
			ret = SHCTX_E_INIT_LOCK;
			goto err;
		}

		if (pthread_mutex_init(&shctx->mutex, &attr)) {
			pthread_mutexattr_destroy(&attr);
			munmap(shctx, sizeof(struct shared_context) + extra + (maxblocks * (sizeof(struct shared_block) + blocksize)));
			shctx = NULL;
			ret = SHCTX_E_INIT_LOCK;
			goto err;
		}
#else
		shctx->waiters = 0;
#endif
		use_shared_mem = 1;
	}
#endif

	LIST_INIT(&shctx->avail);
	LIST_INIT(&shctx->hot);

	shctx->block_size = blocksize;

	/* init the free blocks after the shared context struct */
	cur = (void *)shctx + sizeof(struct shared_context) + extra;
	for (i = 0; i < maxblocks; i++) {
		struct shared_block *cur_block = (struct shared_block *)cur;
		cur_block->len = 0;
		cur_block->refcount = 0;
		cur_block->block_count = 1;
		LIST_ADDQ(&shctx->avail, &cur_block->list);
		shctx->nbav++;
		cur += sizeof(struct shared_block) + blocksize;
	}
	ret = maxblocks;

err:
	*orig_shctx = shctx;
	return ret;
}

