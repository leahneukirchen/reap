/*
 * reap - run process until all its spawned processes are dead
 *
 * To the extent possible under law, Leah Neukirchen <leah@vuxu.org>
 * has waived all copyright and related or neighboring rights to this work.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

sig_atomic_t do_reap;
sig_atomic_t do_wait;

// TERM/INT -> always reap
// EXIT -> reap (default) or wait

void
turn_sharp(int sig)
{
	do_reap = 1;
}

// needs CONFIG_PROC_CHILDREN=y (since Linux 4.2), most modern distros have this
// enabled.
// the alternatives are terrible (enumerating all pids or scanning /proc)
int
reap_children()
{
	char buf[128];
	snprintf(buf, 128, "/proc/%ld/task/%ld/children",
	    (long)getpid(), (long)getpid());
	FILE *file = fopen(buf, "r");
	if (!file) {
		fprintf(stderr,
		    "reap: could not open %s: %s\n",
		    buf, strerror(errno));
		return 0;
	}

	int c;
	int didsth = 0;
	pid_t pid = 0;
	while ((c = getc(file)) != EOF) {
		if (c == ' ') {
			if (kill(pid, SIGTERM) == 0)
				didsth = 1;
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
        while ((c = getopt(argc, argv, "+w")) != -1) {
		switch (c) {
		case 'w': do_wait = 1; break;
		default:
                        fprintf(stderr,
"Usage: %s [-w] COMMAND...\n"
"\t-w\twait for main command to finish (default: start reaping)\n",
                            argv[0]);
                        exit(1);
		}
	}

	if (prctl(PR_SET_CHILD_SUBREAPER, 1) != 0) {
		fprintf(stderr,
		    "reap: failed to become subreaper: %s\n", strerror(errno));
		exit(111);
	}

	sigaction(SIGINT,  &(struct sigaction){.sa_handler=turn_sharp}, 0);
	sigaction(SIGTERM, &(struct sigaction){.sa_handler=turn_sharp}, 0);

	pid_t pid, desc;

	pid = fork();
	if (pid == 0) {  // in child
		execvp(argv[optind], argv+optind);

                fprintf(stderr, "reap: exec %s: %s\n", argv[1], strerror(errno));
                exit(111);
	} else if (pid < 0) {  // fork failed
		fprintf(stderr, "reap: exec %s: %s\n", argv[1], strerror(errno));
                exit(111);
	}

	int wstatus;
	int exitcode = 111;

	while (1) {
		if ((desc = waitpid(-1, &wstatus, 0)) == -1) {
			if (errno == ECHILD)
				break;
			else if (errno == EINTR)
				/* check do_reap below */;
			else {
				fprintf(stderr,
				    "reap: waitpid %s: %s\n", argv[1],
				    strerror(errno));
				exit(111);
			}
		} else if (desc == pid) {
			exitcode = WEXITSTATUS(wstatus);
			if (!do_wait)
				do_reap = 1;
		}

		if (do_reap)
			if (!reap_children())
				break;
	}

	exit(exitcode);
}
