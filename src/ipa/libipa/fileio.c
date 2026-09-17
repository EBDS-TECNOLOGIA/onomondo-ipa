/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
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

/* Make a completed rename survive a power cut: the new directory entry is only durable once the directory itself
 * has been synced. Best effort -- not every filesystem lets a directory be opened this way, and the data is safe
 * either way; what could be lost is only which of the two complete files the name points at. */
static void sync_parent_dir(const char *path)
{
	const char *slash = strrchr(path, '/');
	char *dir;
	int fd;

	if (!slash)
		dir = strdup(".");
	else if (slash == path)
		dir = strdup("/");
	else
		dir = strndup(path, (size_t)(slash - path));
	if (!dir)
		return;

	fd = open(dir, O_RDONLY);
	if (fd >= 0) {
		fsync(fd);
		close(fd);
	}
	free(dir);
}

/* Plain in-place write, for paths that are not regular files. */
static int save_in_place(const char *path, const struct ipa_buf *buf)
{
	FILE *file_ptr;
	size_t written;

	file_ptr = fopen(path, "wb");
	if (!file_ptr)
		return -errno;

	written = fwrite(buf->data, sizeof(char), buf->data_len, file_ptr);
	fclose(file_ptr);

	return written == buf->data_len ? 0 : -EIO;
}

/* Routers lose power routinely, and a file truncated by "w" but not yet rewritten is exactly what a power cut
 * leaves behind. So the new content goes to "<path>.tmp" first, is synced to storage, and only then replaces the
 * old file by rename(), which is atomic: after a power cut the file holds either the old or the new content, never
 * a mix or nothing. */
int ipa_file_save(const char *path, const struct ipa_buf *buf)
{
	FILE *file_ptr;
	size_t written;
	char *tmp_path;
	struct stat st;
	int rc = 0;

	if (!path || !buf)
		return -EINVAL;

	/* Renaming over a device or a FIFO (/dev/null in a test, say) would replace it rather than write to it. */
	if (stat(path, &st) == 0 && !S_ISREG(st.st_mode))
		return save_in_place(path, buf);

	tmp_path = malloc(strlen(path) + sizeof(".tmp"));
	if (!tmp_path)
		return -ENOMEM;
	sprintf(tmp_path, "%s.tmp", path);

	file_ptr = fopen(tmp_path, "wb");
	if (!file_ptr) {
		rc = -errno;
		goto out;
	}

	written = fwrite(buf->data, sizeof(char), buf->data_len, file_ptr);
	if (written != buf->data_len || fflush(file_ptr) != 0 || fsync(fileno(file_ptr)) != 0)
		rc = -EIO;
	if (fclose(file_ptr) != 0 && rc == 0)
		rc = -EIO;

	if (rc == 0 && rename(tmp_path, path) != 0)
		rc = -errno;

	if (rc == 0)
		sync_parent_dir(path);
	else
		remove(tmp_path);
out:
	free(tmp_path);
	return rc;
}
