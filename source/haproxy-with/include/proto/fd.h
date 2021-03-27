/*
 * include/proto/fd.h
 * File descriptors states.
 *
 * Copyright (C) 2000-2014 Willy Tarreau - w@1wt.eu
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, version 2.1
 * exclusively.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef _PROTO_FD_H
#define _PROTO_FD_H

#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <common/config.h>

#include <types/fd.h>

/* public variables */

extern unsigned int *fd_cache;      // FD events cache
extern int fd_cache_num;            // number of events in the cache
extern unsigned long fd_cache_mask; // Mask of threads with events in the cache

extern THREAD_LOCAL int *fd_updt;  // FD updates list
extern THREAD_LOCAL int fd_nbupdt; // number of updates in the list

__decl_hathreads(extern HA_SPINLOCK_T __attribute__((aligned(64))) fdtab_lock);      /* global lock to protect fdtab array */
__decl_hathreads(extern HA_RWLOCK_T   __attribute__((aligned(64))) fdcache_lock);    /* global lock to protect fd_cache array */
__decl_hathreads(extern HA_SPINLOCK_T __attribute__((aligned(64))) poll_lock);       /* global lock to protect poll info */
__decl_hathreads(extern HA_SPINLOCK_T __attribute__((aligned(64))) fd_updt_lock); /* global lock to protect the update list */

extern struct fdlist update_list; // Global update list

/* Deletes an FD from the fdsets, and recomputes the maxfd limit.
 * The file descriptor is also closed.
 */
void fd_delete(int fd);

/* Deletes an FD from the fdsets, and recomputes the maxfd limit.
 * The file descriptor is kept open.
 */
void fd_remove(int fd);

/* disable the specified poller */
void disable_poller(const char *poller_name);

/*
 * Initialize the pollers till the best one is found.
 * If none works, returns 0, otherwise 1.
 * The pollers register themselves just before main() is called.
 */
int init_pollers();

/*
 * Deinitialize the pollers.
 */
void deinit_pollers();

/*
 * Some pollers may lose their connection after a fork(). It may be necessary
 * to create initialize part of them again. Returns 0 in case of failure,
 * otherwise 1. The fork() function may be NULL if unused. In case of error,
 * the the current poller is destroyed and the caller is responsible for trying
 * another one by calling init_pollers() again.
 */
int fork_poller();

/*
 * Lists the known pollers on <out>.
 * Should be performed only before initialization.
 */
int list_pollers(FILE *out);

/*
 * Runs the polling loop
 */
void run_poller();

/* Scan and process the cached events. This should be called right after
 * the poller.
 */
void fd_process_cached_events();

/* Mark fd <fd> as updated for polling and allocate an entry in the update list
 * for this if it was not already there. This can be done at any time.
 * This function expects the FD lock to be locked, and returns with the
 * FD lock unlocked.
 */
static inline void updt_fd_polling(const int fd)
{
	if ((fdtab[fd].update_mask & fdtab[fd].thread_mask) ==
	    fdtab[fd].thread_mask) {
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
		/* already scheduled for update */
		return;
	}
	if (fdtab[fd].thread_mask == tid_bit) {
		fdtab[fd].update_mask |= tid_bit;
		fd_updt[fd_nbupdt++] = fd;
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
	} else {
		/* This is ugly, but we can afford to unlock the FD lock
		 * before we acquire the fd_updt_lock, to prevent a
		 * lock order reversal, because this function is only called
		 * from fd_update_cache(), and all users of fd_update_cache()
		 * used to just unlock the fd lock just after, anyway.
		 */
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
		HA_SPIN_LOCK(FD_UPDATE_LOCK, &fd_updt_lock);
		/* If update_mask is non-nul, then it's already in the list
		 * so we don't have to add it.
		 */
		if ((fdtab[fd].update_mask & all_threads_mask) == 0) {
			if (update_list.first == -1) {
				update_list.first = update_list.last = fd;
				fdtab[fd].update.next = fdtab[fd].update.prev = -1;
			} else {
				fdtab[update_list.last].update.next = fd;
				fdtab[fd].update.prev = update_list.last;
				fdtab[fd].update.next = -1;
				update_list.last = fd;
			}
		}
		fdtab[fd].update_mask |= fdtab[fd].thread_mask;
		HA_SPIN_UNLOCK(FD_UPDATE_LOCK, &fd_updt_lock);

	}
}

/* Called from the poller to acknoledge we read an entry from the global
 * update list, to remove our bit from the update_mask, and remove it from
 * the list if we were the last one.
 */
