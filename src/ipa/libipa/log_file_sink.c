/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ANDROID_PORT_PLAN.md, Phase 3 -- rotating-file log sink for the daemon.
 *
 * The daemon has no terminal to write stderr to, and the device it runs on
 * has a small flash it must not fill.  This sink therefore writes to a file
 * and caps the total space the log can occupy at roughly
 * max_size_bytes * max_files.
 *
 * Every record is flushed as it is written.  A daemon that dies mid-session
 * is exactly the case the log exists for, so buffering the last few lines
 * away would defeat the purpose; the write rate is a handful of lines per
 * poll cycle, so the cost does not matter.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/log_sink.h>

/* Widest decimal generation suffix we can append, plus the dot and the NUL. */
#define SUFFIX_MAX 16

static struct {
	FILE *fp;
	char *path;
	size_t max_size_bytes;
	unsigned int max_files;
	size_t cur_size;
	pthread_mutex_t lock;
} file_sink = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

/* Build "<path>.<gen>" into a caller-provided buffer of at least
 * strlen(path) + SUFFIX_MAX bytes. */
static void gen_path(char *out, size_t out_len, const char *path, unsigned int gen)
{
	snprintf(out, out_len, "%s.%u", path, gen);
}

/* Shift the generations along and start a fresh file.  Called with the lock
 * held, and only when the file is known to be open. */
static void rotate(void)
{
	size_t path_len = strlen(file_sink.path) + SUFFIX_MAX;
	char *from = malloc(path_len);
	char *to = malloc(path_len);
	unsigned int i;

	if (!from || !to)
		goto out;

	fclose(file_sink.fp);
	file_sink.fp = NULL;

	if (file_sink.max_files > 1) {
		/* Drop the oldest generation, then walk backwards so no rename
		 * ever overwrites a file we still need. */
		gen_path(to, path_len, file_sink.path, file_sink.max_files - 1);
		remove(to);

		for (i = file_sink.max_files - 1; i >= 2; i--) {
			gen_path(from, path_len, file_sink.path, i - 1);
			gen_path(to, path_len, file_sink.path, i);
			rename(from, to);
		}

		gen_path(to, path_len, file_sink.path, 1);
		rename(file_sink.path, to);
	} else {
		/* No generations kept: just start over. */
		remove(file_sink.path);
	}

	file_sink.fp = fopen(file_sink.path, "w");
	file_sink.cur_size = 0;

out:
	free(from);
	free(to);
}

static void file_sink_write(const char *line, size_t len)
{
	pthread_mutex_lock(&file_sink.lock);

	if (!file_sink.fp)
		goto out;

	/* Rotate before writing, so a record is never split across two files.
	 * cur_size > 0 keeps a single over-sized record from rotating an empty
	 * file forever. */
	if (file_sink.max_size_bytes && file_sink.cur_size > 0 &&
	    file_sink.cur_size + len > file_sink.max_size_bytes) {
		rotate();
		if (!file_sink.fp)
			goto out;
	}

	file_sink.cur_size += fwrite(line, 1, len, file_sink.fp);
	fflush(file_sink.fp);

out:
	pthread_mutex_unlock(&file_sink.lock);
}

int ipa_log_file_sink_init(const char *path, size_t max_size_bytes, unsigned int max_files)
{
	FILE *fp;
	char *path_copy;
	long size;

	if (!path)
		return -EINVAL;

	/* Append rather than truncate: a restart must not throw away the
	 * evidence of why the previous run stopped. */
	fp = fopen(path, "a");
	if (!fp)
		return -errno;

	if (fseek(fp, 0L, SEEK_END) != 0 || (size = ftell(fp)) < 0)
		size = 0;

	path_copy = strdup(path);
	if (!path_copy) {
		fclose(fp);
		return -ENOMEM;
	}

	ipa_log_file_sink_free();

	pthread_mutex_lock(&file_sink.lock);
	file_sink.fp = fp;
	file_sink.path = path_copy;
	file_sink.max_size_bytes = max_size_bytes;
	file_sink.max_files = max_files;
	file_sink.cur_size = (size_t)size;
	pthread_mutex_unlock(&file_sink.lock);

	ipa_log_set_sink(file_sink_write);
	return 0;
}

void ipa_log_file_sink_free(void)
{
	pthread_mutex_lock(&file_sink.lock);

	if (file_sink.fp) {
		fflush(file_sink.fp);
		fclose(file_sink.fp);
		file_sink.fp = NULL;
	}
	free(file_sink.path);
	file_sink.path = NULL;
	file_sink.cur_size = 0;

	pthread_mutex_unlock(&file_sink.lock);

	ipa_log_set_sink(NULL);
}
