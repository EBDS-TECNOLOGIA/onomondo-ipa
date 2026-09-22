/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

/* ===========================================================================
 * Choosing how the eUICC is reached (OPENWRT_PORT_ANALYSIS.md, section 3)
 * ===========================================================================
 *
 * The core reaches the eUICC through the ipa_scard_* functions of scard.h and does not care what is behind
 * them. On a router there is no smart-card reader: the eUICC sits behind the cellular modem. This picks the
 * implementation at run time, so one binary serves both, and is called by the front end before ipa_init().
 *
 * Transports, named by a URI:
 *
 *   pcsc:N                     PC/SC reader number N (the default, N from ipa_config.reader_num)
 *   at:/dev/ttyUSB2[?options]  APDUs through a modem's AT+CSIM command (3GPP TS 27.007 section 8.17)
 *
 * Options of the AT transport, appended as ?name=value&name=value:
 *
 *   timeout=MS    how long to wait for a response to one AT command; default 5000
 *   baud=RATE     line speed, for a real serial port; USB ports ignore it. Default: left as it is
 *   reset=MODE    how to reset the eUICC, which an eUICC asks for after a profile change:
 *                 none (default) leaves it to the platform and reports the reset as unsupported;
 *                 cfun sends AT+CFUN=0 followed by AT+CFUN=1, which power-cycles the card and the radio
 *   resetwait=MS  how long to wait for the card after a reset; default 20000
 *   quirks=...    comma separated; "echo" keeps the modem's command echo on (ATE0 is sent otherwise)
 */

/*! Select the transport for the contexts opened from now on.
 *  \param[in] uri transport URI, or NULL for the PC/SC default.
 *  \returns 0 when the URI names a transport this build has, -EINVAL otherwise (the previous one stays). */
int ipa_scard_set_transport(const char *uri);

/*! The transport URI in use, for log messages; never NULL. */
const char *ipa_scard_get_transport(void);
