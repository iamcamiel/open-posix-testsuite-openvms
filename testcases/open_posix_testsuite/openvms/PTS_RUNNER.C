#ifdef __VMS
#pragma module PTS_RUNNER "X-5"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-5  CV  Camiel Vanderhoeven  16-Nov-2026: Optionally stop after a crash
 *     and display the completed child's stderr while retaining its results.
 * X-4  CV  Camiel Vanderhoeven  16-Nov-2026: Pass parent AST/direct-I/O limits
 *     explicitly to child processes instead of accepting small PQL defaults.
 * X-1  CV  Camiel Vanderhoeven  15-Nov-2026: Isolated native test runner.
 * X-3  CV  Camiel Vanderhoeven  16-Nov-2026: Support up to 1024 workers.
 * X-2  CV  Camiel Vanderhoeven  15-Nov-2026: Reserve unique run directories.
 */
#ifndef __VMS
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "PTS_PORT.H"

#ifdef __VMS
#define current_directory(buffer, size) getcwd(buffer, size, 1)
#if defined(__INITIAL_POINTER_SIZE) && __INITIAL_POINTER_SIZE == 64
#error Compile the native supervisor with /POINTER_SIZE=32 for ILE3 item lists
#endif
#else
#define current_directory(buffer, size) getcwd(buffer, size)
#endif

#ifdef __VMS
#include <starlet.h>
#include <lib$routines.h>
#include <descrip.h>
#include <ssdef.h>
#include <iodef.h>
#include <dvidef.h>
#include <jpidef.h>
#include <pqldef.h>
#include <efndef.h>
#include <accdef.h>
#include <msgdef.h>
#else
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#endif

#define MAX_CASES 512
struct testcase {
	char id[PTS_ID_MAX];
	unsigned long seconds;
};
static struct testcase cases[MAX_CASES];
static size_t case_count;

struct outcome {
	unsigned long native_status;
	int clean;
	int timeout;
	int error;
};

static double now(void)
{
#ifdef __VMS
	unsigned __int64 ticks;
	if (!(sys$gettim((void *)&ticks) & 1))
		return 0;
	return (double)ticks / 10000000.0;
#else
	struct timespec t;
	if (clock_gettime(CLOCK_MONOTONIC, &t) != 0)
		return 0;
	return t.tv_sec + t.tv_nsec / 1000000000.0;
#endif
}

#ifdef __VMS
struct io_status {
	unsigned short status, count;
	unsigned int detail;
};

struct item3 {
	unsigned short length, code;
	void *buffer;
	unsigned short *returned;
};

struct query_status {
	unsigned int status, reserved;
};

/* SYS$CREPRC expects a byte code followed immediately by a 32-bit quota.
 * Build bytes explicitly; normal C structure alignment would add padding.
 */
#define CHILD_QUOTA_BYTES 11
typedef char quota_value_must_be_four_bytes[(sizeof(unsigned int) == 4) ? 1 : -1];

static unsigned int child_quotas(unsigned char *quotas)
{
	struct item3 items[3];
	struct query_status iosb;
	unsigned int astlm = 0, diolm = 0, status;
	unsigned short ast_length = 0, dio_length = 0;
	static unsigned int printed_astlm, printed_diolm;
	static int printed;

	memset(items, 0, sizeof(items));
	memset(&iosb, 0, sizeof(iosb));
	items[0].length = sizeof(astlm);
	items[0].code = JPI$_ASTLM;
	items[0].buffer = &astlm;
	items[0].returned = &ast_length;
	items[1].length = sizeof(diolm);
	items[1].code = JPI$_DIOLM;
	items[1].buffer = &diolm;
	items[1].returned = &dio_length;
	status = sys$getjpiw(EFN$C_ENF, 0, 0, items, &iosb, 0, 0);
	if (status & 1)
		status = iosb.status;
	if ((status & 1) &&
	    (ast_length != sizeof(astlm) || dio_length != sizeof(diolm)))
		status = SS$_BADPARAM;
	if (!(status & 1)) {
		fprintf(stderr, "Reading child quota limits failed: OpenVMS status %08x\n",
			status);
		return status;
	}
	quotas[0] = PQL$_ASTLM;
	memcpy(quotas + 1, &astlm, sizeof(astlm));
	quotas[5] = PQL$_DIOLM;
	memcpy(quotas + 6, &diolm, sizeof(diolm));
	quotas[10] = PQL$_LISTEND;
	if (!printed || astlm != printed_astlm || diolm != printed_diolm) {
		printf("Child quota request: ASTLM=%u DIOLM=%u (runner limits)\n",
		       astlm, diolm);
		fflush(stdout);
		printed_astlm = astlm;
		printed_diolm = diolm;
		printed = 1;
	}
	return SS$_NORMAL;
}

