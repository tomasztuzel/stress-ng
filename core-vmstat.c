/*
 * Copyright (C)      2021 Canonical, Ltd.
 * Copyright (C) 2021-2023 Colin Ian King
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
 */
#include "stress-ng.h"
#include "core-pragma.h"
#include "core-thermal-zone.h"

#if defined(HAVE_MACH_MACH_H)
#include <mach/mach.h>
#endif

#if defined(HAVE_MACH_MACHINE_H)
#include <mach/machine.h>
#endif

#if defined(HAVE_MACH_VM_STATISTICS_H)
#include <mach/vm_statistics.h>
#endif

#if defined(HAVE_SYS_SYSCTL_H)
STRESS_PRAGMA_PUSH
STRESS_PRAGMA_WARN_CPP_OFF
#include <sys/sysctl.h>
STRESS_PRAGMA_POP
#endif

#include <float.h>

#if defined(HAVE_SYS_SYSMACROS_H)
#include <sys/sysmacros.h>
#endif

#if defined(HAVE_SYS_VMMETER_H)
#include <sys/vmmeter.h>
#endif

#if defined(HAVE_UVM_UVM_EXTERN_H)
#include <uvm/uvm_extern.h>
#endif

#if defined(HAVE_MNTENT_H)
#include <mntent.h>
#endif

/* vmstat information */
typedef struct {			/* vmstat column */
	uint64_t	procs_running;	/* r */
	uint64_t	procs_blocked;	/* b */
	uint64_t	swap_total;	/* swpd info, total */
	uint64_t	swap_free;	/* swpd info, free */
	uint64_t	swap_used;	/* swpd used = total - free */
	uint64_t	memory_free;	/* free */
	uint64_t	memory_buff;	/* buff */
	uint64_t	memory_cached;	/* cached */
	uint64_t	memory_reclaimable; /* reclaimabled cached */
	uint64_t	swap_in;	/* si */
	uint64_t	swap_out;	/* so */
	uint64_t	block_in;	/* bi */
	uint64_t	block_out;	/* bo */
	uint64_t	interrupt;	/* in */
	uint64_t	context_switch;	/* cs */
	uint64_t	user_time;	/* us */
	uint64_t	system_time;	/* sy */
	uint64_t	idle_time;	/* id */
	uint64_t	wait_time;	/* wa */
	uint64_t	stolen_time;	/* st */
} stress_vmstat_t;

/* iostat information, from /sys/block/$dev/stat */
typedef struct {
	uint64_t	read_io;	/* number of read I/Os processed */
	uint64_t	read_merges;	/* number of read I/Os merged with in-queue I/O */
	uint64_t	read_sectors;	/* number of sectors read */
	uint64_t	read_ticks;	/* total wait time for read requests */
	uint64_t	write_io;	/* number of write I/Os processed */
	uint64_t	write_merges;	/* number of write I/Os merged with in-queue I/O */
	uint64_t	write_sectors;	/* number of sectors written */
	uint64_t	write_ticks;	/* total wait time for write requests */
	uint64_t	in_flight;	/* number of I/Os currently in flight */
	uint64_t	io_ticks;	/* total time this block device has been active */
	uint64_t	time_in_queue;	/* total wait time for all requests */
	uint64_t	discard_io;	/* number of discard I/Os processed */
	uint64_t	discard_merges;	/* number of discard I/Os merged with in-queue I/O */
	uint64_t	discard_sectors;/* number of sectors discarded */
	uint64_t	discard_ticks;	/* total wait time for discard requests */
} stress_iostat_t;

static int32_t vmstat_delay = 0;
static int32_t thermalstat_delay = 0;
static int32_t iostat_delay = 0;

#if defined(__FreeBSD__)
static void freebsd_get_cpu_time(
	uint64_t *user_time,
	uint64_t *system_time,
	uint64_t *idle_time)
{
	const int cpus = stress_bsd_getsysctl_int("kern.smp.cpus");
	long int *vals;
	int i;

	*user_time = 0;
	*system_time = 0;
	*idle_time = 0;

	vals = (long int *)calloc(cpus * 5, sizeof(*vals));
	if (!vals)
		return;

	if (stress_bsd_getsysctl("kern.cp_times", vals, cpus * 5 * sizeof(*vals)) < 0)
		return;
	for (i = 0; i < cpus * 5; i += 5) {
		*user_time += vals[i];
		*system_time += vals[i + 2];
		*idle_time += vals[i + 4];
	}
	free(vals);
}
#endif

#if defined(__NetBSD__)
static void netbsd_get_cpu_time(
	uint64_t *user_time,
	uint64_t *system_time,
	uint64_t *idle_time)
{
	long int vals[5];

	*user_time = 0;
	*system_time = 0;
	*idle_time = 0;

	if (stress_bsd_getsysctl("kern.cp_time", vals, sizeof(vals)) < 0)
		return;
	*user_time = vals[0];
	*system_time = vals[2];
	*idle_time = vals[4];;
}
#endif

