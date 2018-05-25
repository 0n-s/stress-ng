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
#include <sys/sysinfo.h>
#include <utime.h>
#include <sys/ptrace.h>
#include <sys/vfs.h>
#if defined(__NR_ustat)
#include <ustat.h>
#endif
#endif

#include <poll.h>
#include <termios.h>

typedef void *(*bad_addr_t)(const args_t *args);
typedef int (*bad_syscall_t)(void *addr);

static uint8_t *ro_page;
static uint8_t *rw_page;

static const int sigs[] = {
#if defined(SIGILL)
	SIGILL,
#endif
#if defined(SIGTRAP)
	SIGTRAP,
#endif
#if defined(SIGFPE)
	SIGFPE,
#endif
#if defined(SIGBUS)
	SIGBUS,
#endif
#if defined(SIGSEGV)
	SIGSEGV,
#endif
#if defined(SIGIOT)
	SIGIOT,
#endif
#if defined(SIGEMT)
	SIGEMT,
#endif
#if defined(SIGALRM)
	SIGALRM,
#endif
#if defined(SIGINT)
	SIGINT,
#endif
#if defined(SIGHUP)
	SIGHUP
#endif
};

/*
 *  limit_procs()
 *	try to limit child resources
 */
static void limit_procs(const int procs)
{
#if defined(RLIMIT_CPU) || defined(RLIMIT_NPROC)
	struct rlimit lim;
#endif

#if defined(RLIMIT_CPU)
	lim.rlim_cur = 1;
	lim.rlim_max = 1;
	(void)setrlimit(RLIMIT_CPU, &lim);
#endif
#if defined(RLIMIT_NPROC)
	lim.rlim_cur = procs;
	lim.rlim_max = procs;
	(void)setrlimit(RLIMIT_NPROC, &lim);
#else
	(void)procs;
#endif
}

static void MLOCKED_TEXT stress_badhandler(int signum)
{
	(void)signum;

	_exit(1);
}

static void *unaligned_addr(const args_t *args)
{
	static uint64_t data[8] = { -1, -1, -1, -1, -1, -1, -1, -1 };
	uint8_t *ptr = (uint8_t *)data;

	(void)args;

	return ptr + 1;
}

static void *readonly_addr(const args_t *args)
{
	(void)args;

	return ro_page;
}

static void *null_addr(const args_t *args)
{
	(void)args;

	return NULL;
}

static void *text_addr(const args_t *args)
{
	(void)args;

	return (void *)&write;
}

static void *bad_end_addr(const args_t *args)
{
	return rw_page + args->page_size - 1;
}

static void *bad_max_addr(const args_t *args)
{
	(void)args;

	return (void *)~(uintptr_t)0;
}

static void *unmapped_addr(const args_t *args)
{
	return rw_page + args->page_size;
}

static bad_addr_t bad_addrs[] = {
	unaligned_addr,
	readonly_addr,
	null_addr,
	text_addr,
	bad_end_addr,
	bad_max_addr,
	unmapped_addr,
};

static int bad_access(void *addr)
{
	return access(addr, R_OK);
}

#if _POSIX_C_SOURCE >= 199309L && defined(CLOCK_REALTIME)
static int bad_clock_gettime(void *addr)
{
	return clock_gettime(CLOCK_REALTIME, addr);
}
#endif

static int bad_execve(void *addr)
{
	return execve(addr, addr, addr);
}


static int bad_getcwd(void *addr)
{
	if (getcwd(addr, 1024) == NULL)
		return -1;

	return 0;
}

#if defined(__linux__) && defined(__NR_get_mempolicy)
static int bad_get_mempolicy(void *addr)
{
	return shim_get_mempolicy(addr, addr, 1, (unsigned long)addr, 0);
}
#endif

#if (defined(__linux__) && defined(__NR_getrandom))
static int bad_getrandom(void *addr)
{
	return shim_getrandom(addr, 1024, 0);
}
#endif

