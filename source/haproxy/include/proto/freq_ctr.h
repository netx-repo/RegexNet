/*
 * include/proto/freq_ctr.h
 * This file contains macros and inline functions for frequency counters.
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#ifndef _PROTO_FREQ_CTR_H
#define _PROTO_FREQ_CTR_H

#include <common/config.h>
#include <common/time.h>
#include <common/hathreads.h>
#include <types/freq_ctr.h>


/* Update a frequency counter by <inc> incremental units. It is automatically
 * rotated if the period is over. It is important that it correctly initializes
 * a null area.
 */
static inline unsigned int update_freq_ctr(struct freq_ctr *ctr, unsigned int inc)
{
	int elapsed;
	unsigned int tot_inc;
	unsigned int curr_sec;

	do {
		/* remove the bit, used for the lock */
		curr_sec = ctr->curr_sec & 0x7fffffff;
	}
	while (!HA_ATOMIC_CAS(&ctr->curr_sec, &curr_sec, curr_sec | 0x80000000));

	elapsed = (now.tv_sec & 0x7fffffff)- curr_sec;
	if (unlikely(elapsed > 0)) {
		ctr->prev_ctr = ctr->curr_ctr;
		ctr->curr_ctr = 0;
		if (likely(elapsed != 1)) {
			/* we missed more than one second */
			ctr->prev_ctr = 0;
		}
		curr_sec = now.tv_sec;
	}

	ctr->curr_ctr += inc;
	tot_inc = ctr->curr_ctr;

	/* release the lock and update the time in case of rotate. */
	HA_ATOMIC_STORE(&ctr->curr_sec, curr_sec & 0x7fffffff);
	return tot_inc;
	/* Note: later we may want to propagate the update to other counters */
}

/* Update a frequency counter by <inc> incremental units. It is automatically
 * rotated if the period is over. It is important that it correctly initializes
 * a null area. This one works on frequency counters which have a period
 * different from one second.
 */
static inline unsigned int update_freq_ctr_period(struct freq_ctr_period *ctr,
						  unsigned int period, unsigned int inc)
{
	unsigned int tot_inc;
	unsigned int curr_tick;

	do {
		/* remove the bit, used for the lock */
		curr_tick = (ctr->curr_tick >> 1) << 1;
	}
	while (!HA_ATOMIC_CAS(&ctr->curr_tick, &curr_tick, curr_tick | 0x1));

	if (now_ms - curr_tick >= period) {
		ctr->prev_ctr = ctr->curr_ctr;
		ctr->curr_ctr = 0;
		curr_tick += period;
		if (likely(now_ms - curr_tick >= period)) {
			/* we missed at least two periods */
			ctr->prev_ctr = 0;
			curr_tick = now_ms;
		}
	}

	ctr->curr_ctr += inc;
	tot_inc = ctr->curr_ctr;
	/* release the lock and update the time in case of rotate. */
	HA_ATOMIC_STORE(&ctr->curr_tick, (curr_tick >> 1) << 1);
	return tot_inc;
	/* Note: later we may want to propagate the update to other counters */
}

/* Read a frequency counter taking history into account for missing time in
 * current period.
 */
unsigned int read_freq_ctr(struct freq_ctr *ctr);

/* returns the number of remaining events that can occur on this freq counter
 * while respecting <freq> and taking into account that <pend> events are
 * already known to be pending. Returns 0 if limit was reached.
 */
unsigned int freq_ctr_remain(struct freq_ctr *ctr, unsigned int freq, unsigned int pend);

/* return the expected wait time in ms before the next event may occur,
 * respecting frequency <freq>, and assuming there may already be some pending
 * events. It returns zero if we can proceed immediately, otherwise the wait
 * time, which will be rounded down 1ms for better accuracy, with a minimum
 * of one ms.
 */
unsigned int next_event_delay(struct freq_ctr *ctr, unsigned int freq, unsigned int pend);

/* process freq counters over configurable periods */
unsigned int read_freq_ctr_period(struct freq_ctr_period *ctr, unsigned int period);
unsigned int freq_ctr_remain_period(struct freq_ctr_period *ctr, unsigned int period,
				    unsigned int freq, unsigned int pend);

