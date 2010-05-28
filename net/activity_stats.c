/* net/activity_stats.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Author: Mike Chan (mike@android.com)
 */

#include <linux/proc_fs.h>
#include <net/net_namespace.h>

/*
 * Track transmission rates in buckets (power of 2).
 * 1,2,4,8...512 seconds.
 *
 * Buckets represent the count of network transmissions at least
 * N seconds apart, where N is 1 << bucket index.
 */
#define BUCKET_MAX 10

/* Track network activity frequency */
static unsigned long activity_stats[BUCKET_MAX];
static ktime_t last_transmit;
static DEFINE_SPINLOCK(activity_lock);

void activity_stats_update(void)
{
	int i;
	unsigned long flags;
	ktime_t now;
	s64 delta;

	spin_lock_irqsave(&activity_lock, flags);
	now = ktime_get();
	delta = ktime_to_ns(ktime_sub(now, last_transmit));

	for (i = BUCKET_MAX - 1; i >= 0; i--) {
		/*
		 * Check if the time delta between network activity is within the
		 * minimum bucket range.
		 */
		if (delta < (1000000000ULL << i))
			continue;

		activity_stats[i]++;
		last_transmit = now;
		break;
	}
	spin_unlock_irqrestore(&activity_lock, flags);
}

static int activity_stats_read_proc(char *page, char **start, off_t off,
					int count, int *eof, void *data)
{
	int i;
	char *p = page;

	p += snprintf(p, PAGE_SIZE, "Min Bucket(sec) Count\n");
	for (i = 0; i < BUCKET_MAX; i++) {
		p += snprintf(p, PAGE_SIZE, "%15d %lu\n", 1 << i, activity_stats[i]);
	}
	return p - page;
}

void __init activity_stats_init(void)
{
	create_proc_read_entry("activity", S_IRUGO,
			init_net.proc_net_stat, activity_stats_read_proc, NULL);
}

core_initcall(activity_stats_init);