/* Expects to be called with the FD lock and the FD update lock held */
static inline void done_update_polling(int fd)
{
	fdtab[fd].update_mask &= ~tid_bit;
	if ((fdtab[fd].update_mask & all_threads_mask) == 0) {
		if (fdtab[fd].update.prev != -1)
			fdtab[fdtab[fd].update.prev].update.next =
			    fdtab[fd].update.next;
		else
			update_list.first = fdtab[fd].update.next;
		if (fdtab[fd].update.next != -1)
			fdtab[fdtab[fd].update.next].update.prev =
			    fdtab[fd].update.prev;
		else
			update_list.last = fdtab[fd].update.prev;
	}
}


/* Allocates a cache entry for a file descriptor if it does not yet have one.
 * This can be done at any time.
 */
static inline void fd_alloc_cache_entry(const int fd)
{
	HA_RWLOCK_WRLOCK(FDCACHE_LOCK, &fdcache_lock);
	if (fdtab[fd].cache)
		goto end;
	fd_cache_num++;
	fd_cache_mask |= fdtab[fd].thread_mask;
	fdtab[fd].cache = fd_cache_num;
	fd_cache[fd_cache_num-1] = fd;
  end:
	HA_RWLOCK_WRUNLOCK(FDCACHE_LOCK, &fdcache_lock);
}

/* Removes entry used by fd <fd> from the FD cache and replaces it with the
 * last one. The fdtab.cache is adjusted to match the back reference if needed.
 * If the fd has no entry assigned, return immediately.
 */
static inline void fd_release_cache_entry(int fd)
{
	unsigned int pos;

	HA_RWLOCK_WRLOCK(FDCACHE_LOCK, &fdcache_lock);
	pos = fdtab[fd].cache;
	if (!pos)
		goto end;
	fdtab[fd].cache = 0;
	fd_cache_num--;
	if (likely(pos <= fd_cache_num)) {
		/* was not the last entry */
		fd = fd_cache[fd_cache_num];
		fd_cache[pos - 1] = fd;
		fdtab[fd].cache = pos;
	}
  end:
	HA_RWLOCK_WRUNLOCK(FDCACHE_LOCK, &fdcache_lock);
}

/* Computes the new polled status based on the active and ready statuses, for
 * each direction. This is meant to be used by pollers while processing updates.
 */
static inline int fd_compute_new_polled_status(int state)
{
	if (state & FD_EV_ACTIVE_R) {
		if (!(state & FD_EV_READY_R))
			state |= FD_EV_POLLED_R;
	}
	else
		state &= ~FD_EV_POLLED_R;

	if (state & FD_EV_ACTIVE_W) {
		if (!(state & FD_EV_READY_W))
			state |= FD_EV_POLLED_W;
	}
	else
		state &= ~FD_EV_POLLED_W;

	return state;
}

/* This function automatically enables/disables caching for an entry depending
 * on its state, and also possibly creates an update entry so that the poller
 * does its job as well. It is only called on state changes.
 */
