/*
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

#if defined(HAVE_LIB_PTHREAD) && defined(__linux__)

#define SYS_BUF_SZ		(4096)
#define MAX_READ_THREADS	(4)
#define DRAIN_DELAY_US		(50000)
#define DURATION_PER_SYSFS_FILE	(40000)

static sigset_t set;
static shim_pthread_spinlock_t lock;
static char *sysfs_path;
static uint32_t mixup;
static volatile bool segv_abort = false;
static volatile bool drain_kmsg = false;
static sigjmp_buf jmp_env;
static char dummy_path[] = "/sys/kernel/notes";

typedef struct ctxt {
	const args_t *args;
	char *badbuf;
	bool writeable;
	int kmsgfd;
} ctxt_t;

static void MLOCKED_TEXT stress_segv_handler(int dummy)
{
	(void)dummy;

	segv_abort = true;
	siglongjmp(jmp_env, 1);
}

static uint32_t path_sum(const char *path)
{
	const char *ptr = path;
	register uint32_t sum = mixup;

	while (*ptr) {
		sum <<= 1;
		sum += *(ptr++);
	}

	return sum;
}

static int mixup_sort(const struct dirent **d1, const struct dirent **d2)
{
	uint32_t s1, s2;

	s1 = path_sum((*d1)->d_name);
	s2 = path_sum((*d2)->d_name);

	if (s1 == s2)
		return 0;
	return (s1 < s2) ? -1 : 1;
}

/*
 *  stress_kmsg_drain()
 *	drain message buffer, return true if we need
 *	to drain lots of backed up messages because
 *	the stressor is spamming the kmsg log
 */
static bool stress_kmsg_drain(const int fd)
{
	int count = 0;

	if (fd == -1)
		return false;

	for (;;) {
		ssize_t ret;
		char buffer[1024];

		ret = read(fd, buffer, sizeof(buffer));
		if (ret <= 0)
			break;

		count += ret;
	}
	return count > 0;
}

/*
 *  stress_sys_rw()
 *	read a proc file
 */
static inline bool stress_sys_rw(const ctxt_t *ctxt)
{
	int fd;
	ssize_t i = 0, ret;
	char buffer[SYS_BUF_SZ];
	char path[PATH_MAX];
	const args_t *args = ctxt->args;
	const double threshold = 0.2;

	while (g_keep_stressing_flag && !segv_abort) {
		double t_start;

		ret = shim_pthread_spin_lock(&lock);
		if (ret)
			return false;
		(void)shim_strlcpy(path, sysfs_path, sizeof(path));
		(void)shim_pthread_spin_unlock(&lock);

		if (!*path || !g_keep_stressing_flag)
			break;

		t_start = time_now();
		if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0)
			goto next;
		if (time_now() - t_start > threshold) {
			(void)close(fd);
			goto next;
		}

		/*
		 *  Multiple randomly sized reads
		 */
		while (i < (4096 * SYS_BUF_SZ)) {
			ssize_t sz = 1 + (mwc32() % (sizeof(buffer) - 1));
			if (!g_keep_stressing_flag)
				break;
			ret = read(fd, buffer, sz);
			if (ret < 0)
				break;
			if (ret < sz)
				break;
			i += sz;

			if (stress_kmsg_drain(ctxt->kmsgfd)) {
				drain_kmsg = true;
				(void)close(fd);
				goto drain;
			}
			if (time_now() - t_start > threshold) {
				(void)close(fd);
				goto next;
			}
		}

		/* file stat should be OK if we've just opened it */
		if (g_opt_flags & OPT_FLAGS_VERIFY) {
			struct stat buf;

			if (fstat(fd, &buf) < 0) {
				pr_fail_err("stat");
			} else {
#if 0
				if ((buf.st_mode & S_IROTH) == 0) {
					pr_fail("%s: read access failed on %s which "
						"could be opened, errno=%d (%s)\n",
					args->name, path, errno, strerror(errno));
				}
#endif
			}
		}
		(void)close(fd);

		if (time_now() - t_start > threshold)
			goto next;
		if ((fd = open(path, O_RDONLY | O_NONBLOCK)) < 0)
			goto next;
		/*
		 *  Zero sized reads
		 */
		ret = read(fd, buffer, 0);
		if (ret < 0)
			goto err;
		if (time_now() - t_start > threshold)
			goto next;
		if (stress_kmsg_drain(ctxt->kmsgfd)) {
			drain_kmsg = true;
			(void)close(fd);
			goto drain;
		}

		/*
		 *  Bad read buffer
		 */
		if (ctxt->badbuf) {
			ret = read(fd, ctxt->badbuf, SYS_BUF_SZ);
			if (ret < 0)
				goto err;
		}
		if (stress_kmsg_drain(ctxt->kmsgfd)) {
			drain_kmsg = true;
			(void)close(fd);
			goto drain;
		}
err:
		(void)close(fd);
		if (time_now() - t_start > threshold)
			goto next;

		/*
		 *  We only attempt writes if we are not
		 *  root
		 */
		if (ctxt->writeable) {
			/*
			 *  Zero sized writes
			 */
			if ((fd = open(path, O_WRONLY | O_NONBLOCK)) < 0)
				goto next;
			ret = write(fd, buffer, 0);
			(void)ret;
			(void)close(fd);

			if (time_now() - t_start > threshold)
				goto next;
		}
next:
		if (stress_kmsg_drain(ctxt->kmsgfd)) {
			drain_kmsg = true;
			goto drain;
		}

		if (drain_kmsg) {
drain:
			shim_usleep(DRAIN_DELAY_US);
			continue;
		}
	}
	return false;
}

