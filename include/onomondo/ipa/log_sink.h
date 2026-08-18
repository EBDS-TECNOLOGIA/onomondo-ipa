/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

/* ===========================================================================
 * Log sinks (ANDROID_PORT_PLAN.md, Phase 3)
 * ===========================================================================
 *
 * Two ready-made sinks:
 *
 *   - the rotating-file sink, for the daemon, which has nowhere to write
 *     stderr to and must not fill the device's flash;
 *   - the ring-buffer sink, for the APK, whose UI tails the log live.
 *
 * Both install themselves on init (ipa_log_add_sink) and remove themselves on
 * free, so a front-end never has to touch the sink API itself.  Both are safe
 * to call from several threads: the IPAd's poll loop runs on its own thread
 * while the UI drains the ring from another.
 *
 * The two coexist, and the APK needs them to: it installs the ring for its
 * live view, and ipa_run() then installs the file sink from the configured
 * log.path.  Every record reaches both.  A sink only ever receives records
 * logged while it is installed, so the file starts at the moment it opens --
 * the ring's earlier content is not replayed into it.
 */

/* ---------------------------------------------------------------------------
 * Rotating-file sink
 * ------------------------------------------------------------------------ */

/*! Default size at which the log file is rotated (256 KiB). */
#define IPA_DEFAULT_LOG_MAX_SIZE_BYTES (256 * 1024)

/*! Default number of log files kept, current file included. */
#define IPA_DEFAULT_LOG_MAX_FILES 5

/*! Open a log file and make it the active log sink.
 *
 *  Rotation: when a record would push the file past max_size_bytes, the
 *  current file becomes "<path>.1", the previous "<path>.1" becomes
 *  "<path>.2" and so on; the oldest generation is deleted.  max_files counts
 *  the current file, so max_files = 5 keeps <path> plus <path>.1 .. <path>.4.
 *  max_files <= 1 keeps no generations at all, i.e. the file is simply
 *  restarted when it reaches the limit.
 *
 *  An existing file is appended to, and its current size counts towards the
 *  rotation threshold, so restarting the daemon does not lose the log or
 *  bypass the size limit.
 *
 *  \param[in] path log file path.
 *  \param[in] max_size_bytes rotation threshold; 0 disables rotation, letting
 *             the file grow without bound (useful when something else, e.g.
 *             logrotate, owns the policy).
 *  \param[in] max_files number of files to keep, current file included.
 *  \returns 0 on success, negative on error -- in which case the previously
 *           active sink stays installed. */
int ipa_log_file_sink_init(const char *path, size_t max_size_bytes, unsigned int max_files);

/*! Flush and close the log file and restore the stderr sink.  NULL-safe in
 *  the sense that it may be called when no file sink is active. */
void ipa_log_file_sink_free(void);

/* ---------------------------------------------------------------------------
 * Ring-buffer sink
 * ------------------------------------------------------------------------ */

/*! Default ring-buffer capacity (64 KiB). */
#define IPA_DEFAULT_LOG_RING_BYTES (64 * 1024)

/*! Allocate an in-memory ring buffer and make it the active log sink.
 *
 *  When the buffer is full the oldest content is dropped, always up to a
 *  record boundary, so a reader never sees the tail end of a truncated line
 *  at the front of the buffer.  A single record larger than the whole buffer
 *  keeps only its last capacity_bytes-1 bytes.
 *
 *  \param[in] capacity_bytes ring capacity; 0 selects
 *             IPA_DEFAULT_LOG_RING_BYTES.
 *  \returns 0 on success, negative on error. */
int ipa_log_ring_sink_init(size_t capacity_bytes);

/*! Free the ring buffer and restore the stderr sink. */
void ipa_log_ring_sink_free(void);

/*! Number of bytes currently waiting to be read. */
size_t ipa_log_ring_sink_avail(void);

/*! Drain buffered log text into out, consuming what it copies.
 *
 *  Copies at most out_len-1 bytes and always NUL-terminates, so the result can
 *  be handed straight to NewStringUTF().  A record may be split across two
 *  reads when it does not fit; that is harmless for a UI that appends the
 *  chunks in order.
 *
 *  \param[out] out destination buffer.
 *  \param[in] out_len size of out in bytes (must be >= 1).
 *  \returns number of bytes copied, excluding the NUL. */
size_t ipa_log_ring_sink_read(char *out, size_t out_len);