/* While the functions above report average event counts per period, we are
 * also interested in average values per event. For this we use a different
 * method. The principle is to rely on a long tail which sums the new value
 * with a fraction of the previous value, resulting in a sliding window of
 * infinite length depending on the precision we're interested in.
 *
 * The idea is that we always keep (N-1)/N of the sum and add the new sampled
 * value. The sum over N values can be computed with a simple program for a
 * constant value 1 at each iteration :
 *
 *     N
 *   ,---
 *    \       N - 1              e - 1
 *     >  ( --------- )^x ~= N * -----
 *    /         N                  e
 *   '---
 *   x = 1
 *
 * Note: I'm not sure how to demonstrate this but at least this is easily
 * verified with a simple program, the sum equals N * 0.632120 for any N
 * moderately large (tens to hundreds).
 *
 * Inserting a constant sample value V here simply results in :
 *
 *    sum = V * N * (e - 1) / e
 *
 * But we don't want to integrate over a small period, but infinitely. Let's
 * cut the infinity in P periods of N values. Each period M is exactly the same
 * as period M-1 with a factor of ((N-1)/N)^N applied. A test shows that given a
 * large N :
 *
 *      N - 1           1
 *   ( ------- )^N ~=  ---
 *        N             e
 *
 * Our sum is now a sum of each factor times  :
 *
 *    N*P                                     P
 *   ,---                                   ,---
 *    \         N - 1               e - 1    \     1
 *     >  v ( --------- )^x ~= VN * -----  *  >   ---
 *    /           N                   e      /    e^x
 *   '---                                   '---
 *   x = 1                                  x = 0
 *
 * For P "large enough", in tests we get this :
 *
 *    P
 *  ,---
 *   \     1        e
 *    >   --- ~=  -----
 *   /    e^x     e - 1
 *  '---
 *  x = 0
 *
 * This simplifies the sum above :
 *
 *    N*P
 *   ,---
 *    \         N - 1
 *     >  v ( --------- )^x = VN
 *    /           N
 *   '---
 *   x = 1
 *
 * So basically by summing values and applying the last result an (N-1)/N factor
 * we just get N times the values over the long term, so we can recover the
 * constant value V by dividing by N. In order to limit the impact of integer
 * overflows, we'll use this equivalence which saves us one multiply :
 *
 *               N - 1                   1             x0
 *    x1 = x0 * -------   =  x0 * ( 1 - --- )  = x0 - ----
 *                 N                     N              N
 *
 * And given that x0 is discrete here we'll have to saturate the values before
 * performing the divide, so the value insertion will become :
 *
 *               x0 + N - 1
 *    x1 = x0 - ------------
 *                    N
 *
 * A value added at the entry of the sliding window of N values will thus be
 * reduced to 1/e or 36.7% after N terms have been added. After a second batch,
 * it will only be 1/e^2, or 13.5%, and so on. So practically speaking, each
 * old period of N values represents only a quickly fading ratio of the global
 * sum :
 *
 *   period    ratio
 *     1       36.7%
 *     2       13.5%
 *     3       4.98%
 *     4       1.83%
 *     5       0.67%
 *     6       0.25%
 *     7       0.09%
 *     8       0.033%
 *     9       0.012%
 *    10       0.0045%
 *
 * So after 10N samples, the initial value has already faded out by a factor of
 * 22026, which is quite fast. If the sliding window is 1024 samples wide, it
 * means that a sample will only count for 1/22k of its initial value after 10k
 * samples went after it, which results in half of the value it would represent
 * using an arithmetic mean. The benefit of this method is that it's very cheap
 * in terms of computations when N is a power of two. This is very well suited
 * to record response times as large values will fade out faster than with an
 * arithmetic mean and will depend on sample count and not time.
 *
 * Demonstrating all the above assumptions with maths instead of a program is
 * left as an exercise for the reader.
 */

/* Adds sample value <v> to sliding window sum <sum> configured for <n> samples.
 * The sample is returned. Better if <n> is a power of two.
 */
static inline unsigned int swrate_add(unsigned int *sum, unsigned int n, unsigned int v)
{
	return *sum = *sum - (*sum + n - 1) / n + v;
}

/* Returns the average sample value for the sum <sum> over a sliding window of
 * <n> samples. Better if <n> is a power of two. It must be the same <n> as the
 * one used above in all additions.
 */
static inline unsigned int swrate_avg(unsigned int sum, unsigned int n)
{
	return (sum + n - 1) / n;
}

#endif /* _PROTO_FREQ_CTR_H */

/*
 * Local variables:
 *  c-indent-level: 8
 *  c-basic-offset: 8
 * End:
  */
