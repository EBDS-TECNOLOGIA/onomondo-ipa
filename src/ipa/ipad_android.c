/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Native IPAd daemon entry point (ANDROID_PORT_PLAN.md, Phase 4).
 *
 * All this does is turn a process into a supervised service: read the config
 * path, install signal handlers, and hand over to ipa_run_from_config(), which
 * is the same entry point the APK uses.  The rotating-file log sink is
 * installed from the configuration's "log" block by ipa_run(), so the daemon
 * needs no logging code of its own.
 *
 * IMPORTANT -- which transport this binary can actually use
 * --------------------------------------------------------
 * The Phase-1 hardware spike established that reaching the ISD-R through
 * Android's TelephonyManager requires two things of the *calling process*:
 * the MODIFY_PHONE_STATE permission, and being the device's LPA (the package
 * that wins EuiccConnector.findBestComponent()).  Both are properties of an
 * Android app, and the telephony transport bridges up into a Kotlin
 * EuiccChannel object over JNI.  A standalone native process started by init
 * has no JVM, no package identity and therefore no way to satisfy either.
 *
 * So this binary is NOT the way to drive an eUICC over Android telephony.
 * Use it when the integrator supplies a different transport -- a vendor APDU
 * channel, or an OMAPI helper -- and use the app-hosted service (see
 * ipa_run_from_config() via com.onomondo.ipa.NativeBridge, and
 * contrib/android/README.md) for the telephony path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/config_json.h>

/* Where an init-launched daemon looks for its configuration unless told
 * otherwise.  /data/misc is writable by system services and survives reboots;
 * an integrator using a vendor partition layout can override it with -c. */
#define DEFAULT_CONFIG_PATH "/data/misc/ipa/config.json"

/* Set by the signal handler; distinct from the library's own stop flag because
 * this one also has to break the sleep between poll cycles. */
static volatile sig_atomic_t stop_requested;

static void handle_stop(int signum)
{
	(void) signum;

	/* Async-signal-safe: both of these just set a volatile sig_atomic_t. */
	stop_requested = 1;
	ipa_run_stop();
}

/* Sleep, but wake up promptly on a stop signal.  nanosleep returns EINTR when
 * the handler runs, and the remaining time it reports lets us resume a long
 * interval that was interrupted by something else (SIGCHLD from a vendor
 * library, say) without restarting the whole wait. */
static void interruptible_sleep(unsigned int seconds)
{
	struct timespec remaining;
	struct timespec request;

	request.tv_sec = (time_t) seconds;
	request.tv_nsec = 0;

	while (!stop_requested && nanosleep(&request, &remaining) != 0) {
		if (errno != EINTR)
			return;
		request = remaining;
	}
}

static void install_signal_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handle_stop;
	sigemptyset(&sa.sa_mask);
	/* SA_RESTART so a signal does not turn an in-flight read into EINTR
	 * halfway through an APDU exchange or an HTTP body. */
	sa.sa_flags = SA_RESTART;

	/* SIGTERM is what init sends on "stop"/"ctl.stop"; SIGINT is for running
	 * the daemon by hand from adb shell; SIGUSR1 matches the Linux CLI's
	 * existing graceful-stop convention. */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGUSR1, &sa, NULL);

	/* A dead peer on the eIM connection must not take the daemon with it;
	 * curl reports the failure through its return code instead. */
	signal(SIGPIPE, SIG_IGN);
}

static void print_usage(const char *argv0)
{
	printf("usage: %s [-c PATH] [-i SECONDS]\n", argv0);
	printf("  -c PATH      JSON configuration file (default: %s)\n", DEFAULT_CONFIG_PATH);
	printf("  -i SECONDS   re-run every SECONDS instead of exiting after one\n");
	printf("               poll cycle (default: 0, i.e. run once and exit)\n");
	printf("  -h           print this text\n");
}

int main(int argc, char **argv)
{
	const char *config_path = DEFAULT_CONFIG_PATH;
	unsigned long interval = 0;
	int i;
	int rc;

	/* Deliberately not getopt(): this runs from an init .rc line with a
	 * fixed argument list, and Bionic's getopt would be the only reason to
	 * pull <getopt.h> into the daemon. */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			config_path = argv[++i];
		} else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
			interval = strtoul(argv[++i], NULL, 10);
		} else if (strcmp(argv[i], "-h") == 0) {
			print_usage(argv[0]);
			return EXIT_SUCCESS;
		} else {
			fprintf(stderr, "unrecognized argument: %s\n", argv[i]);
			print_usage(argv[0]);
			return EXIT_FAILURE;
		}
	}

	install_signal_handlers();

	IPA_LOGP(SMAIN, LINFO, "IPAd daemon starting, configuration %s\n", config_path);

	/* A poll cycle ends as soon as the eIM has nothing further pending, so
	 * the IPAd is a run-to-completion program rather than a resident loop.
	 * Repeating it is a scheduling decision, and it is made here rather than
	 * in the library: an init-launched daemon can simply sleep, whereas the
	 * APK must schedule itself through WorkManager / AlarmManager or Doze
	 * will kill it.  Baking a sleep loop into the core would have forced the
	 * wrong primitive on the app. */
	do {
		rc = ipa_run_from_config(config_path);
		if (rc < 0) {
			IPA_LOGP(SMAIN, LERROR, "IPAd poll cycle failed (%d)\n", rc);
			/* Keep going when we are supervising on an interval: an
			 * unreachable eIM or a busy eUICC is usually temporary,
			 * and exiting would just hand the problem to init. */
			if (interval == 0)
				return EXIT_FAILURE;
		}

		if (interval && !stop_requested) {
			IPA_LOGP(SMAIN, LINFO, "next poll cycle in %lu s\n", interval);
			interruptible_sleep((unsigned int) interval);
		}
	} while (interval && !stop_requested);

	IPA_LOGP(SMAIN, LINFO, "IPAd daemon exiting normally\n");
	return EXIT_SUCCESS;
}
