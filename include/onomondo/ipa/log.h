/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdio.h>
#include <stdint.h>

/*! macro to print a log line.
 *  \param[in] subsys log subsystem identifier.
 *  \param[in] level log level identifier.
 *  \param[in] fmt formtstring.
 *  \param[in] args formatstring arguments. */
#ifdef _MSC_VER
/* MSVC has neither GNU named variadic macro parameters (args...) nor the
 * ##__VA_ARGS__ comma-swallowing extension.  Folding the format string into
 * __VA_ARGS__ sidesteps both: every call site passes at least a format string,
 * so __VA_ARGS__ is never empty and there is no dangling comma to remove.
 * Kept as a separate definition rather than replacing the GNU one so the other
 * platforms' preprocessor output is untouched. */
#define IPA_LOGP(subsys, level, ...) \
	ipa_logp(subsys, level, __FILE__, __LINE__, __VA_ARGS__)
#else
#define IPA_LOGP(subsys, level, fmt, args...) \
	ipa_logp(subsys, level, __FILE__, __LINE__, fmt, ## args)
#endif

/* printf-style format checking is a GCC/Clang attribute; MSVC has no
 * equivalent that works on a plain C function declaration. */
#ifdef __GNUC__
#define IPA_PRINTF_FMT(fmt_idx, arg_idx) __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define IPA_PRINTF_FMT(fmt_idx, arg_idx)
#endif

void ipa_logp(uint32_t subsys, uint32_t level, const char *file, int line,
	      const char *format, ...)
    IPA_PRINTF_FMT(5, 6);

enum log_subsys {
	SMAIN,
	SHTTP,
	SSCARD,
	SIPA,
	SES10X,
	SES10B,
	SEUICC,
	SESIPA,
	_NUM_LOG_SUBSYS
};

enum log_level {
	LERROR,
	LINFO,
	LDEBUG,
	_NUM_LOG_LEVEL
};