#if defined(__linux__)
static int bad_getresgid(void *addr)
{
	return getresgid(addr, addr, addr);
}
#endif

#if defined(__linux__)
static int bad_getresuid(void *addr)
{
	return getresuid(addr, addr, addr);
}
#endif

static int bad_getrlimit(void *addr)
{
	return getrlimit(RLIMIT_CPU, addr);
}

static int bad_getrusage(void *addr)
{
	return getrusage(RUSAGE_SELF, addr);
}

static int bad_gettimeofday(void *addr)
{
	return gettimeofday(addr, addr);
}

#if defined(__linux__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
static int bad_getxattr(void *addr)
{
	return getxattr(addr, addr, addr, 32);
}
#endif

#if defined(TCGETS)
static int bad_ioctl(void *addr)
{
	return ioctl(0, TCGETS, addr);
}
#endif

#if defined(__linux__) && defined(__NR_migrate_pages)
static int bad_migrate_pages(void *addr)
{
	return shim_migrate_pages(getpid(), 1, addr, addr);
}
#endif

static int bad_mincore(void *addr)
{
	return shim_mincore(ro_page, 1, addr);
}

#if defined(__linux__) && defined(__NR_move_pages)
static int bad_move_pages(void *addr)
{
	return shim_move_pages(getpid(), 1, addr, addr, addr, 0);
}
#endif


#if _POSIX_C_SOURCE >= 199309L
static int bad_nanosleep(void *addr)
{
	return nanosleep(addr, addr);
}
#endif

static int bad_open(void *addr)
{
	int fd;

	fd = open(addr, O_RDONLY);
	if (fd != -1)
		(void)close(fd);

	return fd;
}

static int bad_pipe(void *addr)
{
	return pipe(addr);
}

#if defined(__linux__) && defined(PTRACE_GETREGS)
static int bad_ptrace(void *addr)
{
	return ptrace(PTRACE_GETREGS, getpid(), addr, addr);
}
#endif

static int bad_poll(void *addr)
{
	return poll(addr, 16, 1);
}

static int bad_read(void *addr)
{
	int fd, ret = 0;

	fd = open("/dev/zero", O_RDONLY);
	if (fd > -1) {
		ret = read(fd, addr, 1024);
		(void)close(fd);
	}
	return ret;
}

static int bad_readv(void *addr)
{
	int fd, ret = 0;

	fd = open("/dev/zero", O_RDONLY);
	if (fd > -1) {
		ret = readv(fd, addr, 32);
		(void)close(fd);
	}
	return ret;
}

static int bad_select(void *addr)
{
	int fd, ret = 0;

	fd = open("/dev/zero", O_RDONLY);
	if (fd > -1) {
		ret = select(fd, addr, addr, addr, addr);
		(void)close(fd);
	}
	return ret;
}

static int bad_stat(void *addr)
{
	return stat(".", addr);
}

#if defined(__linux__)
static int bad_statfs(void *addr)
{
	return statfs(".", addr);
}
#endif

#if defined(__linux__)
static int bad_sysinfo(void *addr)
{
	return sysinfo(addr);
}
#endif

static int bad_time(void *addr)
{
	return time(addr);
}

#if defined(HAVE_LIB_RT) && defined(__linux__)
static int bad_timer_create(void *addr)
{
	return timer_create(CLOCK_MONOTONIC, addr, addr);
}
#endif

static int bad_times(void *addr)
{
	return times(addr);
}

#if defined(__linux__) && defined(__NR_ustat)
static int bad_ustat(void *addr)
{
	dev_t dev = { 0 };

	return ustat(dev, addr);
}
#endif

#if defined(__linux__)
static int bad_utime(void *addr)
{
	return utime(addr, addr);
}
#endif

static int bad_wait(void *addr)
{
	return wait(addr);
}

static int bad_waitpid(void *addr)
{
	return waitpid(getpid(), addr, 0);
}

#if defined(HAVE_WAITID)
static int bad_waitid(void *addr)
{
	return waitid(P_PID, getpid(), addr, 0);
}
#endif