static int stress_set_generic_stat(
	const char *const opt,
	const char *name,
	int32_t *delay)
{
	*delay = stress_get_int32(opt);
        if ((*delay < 1) || (*delay > 3600)) {
                (void)fprintf(stderr, "%s must in the range 1 to 3600.\n", name);
                _exit(EXIT_FAILURE);
        }
	return 0;
}

int stress_set_vmstat(const char *const opt)
{
	return stress_set_generic_stat(opt, "vmstat", &vmstat_delay);
}

int stress_set_thermalstat(const char *const opt)
{
	g_opt_flags |= OPT_FLAGS_TZ_INFO;
	return stress_set_generic_stat(opt, "thermalstat", &thermalstat_delay);
}

int stress_set_iostat(const char *const opt)
{
	return stress_set_generic_stat(opt, "iostat", &iostat_delay);
}

/*
 *  stress_find_mount_dev()
 *	find the path of the device that the file is located on
 */
char *stress_find_mount_dev(const char *name)
{
#if defined(__linux__) && 	\
    defined(HAVE_GETMNTENT) &&	\
    defined(HAVE_ENDMNTENT) &&	\
    defined(HAVE_SETMNTENT)
	static char dev_path[PATH_MAX];
	struct stat statbuf;
	dev_t dev;
	FILE *mtab_fp;
	struct mntent *mnt;

	if (stat(name, &statbuf) < 0)
		return NULL;

	/* Cater for UBI char mounts */
	if (S_ISBLK(statbuf.st_mode) || S_ISCHR(statbuf.st_mode))
		dev = statbuf.st_rdev;
	else
		dev = statbuf.st_dev;

	/* Try /proc/mounts then /etc/mtab */
	mtab_fp = setmntent("/proc/mounts", "r");
	if (!mtab_fp) {
		mtab_fp = setmntent("/etc/mtab", "r");
		if (!mtab_fp)
			return NULL;
	}

	while ((mnt = getmntent(mtab_fp))) {
		if ((!strcmp(name, mnt->mnt_dir)) ||
		    (!strcmp(name, mnt->mnt_fsname)))
			break;

		if ((mnt->mnt_fsname[0] == '/') &&
		    (stat(mnt->mnt_fsname, &statbuf) == 0) &&
		    (statbuf.st_rdev == dev))
			break;

		if ((stat(mnt->mnt_dir, &statbuf) == 0) &&
		    (statbuf.st_dev == dev))
			break;
	}
	(void)endmntent(mtab_fp);

	if (!mnt)
		return NULL;
	if (!mnt->mnt_fsname)
		return NULL;
	return realpath(mnt->mnt_fsname, dev_path);
#elif defined(HAVE_SYS_SYSMACROS_H)
	static char dev_path[PATH_MAX];
	struct stat statbuf;
	dev_t dev;
	DIR *dir;
	struct dirent *d;
	dev_t majdev;

	if (stat(name, &statbuf) < 0)
		return NULL;

	/* Cater for UBI char mounts */
	if (S_ISBLK(statbuf.st_mode) || S_ISCHR(statbuf.st_mode))
		dev = statbuf.st_rdev;
	else
		dev = statbuf.st_dev;

	majdev = makedev(major(dev), 0);

	dir = opendir("/dev");
	if (!dir)
		return NULL;

	while ((d = readdir(dir)) != NULL) {
		int ret;
		struct stat stat_buf;

		stress_mk_filename(dev_path, sizeof(dev_path), "/dev", d->d_name);
		ret = stat(dev_path, &stat_buf);
		if ((ret == 0) &&
		    (S_ISBLK(stat_buf.st_mode)) &&
		    (stat_buf.st_rdev == majdev)) {
			(void)closedir(dir);
			return dev_path;
		}
	}
	(void)closedir(dir);

	return NULL;
#else
	(void)name;

	return NULL;
#endif
}

static pid_t vmstat_pid;

#if defined(HAVE_SYS_SYSMACROS_H) &&	\
    defined(__linux__)

/*
 *  stress_iostat_iostat_name()
 *	from the stress-ng temp file path try to determine
 *	the iostat file /sys/block/$dev/stat for that file.
 */
static char *stress_iostat_iostat_name(
	char *iostat_name,
	const size_t iostat_name_len)
{
	char *temp_path, *dev, *ptr;
	struct stat statbuf;

	/* Resolve links */
	temp_path = realpath(stress_get_temp_path(), NULL);
	if (!temp_path)
		return NULL;

	/* Find device */
	dev = stress_find_mount_dev(temp_path);
	if (!dev)
		return NULL;

	/* Skip over leading /dev */
	if (!strncmp(dev, "/dev", 4))
		dev += 4;
	if (*dev == '/')
		dev++;

	ptr = dev + strlen(dev) - 1;

	/*
	 *  Try /dev/sda12, then /dev/sda1, then /dev/sda, then terminate
	 */
	while (ptr >= dev) {
		(void)snprintf(iostat_name, iostat_name_len, "/sys/block/%s/stat", dev);
		if (stat(iostat_name, &statbuf) == 0)
			return iostat_name;
		if (!isdigit(*ptr))
			break;
		*ptr = '\0';
		ptr--;
	}

	return NULL;
}

