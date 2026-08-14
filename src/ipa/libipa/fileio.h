/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stddef.h>

struct ipa_buf;

/* Minimal whole-file helpers, shared by the CLI entry point (main.c) and the
 * config-file driven entry point (run.c) so both read and write the nvstate
 * and BER blobs through exactly one implementation.  Neither function logs:
 * the callers report failures in their own wording. */

/*! Read a whole file into a freshly allocated buffer.
 *  \param[in] path file to read.
 *  \param[in] extra_bytes number of spare bytes to allocate past the file
 *             size (0 for an exact fit).  buf->data_len covers the spare
 *             bytes, buf->len counts only what was actually read.
 *  \returns newly allocated buffer (free with IPA_FREE), or NULL when the
 *           file cannot be opened or its size cannot be determined. */
struct ipa_buf *ipa_file_load(const char *path, size_t extra_bytes);

/*! Write a buffer to a file, truncating any previous content.
 *  NB: writes buf->data_len bytes, not buf->len -- the nvstate blob is
 *  serialized to fill the whole allocation.
 *  \param[in] path file to write.
 *  \param[in] buf buffer to write.
 *  \returns 0 on success, negative on error. */
int ipa_file_save(const char *path, const struct ipa_buf *buf);