static struct dsc$descriptor_s descriptor(const char *s)
{
	struct dsc$descriptor_s d;
	d.dsc$w_length = (unsigned short)strlen(s);
	d.dsc$b_dtype = DSC$K_DTYPE_T;
	d.dsc$b_class = DSC$K_CLASS_S;
	d.dsc$a_pointer = (char *)s;
	return d;
}

static struct outcome execute(const char *image, const char *input,
		const char *output, const char *error, unsigned long seconds)
{
	struct outcome result = {0, 0, 0, 0};
	struct dsc$descriptor_s img = descriptor(image), in = descriptor(input);
	struct dsc$descriptor_s out = descriptor(output), err = descriptor(error);
	struct io_status iosb;
	struct item3 items[2];
	unsigned char quotas[CHILD_QUOTA_BYTES];
	union {
		struct accdef accounting;
		unsigned char bytes[512];
	} message;
	unsigned short channel = 0;
	unsigned int pid = 0, unit = 0, event = 0, state = 0, status;
	unsigned int request = 1;
	__int64 delta;
	__int64 pause = -200000; /* Relative 20ms wakeup; no FP-format ABI. */
	int have_event = 0, have_timer = 0, killed = 0;

	status = child_quotas(quotas);
	if (!(status & 1))
		goto setup_error;
	memset(items, 0, sizeof(items));
	status = sys$crembx(0, &channel, sizeof(message), 1024, 0, 0, 0, 0);
	if (!(status & 1))
		goto setup_error;
	items[0].length = sizeof(unit);
	items[0].code = DVI$_UNIT;
	items[0].buffer = &unit;
	status = sys$getdviw(0, channel, 0, items, &iosb, 0, 0, 0);
	if (!(status & 1))
		goto setup_error;
	if (!(iosb.status & 1)) {
		status = iosb.status;
		goto setup_error;
	}
	if (unit > 65535) {
		status = SS$_BADPARAM;
		goto setup_error;
	}
	status = lib$get_ef(&event);
	if (!(status & 1))
		goto setup_error;
	have_event = 1;
	status = sys$clref(event);
	if (!(status & 1))
		goto setup_error;
	delta = -((__int64)seconds * 10000000);
	status = sys$setimr(event, &delta, 0, request, 0);
	if (!(status & 1))
		goto setup_error;
	have_timer = 1;
	/* Start the image directly: no shell, LOGINOUT, or pthread watchdog.
	 * The supervisor is compiled with 32-bit pointers for these ILE3 lists.
	 * A null quota list would use system PQL defaults, not the runner's full
	 * limits. Preserve its AST/direct-I/O limits; leave other quotas alone.
	 */
	status = sys$creprc(&pid, &img, &in, &out, &err,
			   0, quotas, 0, 0, 0, (unsigned short)unit, 0);
	if (!(status & 1))
		goto setup_error;
	for (;;) {
		status = sys$qiow(0, channel, IO$_READVBLK | IO$M_NOW,
				  &iosb, 0, 0, &message, sizeof(message),
				  0, 0, 0, 0);
		if (!(status & 1))
			goto running_error;
		if (iosb.status & 1) {
			if (iosb.count < ACC$K_TERMLEN ||
			    message.accounting.acc$w_msgtyp != MSG$_DELPROC ||
			    message.accounting.acc$l_pid != pid) {
				status = SS$_BADPARAM;
				goto running_error;
			}
			result.native_status = message.accounting.acc$l_finalsts;
			result.clean = result.native_status == SS$_NORMAL;
			break;
		}
		if (iosb.status != SS$_ENDOFFILE) {
			status = iosb.status;
			goto running_error;
		}
		status = sys$readef(event, &state);
		if (!(status & 1))
			goto running_error;
		if (state & (1U << (event % 32))) {
			if (killed) {
				result.error = 1;
				break; /* Stop the run if termination cannot be confirmed. */
			}
			result.timeout = 1;
			status = sys$delprc(&pid, 0);
			if (!(status & 1) && status != SS$_NONEXPR)
				goto running_error;
			killed = 1;
			sys$cantim(request, 0);
			sys$clref(event);
			delta = -50000000;
			status = sys$setimr(event, &delta, 0, request, 0);
			if (!(status & 1))
				goto running_error;
		}
		status = sys$schdwk(0, 0, &pause, 0);
		if (!(status & 1))
			goto running_error;
		status = sys$hiber();
		if (!(status & 1))
			goto running_error;
	}
	goto done;
running_error:
	if (pid)
		sys$delprc(&pid, 0);
setup_error:
	result.error = 1;
	result.native_status = status;
done:
	if (have_timer)
		sys$cantim(request, 0);
	if (have_event)
		lib$free_ef(&event);
	if (channel)
		sys$dassgn(channel);
	return result;
}
#else
static struct outcome execute(const char *image, const char *input,
		const char *output, const char *error, unsigned long seconds)
{
	struct outcome result = {0, 0, 0, 0};
	pid_t pid, got;
	int status, fd, i;
	const char *paths[3] = {input, output, error};
	struct timespec pause = {0, 20000000};
	double deadline = now() + seconds;
	pid = fork();
	if (pid < 0) {
		result.error = 1;
		result.native_status = errno;
		return result;
	}
	if (!pid) {
		if (setpgid(0, 0) != 0)
			_exit(126);
		for (i = 0; i < 3; i++) {
			fd = open(paths[i], i ? O_WRONLY | O_CREAT | O_EXCL : O_RDONLY,
				  0600);
			if (fd < 0 || dup2(fd, i) < 0)
				_exit(126);
			if (fd != i)
				close(fd);
		}
		execl(image, image, (char *)0);
		_exit(127);
	}
	/* The child also establishes its group, closing the fork/exec race. */
	if (setpgid(pid, pid) != 0 && errno != EACCES && errno != ESRCH) {
		kill(pid, SIGKILL);
		waitpid(pid, &status, 0);
		result.error = 1;
		return result;
	}
	for (;;) {
		got = waitpid(pid, &status, WNOHANG);
		if (got == pid)
			break;
		if (got < 0 && errno != EINTR) {
			result.error = 1;
			kill(-pid, SIGKILL);
			return result;
		}
		if (now() >= deadline) {
			result.timeout = 1;
			kill(-pid, SIGKILL);
			do { got = waitpid(pid, &status, 0); }
			while (got < 0 && errno == EINTR);
			if (got != pid)
				result.error = 1;
			break;
		}
		nanosleep(&pause, NULL);
	}
	result.native_status = (unsigned int)status;
	result.clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	return result;
}
#endif