static int bad_write(void *addr)
{
	int fd, ret = 0;

	fd = open("/dev/null", O_WRONLY);
	if (fd > -1) {
		ret = write(fd, addr, 1024);
		(void)close(fd);
	}
	return ret;
}

static int bad_writev(void *addr)
{
	int fd, ret = 0;

	fd = open("/dev/zero", O_RDONLY);
	if (fd > -1) {
		ret = writev(fd, addr, 32);
		(void)close(fd);
	}
	return ret;
}

static bad_syscall_t bad_syscalls[] = {
	bad_access,
#if _POSIX_C_SOURCE >= 199309L && defined(CLOCK_REALTIME)
	bad_clock_gettime,
#endif
	bad_execve,
	bad_getcwd,
#if defined(__linux__) && defined(__NR_get_mempolicy)
	bad_get_mempolicy,
#endif
#if defined(__linux__) && defined(__NR_getrandom)
	bad_getrandom,
#endif
#if defined(__linux__)
	bad_getresgid,
#endif
#if defined(__linux__)
	bad_getresuid,
#endif
	bad_getrlimit,
	bad_getrusage,
	bad_gettimeofday,
#if defined(__linux__) && (defined(HAVE_SYS_XATTR_H) || defined(HAVE_ATTR_XATTR_H))
	bad_getxattr,
#endif
#if defined(TCGETS)
	bad_ioctl,
#endif
#if defined(__linux__) && defined(__NR_migrate_pages)
	bad_migrate_pages,
#endif
	bad_mincore,
#if defined(__linux__) && defined(__NR_move_pages)
	bad_move_pages,
#endif
#if _POSIX_C_SOURCE >= 199309L
	bad_nanosleep,
#endif
	bad_open,
	bad_pipe,
	bad_poll,
#if defined(__linux__) && defined(PTRACE_GETREGS)
	bad_ptrace,
#endif
	bad_read,
	bad_readv,
	bad_select,
	bad_stat,
#if defined(__linux__)
	bad_statfs,
#endif
#if defined(__linux__)
	bad_sysinfo,
#endif
	bad_time,
#if defined(HAVE_LIB_RT) && defined(__linux__)
	bad_timer_create,
#endif
	bad_times,
#if defined(__linux__) && defined(__NR_ustat)
	bad_ustat,
#endif
#if defined(__linux__)
	bad_utime,
#endif
	bad_wait,
	bad_waitpid,
#if defined(HAVE_WAITID)
	bad_waitid,
#endif
	bad_write,
	bad_writev,
};


/*
 *  Call a system call in a child context so we don't clobber
 *  the parent
 */
static inline int stress_do_syscall(
	const args_t *args,
	bad_syscall_t bad_syscall,
	void *addr)
{
	pid_t pid;
	int rc = 0;

	if (!keep_stressing())
		return 0;
	pid = fork();
	if (pid < 0) {
		_exit(EXIT_NO_RESOURCE);
	} else if (pid == 0) {
		struct itimerval it;
		size_t i;
		int ret;

		/* Try to limit child from spawning */
		limit_procs(2);

		/* We don't want bad ops clobbering this region */
		stress_unmap_shared();

		/* Drop all capabilities */
		if (stress_drop_capabilities(args->name) < 0) {
			_exit(EXIT_NO_RESOURCE);
		}
		for (i = 0; i < SIZEOF_ARRAY(sigs); i++) {
			if (stress_sighandler(args->name, sigs[i], stress_badhandler, NULL) < 0)
				_exit(EXIT_FAILURE);
		}

		(void)setpgid(0, g_pgrp);
		stress_parent_died_alarm();

		/*
		 * Force abort if we take too long
		 */
		it.it_interval.tv_sec = 0;
		it.it_interval.tv_usec = 100000;
		it.it_value.tv_sec = 0;
		it.it_value.tv_usec = 100000;
		if (setitimer(ITIMER_REAL, &it, NULL) < 0) {
			pr_fail_dbg("setitimer");
			_exit(EXIT_NO_RESOURCE);
		}

		ret = bad_syscall(addr);
		if (ret < 0)
			ret = errno;
		_exit(ret);
	} else {
		int ret, status;

		ret = waitpid(pid, &status, 0);
		if (ret < 0) {
			if (errno != EINTR)
				pr_dbg("%s: waitpid(): errno=%d (%s)\n",
					args->name, errno, strerror(errno));
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, &status, 0);

		}
		rc = WEXITSTATUS(status);

		inc_counter(args);
	}
	return rc;
}

