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
#include "core-arch.h"
#include "core-asm-x86.h"
#include "core-cpu.h"
#include "core-cpu-cache.h"
#include "core-nt-store.h"

#if defined(HAVE_SYS_IO_H)
#include <sys/io.h>
#endif

#if defined(HAVE_SYS_PRCTL_H)
#include <sys/prctl.h>
#endif

#if defined(HAVE_SYS_AUXV_H)
#include <sys/auxv.h>
#endif

static sigjmp_buf jmp_env;
#if defined(SA_SIGINFO)
static volatile void *fault_addr;
static volatile void *expected_addr;
static volatile int signo;
static volatile int code;
#endif

#define BAD_ADDR	((void *)(0x08))

static const stress_help_t help[] = {
	{ NULL,	"sigsegv N",	 "start N workers generating segmentation faults" },
	{ NULL,	"sigsegv-ops N", "stop after N bogo segmentation faults" },
	{ NULL,	NULL,		 NULL }
};

/*
 *  stress_segvhandler()
 *	SEGV handler
 */
#if defined(SA_SIGINFO)
static void NORETURN MLOCKED_TEXT stress_segvhandler(
	int num,
	siginfo_t *info,
	void *ucontext)
{
	(void)num;
	(void)ucontext;

	fault_addr = info->si_addr;
	signo = info->si_signo;
	code = info->si_code;

	siglongjmp(jmp_env, 1);		/* Ugly, bounce back */
}
#else
static void NORETURN MLOCKED_TEXT stress_segvhandler(int signum)
{
	(void)signum;

	siglongjmp(jmp_env, 1);		/* Ugly, bounce back */
}
#endif

#if defined(STRESS_ARCH_X86) &&	\
    defined(__linux__)
#define HAVE_SIGSEGV_X86_TRAP
/*
 *  stress_sigsegv_x86_trap()
 *	cause an x86 instruction trap by executing an
 *	instruction that is more than the maximum of
 *	15 bytes long.  This is achieved by many REPNE
 *	instruction prefixes before a multiply. The
 *	trap will produce a segmentation fault.
 */
static NOINLINE OPTIMIZE0 void stress_sigsegv_x86_trap(void)
{
	int a = 1, b = 2;

	 __asm__ __volatile__(
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    ".byte 0xf2\n\t"
	    "mul %1\n\t"
	    : "=r" (a)
            : "r" (b), "r" (a));
	/*
 	 *  Should not get here
	 */
}
#endif

#if defined(STRESS_ARCH_X86) &&	\
    defined(__linux__)
#define HAVE_SIGSEGV_X86_INT88
/*
 *  stress_sigsegv_x86_int88()
 *	making an illegal int trap causes a SIGSEGV on
 *	x86 linux implementations, so exercise this
 */
static NOINLINE OPTIMIZE0 void stress_sigsegv_x86_int88(void)
{
	__asm__ __volatile__("int $88\n");
	/*
	 *  Should not get here
	 */
}
#endif

#if defined(STRESS_ARCH_X86) &&	\
    defined(__linux__)
#define HAVE_SIGSEGV_RDMSR
static void stress_sigsegv_rdmsr(void)
{
	uint32_t ecx = 0x00000010, eax, edx;

	__asm__ __volatile__("rdmsr" : "=a" (eax), "=d" (edx) : "c" (ecx));
	/*
	 *  Should not get here
	 */
}
#endif

#if defined(STRESS_ARCH_X86) &&		\
    defined(__linux__) &&		\
    defined(HAVE_NT_STORE128) &&	\
    defined(HAVE_INT128_T)
#define HAVE_SIGSEGV_MISALIGNED128NT
static void stress_sigsegv_misaligned128nt(void)
{
	/* Misaligned non-temporal 128 bit store */

	__uint128_t buffer[2];
	__uint128_t *ptr = (__uint128_t *)((uintptr_t)buffer + 1);

	stress_nt_store128(ptr, ~(__uint128_t)0);
	/*
	 *  Should not get here
	 */
}
#endif

#if defined(STRESS_ARCH_X86) &&		\
    defined(__linux__) && 		\
    defined(HAVE_SYS_PRCTL_H) &&	\
    defined(PR_SET_TSC) &&		\
    defined(PR_TSC_SIGSEGV)