/*
 *  stress_read_iostat()
 *	read the stats from an iostat stat file, linux variant
 */
static void stress_read_iostat(const char *iostat_name, stress_iostat_t *iostat)
{
	FILE *fp;

	fp = fopen(iostat_name, "r");
	if (fp) {
		int ret;

		ret = fscanf(fp,
			    "%" PRIu64 " %" PRIu64
			    " %" PRIu64 " %" PRIu64
			    " %" PRIu64 " %" PRIu64
			    " %" PRIu64 " %" PRIu64
			    " %" PRIu64 " %" PRIu64
			    " %" PRIu64 " %" PRIu64
			    " %" PRIu64 " %" PRIu64
			    " %" PRIu64,
			&iostat->read_io, &iostat->read_merges,
			&iostat->read_sectors, &iostat->read_ticks,
			&iostat->write_io, &iostat->write_merges,
			&iostat->write_sectors, &iostat->write_ticks,
			&iostat->in_flight, &iostat->io_ticks,
			&iostat->time_in_queue,
			&iostat->discard_io, &iostat->discard_merges,
			&iostat->discard_sectors, &iostat->discard_ticks);
		(void)fclose(fp);

		if (ret != 15)
			(void)memset(iostat, 0, sizeof(*iostat));
	}
}

#define STRESS_IOSTAT_DELTA(field)					\
	iostat->field = ((iostat_current.field > iostat_prev.field) ?	\
	(iostat_current.field - iostat_prev.field) : 0)

/*
 *  stress_get_iostat()
 *	read and compute delta since last read of iostats
 */
static void stress_get_iostat(const char *iostat_name, stress_iostat_t *iostat)
{
	static stress_iostat_t iostat_prev;
	stress_iostat_t iostat_current;

	(void)memset(&iostat_current, 0, sizeof(iostat_current));
	stress_read_iostat(iostat_name, &iostat_current);
	STRESS_IOSTAT_DELTA(read_io);
	STRESS_IOSTAT_DELTA(read_merges);
	STRESS_IOSTAT_DELTA(read_sectors);
	STRESS_IOSTAT_DELTA(read_ticks);
	STRESS_IOSTAT_DELTA(write_io);
	STRESS_IOSTAT_DELTA(write_merges);
	STRESS_IOSTAT_DELTA(write_sectors);
	STRESS_IOSTAT_DELTA(write_ticks);
	STRESS_IOSTAT_DELTA(in_flight);
	STRESS_IOSTAT_DELTA(io_ticks);
	STRESS_IOSTAT_DELTA(time_in_queue);
	STRESS_IOSTAT_DELTA(discard_io);
	STRESS_IOSTAT_DELTA(discard_merges);
	STRESS_IOSTAT_DELTA(discard_sectors);
	STRESS_IOSTAT_DELTA(discard_ticks);
	(void)memcpy(&iostat_prev, &iostat_current, sizeof(iostat_prev));
}
#endif

#if defined(__linux__)
/*
 *  stress_next_field()
 *	skip to next field, returns false if end of
 *	string and/or no next field.
 */
static bool stress_next_field(char **str)
{
	char *ptr = *str;

	while (*ptr && *ptr != ' ')
		ptr++;

	if (!*ptr)
		return false;

	while (*ptr == ' ')
		ptr++;

	if (!*ptr)
		return false;

	*str = ptr;
	return true;
}

/*
 *  stress_read_vmstat()
 *	read vmstat statistics
 */
