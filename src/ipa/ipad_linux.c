/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * IPAd daemon for Linux hosts, OpenWrt first among them (OPENWRT_PORT_ANALYSIS.md, section 7).
 *
 * The shape follows the Android daemon (ipad_android.c on the Android port): the core is run-to-completion, so
 * every poll cycle is one ipa_run_from_config(), and repeating it is this program's job. What this front end adds
 * is what a headless router needs around that:
 *
 *   - logging to syslog (-s), besides the rotating file the configuration may ask ipa_run() for;
 *   - a status file (-S) that other programs read instead of talking to the eUICC themselves -- on OpenWrt the
 *     rpcd plugin behind "ubus call ipad status" and the LuCI page;
 *   - a readiness command (-w), run before each cycle and after a profile change until it succeeds, so polling
 *     waits for the WAN link and the clock rather than failing against them. What "ready" means is the
 *     platform's business, hence a command and not code;
 *   - an event command (-e), told about the outcome of each cycle and about profile changes, which is where an
 *     integrator adapts APN switching and similar without touching C;
 *   - "poll now" (SIGUSR2) and "reload" (SIGHUP) on top of the stop signals.
 *
 * It does not fork into the background: procd, systemd and the like supervise a foreground process.
 *
 * The configuration file is re-read on every cycle. It may be maintained by another program, so the daemon also
 * watches it while it sleeps: a changed file starts a cycle at once, as SIGHUP does. The helper commands find the
 * file's absolute path in IPAD_CONFIG (and the status file's in IPAD_STATUS), which is how the OpenWrt scripts
 * read their settings from its "platform" object.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <getopt.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <limits.h>
#include <onomondo/ipa/ipad.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/log_syslog.h>
#include <onomondo/ipa/config_json.h>
#include <onomondo/ipa/run_observer.h>

#define DEFAULT_CONFIG_PATH "/etc/ipad/config.json"

#ifndef IPAD_VERSION
#define IPAD_VERSION "unknown"
#endif

/* How often the readiness command is retried while it fails, and how long any external command may run. */
#define READY_RETRY_S 10
#define CMD_TIMEOUT_S 60

/* A readiness failure is logged when it starts and then only this often, not on every retry. */
#define READY_LOG_EVERY_S 300

/* How often the configuration file is checked for changes while sleeping. */
#define CONFIG_CHECK_S 5

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t wake_requested;
static volatile sig_atomic_t reload_requested;

static struct {
	const char *config_path;
	unsigned long interval_override; /* -i, 0 when not given */
	const char *status_path;
	const char *ready_cmd;
	const char *event_cmd;
} opts = {
	.config_path = DEFAULT_CONFIG_PATH,
};

/* What the status file says. Strings are copied, since the context they come from does not outlive a cycle. */
static struct {
	const char *state;
	unsigned long interval;
	unsigned long cycles;
	unsigned long failures; /* consecutive failed cycles */
	time_t started;
	time_t last_start;
	time_t last_end;
	int last_rc;
	bool have_last;
	int last_poll_rc;
	bool have_poll;
	time_t next_poll;
	char eid[2 * IPA_LEN_EID_BYTES + 1];
	char eim_id[128];
	char eim_fqdn[256];
	const char *ipa_mode;
} status = {
	.state = "starting",
	.ipa_mode = "unknown",
};

static void handle_stop(int signum)
{
	(void)signum;
	stop_requested = 1;
	ipa_run_stop();
}

static void handle_wake(int signum)
{
	wake_requested = 1;
	if (signum == SIGHUP)
		reload_requested = 1;
}

static void install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sigemptyset(&sa.sa_mask);
	/* SA_RESTART so a signal does not turn an in-flight read into EINTR halfway through an APDU exchange or an
	 * HTTP body. The sleeps below are not restarted regardless (nanosleep never is), which is what lets a signal
	 * cut them short. */
	sa.sa_flags = SA_RESTART;

	/* SIGUSR1 stops, as it does for the CLI (src/ipa/main.c) and the Android daemon. */
	sa.sa_handler = handle_stop;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	sa.sa_handler = handle_wake;
	sigaction(SIGUSR2, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);

	/* A dead peer on the eIM connection must not take the daemon with it; curl reports the failure through its
	 * return code instead. */
	signal(SIGPIPE, SIG_IGN);
}