/*
 *  stress_sys_rw_thread
 *	keep exercising a sysfs entry until
 *	controlling thread triggers an exit
 */
static void *stress_sys_rw_thread(void *ctxt_ptr)
{
	static void *nowt = NULL;
	uint8_t stack[SIGSTKSZ + STACK_ALIGNMENT];
	ctxt_t *ctxt = (ctxt_t *)ctxt_ptr;

	/*
	 *  Block all signals, let controlling thread
	 *  handle these
	 */
	(void)sigprocmask(SIG_BLOCK, &set, NULL);

	/*
	 *  According to POSIX.1 a thread should have
	 *  a distinct alternative signal stack.
	 *  However, we block signals in this thread
	 *  so this is probably just totally unncessary.
	 */
	(void)memset(stack, 0, sizeof(stack));
	if (stress_sigaltstack(stack, SIGSTKSZ) < 0)
		return &nowt;

	while (g_keep_stressing_flag && !segv_abort)
		stress_sys_rw(ctxt);

	return &nowt;
}

/*
 *  stress_sys_skip()
 *	skip paths that are known to cause issues
 */
static bool stress_sys_skip(const char *path)
{
	/*
	 *  Can OOPS on Azure when reading
	 *  "/sys/devices/LNXSYSTM:00/LNXSYBUS:00/PNP0A03:00/device:07/" \
	 *  "VMBUS:01/99221fa0-24ad-11e2-be98-001aa01bbf6e/channels/4/read_avail"
	 */
	if (strstr(path, "PNP0A03") && strstr(path, "VMBUS"))
		return true;
	/*
	 *  Has been known to cause issues on s390x
	 *
	if (strstr(path, "virtio0/block") && strstr(path, "cache_type"))
		return true;
	 */
	return false;
}

/*
 *  stress_sys_dir()
 *	read directory
 */
