/*
 * Channel management functions.
 *
 * Copyright 2000-2014 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <common/config.h>
#include <common/buffer.h>

#include <proto/channel.h>


/* Schedule up to <bytes> more bytes to be forwarded via the channel without
 * notifying the owner task. Any data pending in the buffer are scheduled to be
 * sent as well, within the limit of the number of bytes to forward. This must
 * be the only method to use to schedule bytes to be forwarded. If the requested
 * number is too large, it is automatically adjusted. The number of bytes taken
 * into account is returned. Directly touching ->to_forward will cause lockups
 * when buf->o goes down to zero if nobody is ready to push the remaining data.
 */
unsigned long long __channel_forward(struct channel *chn, unsigned long long bytes)
{
	unsigned int budget;
	unsigned int forwarded;

	/* This is more of a safety measure as it's not supposed to happen in
	 * regular code paths.
	 */
	if (unlikely(chn->to_forward == CHN_INFINITE_FORWARD)) {
		b_adv(chn->buf, chn->buf->i);
		return bytes;
	}

	/* Bound the transferred size to a 32-bit count since all our values
	 * are 32-bit, and we don't want to reach CHN_INFINITE_FORWARD.
	 */
	budget = MIN(bytes, CHN_INFINITE_FORWARD - 1);

	/* transfer as much as we can of buf->i */
	forwarded = MIN(chn->buf->i, budget);
	b_adv(chn->buf, forwarded);
	budget -= forwarded;

	if (!budget)
		return forwarded;

	/* Now we must ensure chn->to_forward sats below CHN_INFINITE_FORWARD,
	 * which also implies it won't overflow. It's less operations in 64-bit.
	 */
	bytes = (unsigned long long)chn->to_forward + budget;
	if (bytes >= CHN_INFINITE_FORWARD)
		bytes = CHN_INFINITE_FORWARD - 1;
	budget = bytes - chn->to_forward;

	chn->to_forward += budget;
	forwarded += budget;
	return forwarded;
}

/* writes <len> bytes from message <msg> to the channel's buffer. Returns -1 in
 * case of success, -2 if the message is larger than the buffer size, or the
 * number of bytes available otherwise. The send limit is automatically
 * adjusted to the amount of data written. FIXME-20060521: handle unaligned
 * data. Note: this function appends data to the buffer's output and possibly
 * overwrites any pending input data which are assumed not to exist.
 */
int co_inject(struct channel *chn, const char *msg, int len)
{
	int max;

	if (len == 0)
		return -1;

	if (len < 0 || len > chn->buf->size) {
		/* we can't write this chunk and will never be able to, because
		 * it is larger than the buffer. This must be reported as an
		 * error. Then we return -2 so that writers that don't care can
		 * ignore it and go on, and others can check for this value.
		 */
		return -2;
	}

	buffer_realign(chn->buf);
	max = bo_contig_space(chn->buf);
	if (len > max)
		return max;

	memcpy(chn->buf->p, msg, len);
	chn->buf->o += len;
	chn->buf->p = b_ptr(chn->buf, len);
	chn->total += len;
	return -1;
}

/* Tries to copy character <c> into the channel's buffer after some length
 * controls. The chn->o and to_forward pointers are updated. If the channel
 * input is closed, -2 is returned. If there is not enough room left in the
 * buffer, -1 is returned. Otherwise the number of bytes copied is returned
 * (1). Channel flag READ_PARTIAL is updated if some data can be transferred.
 */
int ci_putchr(struct channel *chn, char c)
{
	if (unlikely(channel_input_closed(chn)))
		return -2;

	if (!channel_may_recv(chn))
		return -1;

	*bi_end(chn->buf) = c;

	chn->buf->i++;
	chn->flags |= CF_READ_PARTIAL;

	if (chn->to_forward >= 1) {
		if (chn->to_forward != CHN_INFINITE_FORWARD)
			chn->to_forward--;
		b_adv(chn->buf, 1);
	}

	chn->total++;
	return 1;
}

/* Tries to copy block <blk> at once into the channel's buffer after length
 * controls. The chn->o and to_forward pointers are updated. If the channel
 * input is closed, -2 is returned. If the block is too large for this buffer,
 * -3 is returned. If there is not enough room left in the buffer, -1 is
 * returned. Otherwise the number of bytes copied is returned (0 being a valid
 * number). Channel flag READ_PARTIAL is updated if some data can be
 * transferred.
 */
