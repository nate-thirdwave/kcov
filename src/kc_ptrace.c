/*
 * Copyright (C) 2010 Simon Kagstrom, Thomas Neumann
 *
 * See COPYING for license details
 *
 * Taken from bcov:Debugger.cpp:
 *
 * Copyright (C) 2007 Thomas Neumann
 */
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <sys/errno.h>
#include <unistd.h>

#include <kc.h>
#include <kc_ptrace_arch.h>
#include <utils.h>

static pid_t active_child, child;

static unsigned long peek_word(unsigned long addr)
{
	unsigned long aligned = get_aligned(addr);

	return ptrace(PTRACE_PEEKTEXT, active_child, aligned, 0);
}

static void poke_word(unsigned long addr, unsigned long val)
{
	ptrace(PTRACE_POKETEXT, active_child, get_aligned(addr), val);
}

static unsigned long ptrace_get_ip_before_trap(struct kc *kc)
{
	uint8_t regs[1024];
	struct kc_ptrace_arch *arch = kc_ptrace_arch_get(kc->e_machine);

	memset(regs, 0, sizeof(regs));
	ptrace(PTRACE_GETREGS, active_child, 0, &regs);

	return arch->get_pc(kc, &regs);
}

static void ptrace_setup_breakpoints(struct kc *kc)
{
	GHashTableIter iter;
	unsigned long key;
	struct kc_addr *addr;
	struct kc_ptrace_arch *arch = kc_ptrace_arch_get(kc->e_machine);

	if (!arch)
		error("The architecture %d isn't supported by KCOV yet\n",
				kc->e_machine);

	/* First lookup the current instruction encoding (without breakpoints) */
	g_hash_table_iter_init(&iter, kc->addrs);
	while (g_hash_table_iter_next(&iter,
			(gpointer*)&key, (gpointer*)&addr))
		addr->saved_code = peek_word(addr->addr);

	/* Then setup the breakpoints */
	g_hash_table_iter_init(&iter, kc->addrs);
	while (g_hash_table_iter_next(&iter,
			(gpointer*)&key, (gpointer*)&addr)) {
		unsigned long cur_data = peek_word(addr->addr);

		poke_word(addr->addr,
				arch->setup_breakpoint(kc, addr->addr, cur_data));
	}
}

void ptrace_eliminate_breakpoint(struct kc *kc, struct kc_addr *addr)
{
   uint8_t regs[1024];
   struct kc_ptrace_arch *arch = kc_ptrace_arch_get(kc->e_machine);
   unsigned long val;

   /* arch C't be NULL, or we would have exited when setting up BPs */
   memset(regs, 0, sizeof(regs));
   ptrace(PTRACE_GETREGS, active_child, 0, &regs);
   arch->adjust_pc_after_breakpoint(kc, &regs);
   ptrace(PTRACE_SETREGS, active_child, 0, &regs);

   val = addr->saved_code;
   if (arch->clear_breakpoint)
	   val = arch->clear_breakpoint(kc, addr->addr,
			   addr->saved_code, peek_word(addr->addr));

   poke_word(addr->addr, val);
   kc_addr_register_hit(addr);
}

enum {
	PT_CODE_ERROR,
	PT_CODE_TRAP,
	PT_CODE_EXIT,
};

static int do_ptrace_run(struct kc *kc)
{
	// Continue the stopped child
	ptrace(PTRACE_CONT, active_child, 0, 0);

	while (1) {
		// Wait for a child
		int status;

		pid_t r = waitpid(-1, &status, __WALL);

		// Got no one? Child probably died
		if (r == -1)
			return PT_CODE_EXIT;
		active_child = r;

		// A signal?
		if (WIFSTOPPED(status)) {
			int sig = WSTOPSIG(status);

			// A trap?
			if (sig == SIGTRAP)
				return PT_CODE_TRAP;
			// A new clone? Ignore the stop event
			else if ((status >> 8) == PTRACE_EVENT_CLONE)
				sig = 0;

			// No, deliver it directly
			ptrace(PTRACE_CONT, active_child, 0, sig);
			continue;
		}
		// Thread died?
		if (WIFSIGNALED(status) || WIFEXITED(status)) {
			if (active_child == child)
				return PT_CODE_EXIT;
			continue;
		}

		// Unknown event
		return PT_CODE_ERROR;
	}
}

static void ptrace_run_debugger(struct kc *kc)
{
	while (1) {
		int err = do_ptrace_run(kc);

		switch (err) {
		case PT_CODE_ERROR:
			fprintf(stderr, "Error while tracing\n");
		case PT_CODE_EXIT:
			return;
		case PT_CODE_TRAP:
		{
			unsigned long where = ptrace_get_ip_before_trap(kc);
			struct kc_addr *addr = kc_lookup_addr(kc, where);

			if (addr)
				ptrace_eliminate_breakpoint(kc, addr);

		} break;
      }
   }
}

static pid_t fork_child(const char *executable, char *const argv[])
{
	int status;
	pid_t who;

	/* Basic check first */
	if (access(executable, X_OK) != 0)
		return -1;

	/* Executable exists, try to launch it */
	if ((child = fork()) == 0) {
		int res;

		/* And launch the process */
		res = ptrace(PTRACE_TRACEME, 0, 0, 0);
		if (res < 0) {
			perror("Can't set me as ptraced");
			return -1;
		}
		execv(executable, argv);

		/* Exec failed */
		return -1;
	}

	/* Fork error? */
	if (child < 0) {
		perror("fork");
		return -1;
	}

	/* Wait for the initial stop */
	who = waitpid(child, &status, 0);
	if (who < 0) {
		perror("waitpid");
		return -1;
	}
	if (!WIFSTOPPED(status)) {
		fprintf(stderr, "Child hasn't stopped: %x\n", status);
		return -1;
	}
	ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK);

	return child;
}



int ptrace_run(struct kc *kc, char *const argv[])
{
	active_child = fork_child(argv[0], &argv[0]);

	if (active_child < 0) {
		fprintf(stderr, "Can't fork child\n");
		return -1;
	}

	ptrace_setup_breakpoints(kc);

	ptrace_run_debugger(kc);

	return 0;
}

int ptrace_pid_run(struct kc *kc, pid_t pid)
{
	active_child = child = pid;

	errno = 0;
	ptrace(PTRACE_ATTACH, active_child, 0, 0);
	if (errno) {
		const char *err = strerror(errno);

		fprintf(stderr, "Can't attach to %d. Error %s\n", pid, err);
		return -1;
	}
	ptrace(PTRACE_SETOPTIONS, child, 0, PTRACE_O_TRACECLONE | PTRACE_O_TRACEFORK);

	ptrace_setup_breakpoints(kc);

	ptrace_run_debugger(kc);

	return 0;
}

int ptrace_detach(struct kc *kc)
{
	GHashTableIter iter;
	struct kc_addr *val;
	unsigned long key;

	/* Eliminate all unhit breakpoints */
	g_hash_table_iter_init(&iter, kc->addrs);
	while (g_hash_table_iter_next(&iter, (gpointer*)&key, (gpointer*)&val)) {
		if (!val->hits)
			ptrace_eliminate_breakpoint(kc, val);
	}

	ptrace(PTRACE_DETACH, active_child, 0, 0);

	return 0;
}