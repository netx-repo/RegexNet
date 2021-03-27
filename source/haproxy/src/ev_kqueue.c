/*
 * FD polling functions for FreeBSD kqueue()
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

#include <sys/event.h>
#include <sys/time.h>

#include <common/compat.h>
#include <common/config.h>
#include <common/hathreads.h>
#include <common/ticks.h>
#include <common/time.h>
#include <common/tools.h>

#include <types/global.h>

#include <proto/fd.h>


/* private data */
static int kqueue_fd[MAX_THREADS]; // per-thread kqueue_fd
static THREAD_LOCAL struct kevent *kev = NULL;
static struct kevent *kev_out = NULL; // Trash buffer for kevent() to write the eventlist in

static int _update_fd(int fd, int start)
{
	int en;
	int changes = start;

	en = fdtab[fd].state;

	if (!(fdtab[fd].thread_mask & tid_bit) || !(en & FD_EV_POLLED_RW)) {
		if (!(fdtab[fd].polled_mask & tid_bit)) {
			/* fd was not watched, it's still not */
			return changes;
		}
		/* fd totally removed from poll list */
		EV_SET(&kev[changes++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
		EV_SET(&kev[changes++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
		HA_ATOMIC_AND(&fdtab[fd].polled_mask, ~tid_bit);
	}
	else {
		/* OK fd has to be monitored, it was either added or changed */

		if (en & FD_EV_POLLED_R)
			EV_SET(&kev[changes++], fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
		else if (fdtab[fd].polled_mask & tid_bit)
			EV_SET(&kev[changes++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);

		if (en & FD_EV_POLLED_W)
			EV_SET(&kev[changes++], fd, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
		else if (fdtab[fd].polled_mask & tid_bit)
			EV_SET(&kev[changes++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

		HA_ATOMIC_OR(&fdtab[fd].polled_mask, tid_bit);
	}
	return changes;
}

/*
 * kqueue() poller
 */
REGPRM2 static void _do_poll(struct poller *p, int exp)
{
	int status;
	int count, fd, delta_ms;
	struct timespec timeout;
	int updt_idx, en, eo;
	int changes = 0;

	timeout.tv_sec  = 0;
	timeout.tv_nsec = 0;
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

		changes = _update_fd(fd, changes);
	}

	/* Scan the global update list */
	HA_SPIN_LOCK(FD_UPDATE_LOCK, &fd_updt_lock);
	for (fd = update_list.first; fd != -1; fd = fdtab[fd].update.next) {
		HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
		if (fdtab[fd].update_mask & tid_bit)
			done_update_polling(fd);
		else {
			HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
			continue;
		}
		fdtab[fd].new = 0;

		eo = fdtab[fd].state;
		en = fd_compute_new_polled_status(eo);
		fdtab[fd].state = en;

		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
		if (!fdtab[fd].owner)
			continue;
		changes = _update_fd(fd, changes);
	}
	HA_SPIN_UNLOCK(FD_UPDATE_LOCK, &fd_updt_lock);

	thread_harmless_now();

	if (changes) {
#ifdef EV_RECEIPT
		kev[0].flags |= EV_RECEIPT;
#else
		/* If EV_RECEIPT isn't defined, just add an invalid entry,
		 * so that we get an error and kevent() stops before scanning
		 * the kqueue.
		 */
		EV_SET(&kev[changes++], -1, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
#endif
		kevent(kqueue_fd[tid], kev, changes, kev_out, changes, &timeout);
	}
	fd_nbupdt = 0;

	delta_ms        = 0;

	if (!exp) {
		delta_ms        = MAX_DELAY_MS;
		timeout.tv_sec  = (MAX_DELAY_MS / 1000);
		timeout.tv_nsec = (MAX_DELAY_MS % 1000) * 1000000;
	}
	else if (!tick_is_expired(exp, now_ms)) {
		delta_ms = TICKS_TO_MS(tick_remain(now_ms, exp)) + 1;
		if (delta_ms > MAX_DELAY_MS)
			delta_ms = MAX_DELAY_MS;
		timeout.tv_sec  = (delta_ms / 1000);
		timeout.tv_nsec = (delta_ms % 1000) * 1000000;
	}
	else
		activity[tid].poll_exp++;

	fd = MIN(maxfd, global.tune.maxpollevents);
	gettimeofday(&before_poll, NULL);
	status = kevent(kqueue_fd[tid], // int kq
			NULL,      // const struct kevent *changelist
			0,         // int nchanges
			kev,       // struct kevent *eventlist
			fd,        // int nevents
			&timeout); // const struct timespec *timeout
	tv_update_date(delta_ms, status);
	measure_idle();

	thread_harmless_end();

	for (count = 0; count < status; count++) {
		unsigned int n = 0;
		fd = kev[count].ident;

		if (!fdtab[fd].owner) {
			activity[tid].poll_dead++;
			continue;
		}

		if (!(fdtab[fd].thread_mask & tid_bit)) {
			activity[tid].poll_skip++;
			continue;
		}

		if (kev[count].filter ==  EVFILT_READ) {
			if (kev[count].data)
				n |= FD_POLL_IN;
			if (kev[count].flags & EV_EOF)
				n |= FD_POLL_HUP;
		}
		else if (kev[count].filter ==  EVFILT_WRITE) {
			n |= FD_POLL_OUT;
			if (kev[count].flags & EV_EOF)
				n |= FD_POLL_ERR;
		}

		fd_update_events(fd, n);
	}
}


static int init_kqueue_per_thread()
{
	int fd;

	/* we can have up to two events per fd, so allocate enough to store
	 * 2*fd event, and an extra one, in case EV_RECEIPT isn't defined,
	 * so that we can add an invalid entry and get an error, to avoid
	 * scanning the kqueue uselessly.
	 */
	kev = calloc(1, sizeof(struct kevent) * (2 * global.maxsock + 1));
	if (kev == NULL)
		goto fail_alloc;

	if (MAX_THREADS > 1 && tid) {
		kqueue_fd[tid] = kqueue();
		if (kqueue_fd[tid] < 0)
			goto fail_fd;
	}

	/* we may have to unregister some events initially registered on the
	 * original fd when it was alone, and/or to register events on the new
	 * fd for this thread. Let's just mark them as updated, the poller will
	 * do the rest.
	 */
	for (fd = 0; fd < maxfd; fd++) {
		HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
		updt_fd_polling(fd);
	}

	return 1;
 fail_fd:
	free(kev);
 fail_alloc:
	return 0;
}

static void deinit_kqueue_per_thread()
{
	if (MAX_THREADS > 1 && tid)
		close(kqueue_fd[tid]);

	free(kev);
	kev = NULL;
}

/*
 * Initialization of the kqueue() poller.
 * Returns 0 in case of failure, non-zero in case of success. If it fails, it
 * disables the poller by setting its pref to 0.
 */
REGPRM1 static int _do_init(struct poller *p)
{
	p->private = NULL;

	/* we can have up to two events per fd, so allocate enough to store
	 * 2*fd event, and an extra one, in case EV_RECEIPT isn't defined,
	 * so that we can add an invalid entry and get an error, to avoid
	 * scanning the kqueue uselessly.
	 */
	kev_out = calloc(1, sizeof(struct kevent) * (2 * global.maxsock + 1));
	if (!kev_out)
		goto fail_alloc;

	kqueue_fd[tid] = kqueue();
	if (kqueue_fd[tid] < 0)
		goto fail_fd;

	hap_register_per_thread_init(init_kqueue_per_thread);
	hap_register_per_thread_deinit(deinit_kqueue_per_thread);
	return 1;

 fail_fd:
	free(kev_out);
	kev_out = NULL;
fail_alloc:
	p->pref = 0;
	return 0;
}

/*
 * Termination of the kqueue() poller.
 * Memory is released and the poller is marked as unselectable.
 */
REGPRM1 static void _do_term(struct poller *p)
{
	if (kqueue_fd[tid] >= 0) {
		close(kqueue_fd[tid]);
		kqueue_fd[tid] = -1;
	}

	p->private = NULL;
	p->pref = 0;
	if (kev_out) {
		free(kev_out);
		kev_out = NULL;
	}
}

/*
 * Check that the poller works.
 * Returns 1 if OK, otherwise 0.
 */
REGPRM1 static int _do_test(struct poller *p)
{
	int fd;

	fd = kqueue();
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
}

/*
 * Recreate the kqueue file descriptor after a fork(). Returns 1 if OK,
 * otherwise 0. Note that some pollers need to be reopened after a fork()
 * (such as kqueue), and some others may fail to do so in a chroot.
 */
REGPRM1 static int _do_fork(struct poller *p)
{
	kqueue_fd[tid] = kqueue();
	if (kqueue_fd[tid] < 0)
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
	int i;

	if (nbpollers >= MAX_POLLERS)
		return;

	for (i = 0; i < MAX_THREADS; i++)
		kqueue_fd[i] = -1;

	p = &pollers[nbpollers++];

	p->name = "kqueue";
	p->pref = 300;
	p->flags = HAP_POLL_F_RDHUP;
	p->private = NULL;

	p->clo  = NULL;
	p->test = _do_test;
	p->init = _do_init;
	p->term = _do_term;
	p->poll = _do_poll;
	p->fork = _do_fork;
}


/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