int ci_putblk(struct channel *chn, const char *blk, int len)
{
	int max;

	if (unlikely(channel_input_closed(chn)))
		return -2;

	if (len < 0)
		return -3;

	max = channel_recv_limit(chn);
	if (unlikely(len > max - buffer_len(chn->buf))) {
		/* we can't write this chunk right now because the buffer is
		 * almost full or because the block is too large. Return the
		 * available space or -2 if impossible.
		 */
		if (len > max)
			return -3;

		return -1;
	}

	if (unlikely(len == 0))
		return 0;

	/* OK so the data fits in the buffer in one or two blocks */
	max = bi_contig_space(chn->buf);
	memcpy(bi_end(chn->buf), blk, MIN(len, max));
	if (len > max)
		memcpy(chn->buf->data, blk + max, len - max);

	chn->buf->i += len;
	chn->total += len;
	if (chn->to_forward) {
		unsigned long fwd = len;
		if (chn->to_forward != CHN_INFINITE_FORWARD) {
			if (fwd > chn->to_forward)
				fwd = chn->to_forward;
			chn->to_forward -= fwd;
		}
		b_adv(chn->buf, fwd);
	}

	/* notify that some data was read from the SI into the buffer */
	chn->flags |= CF_READ_PARTIAL;
	return len;
}

/* Tries to copy the whole buffer <buf> into the channel's buffer after length
 * controls. It will only succeed if the target buffer is empty, in which case
 * it will simply swap the buffers. The buffer not attached to the channel is
 * returned so that the caller can store it locally. The chn->buf->o and
 * to_forward pointers are updated. If the output buffer is a dummy buffer or
 * if it still contains data <buf> is returned, indicating that nothing could
 * be done. Channel flag READ_PARTIAL is updated if some data can be transferred.
 * The chunk's length is updated with the number of bytes sent. On errors, NULL
 * is returned. Note that only buf->i is considered.
 */
struct buffer *ci_swpbuf(struct channel *chn, struct buffer *buf)
{
	struct buffer *old;

	if (unlikely(channel_input_closed(chn)))
		return NULL;

	if (!chn->buf->size || !buffer_empty(chn->buf))
		return buf;

	old = chn->buf;
	chn->buf = buf;

	if (!buf->i)
		return old;

	chn->total += buf->i;

	if (chn->to_forward) {
		unsigned long fwd = buf->i;
		if (chn->to_forward != CHN_INFINITE_FORWARD) {
			if (fwd > chn->to_forward)
				fwd = chn->to_forward;
			chn->to_forward -= fwd;
		}
		b_adv(chn->buf, fwd);
	}

	/* notify that some data was read from the SI into the buffer */
	chn->flags |= CF_READ_PARTIAL;
	return old;
}

/* Gets one text line out of a channel's buffer from a stream interface.
 * Return values :
 *   >0 : number of bytes read. Includes the \n if present before len or end.
 *   =0 : no '\n' before end found. <str> is left undefined.
 *   <0 : no more bytes readable because output is shut.
 * The channel status is not changed. The caller must call co_skip() to
 * update it. The '\n' is waited for as long as neither the buffer nor the
 * output are full. If either of them is full, the string may be returned
 * as is, without the '\n'.
 */
int co_getline(const struct channel *chn, char *str, int len)
{
	int ret, max;
	char *p;

	ret = 0;
	max = len;

	/* closed or empty + imminent close = -1; empty = 0 */
	if (unlikely((chn->flags & CF_SHUTW) || channel_is_empty(chn))) {
		if (chn->flags & (CF_SHUTW|CF_SHUTW_NOW))
			ret = -1;
		goto out;
	}

	p = bo_ptr(chn->buf);

	if (max > chn->buf->o) {
		max = chn->buf->o;
		str[max-1] = 0;
	}
	while (max) {
		*str++ = *p;
		ret++;
		max--;

		if (*p == '\n')
			break;
		p = buffer_wrap_add(chn->buf, p + 1);
	}
	if (ret > 0 && ret < len &&
	    (ret < chn->buf->o || channel_may_recv(chn)) &&
	    *(str-1) != '\n' &&
	    !(chn->flags & (CF_SHUTW|CF_SHUTW_NOW)))
		ret = 0;
 out:
	if (max)
		*str = 0;
	return ret;
}

/* Gets one full block of data at once from a channel's buffer, optionally from
 * a specific offset. Return values :
 *   >0 : number of bytes read, equal to requested size.
 *   =0 : not enough data available. <blk> is left undefined.
 *   <0 : no more bytes readable because output is shut.
 * The channel status is not changed. The caller must call co_skip() to
 * update it.
 */
int co_getblk(const struct channel *chn, char *blk, int len, int offset)
{
	if (chn->flags & CF_SHUTW)
		return -1;

	if (len + offset > chn->buf->o) {
		if (chn->flags & (CF_SHUTW|CF_SHUTW_NOW))
			return -1;
		return 0;
	}

	return bo_getblk(chn->buf, blk, len, offset);
}

