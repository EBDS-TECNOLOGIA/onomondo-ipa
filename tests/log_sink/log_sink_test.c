/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Unit tests for the log sink abstraction (ANDROID_PORT_PLAN.md, Phase 3).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <pthread.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/log_sink.h>

static char tmpdir[] = "/tmp/ipa_log_sink_test_XXXXXX";

/* Each call site gets its own buffer: a shared static one would be silently
 * clobbered by a nested call in the same expression. */
#define TMP_PATH(var, name) \
	char var[512]; \
	snprintf(var, sizeof(var), "%s/%s", tmpdir, (name))

static long file_size(const char *path)
{
	FILE *f = fopen(path, "rb");
	long size;

	if (!f)
		return -1;
	fseek(f, 0L, SEEK_END);
	size = ftell(f);
	fclose(f);
	return size;
}

static char *read_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	long size;
	char *buf;

	if (!f)
		return NULL;
	fseek(f, 0L, SEEK_END);
	size = ftell(f);
	rewind(f);
	buf = malloc(size + 1);
	assert(buf);
	buf[fread(buf, 1, size, f)] = '\0';
	fclose(f);
	return buf;
}

/* ------------------------------------------------------------------------ */

static char capture[64 * 1024];
static size_t capture_len;
static unsigned int capture_records;

static void capture_sink(const char *line, size_t len)
{
	assert(capture_len + len < sizeof(capture));
	memcpy(capture + capture_len, line, len);
	capture_len += len;
	capture[capture_len] = '\0';
	capture_records++;
}

/* A sink must receive one whole record per call, newline included -- the ring
 * sink's record-boundary handling depends on it. */
static void sink_dispatch_test(void)
{
	printf("sink_dispatch_test\n");

	capture_len = 0;
	capture_records = 0;
	ipa_log_set_sink(capture_sink);

	IPA_LOGP(SMAIN, LERROR, "hello %s %d\n", "world", 42);

	assert(capture_records == 1);
	assert(strstr(capture, "hello world 42\n"));
	assert(strstr(capture, "MAIN"));
	assert(strstr(capture, "ERROR"));
	assert(capture[capture_len - 1] == '\n');

	/* A record longer than the internal stack buffer must still arrive
	 * whole, in one call, rather than truncated or split. */
	{
		char big[4096];

		memset(big, 'x', sizeof(big) - 1);
		big[sizeof(big) - 1] = '\0';

		capture_len = 0;
		capture_records = 0;
		IPA_LOGP(SMAIN, LINFO, "%s\n", big);

		assert(capture_records == 1);
		assert(strstr(capture, big));
		assert(capture[capture_len - 1] == '\n');
	}

	ipa_log_set_sink(NULL);
}

/* ------------------------------------------------------------------------ */

static void file_sink_basic_test(void)
{
	TMP_PATH(path, "basic.log");
	char *content;

	printf("file_sink_basic_test\n");

	assert(ipa_log_file_sink_init(path, 0, 5) == 0);
	IPA_LOGP(SMAIN, LINFO, "first line\n");
	IPA_LOGP(SMAIN, LINFO, "second line\n");
	ipa_log_file_sink_free();

	content = read_file(path);
	assert(content);
	assert(strstr(content, "first line\n"));
	assert(strstr(content, "second line\n"));
	free(content);

	/* Re-opening appends rather than truncating: a restart must not throw
	 * away why the previous run stopped. */
	assert(ipa_log_file_sink_init(path, 0, 5) == 0);
	IPA_LOGP(SMAIN, LINFO, "third line\n");
	ipa_log_file_sink_free();

	content = read_file(path);
	assert(strstr(content, "first line\n"));
	assert(strstr(content, "third line\n"));
	free(content);
}

static void file_sink_rotation_test(void)
{
	TMP_PATH(path, "rot.log");
	TMP_PATH(gen1, "rot.log.1");
	TMP_PATH(gen2, "rot.log.2");
	TMP_PATH(gen3, "rot.log.3");
	unsigned int i;
	char *content;

	printf("file_sink_rotation_test\n");

	/* 200-byte threshold, 3 files: rot.log plus rot.log.1 and rot.log.2. */
	assert(ipa_log_file_sink_init(path, 200, 3) == 0);
	for (i = 0; i < 60; i++)
		IPA_LOGP(SMAIN, LINFO, "line %02u padded out to a known width\n", i);
	ipa_log_file_sink_free();

	assert(file_size(path) >= 0);
	assert(file_size(gen1) > 0);
	assert(file_size(gen2) > 0);
	/* max_files = 3 counts the live file, so there must be no third
	 * generation. */
	assert(file_size(gen3) < 0);

	/* No file may exceed the threshold, and the newest content is in the
	 * live file. */
	assert(file_size(path) <= 200);
	assert(file_size(gen1) <= 200);
	content = read_file(path);
	assert(strstr(content, "line 59"));
	free(content);
}