static void stress_read_vmstat(stress_vmstat_t *vmstat)
{
	FILE *fp;
	char buffer[1024];

	fp = fopen("/proc/stat", "r");
	if (fp) {
		while (fgets(buffer, sizeof(buffer), fp)) {
			char *ptr = buffer;

			if (!strncmp(buffer, "cpu ", 4))
				continue;
			if (!strncmp(buffer, "cpu", 3)) {
				if (!stress_next_field(&ptr))
					continue;
				/* user time */
				vmstat->user_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* user time nice */
				vmstat->user_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* system time */
				vmstat->system_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* idle time */
				vmstat->idle_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* iowait time */
				vmstat->wait_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* irq time, account in system time */
				vmstat->system_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* soft time, account in system time */
				vmstat->system_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* stolen time */
				vmstat->stolen_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* guest time, add to stolen stats */
				vmstat->stolen_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;

				/* guest_nice time, add to stolen stats */
				vmstat->stolen_time += (uint64_t)atoll(ptr);
				if (!stress_next_field(&ptr))
					continue;
			}

			if (!strncmp(buffer, "intr", 4)) {
				if (!stress_next_field(&ptr))
					continue;
				/* interrupts */
				vmstat->interrupt = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "ctxt", 4)) {
				if (!stress_next_field(&ptr))
					continue;
				/* context switches */
				vmstat->context_switch = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "procs_running", 13)) {
				if (!stress_next_field(&ptr))
					continue;
				/* processes running */
				vmstat->procs_running = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "procs_blocked", 13)) {
				if (!stress_next_field(&ptr))
					continue;
				/* procesess blocked */
				vmstat->procs_blocked = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "swap", 4)) {
				if (!stress_next_field(&ptr))
					continue;
				/* swap in */
				vmstat->swap_in = (uint64_t)atoll(ptr);

				if (!stress_next_field(&ptr))
					continue;
				/* swap out */
				vmstat->swap_out = (uint64_t)atoll(ptr);
			}
		}
		(void)fclose(fp);
	}

	fp = fopen("/proc/meminfo", "r");
	if (fp) {
		while (fgets(buffer, sizeof(buffer), fp)) {
			char *ptr = buffer;

			if (!strncmp(buffer, "MemFree", 7)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->memory_free = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "Buffers", 7)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->memory_buff = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "Cached", 6)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->memory_cached = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "KReclaimable", 12)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->memory_reclaimable = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "SwapTotal", 9)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->swap_total = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "SwapFree", 8)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->swap_free = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "SwapUsed", 8)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->swap_used = (uint64_t)atoll(ptr);
			}
		}
		(void)fclose(fp);

		if ((vmstat->swap_used == 0) &&
		    (vmstat->swap_free > 0) &&
		    (vmstat->swap_total > 0)) {
			vmstat->swap_used = vmstat->swap_total - vmstat->swap_free;
		}
	}

	fp = fopen("/proc/vmstat", "r");
	if (fp) {
		while (fgets(buffer, sizeof(buffer), fp)) {
			char *ptr = buffer;

			if (!strncmp(buffer, "pgpgin", 6)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->block_in = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "pgpgout", 7)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->block_out = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "pswpin", 6)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->swap_in = (uint64_t)atoll(ptr);
			}
			if (!strncmp(buffer, "pswpout", 7)) {
				if (!stress_next_field(&ptr))
					continue;
				vmstat->swap_out = (uint64_t)atoll(ptr);
			}
		}
		(void)fclose(fp);
	}
}
#elif defined(__FreeBSD__)
/*
 *  stress_read_vmstat()
 *	read vmstat statistics, FreeBSD variant, partially implemented
 */
static void stress_read_vmstat(stress_vmstat_t *vmstat)
{
#if defined(HAVE_SYS_VMMETER_H)
	struct vmtotal t;
#endif

	vmstat->interrupt = stress_bsd_getsysctl_uint64("vm.stats.sys.v_intr");
	vmstat->context_switch = stress_bsd_getsysctl_uint64("vm.stats.sys.v_swtch");
	vmstat->swap_in = stress_bsd_getsysctl_uint64("vm.stats.vm.v_swapin");
	vmstat->swap_out = stress_bsd_getsysctl_uint64("vm.stats.vm.v_swapout");
	vmstat->block_in = stress_bsd_getsysctl_uint64("vm.stats.vm.v_vnodepgsin");
	vmstat->block_out = stress_bsd_getsysctl_uint64("vm.stats.vm.v_vnodepgsin");
	vmstat->memory_free = (uint64_t)stress_bsd_getsysctl_uint32("vm.stats.vm.v_free_count");
	vmstat->memory_cached = (uint64_t)stress_bsd_getsysctl_uint("vm.stats.vm.v_cache_count");

	freebsd_get_cpu_time(&vmstat->user_time, &vmstat->system_time, &vmstat->idle_time);

#if defined(HAVE_SYS_VMMETER_H)
	if (stress_bsd_getsysctl("vm.vmtotal", &t, sizeof(t)) == 0) {
		vmstat->procs_running = t.t_rq - 1;
		vmstat->procs_blocked = t.t_dw + t.t_pw;
	}
#endif
}
#elif defined(__NetBSD__)
/*
 *  stress_read_vmstat()
 *	read vmstat statistics, NetBSD variant, partially implemented
 */
static void stress_read_vmstat(stress_vmstat_t *vmstat)
{
#if defined(HAVE_SYS_VMMETER_H)
	struct vmtotal t;
#endif
#if defined(HAVE_UVM_UVM_EXTERN_H)
	struct uvmexp_sysctl u;
#endif
	netbsd_get_cpu_time(&vmstat->user_time, &vmstat->system_time, &vmstat->idle_time);
#if defined(HAVE_UVM_UVM_EXTERN_H)
	if (stress_bsd_getsysctl("vm.uvmexp2", &u, sizeof(u)) == 0) {
		vmstat->memory_cached = u.filepages;	/* Guess */
		vmstat->interrupt = u.intrs;
		vmstat->context_switch = u.swtch;
		vmstat->swap_in = u.pgswapin;
		vmstat->swap_out = u.pgswapout;
		vmstat->swap_used = u.swpginuse;
		vmstat->memory_free = u.free;
	}
#endif

#if defined(HAVE_SYS_VMMETER_H)
	if (stress_bsd_getsysctl("vm.vmmeter", &t, sizeof(t)) == 0) {
		vmstat->procs_running = t.t_rq - 1;
		vmstat->procs_blocked = t.t_dw + t.t_pw;
	}
#endif
}
#elif defined(__APPLE__) &&		\
      defined(HAVE_SYS_SYSCTL_H) &&	\
      defined(HAVE_MACH_MACH_H) &&	\
      defined(HAVE_MACH_VM_STATISTICS_H)
