/*
 * FD polling functions for generic select()
 *
 * Copyright 2000-2014 Willy Tarreau <w@1wt.eu>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/hathreads.h>
#include <common/ticks.h>
#include <common/time.h>

#include <types/global.h>

#include <proto/fd.h>


/* private data */
static fd_set *fd_evts[2];
static THREAD_LOCAL fd_set *tmp_evts[2];

/* Immediately remove the entry upon close() */
REGPRM1 static void __fd_clo(int fd)
{
	HA_SPIN_LOCK(POLL_LOCK, &poll_lock);
	FD_CLR(fd, fd_evts[DIR_RD]);
	FD_CLR(fd, fd_evts[DIR_WR]);
	HA_SPIN_UNLOCK(POLL_LOCK, &poll_lock);
}

/*
 * Select() poller
 */
REGPRM2 static void _do_poll(struct poller *p, int exp)
{
	int status;
	int fd, i;
	struct timeval delta;
	int delta_ms;
	int fds;
	int updt_idx, en, eo;
	char count;
	int readnotnull, writenotnull;

	/* first, scan the update list to find changes */
	for (updt_idx = 0; updt_idx < fd_nbupdt; updt_idx++) {
		fd = fd_updt[updt_idx];

		HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
		fdtab[fd].update_mask &= ~tid_bit;

		if (!fdtab[fd].owner) {
			activity[tid].poll_drop++;
			HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
			continue;
		}

		fdtab[fd].new = 0;

		eo = fdtab[fd].state;
		en = fd_compute_new_polled_status(eo);
		fdtab[fd].state = en;
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
		if ((eo ^ en) & FD_EV_POLLED_RW) {
			/* poll status changed, update the lists */
			HA_SPIN_LOCK(POLL_LOCK, &poll_lock);
			if ((eo & ~en) & FD_EV_POLLED_R)
				FD_CLR(fd, fd_evts[DIR_RD]);
			else if ((en & ~eo) & FD_EV_POLLED_R)
				FD_SET(fd, fd_evts[DIR_RD]);

			if ((eo & ~en) & FD_EV_POLLED_W)
				FD_CLR(fd, fd_evts[DIR_WR]);
			else if ((en & ~eo) & FD_EV_POLLED_W)
				FD_SET(fd, fd_evts[DIR_WR]);
			HA_SPIN_UNLOCK(POLL_LOCK, &poll_lock);
		}
	}
	HA_SPIN_LOCK(FD_UPDATE_LOCK, &fd_updt_lock);
	for (fd = update_list.first; fd != -1; fd = fdtab[fd].update.next) {
		HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
		if (fdtab[fd].update_mask & tid_bit) {
			/* Cheat a bit, as the state is global to all pollers
			 * we don't need every thread ot take care of the
			 * update.
			 */
			fdtab[fd].update_mask &= ~all_threads_mask;
			done_update_polling(fd);
		} else {
			HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
			continue;
		}

		fdtab[fd].new = 0;

		eo = fdtab[fd].state;
		en = fd_compute_new_polled_status(eo);
		fdtab[fd].state = en;
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
		if ((eo ^ en) & FD_EV_POLLED_RW) {
			/* poll status changed, update the lists */
			HA_SPIN_LOCK(POLL_LOCK, &poll_lock);
			if ((eo & ~en) & FD_EV_POLLED_R)
				FD_CLR(fd, fd_evts[DIR_RD]);
			else if ((en & ~eo) & FD_EV_POLLED_R)
				FD_SET(fd, fd_evts[DIR_RD]);

			if ((eo & ~en) & FD_EV_POLLED_W)
				FD_CLR(fd, fd_evts[DIR_WR]);
			else if ((en & ~eo) & FD_EV_POLLED_W)
				FD_SET(fd, fd_evts[DIR_WR]);
			HA_SPIN_UNLOCK(POLL_LOCK, &poll_lock);
		}

	}
	HA_SPIN_UNLOCK(FD_UPDATE_LOCK, &fd_updt_lock);

	thread_harmless_now();

	fd_nbupdt = 0;

	/* let's restore fdset state */
	readnotnull = 0; writenotnull = 0;
	for (i = 0; i < (maxfd + FD_SETSIZE - 1)/(8*sizeof(int)); i++) {
		readnotnull |= (*(((int*)tmp_evts[DIR_RD])+i) = *(((int*)fd_evts[DIR_RD])+i)) != 0;
		writenotnull |= (*(((int*)tmp_evts[DIR_WR])+i) = *(((int*)fd_evts[DIR_WR])+i)) != 0;
	}

#if 0
	/* just a verification code, needs to be removed for performance */
	for (i=0; i<maxfd; i++) {
		if (FD_ISSET(i, tmp_evts[DIR_RD]) != FD_ISSET(i, fd_evts[DIR_RD]))
			abort();
		if (FD_ISSET(i, tmp_evts[DIR_WR]) != FD_ISSET(i, fd_evts[DIR_WR]))
			abort();
	}
#endif

	delta_ms      = 0;
	delta.tv_sec  = 0;
	delta.tv_usec = 0;

	if (!exp) {
		delta_ms      = MAX_DELAY_MS;
		delta.tv_sec  = (MAX_DELAY_MS / 1000);
		delta.tv_usec = (MAX_DELAY_MS % 1000) * 1000;
	}
	else if (!tick_is_expired(exp, now_ms)) {
		delta_ms = TICKS_TO_MS(tick_remain(now_ms, exp)) + SCHEDULER_RESOLUTION;
		if (delta_ms > MAX_DELAY_MS)
			delta_ms = MAX_DELAY_MS;
		delta.tv_sec  = (delta_ms / 1000);
		delta.tv_usec = (delta_ms % 1000) * 1000;
	}
	else
		activity[tid].poll_exp++;

	gettimeofday(&before_poll, NULL);
	status = select(maxfd,
			readnotnull ? tmp_evts[DIR_RD] : NULL,
			writenotnull ? tmp_evts[DIR_WR] : NULL,
			NULL,
			&delta);

	tv_update_date(delta_ms, status);
	measure_idle();

	thread_harmless_end();

	if (status <= 0)
		return;

	for (fds = 0; (fds * BITS_PER_INT) < maxfd; fds++) {
		if ((((int *)(tmp_evts[DIR_RD]))[fds] | ((int *)(tmp_evts[DIR_WR]))[fds]) == 0)
			continue;

		for (count = BITS_PER_INT, fd = fds * BITS_PER_INT; count && fd < maxfd; count--, fd++) {
			unsigned int n = 0;

			if (!fdtab[fd].owner) {
				activity[tid].poll_dead++;
				continue;
			}

			if (!(fdtab[fd].thread_mask & tid_bit)) {
				activity[tid].poll_skip++;
				continue;
			}

			if (FD_ISSET(fd, tmp_evts[DIR_RD]))
				n |= FD_POLL_IN;

			if (FD_ISSET(fd, tmp_evts[DIR_WR]))
				n |= FD_POLL_OUT;

			fd_update_events(fd, n);
		}
	}
}