/* Gets one or two blocks of data at once from a channel's output buffer.
 * Return values :
 *   >0 : number of blocks filled (1 or 2). blk1 is always filled before blk2.
 *   =0 : not enough data available. <blk*> are left undefined.
 *   <0 : no more bytes readable because output is shut.
 * The channel status is not changed. The caller must call co_skip() to
 * update it. Unused buffers are left in an undefined state.
 */
int co_getblk_nc(const struct channel *chn, char **blk1, int *len1, char **blk2, int *len2)
{
	if (unlikely(chn->buf->o == 0)) {
		if (chn->flags & CF_SHUTW)
			return -1;
		return 0;
	}

	return bo_getblk_nc(chn->buf, blk1, len1, blk2, len2);
}

/* Gets one text line out of a channel's output buffer from a stream interface.
 * Return values :
 *   >0 : number of blocks returned (1 or 2). blk1 is always filled before blk2.
 *   =0 : not enough data available.
 *   <0 : no more bytes readable because output is shut.
 * The '\n' is waited for as long as neither the buffer nor the output are
 * full. If either of them is full, the string may be returned as is, without
 * the '\n'. Unused buffers are left in an undefined state.
 */
int co_getline_nc(const struct channel *chn,
                  char **blk1, int *len1,
                  char **blk2, int *len2)
{
	int retcode;
	int l;

	retcode = co_getblk_nc(chn, blk1, len1, blk2, len2);
	if (unlikely(retcode <= 0))
		return retcode;

	for (l = 0; l < *len1 && (*blk1)[l] != '\n'; l++);
	if (l < *len1 && (*blk1)[l] == '\n') {
		*len1 = l + 1;
		return 1;
	}

	if (retcode >= 2) {
		for (l = 0; l < *len2 && (*blk2)[l] != '\n'; l++);
		if (l < *len2 && (*blk2)[l] == '\n') {
			*len2 = l + 1;
			return 2;
		}
	}

	if (chn->flags & CF_SHUTW) {
		/* If we have found no LF and the buffer is shut, then
		 * the resulting string is made of the concatenation of
		 * the pending blocks (1 or 2).
		 */
		return retcode;
	}

	/* No LF yet and not shut yet */
	return 0;
}

/* Gets one full block of data at once from a channel's input buffer.
 * This function can return the data slitted in one or two blocks.
 * Return values :
 *   >0 : number of blocks returned (1 or 2). blk1 is always filled before blk2.
 *   =0 : not enough data available.
 *   <0 : no more bytes readable because input is shut.
 */
int ci_getblk_nc(const struct channel *chn,
                 char **blk1, int *len1,
                 char **blk2, int *len2)
{
	if (unlikely(chn->buf->i == 0)) {
		if (chn->flags & CF_SHUTR)
			return -1;
		return 0;
	}

	if (unlikely(chn->buf->p + chn->buf->i > chn->buf->data + chn->buf->size)) {
		*blk1 = chn->buf->p;
		*len1 = chn->buf->data + chn->buf->size - chn->buf->p;
		*blk2 = chn->buf->data;
		*len2 = chn->buf->i - *len1;
		return 2;
	}

	*blk1 = chn->buf->p;
	*len1 = chn->buf->i;
	return 1;
}

/* Gets one text line out of a channel's input buffer from a stream interface.
 * Return values :
 *   >0 : number of blocks returned (1 or 2). blk1 is always filled before blk2.
 *   =0 : not enough data available.
 *   <0 : no more bytes readable because output is shut.
 * The '\n' is waited for as long as neither the buffer nor the input are
 * full. If either of them is full, the string may be returned as is, without
 * the '\n'. Unused buffers are left in an undefined state.
 */
int ci_getline_nc(const struct channel *chn,
                  char **blk1, int *len1,
                  char **blk2, int *len2)
{
	int retcode;
	int l;

	retcode = ci_getblk_nc(chn, blk1, len1, blk2, len2);
	if (unlikely(retcode <= 0))
		return retcode;

	for (l = 0; l < *len1 && (*blk1)[l] != '\n'; l++);
	if (l < *len1 && (*blk1)[l] == '\n') {
		*len1 = l + 1;
		return 1;
	}

	if (retcode >= 2) {
		for (l = 0; l < *len2 && (*blk2)[l] != '\n'; l++);
		if (l < *len2 && (*blk2)[l] == '\n') {
			*len2 = l + 1;
			return 2;
		}
	}

	if (chn->flags & CF_SHUTW) {
		/* If we have found no LF and the buffer is shut, then
		 * the resulting string is made of the concatenation of
		 * the pending blocks (1 or 2).
		 */
		return retcode;
	}

	/* No LF yet and not shut yet */
	return 0;
}

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