/*
 *  stress_sysbadaddr
 *	stress system calls with bad addresses
 */
int stress_sysbadaddr(const args_t *args)
{
	pid_t pid;
	size_t page_size = args->page_size;

	ro_page = mmap(NULL, page_size, PROT_READ,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (ro_page == MAP_FAILED) {
		pr_inf("%s: cannot mmap anonymous read-only page: "
		       "errno=%d (%s)\n", args->name,errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}
	rw_page = mmap(NULL, page_size << 1, PROT_READ,
		MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	if (rw_page == MAP_FAILED) {
		(void)munmap(ro_page, page_size);
		pr_inf("%s: cannot mmap anonymous read-write page: "
		       "errno=%d (%s)\n", args->name,errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}
	/*
	 * Unmap last page, so we know we have an unmapped
	 * page following the r/w page
	 */
	(void)munmap(rw_page + page_size, page_size);

again:
	if (!keep_stressing())
		return EXIT_SUCCESS;
	pid = fork();
	if (pid < 0) {
		if (errno == EAGAIN)
			goto again;
		pr_err("%s: fork failed: errno=%d: (%s)\n",
			args->name, errno, strerror(errno));
	} else if (pid > 0) {
		int status, ret;

		(void)setpgid(pid, g_pgrp);
		/* Parent, wait for child */
		ret = waitpid(pid, &status, 0);
		if (ret < 0) {
			if (errno != EINTR)
				pr_dbg("%s: waitpid(): errno=%d (%s)\n",
					args->name, errno, strerror(errno));
			(void)kill(pid, SIGTERM);
			(void)kill(pid, SIGKILL);
			(void)waitpid(pid, &status, 0);
		} else if (WIFSIGNALED(status)) {
			pr_dbg("%s: child died: %s (instance %d)\n",
				args->name, stress_strsignal(WTERMSIG(status)),
				args->instance);
			/* If we got killed by OOM killer, re-start */
			if (WTERMSIG(status) == SIGKILL) {
				if (g_opt_flags & OPT_FLAGS_OOMABLE) {
					log_system_mem_info();
					pr_dbg("%s: assuming killed by OOM "
						"killer, bailing out "
						"(instance %d)\n",
						args->name, args->instance);
					_exit(0);
				} else {
					log_system_mem_info();
					pr_dbg("%s: assuming killed by OOM "
						"killer, restarting again "
						"(instance %d)\n",
						args->name, args->instance);
					goto again;
				}
			}
		}
	} else if (pid == 0) {
		/* Child, wrapped to catch OOMs */
		if (!keep_stressing())
			_exit(0);

		(void)setpgid(0, g_pgrp);
		stress_parent_died_alarm();

		/* Make sure this is killable by OOM killer */
		set_oom_adjustment(args->name, true);

		do {
			size_t i;

			for (i = 0; i < SIZEOF_ARRAY(bad_syscalls); i++) {
				size_t j;

				for (j = 0; j < SIZEOF_ARRAY(bad_addrs); j++) {
					int ret;
					void *addr = bad_addrs[j](args);

					ret = stress_do_syscall(args, bad_syscalls[i], addr);
					(void)ret;
				}
			}
		} while (keep_stressing());
		_exit(EXIT_SUCCESS);
	}

	(void)munmap(rw_page, page_size);
	(void)munmap(ro_page, page_size);

	return EXIT_SUCCESS;
}