/*
 *  stress_read_vmstat()
 *	read vmstat statistics, OS X variant, partially implemented
 */
static void stress_read_vmstat(stress_vmstat_t *vmstat)
{
	vm_statistics64_data_t vm_stat;
	struct xsw_usage xsu;
	mach_port_t host = mach_host_self();
	natural_t count = HOST_VM_INFO64_COUNT;
	size_t page_size = stress_get_page_size();
	int ret;

	(void)memset(&vm_stat, 0, sizeof(vmstat));
	ret = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm_stat, &count);
	if (ret >= 0) {
		vmstat->swap_in = vm_stat.pageins;
		vmstat->swap_out = vm_stat.pageouts;
		vmstat->memory_free = (page_size / 1024) * vm_stat.free_count;
	}
	ret = stress_bsd_getsysctl("vm.swapusage", &xsu, sizeof(xsu));
	if (ret >= 0) {
		vmstat->swap_total = xsu.xsu_total;
		vmstat->swap_used = xsu.xsu_used;
		vmstat->swap_free = xsu.xsu_avail;
	}
	vmstat->user_time = 0;
	vmstat->system_time= 0;
	vmstat->idle_time = 0;
	vmstat->wait_time = 0;
	vmstat->stolen_time = 0;
#if defined(HAVE_MACH_MACH_H) &&	\
    defined(PROCESSOR_CPU_LOAD_INFO) && \
    defined(CPU_STATE_USER) &&		\
    defined(CPU_STATE_SYSTEM) &&	\
    defined(CPU_STATE_IDLE)
	{
		natural_t pcount, pi_array_count;
		processor_info_array_t pi_array;

		ret = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &pcount, &pi_array, &pi_array_count);
		if (ret >= 0) {
			natural_t i;

			for (i = 0; i < pi_array_count; i++) {
				integer_t *cpu_ticks = &pi_array[i * CPU_STATE_MAX];

				vmstat->user_time += cpu_ticks[CPU_STATE_USER];
				vmstat->system_time += cpu_ticks[CPU_STATE_SYSTEM];
				vmstat->idle_time += cpu_ticks[CPU_STATE_IDLE];
			}
		}
	}
#endif
#if defined(HAVE_SYS_SYSCTL_H) &&	\
    defined(CTL_KERN) &&		\
    defined(KERN_PROC) &&		\
    defined(KERN_PROC_ALL) &&		\
    defined(SRUN)
	{
		size_t length;
		static const int name[] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0 };

		vmstat->procs_running = 0;
		vmstat->procs_blocked = 0;

		for (;;) {
			struct kinfo_proc * result;
			size_t i, n;

			ret = sysctl((int *)name, (sizeof(name)/sizeof(*name))-1, NULL,
				&length, NULL, 0);
			if (ret < 0)
				break;

			result = malloc(length);
			if (!result)
				break;

			ret = sysctl((int *)name, (sizeof(name)/sizeof(*name))-1, result,
				&length, NULL, 0);
			if (ret < 0) {
				free(result);
				break;
			}

			n = length / sizeof(struct kinfo_proc);
			for (i = 0; i < n; i++) {
				if (result[i].kp_proc.p_flag & P_SYSTEM)
					continue;
				switch (result[i].kp_proc.p_stat) {
				case SRUN:
					vmstat->procs_running++;
					break;
				default:
					vmstat->procs_blocked++;
					break;
				}
			}
			free(result);
			if (ret == 0)
				break;
		}
	}
#endif
}
#else
/*
 *  stress_read_vmstat()
 *	read vmstat statistics, no-op
 */
static void stress_read_vmstat(stress_vmstat_t *vmstat)
{
	(void)vmstat;
}
#endif

#define STRESS_VMSTAT_COPY(field)	vmstat->field = (vmstat_current.field)
#define STRESS_VMSTAT_DELTA(field)					\
	vmstat->field = ((vmstat_current.field > vmstat_prev.field) ?	\
	(vmstat_current.field - vmstat_prev.field) : 0)

/*
 *  stress_get_vmstat()
 *	collect vmstat data, zero for initial read
 */