static int read_cases(const char *path)
{
	FILE *in = fopen(path, "r");
	char buf[PTS_LINE_MAX], *field[8], *p;
	size_t i, n;
	unsigned long seconds;
	if (!in)
		return -1;
	while (fgets(buf, sizeof(buf), in)) {
		if (buf[0] == '#' || buf[0] == '\n' || buf[0] == '\r')
			continue;
		n = strlen(buf);
		if (!n || buf[n - 1] != '\n')
			goto invalid;
		buf[--n] = 0;
		if (n && buf[n - 1] == '\r')
			buf[--n] = 0;
		field[0] = buf;
		for (i = 1; i < 8; i++) {
			p = strchr(field[i - 1], '|');
			if (!p)
				goto invalid;
			*p = 0;
			field[i] = p + 1;
		}
		if (strchr(field[7], '|') || strcmp(field[7], "ready") ||
		    !pts_safe_id(field[0]) || strlen(field[0]) > 38 ||
		    pts_number(field[2], 1, 86400, &seconds) ||
		    case_count >= MAX_CASES)
			goto invalid;
		for (i = 0; i < case_count; i++)
			if (!strcmp(cases[i].id, field[0]))
				goto invalid;
		strcpy(cases[case_count].id, field[0]);
		cases[case_count++].seconds = seconds;
	}
	if (ferror(in) || !case_count)
		goto invalid;
	return fclose(in) == 0 ? 0 : -1;
invalid:
	fclose(in);
	return -1;
}

static void csv(FILE *out, const char *s)
{
	fputc('"', out);
	for (; *s; s++) {
		if (*s == '"')
			fputc('"', out);
		fputc(*s, out);
	}
	fputc('"', out);
}

static int copy_file(const char *source, const char *target)
{
	FILE *in = fopen(source, "rb"), *out;
	char buffer[4096];
	size_t n;
	int ok = 1;
	if (!in)
		return -1;
	out = fopen(target, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buffer, 1, sizeof(buffer), in)) != 0)
		if (fwrite(buffer, 1, n, out) != n) {
			ok = 0;
			break;
		}
	if (ferror(in))
		ok = 0;
	if (fclose(in))
		ok = 0;
	if (fclose(out))
		ok = 0;
	return ok ? 0 : -1;
}

static int option_equal(const char *value, const char *option)
{
	for (; *value && *option; value++, option++)
		if (toupper((unsigned char)*value) != (unsigned char)*option)
			return 0;
	return !*value && !*option;
}