static int init_select_per_thread()
{
	int fd_set_bytes;

	fd_set_bytes = sizeof(fd_set) * (global.maxsock + FD_SETSIZE - 1) / FD_SETSIZE;
	if ((tmp_evts[DIR_RD] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail;
	if ((tmp_evts[DIR_WR] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail;
	return 1;
  fail:
	free(tmp_evts[DIR_RD]);
	free(tmp_evts[DIR_WR]);
	return 0;
}

static void deinit_select_per_thread()
{
	free(tmp_evts[DIR_WR]); tmp_evts[DIR_WR] = NULL;
	free(tmp_evts[DIR_RD]); tmp_evts[DIR_RD] = NULL;
}

/*
 * Initialization of the select() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
 */
REGPRM1 static int _do_init(struct poller *p)
{
	__label__ fail_swevt, fail_srevt, fail_revt;
	int fd_set_bytes;

	p->private = NULL;

	if (global.maxsock > FD_SETSIZE)
		goto fail_revt;

	fd_set_bytes = sizeof(fd_set) * (global.maxsock + FD_SETSIZE - 1) / FD_SETSIZE;

	if ((fd_evts[DIR_RD] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_srevt;
	if ((fd_evts[DIR_WR] = (fd_set *)calloc(1, fd_set_bytes)) == NULL)
		goto fail_swevt;

	hap_register_per_thread_init(init_select_per_thread);
	hap_register_per_thread_deinit(deinit_select_per_thread);

	return 1;

 fail_swevt:
	free(fd_evts[DIR_RD]);
 fail_srevt:
	free(tmp_evts[DIR_WR]);
	free(tmp_evts[DIR_RD]);
 fail_revt:
	p->pref = 0;
	return 0;
}

/*
 * Termination of the select() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void _do_term(struct poller *p)
{
	free(fd_evts[DIR_WR]);
	free(fd_evts[DIR_RD]);
	p->private = NULL;
	p->pref = 0;
}

/*
 * Check that the poller works.
 * Returns 1 if OK, otherwise 0.
 */
REGPRM1 static int _do_test(struct poller *p)
{
	if (global.maxsock > FD_SETSIZE)
		return 0;

	return 1;
}

/*
 * It is a constructor, which means that it will automatically be called before
 * main(). This is GCC-specific but it works at least since 2.95.
 * Special care must be taken so that it does not need any uninitialized data.
 */
__attribute__((constructor))
static void _do_register(void)
{
	struct poller *p;

	if (nbpollers >= MAX_POLLERS)
		return;
	p = &pollers[nbpollers++];

	p->name = "select";
	p->pref = 150;
	p->flags = 0;
	p->private = NULL;

	p->clo  = __fd_clo;
	p->test = _do_test;
	p->init = _do_init;
	p->term = _do_term;
	p->poll = _do_poll;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
