/*
 * Copyright (C) 2013-2018 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * This code is a complete clean re-write of the stress tool by
 * Colin Ian King <colin.king@canonical.com> and attempts to be
 * backwardly compatible with the stress tool by Amos Waterland
 * <apw@rossby.metr.ou.edu> but has more stress tests and more
 * functionality.
 *
 */
#include "stress-ng.h"

#if defined(__linux__)
#include <sys/timerfd.h>

static double rate_ns;
#endif

#define COUNT_MAX		(256)

/*
 *  stress_set_timerfd_freq()
 *	set timer frequency from given option
 */
int stress_set_timerfd_freq(const char *opt)
{
	uint64_t timerfd_freq;

	timerfd_freq = get_uint64(opt);
	check_range("timerfd-freq", timerfd_freq,
		MIN_TIMERFD_FREQ, MAX_TIMERFD_FREQ);
	return set_setting("timerfd-freq", TYPE_ID_UINT64, &timerfd_freq);
}

#if defined(__linux__)

/*
 *  stress_timerfd_set()
 *	set timerfd, ensure it is never zero
 */
static void stress_timerfd_set(struct itimerspec *timer)
{
	double rate;

	if (g_opt_flags & OPT_FLAGS_TIMERFD_RAND) {
		/* Mix in some random variation */
		double r = ((double)(mwc32() % 10000) - 5000.0) / 40000.0;
		rate = rate_ns + (rate_ns * r);
	} else {
		rate = rate_ns;
	}

	timer->it_value.tv_sec = (time_t)rate / 1000000000;
	timer->it_value.tv_nsec = (suseconds_t)rate % 1000000000;

	if (timer->it_value.tv_sec == 0 &&
	    timer->it_value.tv_nsec < 1)
		timer->it_value.tv_nsec = 1;

	timer->it_interval.tv_sec = timer->it_value.tv_sec;
	timer->it_interval.tv_nsec = timer->it_value.tv_nsec;
}

/*
 *  stress_timerfd
 *	stress timerfd
 */
static int stress_timerfd(const args_t *args)
{
	struct itimerspec timer;
	uint64_t timerfd_freq = DEFAULT_TIMERFD_FREQ;
	int timerfd, procfd, count = 0;
	char filename[PATH_MAX];

	if (!get_setting("timerfd-freq", &timerfd_freq)) {
		if (g_opt_flags & OPT_FLAGS_MAXIMIZE)
			timerfd_freq = MAX_TIMERFD_FREQ;
		if (g_opt_flags & OPT_FLAGS_MINIMIZE)
			timerfd_freq = MIN_TIMERFD_FREQ;
	}
	rate_ns = timerfd_freq ? 1000000000.0 / timerfd_freq : 1000000000.0;

	timerfd = timerfd_create(CLOCK_REALTIME, 0);
	if (timerfd < 0) {
		pr_fail_err("timerfd_create");
		(void)close(timerfd);
		return EXIT_FAILURE;
	}
	stress_timerfd_set(&timer);
	if (timerfd_settime(timerfd, 0, &timer, NULL) < 0) {
		pr_fail_err("timer_settime");
		(void)close(timerfd);
		return EXIT_FAILURE;
	}

	(void)snprintf(filename, sizeof(filename), "/proc/%d/fdinfo/%d",
		args->pid, timerfd);
	procfd = open(filename, O_RDONLY);

	do {
		int ret;
		uint64_t exp;
		struct itimerspec value;
		struct timeval timeout;
		fd_set rdfs;

		FD_ZERO(&rdfs);
		FD_SET(timerfd, &rdfs);
		timeout.tv_sec = 0;
		timeout.tv_usec = 500000;

		if (!g_keep_stressing_flag)
			break;
		ret = select(timerfd + 1, &rdfs, NULL, NULL, &timeout);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			pr_fail_err("select");
			break;
		}
		if (ret < 1)
			continue; /* Timeout */

		ret = read(timerfd, &exp, sizeof exp);
		if (ret < 0) {
			pr_fail_err("timerfd read");
			break;
		}
		if (timerfd_gettime(timerfd, &value) < 0) {
			pr_fail_err("timerfd_gettime");
			break;
		}
		if (g_opt_flags & OPT_FLAGS_TIMERFD_RAND) {
			stress_timerfd_set(&timer);
			if (timerfd_settime(timerfd, 0, &timer, NULL) < 0) {
				pr_fail_err("timer_settime");
				break;
			}
		}

		/*
		 *  Periodically read /proc/$pid/fdinfo/$timerfd,
		 *  we don't care about failures, we just want to
		 *  exercise this interface
		 */
		if (LIKELY(procfd > -1) && UNLIKELY(count++ >= COUNT_MAX)) {
			char buffer[4096];

			ret = lseek(procfd, 0, SEEK_SET);
			if (LIKELY(ret == 0)) {
				ret = read(procfd, buffer, sizeof(buffer));
				(void)ret;
			}
			count = 0;
		}
		inc_counter(args);
	} while (keep_stressing());

	(void)close(timerfd);
	if (procfd > -1)
		(void)close(procfd);

	return EXIT_SUCCESS;
}

stressor_info_t stress_timerfd_info = {
	.stressor = stress_timerfd,
	.class = CLASS_INTERRUPT | CLASS_OS
};
#else
stressor_info_t stress_timerfd_info = {
	.stressor = stress_not_implemented,
	.class = CLASS_INTERRUPT | CLASS_OS
};
#endif