/* Replay only after the supervisor has confirmed child termination. Text
 * mode preserves the normal OpenVMS record-to-newline translation. Keep
 * the saved file and CRASH classification even if replay is unavailable.
 */
static void print_crash_stderr(const char *id, unsigned long repeat,
		unsigned long status, const char *path)
{
	FILE *in;
	char buffer[4096];
	size_t n;
	int have_data = 0, last = '\n', read_error = 0, write_error = 0;

	fflush(stdout);
	fprintf(stderr, "Stopping after CRASH: %s repeat=%lu native_status=0x%08lx\n",
		id, repeat, status);
	fprintf(stderr, "----- BEGIN STDERR.LOG: %s -----\n", path);
	in = fopen(path, "r");
	if (!in) {
		fprintf(stderr, "Cannot open stderr log: %s\n", strerror(errno));
	} else {
		while ((n = fread(buffer, 1, sizeof(buffer), in)) != 0) {
			have_data = 1;
			last = (unsigned char)buffer[n - 1];
			if (fwrite(buffer, 1, n, stderr) != n) {
				write_error = 1;
				break;
			}
		}
		if (ferror(in))
			read_error = errno;
		if (have_data && last != '\n')
			fputc('\n', stderr);
		if (ferror(in))
			fprintf(stderr, "Error reading stderr log: %s\n", strerror(read_error));
		else if (!have_data)
			fputs("(stderr is empty)\n", stderr);
		if (fclose(in))
			fprintf(stderr, "Error closing stderr log: %s\n", strerror(errno));
		if (write_error)
			fputs("Error displaying stderr log; see the saved file.\n", stderr);
	}
	fputs("----- END STDERR.LOG -----\n", stderr);
	fflush(stderr);
}

