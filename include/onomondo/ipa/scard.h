/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdbool.h>

void *ipa_scard_init(unsigned int reader_num);
int ipa_scard_reset(void *scard_ctx);
int ipa_scard_atr(void *scard_ctx, struct ipa_buf *atr);
int ipa_scard_transceive(void *scard_ctx, struct ipa_buf *res,
			 const struct ipa_buf *req);
int ipa_scard_free(void *scard_ctx);

/*! Query whether the smartcard transport manages the ES10x logical channel
 *  itself.  A PC/SC backend returns false: the core opens the channel with
 *  MANAGE CHANNEL, selects the ISD-R, and ORs the channel number into every
 *  APDU's CLA.  A backend layered on the Android telephony stack (or OMAPI)
 *  returns true: the framework performs MANAGE CHANNEL + the ISD-R SELECT when
 *  the channel is opened and rewrites the CLA channel bits on every transmit,
 *  so the core must NOT do any of that.  See ANDROID_PORT_PLAN.md, Phase 1.
 *  \param[in] scard_ctx smartcard reader context.
 *  \returns true if the transport owns the logical channel, false otherwise. */
bool ipa_scard_manages_channel(void *scard_ctx);
