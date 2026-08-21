/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Portability shims for building the CLI on Windows with MSVC.
 *
 * This header expands to nothing on every other platform, so it can be
 * included unconditionally next to the POSIX headers it stands in for.  It
 * deliberately does NOT pull in <windows.h> -- that would leak WinAPI macros
 * (min/max, near/far, ERROR, ...) into translation units that have no business
 * seeing them.  The few functions declared here are implemented in
 * src/ipa/compat/compat_win32.c, which is the only place that includes it.
 */

#pragma once

/* ------------------------------------------------------------------------ *
 * Always active: compiler-extension spellings that differ between toolchains.
 * ------------------------------------------------------------------------ */

/* Struct packing.  GCC/Clang take a trailing attribute; MSVC takes a pragma
 * pair around the declaration and __pragma() is the form usable from a macro.
 * Use all three together:
 *
 *     IPA_PACKED_PUSH
 *     struct foo { ... } IPA_PACKED_ATTR;
 *     IPA_PACKED_POP
 *
 * On each compiler two of the three expand to nothing, so the layout comes out
 * the same either way. */
#ifdef _MSC_VER
#define IPA_PACKED_PUSH __pragma(pack(push, 1))
#define IPA_PACKED_POP  __pragma(pack(pop))
#define IPA_PACKED_ATTR
#else
#define IPA_PACKED_PUSH
#define IPA_PACKED_POP
#define IPA_PACKED_ATTR __attribute__((packed))
#endif

/* ------------------------------------------------------------------------ *
 * Windows only: POSIX interfaces the MS CRT does not provide.
 * ------------------------------------------------------------------------ */

#ifdef _WIN32

#include <stdlib.h>		/* _MAX_PATH */
#include <io.h>			/* _access */
#include <BaseTsd.h>		/* SSIZE_T */

/* POSIX spells the maximum path length PATH_MAX in <limits.h>; the MS CRT
 * spells it _MAX_PATH in <stdlib.h> and puts nothing in <limits.h>. */
#ifndef PATH_MAX
#define PATH_MAX _MAX_PATH
#endif

/* _access() takes the same mode bits as access(2) but names none of them.
 * Only R_OK is used by the IPA; the others are here so the set is complete. */
#ifndef R_OK
#define F_OK 0
#define W_OK 2
#define R_OK 4
#endif
#ifndef access
#define access _access
#endif

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef SSIZE_T ssize_t;
#endif

/*! Suspend the calling thread, POSIX sleep(3) semantics.
 *  Wraps the Win32 Sleep(), which takes milliseconds and returns void.
 *  \param[in] seconds time to sleep.
 *  \returns 0 (the sleep is never interrupted here, so there is no remainder). */
unsigned int sleep(unsigned int seconds);

/* ------------------------------------------------------------------------ *
 * getopt(3)
 *
 * The MS CRT has no getopt.  src/ipa/compat/getopt_win32.c provides the POSIX
 * short-option subset that src/ipa/main.c uses (no getopt_long, no GNU
 * argument permutation).
 * ------------------------------------------------------------------------ */
extern char *optarg;
extern int optind;
extern int opterr;
extern int optopt;

int getopt(int argc, char *const argv[], const char *optstring);

#endif /* _WIN32 */
