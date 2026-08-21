/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Implementations for the shims declared in <onomondo/ipa/compat.h>.  This is
 * the only translation unit that includes <windows.h>, keeping the WinAPI
 * macro soup out of the rest of the tree.  Built only when WIN32.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <onomondo/ipa/compat.h>

/*! Suspend the calling thread, POSIX sleep(3) semantics.
 *  \param[in] seconds time to sleep.
 *  \returns 0; the Win32 Sleep() cannot be interrupted by a signal, so there
 *           is never an unslept remainder to report. */
unsigned int sleep(unsigned int seconds)
{
	/* Sleep() takes milliseconds in a DWORD.  Clamp rather than wrap: the
	 * ESipa retry backoff is quadratic, and a caller asking for more than
	 * ~49 days wants "a very long time", not 300 ms. */
	if (seconds > MAXDWORD / 1000)
		Sleep(MAXDWORD);
	else
		Sleep((DWORD) seconds * 1000);

	return 0;
}