static void file_sink_no_rotation_test(void)
{
	TMP_PATH(path, "norot.log");
	TMP_PATH(gen1, "norot.log.1");
	unsigned int i;

	printf("file_sink_no_rotation_test\n");

	/* max_size_bytes = 0 means "let it grow", for when logrotate or the
	 * platform owns the policy. */
	assert(ipa_log_file_sink_init(path, 0, 3) == 0);
	for (i = 0; i < 60; i++)
		IPA_LOGP(SMAIN, LINFO, "line %02u padded out to a known width\n", i);
	ipa_log_file_sink_free();

	assert(file_size(path) > 200);
	assert(file_size(gen1) < 0);
}

static void file_sink_single_file_test(void)
{
	TMP_PATH(path, "single.log");
	TMP_PATH(gen1, "single.log.1");
	unsigned int i;

	printf("file_sink_single_file_test\n");

	/* max_files = 1 keeps no generations: the file just restarts. */
	assert(ipa_log_file_sink_init(path, 200, 1) == 0);
	for (i = 0; i < 60; i++)
		IPA_LOGP(SMAIN, LINFO, "line %02u padded out to a known width\n", i);
	ipa_log_file_sink_free();

	assert(file_size(path) <= 200);
	assert(file_size(gen1) < 0);
}

static void file_sink_error_test(void)
{
	printf("file_sink_error_test\n");

	/* An unopenable path must fail and leave the previous sink in place,
	 * not swallow the log. */
	assert(ipa_log_file_sink_init("/nonexistent-dir/ipa.log", 0, 3) < 0);
	assert(ipa_log_file_sink_init(NULL, 0, 3) < 0);

	capture_len = 0;
	capture_records = 0;
	ipa_log_set_sink(capture_sink);
	assert(ipa_log_file_sink_init("/nonexistent-dir/ipa.log", 0, 3) < 0);
	IPA_LOGP(SMAIN, LINFO, "still captured\n");
	assert(strstr(capture, "still captured\n"));
	ipa_log_set_sink(NULL);
}

/* ------------------------------------------------------------------------ */

static void ring_sink_basic_test(void)
{
	char out[4096];
	size_t n;

	printf("ring_sink_basic_test\n");

	assert(ipa_log_ring_sink_init(4096) == 0);
	assert(ipa_log_ring_sink_avail() == 0);

	IPA_LOGP(SMAIN, LINFO, "alpha\n");
	IPA_LOGP(SMAIN, LINFO, "beta\n");
	assert(ipa_log_ring_sink_avail() > 0);

	n = ipa_log_ring_sink_read(out, sizeof(out));
	assert(n > 0);
	assert(out[n] == '\0');
	assert(strstr(out, "alpha\n"));
	assert(strstr(out, "beta\n"));
	assert(strstr(out, "alpha") < strstr(out, "beta"));

	/* Reading consumes. */
	assert(ipa_log_ring_sink_avail() == 0);
	assert(ipa_log_ring_sink_read(out, sizeof(out)) == 0);
	assert(out[0] == '\0');

	ipa_log_ring_sink_free();
}

static void ring_sink_overflow_test(void)
{
	char out[8192];
	unsigned int i;
	size_t n;

	printf("ring_sink_overflow_test\n");

	assert(ipa_log_ring_sink_init(512) == 0);
	for (i = 0; i < 100; i++)
		IPA_LOGP(SMAIN, LINFO, "record %03u\n", i);

	n = ipa_log_ring_sink_read(out, sizeof(out));
	assert(n > 0);
	assert(n <= 512);

	/* Oldest records were dropped, newest kept ... */
	assert(strstr(out, "record 099\n"));
	assert(!strstr(out, "record 000\n"));

	/* ... and dropping happened on a record boundary, so the buffer does
	 * not start with the tail of a line the reader never saw. */
	assert(strncmp(out, "    MAIN", 8) == 0);

	ipa_log_ring_sink_free();
}

