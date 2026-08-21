/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * getopt(3) for MSVC, which ships no equivalent.
 *
 * This is the POSIX short-option subset -- the whole of what src/ipa/main.c
 * uses.  Deliberately NOT implemented, because the IPA does not use them and a
 * silently half-working getopt is worse than an absent one:
 *
 *   - getopt_long() / long options;
 *   - the GNU argument permutation extension.  Like POSIX, this stops at the
 *     first non-option argument, so `ipa -r 0 file -S` leaves -S unparsed;
 *   - the GNU "::" optional-argument suffix in optstring.
 *
 * A leading ':' in optstring selects silent mode and a ':' return for a
 * missing argument, as POSIX specifies; that much is free to support.
 */

#include <stdio.h>
#include <string.h>

#include <onomondo/ipa/compat.h>

char *optarg = NULL;
int optind = 1;
int opterr = 1;
int optopt = 0;

/* Offset of the next option letter inside argv[optind], for clustered forms
 * such as `-SIE`.  Zero means "start of a fresh argument". */
static int nextchar = 0;

/*! Parse the next command line option, POSIX getopt(3) semantics.
 *  \param[in] argc argument count as handed to main().
 *  \param[in] argv argument vector as handed to main().
 *  \param[in] optstring accepted option letters; ':' after a letter means the
 *             option takes an argument, a leading ':' selects silent mode.
 *  \returns the option letter; '?' on an unknown option or (in non-silent
 *           mode) a missing argument; ':' on a missing argument in silent
 *           mode; -1 when there is nothing left to parse. */
int getopt(int argc, char *const argv[], const char *optstring)
{
	const int silent = (optstring[0] == ':');
	const char *spec;
	const char *arg;
	int c;

	/* GNU convention: optind == 0 requests a reset of the scanner state. */
	if (optind == 0) {
		optind = 1;
		nextchar = 0;
	}

	if (nextchar == 0) {
		if (optind >= argc)
			return -1;

		arg = argv[optind];

		/* Not an option: a lone "-", or anything not starting with '-'.
		 * POSIX stops scanning here and leaves it in optind. */
		if (arg == NULL || arg[0] != '-' || arg[1] == '\0')
			return -1;

		/* "--" terminates the options and is itself consumed. */
		if (arg[1] == '-' && arg[2] == '\0') {
			optind++;
			return -1;
		}

		nextchar = 1;
	}

	arg = argv[optind];
	c = (unsigned char)arg[nextchar++];

	/* ':' is never a valid option letter -- in optstring it is the
	 * takes-an-argument marker, so finding it here means the caller passed
	 * "-:" on the command line. */
	spec = (c == ':') ? NULL : strchr(optstring, c);

	if (spec == NULL) {
		optopt = c;
		if (opterr && !silent)
			fprintf(stderr, "%s: invalid option -- '%c'\n",
				argv[0] ? argv[0] : "ipa", c);
		if (arg[nextchar] == '\0') {
			optind++;
			nextchar = 0;
		}
		return '?';
	}

	if (spec[1] == ':') {
		/* Takes an argument, either glued on (-r0) or as the next
		 * element of argv (-r 0). */
		if (arg[nextchar] != '\0') {
			optarg = (char *)&arg[nextchar];
			optind++;
		} else if (optind + 1 < argc) {
			optarg = argv[optind + 1];
			optind += 2;
		} else {
			optarg = NULL;
			optopt = c;
			optind++;
			nextchar = 0;
			if (opterr && !silent)
				fprintf(stderr,
					"%s: option requires an argument -- '%c'\n",
					argv[0] ? argv[0] : "ipa", c);
			return silent ? ':' : '?';
		}
		nextchar = 0;
	} else {
		optarg = NULL;
		/* Clustered flags (-SIE): stay inside this argv element until
		 * its letters are exhausted. */
		if (arg[nextchar] == '\0') {
			optind++;
			nextchar = 0;
		}
	}

	return c;
}
