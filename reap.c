/*
 * reap - run process until all its spawned processes are dead
 *
 * To the extent possible under law, Leah Neukirchen <leah@vuxu.org>
 * has waived all copyright and related or neighboring rights to this work.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#define _GNU_SOURCE

#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__FreeBSD__)
#include <sys/procctl.h>
#else
#error OS not supported
#endif

#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

volatile sig_atomic_t do_slay;
int do_wait;
int verbose;
int no_new_privs;

#define E(str, ...) do { fprintf(stderr, "reap: " str ": %s\n", ## __VA_ARGS__, strerror(errno)); } while (0)
#define F(str, ...) do { E(str, ## __VA_ARGS__); exit(111); } while (0)
#define V(...) do { if (verbose) fprintf(stderr, "reap: " __VA_ARGS__); } while (0)

// TERM/INT -> always reap
// EXIT -> reap (default) or wait

void
start_slaying(int sig)
{
	(void)sig;

	int old_errno = errno;

	if (verbose)
		write(2, "reap: slaying\n", 14);

	if (sig == SIGTERM)
		do_slay += 3;
	else
		do_slay++;


	errno = old_errno;
}

void
slay_children()
{
	int sig = do_slay > 2 ? SIGKILL : SIGTERM;

#if defined(__linux__)
	// needs CONFIG_PROC_CHILDREN=y (since Linux 4.2), most modern distros
	// have this enabled.
	// the alternatives are terrible (enumerating all pids or scanning /proc)

	char buf[128];
	snprintf(buf, 128, "/proc/%ld/task/%ld/children",
	    (long)getpid(), (long)getpid());
	FILE *file = fopen(buf, "r");
	if (!file) {
		E("could not open %s", buf);
		return;
	}

	int c;
	pid_t pid = 0;
	while ((c = getc(file)) != EOF) {
		if (c == ' ') {
			V("killing %ld\n", (long)pid);
			if (kill(pid, sig) != 0)
				E("kill %ld", (long)pid);

			pid = 0;
		} else if (isdigit(c)) {
			pid = (pid * 10) + (c - '0');
		} else {
			fprintf(stderr,
			    "reap: weird byte in /children: 0x%02x\n", c);
			break;
		}
	}

	fclose(file);
#elif defined(__FreeBSD__)
	struct procctl_reaper_kill req = { .rk_sig = sig };
	if (procctl(P_PID, getpid(), PROC_REAP_KILL, &req) == 0)
		V("killed %d processes\n", req.rk_killed);
#endif
}


int
main(int argc, char *argv[]) {

	int c;
        while ((c = getopt(argc, argv, "+vwx")) != -1) {
		switch (c) {
		case 'v': verbose = 1; break;
		case 'w': do_wait = 1; break;
		case 'x': no_new_privs = 1; break;
		default:
usage:
                        fprintf(stderr,
"Usage: %s [-vwx] COMMAND...\n"
"\t-v\tverbose\n"
"\t-w\twait for all spawned processes to finish (default: start reaping)\n"
"\t-x\tforbid execution of binaries we cannot kill\n",
                            argv[0]);
                        exit(1);
		}
	}

        if (optind == argc)
	        goto usage;

#if defined(__linux__)
	if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
		F("failed to become subreaper");
#elif defined(__FreeBSD__)
	if (procctl(P_PID, 0, PROC_REAP_ACQUIRE, 0) != 0)
		F("failed to become subreaper");
#endif

	sigaction(SIGINT,  &(struct sigaction){.sa_handler=start_slaying}, 0);
	sigaction(SIGTERM, &(struct sigaction){.sa_handler=start_slaying}, 0);

	pid_t pid, desc;

	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) < 0)
		F("pipe2");

	pid = fork();
	if (pid == 0) {  // in child
		close(pipefd[0]);
		if (no_new_privs) {
#if defined(__linux__)
			if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0)
				F("failed to SET_NO_NEW_PRIVS");
#elif defined(__FreeBSD__)
			if (procctl(P_PID, 0, PROC_NO_NEW_PRIVS_CTL,
			    &(int){ PROC_NO_NEW_PRIVS_ENABLE }) != 0)
				F("failed to set PROC_NO_NEW_PRIVS_ENABLE");
#endif
		}
		execvp(argv[optind], argv+optind);
		unsigned char err = errno;
		write(pipefd[1], &err, 1);
                _exit(111);
	} else if (pid < 0) {  // fork failed
		F("exec %s", argv[optind]);
	}

	close(pipefd[1]);

	unsigned char err = 0;
	int n = read(pipefd[0], &err, 1);

	if (n >= 0 && err) {
		errno = err;
		F("exec %s", argv[optind]);
	}

	V("spawned child %ld\n", (long)pid);

	int wstatus;
	int exitcode = 111;

	while (1) {
		if ((desc = waitpid(-1, &wstatus, 0)) == -1) {
			if (errno == ECHILD)
				break;
			else if (errno == EINTR)
				/* check do_slay below */;
			else
				F("waitpid");
		} else if (desc == pid) {
			exitcode = WEXITSTATUS(wstatus);
			V("reaped child %ld [status %d]\n", (long)desc, exitcode);
			if (!do_wait) {
				V("slaying\n");
				do_slay = 1;
			}
		} else {
			V("reaped descendant %ld\n", (long)desc);
			continue;
		}

		if (do_slay)
			slay_children();
	}

	V("exiting [status %d]\n", exitcode);

	exit(exitcode);
}