static void stress_sys_dir(
	const ctxt_t *ctxt,
	const char *path,
	const bool recurse,
	const int depth)
{
	struct dirent **dlist;
	const args_t *args = ctxt->args;
	const mode_t flags = S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;
	int i, n;

	if (!g_keep_stressing_flag || segv_abort)
		return;

	/* Don't want to go too deep */
	if (depth > 20)
		return;

	mixup = mwc32();
	dlist = NULL;
	n = scandir(path, &dlist, NULL, mixup_sort);
	if (n <= 0)
		goto done;

	for (i = 0; (i < n) && !segv_abort; i++) {
		int ret;
		struct stat buf;
		char filename[PATH_MAX];
		char tmp[PATH_MAX];
		struct dirent *d = dlist[i];

		if (!keep_stressing())
			break;
		if (is_dot_filename(d->d_name))
			continue;

		(void)snprintf(tmp, sizeof(tmp), "%s/%s", path, d->d_name);
		if (stress_sys_skip(tmp))
			continue;

		switch (d->d_type) {
		case DT_DIR:
			if (!recurse)
				continue;

			ret = stat(tmp, &buf);
			if (ret < 0)
				continue;
			if ((buf.st_mode & flags) == 0)
				continue;

			inc_counter(args);
			stress_sys_dir(ctxt, tmp, recurse, depth + 1);
			break;
		case DT_REG:
			ret = stat(tmp, &buf);
			if (ret < 0)
				continue;

			if ((buf.st_mode & flags) == 0)
				continue;

			ret = shim_pthread_spin_lock(&lock);
			if (!ret) {
				(void)shim_strlcpy(filename, tmp, sizeof(filename));
				sysfs_path = filename;
				(void)shim_pthread_spin_unlock(&lock);
				drain_kmsg = false;
				shim_usleep(DURATION_PER_SYSFS_FILE);
				if (segv_abort)
					break;
				inc_counter(args);
			}
			break;
		default:
			break;
		}
	}
done:
	if (dlist) {
		for (i = 0; i < n; i++)
			free(dlist[i]);
		free(dlist);
	}
}


/*
 *  stress_sysfs
 *	stress reading all of /sys
 */
static int stress_sysfs(const args_t *args)
{
	size_t i;
	pthread_t pthreads[MAX_READ_THREADS];
	int rc, ret[MAX_READ_THREADS];
	ctxt_t ctxt;

	memset(&ctxt, 0, sizeof(ctxt));
	rc = sigsetjmp(jmp_env, 1);
	if (rc) {
		pr_err("%s: A SIGSEGV occurred while exercising %s, aborting\n",
			args->name, sysfs_path);
		return EXIT_FAILURE;
	}
	if (stress_sighandler(args->name, SIGSEGV, stress_segv_handler, NULL) < 0)
		return EXIT_FAILURE;

	sysfs_path = dummy_path;

	ctxt.args = args;
	ctxt.writeable = (geteuid() != 0);
	ctxt.kmsgfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	(void)stress_kmsg_drain(ctxt.kmsgfd);

	rc = shim_pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
	if (rc) {
		pr_inf("%s: pthread_spin_init failed, errno=%d (%s)\n",
			args->name, rc, strerror(rc));
		return EXIT_NO_RESOURCE;
	}

	(void)memset(ret, 0, sizeof(ret));

	ctxt.badbuf = mmap(NULL, SYS_BUF_SZ, PROT_READ,
			MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ctxt.badbuf == MAP_FAILED) {
		pr_inf("%s: mmap failed: errno=%d (%s)\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}

	for (i = 0; i < MAX_READ_THREADS; i++) {
		ret[i] = pthread_create(&pthreads[i], NULL,
				stress_sys_rw_thread, &ctxt);
	}

	do {
		stress_sys_dir(&ctxt, "/sys", true, 0);
	} while (keep_stressing() && !segv_abort);

	rc = shim_pthread_spin_lock(&lock);
	if (rc) {
		pr_dbg("%s: failed to lock spin lock for sysfs_path\n", args->name);
	} else {
		sysfs_path = "";
		rc = shim_pthread_spin_unlock(&lock);
		(void)rc;
	}

	/* Forcefully kill threads */
	for (i = 0; i < MAX_READ_THREADS; i++) {
		if (ret[i] == 0)
			(void)pthread_kill(pthreads[i], SIGHUP);
	}

	for (i = 0; i < MAX_READ_THREADS; i++) {
		if (ret[i] == 0)
			(void)pthread_join(pthreads[i], NULL);
	}
	if (ctxt.kmsgfd != -1)
		(void)close(ctxt.kmsgfd);
	(void)munmap(ctxt.badbuf, SYS_BUF_SZ);
	(void)shim_pthread_spin_destroy(&lock);

	return EXIT_SUCCESS;
}

stressor_info_t stress_sysfs_info = {
	.stressor = stress_sysfs,
	.class = CLASS_OS
};
#else
stressor_info_t stress_sysfs_info = {
	.stressor = stress_not_implemented,
	.class = CLASS_OS
};
#endif
