/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <stdio.h>
#include <errno.h>
#include <onomondo/ipa/mem.h>
#include <onomondo/ipa/utils.h>
#include "fileio.h"

struct ipa_buf *ipa_file_load(const char *path, size_t extra_bytes)
{
	FILE *file_ptr;
	struct ipa_buf *buf;
	long file_size;

	if (!path)
		return NULL;

	file_ptr = fopen(path, "rb");
	if (!file_ptr)
		return NULL;

	/* ftell on a non-seekable file (a pipe, /proc, ...) returns -1; treat
	 * that as unreadable rather than allocating a bogus size. */
	if (fseek(file_ptr, 0L, SEEK_END) != 0) {
		fclose(file_ptr);
		return NULL;
	}
	file_size = ftell(file_ptr);
	if (file_size < 0) {
		fclose(file_ptr);
		return NULL;
	}
	rewind(file_ptr);

	buf = ipa_buf_alloc((size_t)file_size + extra_bytes);
	assert(buf);

	buf->len = fread(buf->data, sizeof(char), (size_t)file_size, file_ptr);
	fclose(file_ptr);

	return buf;
}

int ipa_file_save(const char *path, const struct ipa_buf *buf)
{
	FILE *file_ptr;
	size_t written;

	if (!path || !buf)
		return -EINVAL;

	file_ptr = fopen(path, "wb");
	if (!file_ptr)
		return -errno;

	written = fwrite(buf->data, sizeof(char), buf->data_len, file_ptr);
	fclose(file_ptr);

	if (written != buf->data_len)
		return -EIO;

	return 0;
}