static void stress_get_vmstat(stress_vmstat_t *vmstat)
{
	static stress_vmstat_t vmstat_prev;
	stress_vmstat_t vmstat_current;

	(void)memset(&vmstat_current, 0, sizeof(vmstat_current));
	(void)memset(vmstat, 0, sizeof(*vmstat));
	stress_read_vmstat(&vmstat_current);
	STRESS_VMSTAT_COPY(procs_running);
	STRESS_VMSTAT_COPY(procs_blocked);
	STRESS_VMSTAT_COPY(swap_total);
	STRESS_VMSTAT_COPY(swap_used);
	STRESS_VMSTAT_COPY(swap_free);
	STRESS_VMSTAT_COPY(memory_free);
	STRESS_VMSTAT_COPY(memory_buff);
	STRESS_VMSTAT_COPY(memory_cached);
	STRESS_VMSTAT_COPY(memory_reclaimable);
	STRESS_VMSTAT_DELTA(swap_in);
	STRESS_VMSTAT_DELTA(swap_out);
	STRESS_VMSTAT_DELTA(block_in);
	STRESS_VMSTAT_DELTA(block_out);
	STRESS_VMSTAT_DELTA(interrupt);
	STRESS_VMSTAT_DELTA(context_switch);
	STRESS_VMSTAT_DELTA(user_time);
	STRESS_VMSTAT_DELTA(system_time);
	STRESS_VMSTAT_DELTA(idle_time);
	STRESS_VMSTAT_DELTA(wait_time);
	STRESS_VMSTAT_DELTA(stolen_time);
	(void)memcpy(&vmstat_prev, &vmstat_current, sizeof(vmstat_prev));
}

#if defined(__linux__)
/*
 *  stress_get_tz_info()
 *	get temperature in degrees C from a thermal zone
 */
static double stress_get_tz_info(stress_tz_info_t *tz_info)
{
	double temp = 0.0;
	FILE *fp;
	char path[PATH_MAX];

	(void)snprintf(path, sizeof(path),
		"/sys/class/thermal/%s/temp",
		tz_info->path);

	if ((fp = fopen(path, "r")) != NULL) {
		if (fscanf(fp, "%lf", &temp) == 1)
			temp /= 1000.0;
		(void)fclose(fp);
	}
	return temp;
}
#endif

static void stress_zero_cpu_ghz(
	double *avg_ghz,
	double *min_ghz,
	double *max_ghz)
{
	*avg_ghz = 0.0;
	*min_ghz = 0.0;
	*max_ghz = 0.0;
}

#if defined(__linux__)
/*
 *  stress_get_cpu_ghz()
 *	get CPU frequencies in GHz
 */
static void stress_get_cpu_ghz(
	double *avg_ghz,
	double *min_ghz,
	double *max_ghz)
{
	struct dirent **cpu_list = NULL;
	int i, n_cpus, n = 0;
	double total_freq = 0.0;

	*min_ghz = DBL_MAX;
	*max_ghz = 0.0;

	n_cpus = scandir("/sys/devices/system/cpu", &cpu_list, NULL, alphasort);
	for (i = 0; i < n_cpus; i++) {
		const char *name = cpu_list[i]->d_name;

		if (!strncmp(name, "cpu", 3) && isdigit(name[3])) {
			char path[PATH_MAX];
			double freq;
			FILE *fp;

			(void)snprintf(path, sizeof(path),
				"/sys/devices/system/cpu/%s/cpufreq/scaling_cur_freq",
				name);
			if ((fp = fopen(path, "r")) != NULL) {
				if (fscanf(fp, "%lf", &freq) == 1) {
					if (freq >= 0.0) {
						total_freq += freq;
						if (*min_ghz > freq)
							*min_ghz = freq;
						if (*max_ghz < freq)
							*max_ghz = freq;
						n++;
					}
				}
				(void)fclose(fp);
			}
		}
		free(cpu_list[i]);
	}
	if (n_cpus > -1)
		free(cpu_list);

	if (n == 0) {
		stress_zero_cpu_ghz(avg_ghz, min_ghz, max_ghz);
	} else {
		*avg_ghz = (total_freq / n) * ONE_MILLIONTH;
		*min_ghz *= ONE_MILLIONTH;
		*max_ghz *= ONE_MILLIONTH;
	}
}
#elif defined(__FreeBSD__) ||	\
      defined(__APPLE__)
static void stress_get_cpu_ghz(
	double *avg_ghz,
	double *min_ghz,
	double *max_ghz)
{
	const int32_t ncpus = stress_get_processors_configured();
	int32_t i;
	double total_freq = 0.0;
	int n = 0;

	*min_ghz = DBL_MAX;
	*max_ghz = 0.0;

	for (i = 0; i < ncpus; i++) {
		double freq;
#if defined(__FreeBSD__)
		{
			char name[32];

			(void)snprintf(name, sizeof(name), "dev.cpu.%" PRIi32 ".freq", i);
			freq = (double)stress_bsd_getsysctl_uint(name) * ONE_THOUSANDTH;
		}
#endif
#if defined(__APPLE__)
		freq = (double)stress_bsd_getsysctl_uint64("hw.cpufrequency") * ONE_BILLIONTH;
#endif
		if (freq >= 0.0) {
			total_freq += freq;
			if (*min_ghz > freq)
				*min_ghz = freq;
			if (*max_ghz < freq)
				*max_ghz = freq;
			n++;
		}
	}
	if (n == 0) {
		stress_zero_cpu_ghz(avg_ghz, min_ghz, max_ghz);
	} else {
		*avg_ghz = (total_freq / n);
	}
}
#else
static void stress_get_cpu_ghz(
	double *avg_ghz,
	double *min_ghz,
	double *max_ghz)
{
	stress_zero_cpu_ghz(avg_ghz, min_ghz, max_ghz);
}
#endif

