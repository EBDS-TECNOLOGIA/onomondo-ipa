/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <onomondo/ipa/utils.h>

/* The AT+CSIM transport, behind the dispatcher in scard_dispatch.c. See scard_at.c and, for the URI it takes,
 * onomondo/ipa/scard_transport.h. */

void *ipa_scard_at_init(const char *uri, unsigned int reader_num);
int ipa_scard_at_reset(void *scard_ctx);
int ipa_scard_at_atr(void *scard_ctx, struct ipa_buf *atr);
int ipa_scard_at_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req);
int ipa_scard_at_free(void *scard_ctx);