/* Sleep for up to ms milliseconds; true when the sleep was cut short by a stop or wake request. */
static bool nap_ms(unsigned long ms)
{
	/* nanosleep() is not restarted after a signal, so this loop ends as soon as a handler has run. */
	struct timespec req = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000L };
	struct timespec rem;

	while (!stop_requested && !wake_requested && nanosleep(&req, &rem) != 0) {
		if (errno != EINTR)
			break;
		req = rem;
	}
	return stop_requested || wake_requested;
}

/* ------------------------------------------------------------------------
 * Configuration file watch
 * --------------------------------------------------------------------- */

/* What identifies a version of the configuration file. A program that replaces the file atomically (write a new
 * one, rename it over the old) changes the inode; one that rewrites it in place changes size or mtime. */
struct file_id {
	bool exists;
	dev_t dev;
	ino_t ino;
	off_t size;
	struct timespec mtime;
};

static struct file_id config_id;

static struct file_id file_id_of(const char *path)
{
	struct file_id id = { 0 };
	struct stat st;

	if (stat(path, &st) == 0) {
		id.exists = true;
		id.dev = st.st_dev;
		id.ino = st.st_ino;
		id.size = st.st_size;
		id.mtime = st.st_mtim;
	}
	return id;
}

static bool file_id_equal(const struct file_id *a, const struct file_id *b)
{
	return a->exists == b->exists && a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
	       a->mtime.tv_sec == b->mtime.tv_sec && a->mtime.tv_nsec == b->mtime.tv_nsec;
}

/* Remember the version of the file the coming cycle reads. */
static void config_snapshot(void)
{
	config_id = file_id_of(opts.config_path);
}

static bool config_changed(void)
{
	struct file_id now = file_id_of(opts.config_path);

	return !file_id_equal(&now, &config_id);
}

/* Sleep for the poll interval, in steps, checking the configuration file in between. True when the sleep ended
 * early: on a stop or wake request, or because the file changed. */