/*
 *  stress_vmstat_start()
 *	start vmstat statistics (1 per second)
 */
void stress_vmstat_start(void)
{
	stress_vmstat_t vmstat;
	size_t tz_num = 0;
	stress_tz_info_t *tz_info;
	int32_t vmstat_sleep, thermalstat_sleep, iostat_sleep;
	double t1, t2;
#if defined(HAVE_SYS_SYSMACROS_H) &&	\
    defined(__linux__)
	char iostat_name[PATH_MAX];
	stress_iostat_t iostat;
#endif

	if ((vmstat_delay == 0) &&
	    (thermalstat_delay == 0) &&
	    (iostat_delay == 0))
		return;

	vmstat_sleep = vmstat_delay;
	thermalstat_sleep = thermalstat_delay;
	iostat_sleep = iostat_delay;

	vmstat_pid = fork();
	if ((vmstat_pid < 0) || (vmstat_pid > 0))
		return;

	stress_set_proc_name("stat [periodic]");

	if (vmstat_delay)
		stress_get_vmstat(&vmstat);

	if (thermalstat_delay) {
		for (tz_info = g_shared->tz_info; tz_info; tz_info = tz_info->next)
			tz_num++;
	}

#if defined(HAVE_SYS_SYSMACROS_H) &&	\
    defined(__linux__)
	if (stress_iostat_iostat_name(iostat_name, sizeof(iostat_name)) == NULL)
		iostat_sleep = 0;
	if (iostat_delay)
		stress_get_iostat(iostat_name, &iostat);
#endif

#if defined(SCHED_DEADLINE)
	VOID_RET(int, stress_set_sched(getpid(), SCHED_DEADLINE, 99, true));
#endif

	t1 = stress_time_now();

	while (keep_stressing_flag()) {
		int32_t sleep_delay = INT_MAX;
		long clk_tick;
		double delta;

		if (vmstat_delay > 0)
			sleep_delay = STRESS_MINIMUM(vmstat_delay, sleep_delay);
		if (thermalstat_delay > 0)
			sleep_delay = STRESS_MINIMUM(thermalstat_delay, sleep_delay);
#if defined(HAVE_SYS_SYSMACROS_H) &&	\
    defined(__linux__)
		if (iostat_delay > 0)
			sleep_delay = STRESS_MINIMUM(iostat_delay, sleep_delay);
#endif
		t1 += sleep_delay;
		t2 = stress_time_now();

		delta = t1 - t2;
		if (delta > 0) {
			uint64_t nsec = (uint64_t)(delta * STRESS_DBL_NANOSECOND);
			(void)shim_nanosleep_uint64(nsec);
		}

		/* This may change each time we get stats */
		clk_tick = sysconf(_SC_CLK_TCK) * sysconf(_SC_NPROCESSORS_ONLN);

		vmstat_sleep -= sleep_delay;
		thermalstat_sleep -= sleep_delay;
		iostat_sleep -= sleep_delay;

		if ((vmstat_delay > 0) && (vmstat_sleep <= 0))
			vmstat_sleep = vmstat_delay;
		if ((thermalstat_delay > 0) && (thermalstat_sleep <= 0))
			thermalstat_sleep = thermalstat_delay;
		if ((iostat_delay > 0) && (iostat_sleep <= 0))
			iostat_sleep = iostat_delay;

		if (vmstat_sleep == vmstat_delay) {
			double clk_tick_vmstat_delay = (double)clk_tick * (double)vmstat_delay;
			static uint32_t vmstat_count = 0;

			if ((vmstat_count++ % 25) == 0)
				pr_inf("vmstat: %2s %2s %9s %9s %9s %9s "
					"%4s %4s %6s %6s %4s %4s %2s %2s "
					"%2s %2s %2s\n",
					"r", "b", "swpd", "free", "buff",
					"cache", "si", "so", "bi", "bo",
					"in", "cs", "us", "sy", "id",
					"wa", "st");

			stress_get_vmstat(&vmstat);
			pr_inf("vmstat: %2" PRIu64 " %2" PRIu64 /* procs */
			       " %9" PRIu64 " %9" PRIu64	/* vm used */
			       " %9" PRIu64 " %9" PRIu64	/* memory_buff */
			       " %4" PRIu64 " %4" PRIu64	/* si, so*/
			       " %6" PRIu64 " %6" PRIu64	/* bi, bo*/
			       " %4" PRIu64 " %4" PRIu64	/* int, cs*/
			       " %2.0f %2.0f" 			/* us, sy */
			       " %2.0f %2.0f" 			/* id, wa */
			       " %2.0f\n",			/* st */
				vmstat.procs_running,
				vmstat.procs_blocked,
				vmstat.swap_used,
				vmstat.memory_free,
				vmstat.memory_buff,
				vmstat.memory_cached + vmstat.memory_reclaimable,
				vmstat.swap_in / (uint64_t)vmstat_delay,
				vmstat.swap_out / (uint64_t)vmstat_delay,
				vmstat.block_in / (uint64_t)vmstat_delay,
				vmstat.block_out / (uint64_t)vmstat_delay,
				vmstat.interrupt / (uint64_t)vmstat_delay,
				vmstat.context_switch / (uint64_t)vmstat_delay,
				100.0 * (double)vmstat.user_time / clk_tick_vmstat_delay,
				100.0 * (double)vmstat.system_time / clk_tick_vmstat_delay,
				100.0 * (double)vmstat.idle_time / clk_tick_vmstat_delay,
				100.0 * (double)vmstat.wait_time / clk_tick_vmstat_delay,
				100.0 * (double)vmstat.stolen_time / clk_tick_vmstat_delay);
		}

		if (thermalstat_delay == thermalstat_sleep) {
			double min1, min5, min15, avg_ghz, min_ghz, max_ghz;
			size_t therms_len = 1 + (tz_num * 7);
			char *therms;
			char cpuspeed[19];
#if defined(__linux__)
			char *ptr;
#endif
			static uint32_t thermalstat_count = 0;

			therms = calloc(therms_len, sizeof(*therms));
			if (therms) {
#if defined(__linux__)
				for (ptr = therms, tz_info = g_shared->tz_info; tz_info; tz_info = tz_info->next) {
					(void)snprintf(ptr, 8, " %6.6s", tz_info->type);
					ptr += 7;
				}
#endif
				if ((thermalstat_count++ % 25) == 0)
					pr_inf("therm: AvGHz MnGhz MxGHz  LdA1  LdA5 LdA15 %s\n", therms);

#if defined(__linux__)
				for (ptr = therms, tz_info = g_shared->tz_info; tz_info; tz_info = tz_info->next) {
					(void)snprintf(ptr, 8, " %6.2f", stress_get_tz_info(tz_info));
					ptr += 7;
				}
#endif
				stress_get_cpu_ghz(&avg_ghz, &min_ghz, &max_ghz);
				if (avg_ghz > 0.0)
					(void)snprintf(cpuspeed, sizeof(cpuspeed), "%5.2f %5.2f %5.2f",
						avg_ghz, min_ghz, max_ghz);
				else
					(void)snprintf(cpuspeed, sizeof(cpuspeed), "%5.5s %5.5s %5.5s",
						" n/a ", " n/a ", " n/a ");

				if (stress_get_load_avg(&min1, &min5, &min15) < 0)  {
					pr_inf("therm: %18s %5.5s %5.5s %5.5s %s\n",
						cpuspeed, "n/a", "n/a", "n/a", therms);
				} else {
					pr_inf("therm: %5s %5.2f %5.2f %5.2f %s\n",
						cpuspeed, min1, min5, min15, therms);
				}
				free(therms);
			}
		}

#if defined(HAVE_SYS_SYSMACROS_H) &&	\
    defined(__linux__)
		if (iostat_delay == iostat_sleep) {
			double clk_scale = (iostat_delay > 0) ? 1.0 / iostat_delay : 0.0;
			static uint32_t iostat_count = 0;

			if ((iostat_count++ % 25) == 0)
				pr_inf("iostat: Inflght   Rd K/s   Wr K/s Dscd K/s     Rd/s     Wr/s   Dscd/s\n");

			stress_get_iostat(iostat_name, &iostat);
			/* sectors are 512 bytes, so >> 1 to get stats in 1024 bytes */
			pr_inf("iostat: %7.0f %8.0f %8.0f %8.0f %8.0f %8.0f %8.0f\n",
				(double)iostat.in_flight * clk_scale,
				(double)(iostat.read_sectors >> 1) * clk_scale,
				(double)(iostat.write_sectors >> 1) * clk_scale,
				(double)(iostat.discard_sectors >> 1) * clk_scale,
				(double)iostat.read_io * clk_scale,
				(double)iostat.write_io * clk_scale,
				(double)iostat.discard_io * clk_scale);
		}
#endif
	}
	_exit(0);
}

/*
 *  stress_vmstat_stop()
 *	stop vmstat statistics
 */
void stress_vmstat_stop(void)
{
	if (vmstat_pid > 0) {
		int status;

		(void)kill(vmstat_pid, SIGKILL);
		(void)waitpid(vmstat_pid, &status, 0);
	}
}
