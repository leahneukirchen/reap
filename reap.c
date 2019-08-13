/*
 * reap - run process until all its spawned processes are dead
 *
 * To the extent possible under law, Leah Neukirchen <leah@vuxu.org>
 * has waived all copyright and related or neighboring rights to this work.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#define _GNU_SOURCE

#include <sys/prctl.h>
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

sig_atomic_t do_slay;
int do_wait;
int verbose;

#define E(str, ...) do { fprintf(stderr, "reap: " str ": %s\n", ## __VA_ARGS__, strerror(errno)); } while (0)
#define F(str, ...) do { E(str, ## __VA_ARGS__); exit(111); } while (0)
#define V(...) do { if (verbose) fprintf(stderr, "reap: " __VA_ARGS__); } while (0)

// TERM/INT -> always reap
// EXIT -> reap (default) or wait

void
start_slaying(int sig)
{
	(void)sig;
	if (verbose)
		write(2, "reap: slaying\n", 14);  // async safe
	do_slay = 1;
}

// needs CONFIG_PROC_CHILDREN=y (since Linux 4.2), most modern distros have this
// enabled.
// the alternatives are terrible (enumerating all pids or scanning /proc)
int
slay_children()
{
	char buf[128];
	snprintf(buf, 128, "/proc/%ld/task/%ld/children",
	    (long)getpid(), (long)getpid());
	FILE *file = fopen(buf, "r");
	if (!file) {
		E("could not open %s", buf);
		return 0;
	}

	int c;
	int didsth = 0;
	pid_t pid = 0;
	while ((c = getc(file)) != EOF) {
		if (c == ' ') {
			V("killing %ld\n", (long)pid);
			if (kill(pid, SIGTERM) == 0)
				didsth = 1;
			else {
				E("kill %ld", (long)pid);
			}

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
	return didsth;
}

int
main(int argc, char *argv[]) {

	int c;
        while ((c = getopt(argc, argv, "+vw")) != -1) {
		switch (c) {
		case 'w': do_wait = 1; break;
		case 'v': verbose = 1; break;
		default:
                        fprintf(stderr,
"Usage: %s [-wv] COMMAND...\n"
"\t-w\twait for main command to finish (default: start reaping)\n"
"\t-v\tverbose\n",
                            argv[0]);
                        exit(1);
		}
	}

	if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
		F("failed to become subreaper");

	sigaction(SIGINT,  &(struct sigaction){.sa_handler=start_slaying}, 0);
	sigaction(SIGTERM, &(struct sigaction){.sa_handler=start_slaying}, 0);

	pid_t pid, desc;

	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC) < 0)
		F("pipe2");

	pid = fork();
	if (pid == 0) {  // in child
		close(pipefd[0]);
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
		}

		if (do_slay)
			if (!slay_children())
				break;
	}

	V("exiting [status %d]\n", exitcode);

	exit(exitcode);
}
