/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Tests for how ipa_run() stores the nvstate on a device that loses power and wears its flash: the file is
 * replaced atomically (never truncated in place), and not rewritten at all when nothing changed.
 *
 * The eUICC and eIM backends are stubbed so that ipa_init() fails at once; the run still creates a context and
 * saves its (fresh) nvstate, which is all these tests need.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/http.h>
#include <onomondo/ipa/config_json.h>
#include <onomondo/ipa/run_observer.h>
#include "src/ipa/libipa/fileio.h"

void *ipa_scard_init(unsigned int reader_num) { (void)reader_num; return NULL; }
int ipa_scard_reset(void *c) { (void)c; return -1; }
int ipa_scard_atr(void *c, struct ipa_buf *a) { (void)c; (void)a; return -1; }
int ipa_scard_transceive(void *c, struct ipa_buf *r, const struct ipa_buf *s)
{ (void)c; (void)r; (void)s; return -1; }
int ipa_scard_free(void *c) { (void)c; return 0; }

void *ipa_http_init(const char *ca, bool nv) { (void)ca; (void)nv; return (void *)1; }
struct ipa_buf *ipa_http_req(void *c, const struct ipa_buf *r, const char *u)
{ (void)c; (void)r; (void)u; return NULL; }
struct ipa_buf *ipa_http_req_with_ct(void *c, const struct ipa_buf *r, const char *u, const char *t)
{ (void)c; (void)r; (void)u; (void)t; return NULL; }
void ipa_http_close(void *c) { (void)c; }
void ipa_http_free(void *c) { (void)c; }
void ipa_http_set_timeouts(void *c, long a, long b) { (void)c; (void)a; (void)b; }
int ipa_http_set_ca_cert_der(void *c, const uint8_t *d, size_t l) { (void)c; (void)d; (void)l; return 0; }
int ipa_http_set_ca_pk_spki(void *c, const uint8_t *s, size_t l) { (void)c; (void)s; (void)l; return 0; }

static unsigned int observer_calls;

static void observer(struct ipa_context *ctx, enum ipa_run_event ev, int rc, void *priv)
{
	(void)ctx;
	(void)ev;
	(void)rc;
	(void)priv;
	observer_calls++;
}

static char tmpdir[] = "/tmp/ipa_nvstate_save_test_XXXXXX";

static ino_t inode_of(const char *path)
{
	struct stat st;

	assert(stat(path, &st) == 0);
	return st.st_ino;
}

static struct ipa_run_config *config_for(const char *nvstate_path)
{
	char json[512];
	struct ipa_run_config *rcfg;

	snprintf(json, sizeof(json), "{\"nvstate_path\": \"%s\"}", nvstate_path);
	rcfg = ipa_config_json_parse(json, strlen(json));
	return rcfg;
}

/* ipa_file_save() replaces the file through a rename: the inode changes, no temporary is left behind, and the
 * content is complete. */
static void atomic_save_test(const char *path)
{
	struct ipa_buf *buf = ipa_buf_alloc_data(5, (uint8_t *)"hello");
	struct ipa_buf *back;
	char tmp_path[256];
	ino_t before;

	printf("atomic_save_test\n");

	assert(ipa_file_save(path, buf) == 0);
	before = inode_of(path);
	assert(ipa_file_save(path, buf) == 0);
	assert(inode_of(path) != before);

	snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
	assert(access(tmp_path, F_OK) != 0);

	back = ipa_file_load(path, 0);
	assert(back && back->len == 5 && memcmp(back->data, "hello", 5) == 0);
	IPA_FREE(back);

	/* A path that is not a regular file is written in place, not replaced. */
	assert(ipa_file_save("/dev/null", buf) == 0);
	assert(access("/dev/null.tmp", F_OK) != 0);

	/* A directory that does not exist fails cleanly. */
	assert(ipa_file_save("/nonexistent-dir/nvstate.bin", buf) < 0);
	IPA_FREE(buf);
	unlink(path);
}

static void write_on_change_test(const char *path)
{
	struct ipa_run_config *rcfg = config_for(path);
	ino_t first;

	printf("write_on_change_test\n");
	if (!rcfg) {
		printf("built without jansson -- skipping\n");
		return;
	}

	/* No file yet: the run creates it. */
	assert(ipa_run(rcfg) < 0); /* ipa_init() fails, as the stubs arrange */
	first = inode_of(path);

	/* Same state again: the file is left alone. */
	assert(ipa_run(rcfg) < 0);
	assert(inode_of(path) == first);

	/* A truncated file (the old failure mode) is replaced by a complete one. */
	assert(truncate(path, 3) == 0);
	assert(ipa_run(rcfg) < 0);
	assert(inode_of(path) != first);

	/* ipa_init() failed every time, so the observer was never called. */
	assert(observer_calls == 0);

	ipa_run_config_free(rcfg);
	unlink(path);
}

int main(void)
{
	char path[256];

	assert(mkdtemp(tmpdir));
	snprintf(path, sizeof(path), "%s/nvstate.bin", tmpdir);

	ipa_run_set_observer(observer, NULL);

	atomic_save_test(path);
	write_on_change_test(path);

	rmdir(tmpdir);
	printf("nvstate_save_test: all tests passed\n");
	return 0;
}
