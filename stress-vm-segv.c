/*
 * Copyright (C) 2013-2021 Canonical, Ltd.
 * Copyright (C) 2022-2023 Colin Ian King.
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
#include "core-cpu-cache.h"

#if defined(HAVE_PTRACE)
#include <sys/ptrace.h>
#else
UNEXPECTED
#endif

static const stress_help_t help[] = {
	{ NULL,	"vm-segv N",	 "start N workers that unmap their address space" },
	{ NULL,	"vm-segv-ops N", "stop after N vm-segv unmap'd SEGV faults" },
	{ NULL,	NULL,		 NULL }
};

static NOINLINE void vm_unmap_child(const size_t page_size)
{
	size_t len = ~(size_t)0;
	void *addr = stress_align_address((void *)vm_unmap_child, page_size);

	len = len ^ (len >> 1);
	while (len > page_size) {
		(void)munmap((void *)0, len - page_size);
		len >>= 1;
#if !defined(__DragonFly__)
		shim_clflush(addr);
#endif
		shim_flush_icache(addr, (void *)(((uint8_t *)addr) + 64));
	}
}

static NOINLINE void vm_unmap_self(const size_t page_size)
{
	void *addr = stress_align_address((void *)vm_unmap_self, page_size);

	(void)munmap(addr, page_size);
#if !defined(__DragonFly__)
	shim_clflush(addr);
#endif
	shim_flush_icache(addr, (void *)(((uint8_t *)addr) + 64));
}

/*
 *  stress_vm_segv()
 *	stress vm segv by unmapping child's address space
 *	and generating a segv on return because child has
 *	no address space on return.
 */
static int stress_vm_segv(const stress_args_t *args)
{
	bool test_valid = false;
	stress_set_oom_adjustment(args->name, true);

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	do {
		pid_t pid;

again:
		pid = fork();
		if (pid < 0) {
			if (stress_redo_fork(errno))
				goto again;
			if (!keep_stressing(args))
				goto finish;
			pr_err("%s: fork failed: errno=%d: (%s)\n",
				args->name, errno, strerror(errno));
			return EXIT_NO_RESOURCE;
		} else if (pid > 0) {
			int status, ret;
	
			/* Parent, wait for child */
			ret = shim_waitpid(pid, &status, 0);
			if (ret < 0)
				goto kill_child;

			test_valid = true;
#if !defined(HAVE_PTRACE)
			if (WTERMSIG(status) == SIGSEGV) {
				inc_counter(args);
				continue;
			}
#else

			(void)ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_TRACESYSGOOD);

			while (keep_stressing(args)) {
				(void)ptrace(PTRACE_SYSCALL, pid, 0, 0);

				ret = shim_waitpid(pid, &status, 0);
				if (ret < 0)
					goto kill_child;
				if (WIFSTOPPED(status)) {
					int signum = WSTOPSIG(status);

					if ((signum & 0x7f) == SIGSEGV) {
						inc_counter(args);
						break;
					}
					if (signum & 0x80)
						continue;
				}
				if (WIFEXITED(status)) {
					inc_counter(args);
					break;
				}
			}
#endif
kill_child:
			(void)kill(pid, SIGTERM);
			(void)stress_killpid(pid);
			(void)shim_waitpid(pid, &status, 0);
		} else {
			/* Child */
			sigset_t set;
			const size_t page_size = args->page_size;

			stress_set_oom_adjustment(args->name, true);
			stress_process_dumpable(false);
			(void)sched_settings_apply(true);

#if defined(HAVE_PTRACE)
			(void)ptrace(PTRACE_TRACEME);
			(void)kill(getpid(), SIGSTOP);
#endif
			(void)sigemptyset(&set);
			(void)sigaddset(&set, SIGSEGV);
			(void)sigprocmask(SIG_BLOCK, &set, NULL);

			/*
			 *  Try to ummap the child's address space, should cause
			 *  a SIGSEGV at some point..
			 */
			vm_unmap_child(page_size);

			/*
			 *  That failed, so try unmapping this function
			 */
			vm_unmap_self(page_size);

			/* No luck, well that's unexpected.. */
			_exit(EXIT_FAILURE);
		}
	} while (keep_stressing(args));

finish:
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);

	if (test_valid && (get_counter(args) == 0))
		pr_fail("%s: no SIGSEGV signals detected\n", args->name);

	return EXIT_SUCCESS;
}

stressor_info_t stress_vm_segv_info = {
	.stressor = stress_vm_segv,
	.class = CLASS_VM | CLASS_MEMORY | CLASS_OS,
	.verify = VERIFY_ALWAYS,
	.help = help
};