static inline void fd_update_cache(int fd)
{
	/* only READY and ACTIVE states (the two with both flags set) require a cache entry */
	if (((fdtab[fd].state & (FD_EV_READY_R | FD_EV_ACTIVE_R)) == (FD_EV_READY_R | FD_EV_ACTIVE_R)) ||
	    ((fdtab[fd].state & (FD_EV_READY_W | FD_EV_ACTIVE_W)) == (FD_EV_READY_W | FD_EV_ACTIVE_W))) {
		fd_alloc_cache_entry(fd);
	}
	else {
		fd_release_cache_entry(fd);
	}
	/* 3 states for each direction require a polling update */
	if ((fdtab[fd].state & (FD_EV_POLLED_R |                 FD_EV_ACTIVE_R)) == FD_EV_POLLED_R ||
	    (fdtab[fd].state & (FD_EV_POLLED_R | FD_EV_READY_R | FD_EV_ACTIVE_R)) == FD_EV_ACTIVE_R ||
	    (fdtab[fd].state & (FD_EV_POLLED_W |                 FD_EV_ACTIVE_W)) == FD_EV_POLLED_W ||
	    (fdtab[fd].state & (FD_EV_POLLED_W | FD_EV_READY_W | FD_EV_ACTIVE_W)) == FD_EV_ACTIVE_W)
		updt_fd_polling(fd);
	else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/*
 * returns the FD's recv state (FD_EV_*)
 */
static inline int fd_recv_state(const int fd)
{
	return ((unsigned)fdtab[fd].state >> (4 * DIR_RD)) & FD_EV_STATUS;
}

/*
 * returns true if the FD is active for recv
 */
static inline int fd_recv_active(const int fd)
{
	return (unsigned)fdtab[fd].state & FD_EV_ACTIVE_R;
}

/*
 * returns true if the FD is ready for recv
 */
static inline int fd_recv_ready(const int fd)
{
	return (unsigned)fdtab[fd].state & FD_EV_READY_R;
}

/*
 * returns true if the FD is polled for recv
 */
static inline int fd_recv_polled(const int fd)
{
	return (unsigned)fdtab[fd].state & FD_EV_POLLED_R;
}

/*
 * returns the FD's send state (FD_EV_*)
 */
static inline int fd_send_state(const int fd)
{
	return ((unsigned)fdtab[fd].state >> (4 * DIR_WR)) & FD_EV_STATUS;
}

/*
 * returns true if the FD is active for send
 */
static inline int fd_send_active(const int fd)
{
	return (unsigned)fdtab[fd].state & FD_EV_ACTIVE_W;
}

/*
 * returns true if the FD is ready for send
 */
static inline int fd_send_ready(const int fd)
{
	return (unsigned)fdtab[fd].state & FD_EV_READY_W;
}

/*
 * returns true if the FD is polled for send
 */
static inline int fd_send_polled(const int fd)
{
	return (unsigned)fdtab[fd].state & FD_EV_POLLED_W;
}

/*
 * returns true if the FD is active for recv or send
 */
static inline int fd_active(const int fd)
{
	return (unsigned)fdtab[fd].state & FD_EV_ACTIVE_RW;
}

/* Disable processing recv events on fd <fd> */
static inline void fd_stop_recv(int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (fd_recv_active(fd)) {
		fdtab[fd].state &= ~FD_EV_ACTIVE_R;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Disable processing send events on fd <fd> */
static inline void fd_stop_send(int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (fd_send_active(fd)) {
		fdtab[fd].state &= ~FD_EV_ACTIVE_W;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Disable processing of events on fd <fd> for both directions. */
static inline void fd_stop_both(int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (fd_active(fd)) {
		fdtab[fd].state &= ~FD_EV_ACTIVE_RW;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Report that FD <fd> cannot receive anymore without polling (EAGAIN detected). */
static inline void fd_cant_recv(const int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (fd_recv_ready(fd)) {
		fdtab[fd].state &= ~FD_EV_READY_R;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Report that FD <fd> can receive anymore without polling. */
static inline void fd_may_recv(const int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (!fd_recv_ready(fd)) {
		fdtab[fd].state |= FD_EV_READY_R;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Disable readiness when polled. This is useful to interrupt reading when it
 * is suspected that the end of data might have been reached (eg: short read).
 * This can only be done using level-triggered pollers, so if any edge-triggered
 * is ever implemented, a test will have to be added here.
 */
static inline void fd_done_recv(const int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (fd_recv_polled(fd) && fd_recv_ready(fd)) {
		fdtab[fd].state &= ~FD_EV_READY_R;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Report that FD <fd> cannot send anymore without polling (EAGAIN detected). */
static inline void fd_cant_send(const int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (fd_send_ready(fd)) {
		fdtab[fd].state &= ~FD_EV_READY_W;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Report that FD <fd> can send anymore without polling (EAGAIN detected). */
static inline void fd_may_send(const int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (!fd_send_ready(fd)) {
		fdtab[fd].state |= FD_EV_READY_W;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Prepare FD <fd> to try to receive */
static inline void fd_want_recv(int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (!fd_recv_active(fd)) {
		fdtab[fd].state |= FD_EV_ACTIVE_R;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Prepare FD <fd> to try to send */
static inline void fd_want_send(int fd)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	if (!fd_send_active(fd)) {
		fdtab[fd].state |= FD_EV_ACTIVE_W;
		fd_update_cache(fd); /* need an update entry to change the state */
		/* the FD lock is unlocked by fd_update_cache() */
	} else
		HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);
}

/* Update events seen for FD <fd> and its state if needed. This should be called
 * by the poller to set FD_POLL_* flags. */
static inline void fd_update_events(int fd, int evts)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	fdtab[fd].ev &= FD_POLL_STICKY;
	fdtab[fd].ev |= evts;
	HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);

	if (fdtab[fd].ev & (FD_POLL_IN | FD_POLL_HUP | FD_POLL_ERR))
		fd_may_recv(fd);

	if (fdtab[fd].ev & (FD_POLL_OUT | FD_POLL_ERR))
		fd_may_send(fd);
}

/* Prepares <fd> for being polled */
static inline void fd_insert(int fd, unsigned long thread_mask)
{
	HA_SPIN_LOCK(FD_LOCK, &fdtab[fd].lock);
	fdtab[fd].ev = 0;
	fdtab[fd].new = 1;
	fdtab[fd].linger_risk = 0;
	fdtab[fd].cloned = 0;
	fdtab[fd].cache = 0;
	fdtab[fd].thread_mask = thread_mask;
	/* note: do not reset polled_mask here as it indicates which poller
	 * still knows this FD from a possible previous round.
	 */
	HA_SPIN_UNLOCK(FD_LOCK, &fdtab[fd].lock);

	HA_SPIN_LOCK(FDTAB_LOCK, &fdtab_lock);
	if (fd + 1 > maxfd)
		maxfd = fd + 1;
	HA_SPIN_UNLOCK(FDTAB_LOCK, &fdtab_lock);
}


#endif /* _PROTO_FD_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
 */
