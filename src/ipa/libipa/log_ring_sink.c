/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * ANDROID_PORT_PLAN.md, Phase 3 -- in-memory ring-buffer log sink for the APK.
 *
 * The APK shows the log live while the IPAd runs.  Writers (the poll loop, on
 * its own native thread) and the reader (the UI, polling from another) meet
 * here: records go in at the tail, the UI drains from the head, and when the
 * buffer fills the oldest records are dropped rather than blocking the IPAd
 * or growing without bound.
 *
 * Oldest content is always dropped up to a record boundary, so the reader
 * never receives the tail half of a line it never saw the start of.
 */

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/log_sink.h>

static struct {
	char *buf;
	size_t cap;
	size_t head; /* offset of the oldest byte */
	size_t size; /* bytes currently stored */
	pthread_mutex_t lock;
} ring = {
	.lock = PTHREAD_MUTEX_INITIALIZER,
};

/* Drop the oldest record, i.e. everything up to and including the next
 * newline.  If there is no newline the whole buffer is one partial record and
 * goes as a unit.  Called with the lock held. */
static void drop_oldest_record(void)
{
	size_t i;

	for (i = 0; i < ring.size; i++) {
		if (ring.buf[(ring.head + i) % ring.cap] == '\n') {
			ring.head = (ring.head + i + 1) % ring.cap;
			ring.size -= i + 1;
			return;
		}
	}

	ring.head = 0;
	ring.size = 0;
}

/* Copy len bytes into the ring at the tail.  Caller guarantees they fit. */
static void append(const char *data, size_t len)
{
	size_t tail = (ring.head + ring.size) % ring.cap;
	size_t first = ring.cap - tail;

	if (first > len)
		first = len;

	memcpy(ring.buf + tail, data, first);
	if (len > first)
		memcpy(ring.buf, data + first, len - first);

	ring.size += len;
}

static void ring_sink_write(const char *line, size_t len)
{
	pthread_mutex_lock(&ring.lock);

	if (!ring.buf || len == 0)
		goto out;

	/* A record too big for the whole buffer cannot be kept intact; keep its
	 * end, which is where the interesting part of a truncated hexdump is. */
	if (len >= ring.cap) {
		line += len - (ring.cap - 1);
		len = ring.cap - 1;
		ring.head = 0;
		ring.size = 0;
	}

	while (ring.size + len > ring.cap)
		drop_oldest_record();

	append(line, len);

out:
	pthread_mutex_unlock(&ring.lock);
}

int ipa_log_ring_sink_init(size_t capacity_bytes)
{
	char *buf;

	if (capacity_bytes == 0)
		capacity_bytes = IPA_DEFAULT_LOG_RING_BYTES;

	buf = malloc(capacity_bytes);
	if (!buf)
		return -ENOMEM;

	ipa_log_ring_sink_free();

	pthread_mutex_lock(&ring.lock);
	ring.buf = buf;
	ring.cap = capacity_bytes;
	ring.head = 0;
	ring.size = 0;
	pthread_mutex_unlock(&ring.lock);

	ipa_log_add_sink(ring_sink_write);
	return 0;
}

void ipa_log_ring_sink_free(void)
{
	pthread_mutex_lock(&ring.lock);
	free(ring.buf);
	ring.buf = NULL;
	ring.cap = 0;
	ring.head = 0;
	ring.size = 0;
	pthread_mutex_unlock(&ring.lock);

	ipa_log_del_sink(ring_sink_write);
}

size_t ipa_log_ring_sink_avail(void)
{
	size_t avail;

	pthread_mutex_lock(&ring.lock);
	avail = ring.size;
	pthread_mutex_unlock(&ring.lock);

	return avail;
}

size_t ipa_log_ring_sink_read(char *out, size_t out_len)
{
	size_t n;
	size_t first;

	if (!out || out_len == 0)
		return 0;

	pthread_mutex_lock(&ring.lock);

	n = ring.size;
	if (n > out_len - 1)
		n = out_len - 1;

	first = ring.cap ? ring.cap - ring.head : 0;
	if (first > n)
		first = n;

	if (n) {
		memcpy(out, ring.buf + ring.head, first);
		if (n > first)
			memcpy(out + first, ring.buf, n - first);

		ring.head = (ring.head + n) % ring.cap;
		ring.size -= n;
	}

	pthread_mutex_unlock(&ring.lock);

	out[n] = '\0';
	return n;
}