#define HAVE_SIGSEGV_READ_TSC
static void stress_sigsegv_readtsc(void)
{
	/* SEGV reading tsc when tsc is not allowed */
	if (prctl(PR_SET_TSC, PR_TSC_SIGSEGV, 0, 0, 0) == 0)
		(void)stress_asm_x86_rdtsc();
}

static void stress_enable_readtsc(void)
{
	(void)prctl(PR_SET_TSC, PR_TSC_ENABLE, 0, 0, 0);
}
#endif

#if defined(STRESS_ARCH_X86) &&		\
    defined(__linux__) && 		\
    defined(HAVE_SYS_IO_H) &&		\
    defined(HAVE_IOPORT)
#define HAVE_SIGSEGV_READ_IO
static void stress_sigsegv_read_io(void)
{
	/* SIGSEGV on illegal port read access */
	(void)inb(0x80);
}
#endif

#if defined(__linux__) &&	\
    defined(HAVE_SYS_AUXV_H)
#define HAVE_SIGSEGV_VDSO
static void stress_sigsegv_vdso(void)
{
	const uintptr_t vdso = (uintptr_t)getauxval(AT_SYSINFO_EHDR);

	/* No vdso, don't bother */
	if (!vdso)
		return;

#if defined(HAVE_CLOCK_GETTIME) &&	\
    (defined(STRESS_ARCH_ARM) ||	\
     defined(STRESS_ARCH_MIPS) ||	\
     defined(STRESS_ARCH_PPC64) || 	\
     defined(STRESS_ARCH_RISCV) ||	\
     defined(STRESS_ARCH_S390) ||	\
     defined(STRESS_ARCH_X86))
	(void)clock_gettime(CLOCK_REALTIME, BAD_ADDR);
#endif
#if defined(STRESS_ARCH_ARM) ||		\
    defined(STRESS_ARCH_MIPS) ||	\
    defined(STRESS_ARCH_PPC64) || 	\
    defined(STRESS_ARCH_RISCV) ||	\
    defined(STRESS_ARCH_S390) ||	\
    defined(STRESS_ARCH_X86)
	(void)gettimeofday(BAD_ADDR, NULL);
#endif
}
#endif

/*
 *  stress_sigsegv
 *	stress by generating segmentation faults by
 *	writing to a read only page
 */