static void ring_sink_oversized_record_test(void)
{
	char big[2048];
	char out[4096];
	size_t n;

	printf("ring_sink_oversized_record_test\n");

	memset(big, 'y', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';

	assert(ipa_log_ring_sink_init(256) == 0);
	IPA_LOGP(SMAIN, LINFO, "%s\n", big);

	n = ipa_log_ring_sink_read(out, sizeof(out));
	/* A record bigger than the whole ring keeps its tail and nothing more. */
	assert(n == 255);
	assert(out[n - 1] == '\n');

	ipa_log_ring_sink_free();
}

static void ring_sink_partial_read_test(void)
{
	char out[8];
	char joined[512] = { 0 };
	size_t n;

	printf("ring_sink_partial_read_test\n");

	assert(ipa_log_ring_sink_init(1024) == 0);
	IPA_LOGP(SMAIN, LINFO, "chunked read\n");

	/* A reader with a small buffer gets the record across several calls,
	 * in order and without loss. */
	while ((n = ipa_log_ring_sink_read(out, sizeof(out))) > 0) {
		assert(n <= sizeof(out) - 1);
		assert(out[n] == '\0');
		strcat(joined, out);
	}
	assert(strstr(joined, "chunked read\n"));

	ipa_log_ring_sink_free();
}

#define WRITER_THREADS 4
#define RECORDS_PER_THREAD 250

static void *writer_thread(void *arg)
{
	unsigned int id = *(unsigned int *)arg;
	unsigned int i;

	for (i = 0; i < RECORDS_PER_THREAD; i++)
		IPA_LOGP(SMAIN, LINFO, "thread %u record %03u\n", id, i);

	return NULL;
}

/* The IPAd logs from its own thread while the UI drains from another, which is
 * what the sink's mutex is there for.  Size the ring so nothing is dropped,
 * then every byte written must come back out exactly once. */
static void ring_sink_threaded_test(void)
{
	pthread_t threads[WRITER_THREADS];
	unsigned int ids[WRITER_THREADS];
	char out[4096];
	size_t total_read = 0;
	size_t expected;
	unsigned int i;
	size_t n;

	printf("ring_sink_threaded_test\n");

	/* "    MAIN     INFO thread N record NNN\n" is a fixed 38 bytes. */
	expected = (size_t)WRITER_THREADS * RECORDS_PER_THREAD * 38;

	assert(ipa_log_ring_sink_init(expected + 1) == 0);

	for (i = 0; i < WRITER_THREADS; i++) {
		ids[i] = i;
		assert(pthread_create(&threads[i], NULL, writer_thread, &ids[i]) == 0);
	}
	for (i = 0; i < WRITER_THREADS; i++)
		assert(pthread_join(threads[i], NULL) == 0);

	while ((n = ipa_log_ring_sink_read(out, sizeof(out))) > 0)
		total_read += n;

	assert(total_read == expected);
	assert(ipa_log_ring_sink_avail() == 0);

	ipa_log_ring_sink_free();
}

/* The file sink and the ring sink coexist: this is what the APK needs, since
 * ipa_run() installs the rotating log file while the UI is already draining the
 * ring for its live view.  Regression test -- these two used to displace each
 * other, which silently blanked the app's log window. */
static void sink_coexist_test(void)
{
	TMP_PATH(path, "coexist.log");
	char out[1024];
	char *content;

	printf("sink_coexist_test\n");

	assert(ipa_log_ring_sink_init(1024) == 0);
	IPA_LOGP(SMAIN, LINFO, "ring only\n");

	assert(ipa_log_file_sink_init(path, 0, 2) == 0);
	IPA_LOGP(SMAIN, LINFO, "to both\n");

	/* The ring kept its earlier record and still receives new ones. */
	ipa_log_ring_sink_read(out, sizeof(out));
	assert(strstr(out, "ring only\n"));
	assert(strstr(out, "to both\n"));

	/* The file has only what was logged after it was installed. */
	content = read_file(path);
	assert(content);
	assert(strstr(content, "to both\n"));
	assert(!strstr(content, "ring only\n"));
	free(content);

	/* Dropping the file sink leaves the ring installed and working. */
	ipa_log_file_sink_free();
	IPA_LOGP(SMAIN, LINFO, "ring again\n");
	ipa_log_ring_sink_read(out, sizeof(out));
	assert(strstr(out, "ring again\n"));

	content = read_file(path);
	assert(content);
	assert(!strstr(content, "ring again\n"));
	free(content);

	ipa_log_ring_sink_free();
}

/* ipa_log_set_sink() keeps its "replace everything" meaning, and removing a
 * sink that was never added is harmless. */
static void sink_set_replaces_test(void)
{
	TMP_PATH(path, "replaced.log");
	char *content;

	printf("sink_set_replaces_test\n");

	assert(ipa_log_file_sink_init(path, 0, 2) == 0);
	IPA_LOGP(SMAIN, LINFO, "before\n");

	/* Displaces the file sink without going through its _free(). */
	capture_len = 0;
	ipa_log_set_sink(capture_sink);
	IPA_LOGP(SMAIN, LINFO, "after\n");
	assert(strstr(capture, "after\n"));

	content = read_file(path);
	assert(content);
	assert(strstr(content, "before\n"));
	assert(!strstr(content, "after\n"));
	free(content);

	/* The file sink is no longer installed, but freeing it must still close
	 * the file cleanly rather than trip over the missing registration. */
	ipa_log_file_sink_free();
	ipa_log_set_sink(NULL);
}

int main(int argc, char **argv)
{
	if (!mkdtemp(tmpdir)) {
		printf("cannot create temp dir\n");
		return 1;
	}

	sink_dispatch_test();
	file_sink_basic_test();
	file_sink_rotation_test();
	file_sink_no_rotation_test();
	file_sink_single_file_test();
	file_sink_error_test();
	ring_sink_basic_test();
	ring_sink_overflow_test();
	ring_sink_oversized_record_test();
	ring_sink_partial_read_test();
	ring_sink_threaded_test();
	sink_coexist_test();
	sink_set_replaces_test();

	printf("log_sink_test: all tests passed\n");
	return 0;
}