int main(int argc, char **argv)
{
	char root[PTS_PATH_MAX], output_root[PTS_PATH_MAX], build[PTS_PATH_MAX];
	char run[PTS_PATH_MAX], work[PTS_PATH_MAX], path[PTS_PATH_MAX];
	char image[PTS_PATH_MAX], input[PTS_PATH_MAX], output[PTS_PATH_MAX];
	char error[PTS_PATH_MAX], leaf[PTS_ID_MAX], image_name[PTS_ID_MAX + 5];
	struct pts_control ctl;
	struct outcome result;
	FILE *summary, *control, *check;
	unsigned long repeats = 1, seed = 1, threads = 8, iterations = 2000;
	unsigned long repeat, bad = 0, passed = 0, other = 0;
	size_t i;
	int code, rc = EXIT_FAILURE, stop_on_crash = 0;
	const char *name;
	double start, elapsed;
	const char *manifest = argc > 1 ? argv[1] : "CASES.DAT";
	const char *results = argc > 2 ? argv[2] : "RESULTS";

	if (argc > 8 ||
	    (argc > 3 && pts_number(argv[3], 1, 100000, &repeats)) ||
	    (argc > 4 && pts_number(argv[4], 1, 2147483647UL, &seed)) ||
	    (argc > 5 && pts_number(argv[5], 1, PTS_MAX_THREADS, &threads)) ||
	    (argc > 6 && pts_number(argv[6], 1, 10000000UL, &iterations)) ||
	    (argc > 7 && !option_equal(argv[7], "STOP_ON_CRASH") &&
	     !option_equal(argv[7], "CONTINUE"))) {
		fprintf(stderr, "Usage: PTS_RUNNER [manifest [results_dir [repeats [seed [threads [iterations [CONTINUE|STOP_ON_CRASH]]]]]]]\n");
		return EXIT_FAILURE;
	}
	if (argc > 7)
		stop_on_crash = option_equal(argv[7], "STOP_ON_CRASH");
	if (read_cases(manifest) || !current_directory(root, sizeof(root))) {
		fprintf(stderr, "Cannot read a valid case manifest or current directory\n");
		return EXIT_FAILURE;
	}
	if (chdir(results) || !current_directory(output_root, sizeof(output_root)) || chdir(root)) {
		perror("results directory must exist");
		return EXIT_FAILURE;
	}
	if (pts_subdirectory(build, sizeof(build), root, "BUILD"))
		return EXIT_FAILURE;
	if (pts_join(path, sizeof(path), build, "BUILD_STATE.DAT"))
		return EXIT_FAILURE;
	check = fopen(path, "r");
	if (!check || !fgets(leaf, sizeof(leaf), check) ||
	    (strcmp(leaf, "READY\n") && strcmp(leaf, "READY\r\n"))) {
		if (check)
			fclose(check);
		fprintf(stderr, "No successful build: rerun BUILD.COM or BUILD_HOST.py\n");
		return EXIT_FAILURE;
	}
	fclose(check);
	if (pts_create_run_directory(run, sizeof(run), output_root,
				     (unsigned long)getpid(), (unsigned long)time(NULL))) {
		perror("creating unique run directory");
		return EXIT_FAILURE;
	}
	if (pts_join(path, sizeof(path), run, "MANIFEST.DAT") ||
	    copy_file(manifest, path))
		return EXIT_FAILURE;
	if (pts_join(input, sizeof(input), build, "BUILD_INFO.TXT") ||
	    pts_join(path, sizeof(path), run, "BUILD_INFO.TXT") ||
	    copy_file(input, path))
		return EXIT_FAILURE;
	if (pts_join(path, sizeof(path), run, "SUMMARY.CSV"))
		return EXIT_FAILURE;
	summary = fopen(path, "w");
	if (!summary)
		return EXIT_FAILURE;
	fprintf(summary, "case,repeat,result,pts_code,native_status,seed,threads,iterations,seconds,directory\n");
	printf("Results: %s\n", path);
	for (repeat = 1; repeat <= repeats; repeat++) {
		for (i = 0; i < case_count; i++) {
			snprintf(leaf, sizeof(leaf), "T_%04lu_%04lu", repeat, (unsigned long)i);
			if (pts_subdirectory(work, sizeof(work), run, leaf) || mkdir(work, 0700))
				goto finish;
			memset(&ctl, 0, sizeof(ctl));
			strcpy(ctl.id, cases[i].id);
			snprintf(ctl.token, sizeof(ctl.token), "P%lu_R%lu_C%lu_S%lu",
				 (unsigned long)getpid(), repeat, (unsigned long)i, seed);
			strcpy(ctl.directory, work);
			ctl.seed = 1 + ((seed - 1 + repeat - 1) % 2147483647UL);
			ctl.threads = threads;
			ctl.iterations = iterations;
			if (pts_join(ctl.result, sizeof(ctl.result), work,
#ifdef __VMS
				     "RESULT.PTS;1") ||
#else
				     "RESULT.PTS") ||
#endif
			    pts_join(input, sizeof(input), work, "CONTROL.DAT") ||
			    pts_join(output, sizeof(output), work, "STDOUT.LOG") ||
			    pts_join(error, sizeof(error), work, "STDERR.LOG"))
				goto finish;
			control = fopen(input, "w");
			if (!control)
				goto finish;
			if (pts_write_control(control, &ctl)) {
				fclose(control);
				goto finish;
			}
			if (fclose(control))
				goto finish;
			snprintf(image_name, sizeof(image_name), "%s.EXE", ctl.id);
			if (pts_join(image, sizeof(image), build, image_name))
				goto finish;
			code = -1;
			memset(&result, 0, sizeof(result));
			check = fopen(image, "rb");
			start = now();
			if (!check) {
				name = "BUILD_ERROR";
			} else {
				fclose(check);
				result = execute(image, input, output, error, cases[i].seconds);
				if (result.error)
					name = "HARNESS_ERROR";
				else if (result.timeout)
					name = "TIMEOUT";
				else if (!result.clean)
					name = "CRASH";
				else if (pts_read_result(ctl.result, ctl.id, ctl.token, &code))
					name = "HARNESS_ERROR";
				else
					name = pts_code_name(code);
			}
			elapsed = now() - start;
			if (!strcmp(name, "PASS"))
				passed++;
			else if (!strcmp(name, "UNSUPPORTED") || !strcmp(name, "UNTESTED"))
				other++;
			else
				bad++;
			printf("%s repeat=%lu %s (%.3fs)\n", ctl.id, repeat, name, elapsed);
			fprintf(summary, "%s,%lu,%s,%d,0x%08lx,%lu,%lu,%lu,%.3f,",
				ctl.id, repeat, name, code, result.native_status,
				ctl.seed, threads, iterations, elapsed);
			csv(summary, work);
			fputc('\n', summary);
			if (fflush(summary))
				goto finish;
			if (stop_on_crash && !strcmp(name, "CRASH")) {
				print_crash_stderr(ctl.id, repeat, result.native_status, error);
				goto completed;
			}
			if (result.error)
				goto finish;
		}
	}
completed:
	printf("Totals: PASS=%lu NONPASS=%lu UNSUPPORTED_OR_UNTESTED=%lu\n", passed, bad, other);
	rc = bad || !passed ? EXIT_FAILURE : EXIT_SUCCESS;
finish:
	if (fclose(summary))
		rc = EXIT_FAILURE;
	return rc;
}