static int stress_sigsegv(const stress_args_t *args)
{
	uint8_t *ptr;
	NOCLOBBER int rc = EXIT_FAILURE;
#if defined(SA_SIGINFO)
	const bool verify = !!(g_opt_flags & OPT_FLAGS_VERIFY);
#endif
#if defined(STRESS_ARCH_X86) &&		\
   defined(__linux__)	
	const bool has_msr = stress_cpu_x86_has_msr();
#if defined(HAVE_NT_STORE128) &&	\
    defined(HAVE_INT128_T)
	const bool has_sse2 = stress_cpu_x86_has_sse2();
#endif
#endif

	/* Allocate read only page */
	ptr = (uint8_t *)mmap(NULL, args->page_size, PROT_READ,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (ptr == MAP_FAILED) {
		pr_inf_skip("%s: mmap of shared read only page failed: "
			"errno = %d (%s), skipping stressor\n",
			args->name, errno, strerror(errno));
		return EXIT_NO_RESOURCE;
	}

	stress_set_proc_state(args->name, STRESS_STATE_RUN);

	for (;;) {
		int ret;
		struct sigaction action;

		(void)memset(&action, 0, sizeof action);
#if defined(SA_SIGINFO)
		action.sa_sigaction = stress_segvhandler;
#else
		action.sa_handler = stress_segvhandler;
#endif
		(void)sigemptyset(&action.sa_mask);
#if defined(SA_SIGINFO)
		action.sa_flags = SA_SIGINFO;
#endif
		ret = sigaction(SIGSEGV, &action, NULL);
		if (ret < 0) {
			pr_fail("%s: sigaction SIGSEGV: errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			goto tidy;
		}
		ret = sigaction(SIGILL, &action, NULL);
		if (ret < 0) {
			pr_fail("%s: sigaction SIGILL: errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			goto tidy;
		}
		ret = sigaction(SIGBUS, &action, NULL);
		if (ret < 0) {
			pr_fail("%s: sigaction SIGBUS: errno=%d (%s)\n",
				args->name, errno, strerror(errno));
			goto tidy;
		}

		ret = sigsetjmp(jmp_env, 1);
		/*
		 * We return here if we segfault, so
		 * first check if we need to terminate
		 */
		if (!keep_stressing(args))
			break;

		if (ret) {
			/* Signal was tripped */
#if defined(SA_SIGINFO)
			if (verify && expected_addr && fault_addr && fault_addr != expected_addr) {
				pr_fail("%s: expecting fault address %p, got %p instead\n",
					args->name, (volatile void *)expected_addr, fault_addr);
			}
			if (verify &&
			    (signo != -1) &&
			    (signo != SIGSEGV) &&
			    (signo != SIGILL) &&
			    (signo != SIGBUS)) {
				pr_fail("%s: expecting SIGSEGV/SIGILL/SIGBUS, got %s instead\n",
					args->name, strsignal(signo));
			}
#if defined(SEGV_ACCERR)
			if (verify && (signo == SIGBUS) && (code != SEGV_ACCERR)) {
				pr_fail("%s: expecting SIGBUS si_code SEGV_ACCERR (%d), got %d instead\n",
					args->name, SEGV_ACCERR, code);
			}
#endif
#endif
			inc_counter(args);
		} else {
#if defined(SA_SIGINFO)
			signo = -1;
			code = -1;
			fault_addr = NULL;
			expected_addr = NULL;
#endif
			switch (stress_mwc8() & 7) {
#if defined(HAVE_SIGSEGV_X86_TRAP)
			case 0:
				/* Trip a SIGSEGV/SIGILL/SIGBUS */
				stress_sigsegv_x86_trap();
				CASE_FALLTHROUGH;
#endif
#if defined(HAVE_SIGSEGV_X86_INT88)
			case 1:
				/* Illegal int $88 */
				stress_sigsegv_x86_int88();
				CASE_FALLTHROUGH;
#endif
#if defined(HAVE_SIGSEGV_RDMSR)
			case 2:
				/* Privileged instruction -> SIGSEGV */
				if (has_msr)
					stress_sigsegv_rdmsr();
				CASE_FALLTHROUGH;
#endif
#if defined(HAVE_SIGSEGV_MISALIGNED128NT)
			case 3:
				if (has_sse2)
					stress_sigsegv_misaligned128nt();
				CASE_FALLTHROUGH;
#endif
#if defined(HAVE_SIGSEGV_READ_TSC)
			case 4:
				stress_sigsegv_readtsc();
				CASE_FALLTHROUGH;
#endif
#if defined(HAVE_SIGSEGV_READ_IO)
			case 5:
				stress_sigsegv_read_io();
				CASE_FALLTHROUGH;
#endif
#if defined(HAVE_SIGSEGV_VDSO)
			case 6:
#if defined(SA_SIGINFO)
				expected_addr = BAD_ADDR;
				shim_cacheflush((char *)&expected_addr, (int)sizeof(*expected_addr), SHIM_DCACHE);
#endif
				stress_sigsegv_vdso();
				CASE_FALLTHROUGH;
#endif
			default:
#if defined(SA_SIGINFO)
				expected_addr = ptr;
				shim_cacheflush((char *)&expected_addr, (int)sizeof(*expected_addr), SHIM_DCACHE);
#endif
				*ptr = 0;
				break;
			}
		}
	}
	rc = EXIT_SUCCESS;
tidy:
#if defined(HAVE_SIGSEGV_READ_TSC)
	stress_enable_readtsc();
#endif
	stress_set_proc_state(args->name, STRESS_STATE_DEINIT);
	(void)munmap((void *)ptr, args->page_size);

	return rc;

}

stressor_info_t stress_sigsegv_info = {
	.stressor = stress_sigsegv,
	.class = CLASS_INTERRUPT | CLASS_OS,
#if defined(SA_SIGINFO)
	.verify = VERIFY_OPTIONAL,
#endif
	.help = help
};