static bool sleep_interval(unsigned long seconds, bool *config_was_changed)
{
	unsigned long slept = 0;
	unsigned long step;

	*config_was_changed = false;
	while (slept < seconds) {
		step = seconds - slept < CONFIG_CHECK_S ? seconds - slept : CONFIG_CHECK_S;
		if (nap_ms(step * 1000UL))
			return true;
		slept += step;
		if (config_changed()) {
			*config_was_changed = true;
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------------
 * Status file
 * --------------------------------------------------------------------- */

static void json_str(FILE *fp, const char *key, const char *val, bool comma)
{
	const unsigned char *p;

	fprintf(fp, "\t\"%s\": ", key);
	if (!val) {
		fprintf(fp, "null");
	} else {
		fputc('"', fp);
		for (p = (const unsigned char *)val; *p; p++) {
			if (*p == '"' || *p == '\\')
				fprintf(fp, "\\%c", *p);
			else if (*p < 0x20)
				fprintf(fp, "\\u%04x", *p);
			else
				fputc(*p, fp);
		}
		fputc('"', fp);
	}
	fprintf(fp, "%s\n", comma ? "," : "");
}

static void json_num(FILE *fp, const char *key, long long val, bool present)
{
	if (present)
		fprintf(fp, "\t\"%s\": %lld,\n", key, val);
	else
		fprintf(fp, "\t\"%s\": null,\n", key);
}

static const char *poll_rc_name(int rc)
{
	switch (rc) {
	case IPA_POLL_AGAIN:
		return "again";
	case IPA_POLL_AGAIN_LATER:
		return "done";
	case IPA_POLL_AGAIN_WHEN_ONLINE:
		return "profile_changed";
	case IPA_POLL_CHECK_SCARD:
		return "euicc_unreachable";
	case IPA_POLL_CHECK_HTTP:
		return "eim_unreachable";
	default:
		return rc < 0 ? "error" : "unknown";
	}
}

/* Written to a temporary file and renamed into place, so a reader never sees half a file. No fsync: the file
 * describes the running process and is meant for a RAM filesystem (/var/run). */
static void write_status(void)
{
	char *tmp;
	FILE *fp;

	if (!opts.status_path)
		return;

	tmp = malloc(strlen(opts.status_path) + sizeof(".tmp"));
	if (!tmp)
		return;
	sprintf(tmp, "%s.tmp", opts.status_path);

	fp = fopen(tmp, "w");
	if (!fp) {
		IPA_LOGP(SMAIN, LERROR, "cannot write status file %s: %s\n", tmp, strerror(errno));
		free(tmp);
		return;
	}

	fprintf(fp, "{\n");
	json_str(fp, "version", IPAD_VERSION, true);
	json_num(fp, "pid", (long long)getpid(), true);
	json_str(fp, "state", status.state, true);
	json_str(fp, "config", opts.config_path, true);
	json_num(fp, "interval", (long long)status.interval, true);
	json_num(fp, "started", (long long)status.started, true);
	json_num(fp, "updated", (long long)time(NULL), true);
	json_num(fp, "cycles", (long long)status.cycles, true);
	json_num(fp, "consecutive_failures", (long long)status.failures, true);
	json_num(fp, "last_start", (long long)status.last_start, status.last_start != 0);
	json_num(fp, "last_end", (long long)status.last_end, status.have_last);
	json_num(fp, "last_rc", status.last_rc, status.have_last);
	json_str(fp, "last_result", status.have_last ? (status.last_rc < 0 ? "error" : "ok") : NULL, true);
	json_num(fp, "last_poll_rc", status.last_poll_rc, status.have_poll);
	json_str(fp, "last_poll_result", status.have_poll ? poll_rc_name(status.last_poll_rc) : NULL, true);
	json_num(fp, "next_poll", (long long)status.next_poll, status.next_poll != 0);
	json_str(fp, "eid", status.eid[0] ? status.eid : NULL, true);
	json_str(fp, "eim_id", status.eim_id[0] ? status.eim_id : NULL, true);
	json_str(fp, "eim_fqdn", status.eim_fqdn[0] ? status.eim_fqdn : NULL, true);
	json_str(fp, "ipa_mode", status.ipa_mode, false);
	fprintf(fp, "}\n");

	if (fclose(fp) != 0 || rename(tmp, opts.status_path) != 0) {
		IPA_LOGP(SMAIN, LERROR, "cannot update status file %s: %s\n", opts.status_path, strerror(errno));
		remove(tmp);
	}
	free(tmp);
}

static void set_state(const char *state)
{
	status.state = state;
	write_status();
}

/* ------------------------------------------------------------------------
 * External commands
 * --------------------------------------------------------------------- */

/* Run "cmd arg1 arg2" (through the shell, so cmd may carry its own arguments) and wait for it, killing it after
 * CMD_TIMEOUT_S. Returns its exit status, or -1 when it could not be run, was killed or died on a signal.
 * A stop request does not abandon the child early: a readiness or event script is short, and leaving it
 * orphaned would be worse than the wait. */
static int run_cmd(const char *cmd, const char *arg1, const char *arg2)
{
	char *line;
	size_t len;
	pid_t pid;
	int wstatus;
	unsigned long waited_ms = 0;
	pid_t r;

	/* "$@" hands the arguments to cmd without the shell re-parsing them. */
	len = strlen(cmd) + sizeof(" \"$@\"");
	line = malloc(len);
	if (!line)
		return -1;
	snprintf(line, len, "%s \"$@\"", cmd);

	pid = fork();
	if (pid < 0) {
		IPA_LOGP(SMAIN, LERROR, "cannot run %s: %s\n", cmd, strerror(errno));
		free(line);
		return -1;
	}
	if (pid == 0) {
		signal(SIGPIPE, SIG_DFL);
		execl("/bin/sh", "sh", "-c", line, "sh", arg1, arg2, (char *)NULL);
		_exit(127);
	}
	free(line);

	for (;;) {
		r = waitpid(pid, &wstatus, WNOHANG);
		if (r == pid)
			break;
		if (r < 0 && errno != EINTR) {
			IPA_LOGP(SMAIN, LERROR, "waiting for %s failed: %s\n", cmd, strerror(errno));
			return -1;
		}
		if (waited_ms >= CMD_TIMEOUT_S * 1000UL) {
			IPA_LOGP(SMAIN, LERROR, "%s did not finish within %d s, killing it\n", cmd, CMD_TIMEOUT_S);
			kill(pid, SIGKILL);
			waitpid(pid, &wstatus, 0);
			return -1;
		}
		/* Not nap_ms(): a pending wake request must not turn this into a busy loop. */
		{
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };
			nanosleep(&ts, NULL);
		}
		waited_ms += 100;
	}

	if (WIFEXITED(wstatus))
		return WEXITSTATUS(wstatus);
	return -1;
}

static void run_event(const char *event, int rc)
{
	char rc_str[16];
	int ret;

	if (!opts.event_cmd)
		return;

	snprintf(rc_str, sizeof(rc_str), "%d", rc);
	ret = run_cmd(opts.event_cmd, event, rc_str);
	if (ret != 0)
		IPA_LOGP(SMAIN, LERROR, "event command for \"%s\" exited with %d\n", event, ret);
}

/* Block until the readiness command succeeds. Returns false when a stop was requested meanwhile. A wake request
 * (SIGUSR2/SIGHUP) retries at once rather than ending the wait: polling while not ready is what this avoids. */
static bool wait_ready(const char *why)
{
	time_t first_fail = 0;
	time_t last_log = 0;
	const char *prev_state = status.state;
	int rc;

	if (!opts.ready_cmd)
		return !stop_requested;

	while (!stop_requested) {
		rc = run_cmd(opts.ready_cmd, why, NULL);
		if (rc == 0) {
			if (first_fail)
				IPA_LOGP(SMAIN, LINFO, "ready after %lld s (%s)\n",
					 (long long)(time(NULL) - first_fail), why);
			if (first_fail)
				set_state(prev_state);
			return true;
		}

		if (!first_fail) {
			first_fail = last_log = time(NULL);
			IPA_LOGP(SMAIN, LINFO, "not ready (%s: %s exited with %d), waiting\n", why, opts.ready_cmd, rc);
			set_state("waiting");
		} else if (time(NULL) - last_log >= READY_LOG_EVERY_S) {
			last_log = time(NULL);
			IPA_LOGP(SMAIN, LINFO, "still not ready after %lld s (%s)\n",
				 (long long)(last_log - first_fail), why);
		}

		wake_requested = 0;
		nap_ms(READY_RETRY_S * 1000UL);
	}
	return false;
}

/* ------------------------------------------------------------------------
 * Run observer
 * --------------------------------------------------------------------- */

static void capture_ctx_info(struct ipa_context *ctx)
{
	struct ipa_ctx_info info;
	size_t i;

	if (ipa_get_ctx_info(ctx, &info) < 0)
		return;

	if (info.eid_valid) {
		for (i = 0; i < sizeof(info.eid); i++)
			sprintf(status.eid + 2 * i, "%02X", info.eid[i]);
	}
	if (info.eim_id)
		snprintf(status.eim_id, sizeof(status.eim_id), "%s", info.eim_id);
	if (info.eim_fqdn)
		snprintf(status.eim_fqdn, sizeof(status.eim_fqdn), "%s", info.eim_fqdn);
	switch (info.ipa_mode) {
	case IPA_MODE_IPAD:
		status.ipa_mode = "ipad";
		break;
	case IPA_MODE_IPAE:
		status.ipa_mode = "ipae";
		break;
	default:
		status.ipa_mode = "unknown";
		break;
	}
}

static void observer(struct ipa_context *ctx, enum ipa_run_event ev, int rc, void *priv)
{
	(void)priv;

	switch (ev) {
	case IPA_RUN_EV_INITIALIZED:
	case IPA_RUN_EV_EIM_READY:
		capture_ctx_info(ctx);
		write_status();
		break;
	case IPA_RUN_EV_POLLED:
		status.last_poll_rc = rc;
		status.have_poll = true;
		write_status();
		if (rc == IPA_POLL_AGAIN_WHEN_ONLINE) {
			/* The eUICC switched profiles, so the WAN link is about to drop and may come back with other
			 * settings. Tell the platform, then hold the poll loop until the link is usable again: ipa_run()
			 * carries on with the next poll only once this returns. */
			IPA_LOGP(SMAIN, LINFO, "profile change reported, waiting for connectivity\n");
			run_event("profile-changed", rc);
			if (!wait_ready("profile-changed"))
				ipa_run_stop();
			set_state("polling");
		}
		break;
	}
}

/* ------------------------------------------------------------------------
 * Main loop
 * --------------------------------------------------------------------- */

/* The interval for the wait after this cycle: -i when given, otherwise whatever the configuration file says now.
 * Read once per cycle, like the rest of the file, so an edited interval applies from the next cycle on (or at once
 * after SIGHUP). */
static unsigned long current_interval(void)
{
	struct ipa_run_config *rcfg;
	unsigned long interval;

	if (opts.interval_override)
		return opts.interval_override;

	rcfg = ipa_config_json_load(opts.config_path);
	if (!rcfg)
		return status.interval; /* keep the previous one; the cycle itself reports the broken file */
	interval = ipa_run_config_poll_seconds(rcfg);
	ipa_run_config_free(rcfg);
	return interval;
}

/* The log levels given on the command line, reapplied before every cycle: the configuration file may override
 * them for a cycle, and a level removed from the file must fall back to these rather than stick. */
static const char *level_all;
static const char *level_subsys[16];
static unsigned int num_level_subsys;

/* -l LEVEL (every subsystem) or -d SUBSYS:LEVEL (one subsystem), as the CLI accepts them. */
static int parse_log_level_opt(const char *arg, bool per_subsys)
{
	char buf[32];
	char *sep;
	int subsys;
	int level;

	if (!per_subsys) {
		level = ipa_log_level_by_name(arg);
		if (level < 0) {
			fprintf(stderr, "unknown log level \"%s\"\n", arg);
			return -EINVAL;
		}
		ipa_log_set_level_all(level);
		return 0;
	}

	if (strlen(arg) >= sizeof(buf) || !strchr(arg, ':')) {
		fprintf(stderr, "log level specification \"%s\" is not of the form SUBSYS:LEVEL\n", arg);
		return -EINVAL;
	}
	strcpy(buf, arg);
	sep = strchr(buf, ':');
	*sep = '\0';

	subsys = ipa_log_subsys_by_name(buf);
	level = ipa_log_level_by_name(sep + 1);
	if (subsys < 0 || level < 0) {
		fprintf(stderr, "unknown log subsystem or level in \"%s\"\n", arg);
		return -EINVAL;
	}
	ipa_log_set_level(subsys, level);
	return 0;
}

/* Returns false when a level given on the command line is invalid. */
static bool apply_baseline_levels(void)
{
	unsigned int i;

	/* The library default: everything. -l narrows it, -d refines -l whatever the order on the command line. */
	ipa_log_set_level_all(LDEBUG);
	if (level_all && parse_log_level_opt(level_all, false) < 0)
		return false;
	for (i = 0; i < num_level_subsys; i++) {
		if (parse_log_level_opt(level_subsys[i], true) < 0)
			return false;
	}
	return true;
}

static void print_usage(const char *argv0)
{
	unsigned int i;

	printf("usage: %s [-c PATH] [-D DIR] [-i SECONDS] [-s] [-l LEVEL] [-d SUBSYS:LEVEL] [-S PATH] [-w CMD]"
	       " [-e CMD]\n", argv0);
	printf("  -c PATH     JSON configuration file (default: %s); a change to it starts a cycle\n",
	       DEFAULT_CONFIG_PATH);
	printf("  -D DIR      working directory, against which relative paths are resolved\n");
	printf("  -i SECONDS  poll interval, overriding \"poll_interval\" in the configuration;\n");
	printf("              without either, run one cycle and exit\n");
	printf("  -s          log to syslog instead of stderr\n");
	printf("  -l LEVEL    highest level logged by every subsystem\n");
	printf("  -d S:LEVEL  highest level logged by subsystem S (may be repeated, applied after -l);\n");
	printf("              \"log\": {\"level\", \"subsys_levels\"} in the configuration take precedence\n");
	printf("  -S PATH     keep a JSON status file at PATH\n");
	printf("  -w CMD      readiness command: run before each cycle and after a profile change,\n");
	printf("              polling waits until it exits with 0 (argument: the reason)\n");
	printf("  -e CMD      event command, called as: CMD EVENT RC\n");
	printf("              (events: cycle-done, cycle-failed, profile-changed)\n");
	printf("              both commands get IPAD_CONFIG and IPAD_STATUS in their environment\n");
	printf("  -h          print this text\n");
	printf("signals: TERM/INT/USR1 stop, USR2 poll now, HUP re-read the configuration and poll now\n");
	printf("log subsystems:");
	for (i = 0; i < _NUM_LOG_SUBSYS; i++)
		printf(" %s", ipa_log_subsys_name(i));
	printf("\nlog levels:");
	for (i = 0; i < _NUM_LOG_LEVEL; i++)
		printf(" %s", ipa_log_level_name(i));
	printf("\n");
}

int main(int argc, char **argv)
{
	bool use_syslog = false;
	bool config_was_changed;
	const char *workdir = NULL;
	char abs_config[PATH_MAX];
	char abs_status[PATH_MAX];
	int opt;
	int rc;

	while ((opt = getopt(argc, argv, "c:D:i:sl:d:S:w:e:h")) != -1) {
		switch (opt) {
		case 'c':
			opts.config_path = optarg;
			break;
		case 'D':
			workdir = optarg;
			break;
		case 'i':
			opts.interval_override = strtoul(optarg, NULL, 10);
			if (opts.interval_override > IPA_MAX_POLL_INTERVAL_SECONDS) {
				fprintf(stderr, "-i: at most %d seconds\n", IPA_MAX_POLL_INTERVAL_SECONDS);
				return EXIT_FAILURE;
			}
			break;
		case 's':
			use_syslog = true;
			break;
		case 'l':
			level_all = optarg;
			break;
		case 'd':
			if (num_level_subsys == IPA_ARRAY_SIZE(level_subsys)) {
				fprintf(stderr, "too many -d options\n");
				return EXIT_FAILURE;
			}
			level_subsys[num_level_subsys++] = optarg;
			break;
		case 'S':
			opts.status_path = optarg;
			break;
		case 'w':
			opts.ready_cmd = optarg;
			break;
		case 'e':
			opts.event_cmd = optarg;
			break;
		case 'h':
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		default:
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (optind < argc) {
		fprintf(stderr, "unexpected argument: %s\n", argv[optind]);
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	if (!apply_baseline_levels())
		return EXIT_FAILURE;

	if (workdir && chdir(workdir) != 0) {
		fprintf(stderr, "cannot change to %s: %s\n", workdir, strerror(errno));
		return EXIT_FAILURE;
	}

	/* The helper commands may run with another working directory, so they get absolute paths. A file that does
	 * not exist yet keeps its path as given. */
	if (realpath(opts.config_path, abs_config))
		opts.config_path = abs_config;
	setenv("IPAD_CONFIG", opts.config_path, 1);
	if (opts.status_path) {
		if (opts.status_path[0] != '/' && getcwd(abs_status, sizeof(abs_status)) &&
		    strlen(abs_status) + 1 + strlen(opts.status_path) < sizeof(abs_status)) {
			strcat(abs_status, "/");
			strcat(abs_status, opts.status_path);
			opts.status_path = abs_status;
		}
		setenv("IPAD_STATUS", opts.status_path, 1);
	}

	if (use_syslog && ipa_log_syslog_sink_init("ipad") < 0) {
		fprintf(stderr, "cannot log to syslog\n");
		return EXIT_FAILURE;
	}

	install_signal_handlers();
	ipa_run_set_observer(observer, NULL);

	status.started = time(NULL);
	IPA_LOGP(SMAIN, LINFO, "IPAd daemon %s starting, configuration %s\n", IPAD_VERSION, opts.config_path);

	rc = 0;
	while (!stop_requested) {
		status.interval = current_interval();

		if (!wait_ready("poll"))
			break;

		/* Requests that arrived while waiting are answered by this very cycle. */
		wake_requested = 0;
		reload_requested = 0;
		config_snapshot();
		apply_baseline_levels();

		status.last_start = time(NULL);
		status.next_poll = 0;
		set_state("polling");

		rc = ipa_run_from_config(opts.config_path);

		status.cycles++;
		status.last_end = time(NULL);
		status.last_rc = rc;
		status.have_last = true;
		if (rc < 0) {
			status.failures++;
			IPA_LOGP(SMAIN, LERROR, "poll cycle failed (%d)\n", rc);
		} else {
			status.failures = 0;
		}
		write_status();
		run_event(rc < 0 ? "cycle-failed" : "cycle-done", rc);

		if (stop_requested)
			break;

		if (!status.interval)
			break;

		status.next_poll = time(NULL) + (time_t)status.interval;
		set_state("sleeping");
		IPA_LOGP(SMAIN, LINFO, "next poll cycle in %lu s\n", status.interval);
		if (sleep_interval(status.interval, &config_was_changed) && !stop_requested)
			IPA_LOGP(SMAIN, LINFO, "%s, polling now\n",
				 config_was_changed ? "configuration file changed" :
				 (reload_requested ? "reload requested" : "poll requested"));
	}

	status.next_poll = 0;
	set_state("stopped");
	IPA_LOGP(SMAIN, LINFO, "IPAd daemon exiting\n");
	if (use_syslog)
		ipa_log_syslog_sink_free();

	/* With an interval, a failed cycle is retried rather than ending the daemon, so only the single-cycle mode
	 * reports it in the exit status. */
	return (!status.interval && rc < 0) ? EXIT_FAILURE : EXIT_SUCCESS;
}
