/*
 * Copyright (c) 2026 Onomondo ApS & sysmocom - s.f.m.c. GmbH & EBDS Tecnologia Ltda. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * eUICC transport over a modem's AT command port, using AT+CSIM (3GPP TS 27.007 section 8.17): the APDU is
 * passed to the card as it is, so the core keeps doing what it does over PC/SC -- open the logical channel,
 * select the ISD-R, chain GET RESPONSE. AT+CCHO/AT+CGLA, where the modem owns the channel, are deliberately not
 * used: the two modems tested either do not implement them (Fibocom NL668) or do not need them (Quectel
 * EC200A, where AT+CSIM reads the EID of two different eUICCs).
 *
 * What this expects of the modem:
 *
 *   - AT+CSIM passes APDUs through, MANAGE CHANNEL and SELECT included;
 *   - the card, not the modem, resolves 61xx chaining (the core sends GET RESPONSE itself);
 *   - the channel number comes from the card (ipa_config.euicc_channel = IPA_EUICC_CHANNEL_AUTO), because the
 *     modem keeps channels of its own.
 *
 * Sharing the port: the port is locked with flock(2), which keeps two IPAd instances apart but says nothing to
 * ModemManager, which locks nothing. Give the IPAd a port ModemManager does not use, or stop it for the test;
 * see contrib/openwrt/README.md.
 *
 * Unsolicited result codes (+CREG, RING, ...) may arrive between the command and its answer and are skipped.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#include <poll.h>
#include <time.h>
#include <sys/file.h>
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/mem.h>
#include "scard_at.h"

/* A command APDU is at most 261 bytes, twice that in hex, plus "AT+CSIM=261," and the quotes. The response of a
 * GET RESPONSE can carry 256 bytes plus the status word. Room for a long unsolicited line on top. */
#define AT_LINE_MAX 2048

/* Defaults, see scard_transport.h. */
#define AT_TIMEOUT_MS 5000
#define AT_RESET_WAIT_MS 20000

/* AT+CFUN=0 takes a while to detach from the network. */
#define AT_CFUN_SETTLE_MS 2000

enum at_reset_mode {
	AT_RESET_NONE,
	AT_RESET_CFUN,
};

struct at_ctx {
	int fd;
	char *dev;
	unsigned int timeout_ms;
	unsigned int reset_wait_ms;
	enum at_reset_mode reset_mode;
	speed_t baud;    /* B0 when the line speed is left alone */
	bool keep_echo;

	/* Bytes read from the port but not yet consumed as a line. */
	char rx[AT_LINE_MAX];
	size_t rx_len;
};

/* ------------------------------------------------------------------------
 * URI
 * --------------------------------------------------------------------- */

static speed_t baud_of(unsigned long rate)
{
	switch (rate) {
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default: return B0;
	}
}

/* Read "name=value" pairs separated by & into the context. */
static int parse_options(struct at_ctx *ctx, char *opts)
{
	char *pair;
	char *save = NULL;

	for (pair = strtok_r(opts, "&", &save); pair; pair = strtok_r(NULL, "&", &save)) {
		char *value = strchr(pair, '=');

		if (!value) {
			IPA_LOGP(SSCARD, LERROR, "AT transport: option \"%s\" has no value\n", pair);
			return -EINVAL;
		}
		*value++ = '\0';

		if (strcmp(pair, "timeout") == 0) {
			ctx->timeout_ms = (unsigned int)strtoul(value, NULL, 10);
		} else if (strcmp(pair, "resetwait") == 0) {
			ctx->reset_wait_ms = (unsigned int)strtoul(value, NULL, 10);
		} else if (strcmp(pair, "baud") == 0) {
			ctx->baud = baud_of(strtoul(value, NULL, 10));
			if (ctx->baud == B0) {
				IPA_LOGP(SSCARD, LERROR, "AT transport: unsupported baud rate \"%s\"\n", value);
				return -EINVAL;
			}
		} else if (strcmp(pair, "reset") == 0) {
			if (strcmp(value, "none") == 0)
				ctx->reset_mode = AT_RESET_NONE;
			else if (strcmp(value, "cfun") == 0)
				ctx->reset_mode = AT_RESET_CFUN;
			else {
				IPA_LOGP(SSCARD, LERROR, "AT transport: unknown reset mode \"%s\"\n", value);
				return -EINVAL;
			}
		} else if (strcmp(pair, "quirks") == 0) {
			ctx->keep_echo = strstr(value, "echo") != NULL;
		} else {
			IPA_LOGP(SSCARD, LERROR, "AT transport: unknown option \"%s\"\n", pair);
			return -EINVAL;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------------
 * Port
 * --------------------------------------------------------------------- */

static int open_port(struct at_ctx *ctx)
{
	struct termios tio;

	ctx->fd = open(ctx->dev, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
	if (ctx->fd < 0) {
		IPA_LOGP(SSCARD, LERROR, "AT transport: cannot open %s: %s\n", ctx->dev, strerror(errno));
		return -errno;
	}

	/* Against a second IPAd on the same port. ModemManager takes no such lock, so it is not kept out by this. */
	if (flock(ctx->fd, LOCK_EX | LOCK_NB) != 0) {
		IPA_LOGP(SSCARD, LERROR, "AT transport: %s is locked by another process\n", ctx->dev);
		close(ctx->fd);
		ctx->fd = -1;
		return -EBUSY;
	}

	if (tcgetattr(ctx->fd, &tio) == 0) {
		cfmakeraw(&tio);
		tio.c_cflag |= CLOCAL | CREAD;
		tio.c_cflag &= ~CRTSCTS;
		tio.c_cc[VMIN] = 0;
		tio.c_cc[VTIME] = 0;
		if (ctx->baud != B0) {
			cfsetispeed(&tio, ctx->baud);
			cfsetospeed(&tio, ctx->baud);
		}
		if (tcsetattr(ctx->fd, TCSANOW, &tio) != 0)
			IPA_LOGP(SSCARD, LINFO, "AT transport: cannot set terminal attributes on %s: %s\n",
				 ctx->dev, strerror(errno));
		tcflush(ctx->fd, TCIOFLUSH);
	} else if (errno != ENOTTY) {
		/* Not a terminal is fine (a pipe in the tests); anything else is worth a line. */
		IPA_LOGP(SSCARD, LINFO, "AT transport: %s is not a terminal (%s)\n", ctx->dev, strerror(errno));
	}

	return 0;
}

static unsigned long now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long)ts.tv_sec * 1000UL + (unsigned long)(ts.tv_nsec / 1000000L);
}

/* Read one line into out, without its terminator. Empty lines are skipped, as modems frame everything in
 * <CR><LF>. Returns the length, -ETIMEDOUT when the deadline passed, or another negative errno. */
static int read_line(struct at_ctx *ctx, char *out, size_t out_len, unsigned long deadline)
{
	for (;;) {
		char *nl = memchr(ctx->rx, '\n', ctx->rx_len);
		struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
		unsigned long now;
		size_t line_len;
		ssize_t got;
		int rc;

		if (nl) {
			line_len = (size_t)(nl - ctx->rx);
			if (line_len && ctx->rx[line_len - 1] == '\r')
				line_len--;
			if (line_len >= out_len)
				line_len = out_len - 1;
			memcpy(out, ctx->rx, line_len);
			out[line_len] = '\0';

			ctx->rx_len -= (size_t)(nl - ctx->rx) + 1;
			memmove(ctx->rx, nl + 1, ctx->rx_len);

			if (line_len == 0)
				continue;
			return (int)line_len;
		}

		now = now_ms();
		if (now >= deadline)
			return -ETIMEDOUT;

		rc = poll(&pfd, 1, (int)(deadline - now));
		if (rc < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (rc == 0)
			return -ETIMEDOUT;

		if (ctx->rx_len == sizeof(ctx->rx)) {
			/* A line longer than anything this protocol produces; drop what was collected rather than
			 * stall for ever. */
			IPA_LOGP(SSCARD, LERROR, "AT transport: response line too long, discarding it\n");
			ctx->rx_len = 0;
		}
		got = read(ctx->fd, ctx->rx + ctx->rx_len, sizeof(ctx->rx) - ctx->rx_len);
		if (got < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -errno;
		}
		if (got == 0)
			return -EIO; /* the modem went away */
		ctx->rx_len += (size_t)got;
	}
}

static int write_all(struct at_ctx *ctx, const char *data, size_t len)
{
	while (len) {
		ssize_t put = write(ctx->fd, data, len);

		if (put < 0) {
			if (errno == EINTR || errno == EAGAIN)
				continue;
			return -errno;
		}
		data += put;
		len -= (size_t)put;
	}
	return 0;
}

/* Send one AT command and wait for its final result code. A line starting with prefix (when given) is copied to
 * out; other lines are unsolicited result codes and are skipped. Returns 0 on "OK", -EIO on an error result. */
static int at_command(struct at_ctx *ctx, const char *cmd, const char *prefix, char *out, size_t out_len,
		      unsigned int timeout_ms)
{
	char line[AT_LINE_MAX];
	unsigned long deadline;
	size_t prefix_len = prefix ? strlen(prefix) : 0;
	bool got_wanted = false;
	int rc;

	IPA_LOGP(SSCARD, LDEBUG, "AT transport: > %s\n", cmd);

	rc = write_all(ctx, cmd, strlen(cmd));
	if (rc == 0)
		rc = write_all(ctx, "\r", 1);
	if (rc < 0) {
		IPA_LOGP(SSCARD, LERROR, "AT transport: cannot write to %s: %s\n", ctx->dev, strerror(-rc));
		return rc;
	}

	deadline = now_ms() + timeout_ms;
	for (;;) {
		rc = read_line(ctx, line, sizeof(line), deadline);
		if (rc < 0) {
			IPA_LOGP(SSCARD, LERROR, "AT transport: no answer to \"%s\" (%s)\n", cmd,
				 rc == -ETIMEDOUT ? "timeout" : strerror(-rc));
			return rc;
		}
		IPA_LOGP(SSCARD, LDEBUG, "AT transport: < %s\n", line);

		/* The command itself, echoed back by a modem that was not told to stop. */
		if (strcmp(line, cmd) == 0)
			continue;

		if (strcmp(line, "OK") == 0)
			return (prefix && !got_wanted) ? -ENOMSG : 0;
		if (strcmp(line, "ERROR") == 0 || strncmp(line, "+CME ERROR:", 11) == 0 ||
		    strncmp(line, "+CMS ERROR:", 11) == 0) {
			IPA_LOGP(SSCARD, LERROR, "AT transport: \"%s\" failed: %s\n", cmd, line);
			return -EIO;
		}

		if (prefix_len && strncmp(line, prefix, prefix_len) == 0 && out) {
			snprintf(out, out_len, "%s", line);
			got_wanted = true;
			continue;
		}
		IPA_LOGP(SSCARD, LDEBUG, "AT transport: unsolicited line ignored: %s\n", line);
	}
}

/* Wait for an unsolicited line starting with prefix; used after a reset. */
static int wait_for_urc(struct at_ctx *ctx, const char *prefix, unsigned int timeout_ms)
{
	char line[AT_LINE_MAX];
	unsigned long deadline = now_ms() + timeout_ms;
	size_t prefix_len = strlen(prefix);

	for (;;) {
		int rc = read_line(ctx, line, sizeof(line), deadline);

		if (rc < 0)
			return rc;
		IPA_LOGP(SSCARD, LDEBUG, "AT transport: < %s\n", line);
		if (strncmp(line, prefix, prefix_len) == 0)
			return 0;
	}
}

/* ------------------------------------------------------------------------
 * Hex
 * --------------------------------------------------------------------- */

static void hex_encode(char *out, const uint8_t *data, size_t len)
{
	static const char digits[] = "0123456789ABCDEF";
	size_t i;

	for (i = 0; i < len; i++) {
		out[2 * i] = digits[data[i] >> 4];
		out[2 * i + 1] = digits[data[i] & 0x0f];
	}
	out[2 * len] = '\0';
}

static int hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Decode hex into out; returns the number of bytes, or -EINVAL. */
static int hex_decode(uint8_t *out, size_t out_len, const char *hex, size_t hex_len)
{
	size_t i;

	if (hex_len % 2 || hex_len / 2 > out_len)
		return -EINVAL;
	for (i = 0; i < hex_len / 2; i++) {
		int hi = hex_value(hex[2 * i]);
		int lo = hex_value(hex[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return -EINVAL;
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return (int)(hex_len / 2);
}

/* ------------------------------------------------------------------------
 * scard backend
 * --------------------------------------------------------------------- */

void *ipa_scard_at_init(const char *uri, unsigned int reader_num)
{
	struct at_ctx *ctx;
	char *spec;
	char *opts;
	(void)reader_num;

	if (!uri || !*uri) {
		IPA_LOGP(SSCARD, LERROR, "AT transport: no device given (at:/dev/ttyUSB2)\n");
		return NULL;
	}

	ctx = IPA_ALLOC_ZERO(struct at_ctx);
	ctx->fd = -1;
	ctx->timeout_ms = AT_TIMEOUT_MS;
	ctx->reset_wait_ms = AT_RESET_WAIT_MS;
	ctx->reset_mode = AT_RESET_NONE;

	spec = strdup(uri);
	if (!spec) {
		IPA_FREE(ctx);
		return NULL;
	}
	opts = strchr(spec, '?');
	if (opts)
		*opts++ = '\0';

	ctx->dev = strdup(spec);
	if (!ctx->dev || (opts && parse_options(ctx, opts) < 0))
		goto error;

	if (open_port(ctx) < 0)
		goto error;

	/* The echo is off on most modems; turning it off again costs one command and makes the answers easier to
	 * read. A modem that does not understand ATE0 is not fatal. */
	if (!ctx->keep_echo)
		at_command(ctx, "ATE0", NULL, NULL, 0, ctx->timeout_ms);
	/* Numeric +CME ERROR codes, so a failure says why. */
	at_command(ctx, "AT+CMEE=1", NULL, NULL, 0, ctx->timeout_ms);

	if (at_command(ctx, "AT+CSIM=?", NULL, NULL, 0, ctx->timeout_ms) < 0) {
		IPA_LOGP(SSCARD, LERROR,
			 "AT transport: %s does not accept AT+CSIM, so the eUICC cannot be reached through it\n",
			 ctx->dev);
		goto error;
	}

	IPA_LOGP(SSCARD, LINFO, "AT transport on %s ready\n", ctx->dev);
	free(spec);
	return ctx;

error:
	free(spec);
	ipa_scard_at_free(ctx);
	return NULL;
}

int ipa_scard_at_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req)
{
	struct at_ctx *ctx = scard_ctx;
	char cmd[AT_LINE_MAX];
	char answer[AT_LINE_MAX];
	char hex[2 * 261 + 1];
	const char *quote;
	const char *end;
	int len;
	int rc;

	assert(ctx && res && req);

	if (req->len > sizeof(hex) / 2 - 1) {
		IPA_LOGP(SSCARD, LERROR, "AT transport: APDU of %zu bytes is too long for AT+CSIM\n", req->len);
		return -EINVAL;
	}

	hex_encode(hex, req->data, req->len);
	snprintf(cmd, sizeof(cmd), "AT+CSIM=%zu,\"%s\"", 2 * req->len, hex);

	IPA_LOGP(SSCARD, LDEBUG, "AT transport TX:\n");
	ipa_buf_hexdump_multiline(req, 64, 1, SSCARD, LDEBUG);

	answer[0] = '\0';
	rc = at_command(ctx, cmd, "+CSIM:", answer, sizeof(answer), ctx->timeout_ms);
	if (rc < 0) {
		if (rc == -ENOMSG)
			IPA_LOGP(SSCARD, LERROR, "AT transport: the modem answered without a +CSIM line\n");
		return -EIO;
	}

	/* +CSIM: <length>,"<response hex>" */
	quote = strchr(answer, '"');
	end = quote ? strchr(quote + 1, '"') : NULL;
	if (!end) {
		IPA_LOGP(SSCARD, LERROR, "AT transport: cannot read the response from \"%s\"\n", answer);
		return -EIO;
	}

	len = hex_decode(res->data, res->data_len, quote + 1, (size_t)(end - quote - 1));
	if (len < 0) {
		IPA_LOGP(SSCARD, LERROR, "AT transport: response is not %s: %s\n",
			 len == -EINVAL ? "valid hex of a size that fits" : "usable", answer);
		return -EIO;
	}
	res->len = (size_t)len;

	IPA_LOGP(SSCARD, LDEBUG, "AT transport RX:\n");
	ipa_buf_hexdump_multiline(res, 64, 1, SSCARD, LDEBUG);
	return 0;
}

int ipa_scard_at_reset(void *scard_ctx)
{
	struct at_ctx *ctx = scard_ctx;
	struct timespec settle = { .tv_sec = AT_CFUN_SETTLE_MS / 1000,
				   .tv_nsec = (AT_CFUN_SETTLE_MS % 1000) * 1000000L };

	assert(ctx);

	if (ctx->reset_mode == AT_RESET_NONE) {
		/* The eUICC wants a reset after a profile change. Which command does that without upsetting the
		 * rest of the system is the platform's business, so by default the IPAd reports it cannot and the
		 * platform (on OpenWrt, the profile-changed hook) deals with it. */
		IPA_LOGP(SSCARD, LERROR,
			 "AT transport: no reset method configured (add ?reset=cfun to the transport URI, or let "
			 "the platform power-cycle the modem)\n");
		return -ENOTSUP;
	}

	IPA_LOGP(SSCARD, LINFO, "AT transport: resetting the card with AT+CFUN=0 / AT+CFUN=1\n");
	if (at_command(ctx, "AT+CFUN=0", NULL, NULL, 0, ctx->timeout_ms) < 0)
		return -EIO;
	nanosleep(&settle, NULL);
	if (at_command(ctx, "AT+CFUN=1", NULL, NULL, 0, ctx->timeout_ms) < 0)
		return -EIO;

	/* The card is usable again when the modem reports the SIM ready; a modem that does not say so leaves us
	 * with the wait as the only guarantee. */
	if (wait_for_urc(ctx, "+CPIN: READY", ctx->reset_wait_ms) < 0)
		IPA_LOGP(SSCARD, LINFO, "AT transport: no \"+CPIN: READY\" after the reset, carrying on\n");

	return 0;
}

int ipa_scard_at_atr(void *scard_ctx, struct ipa_buf *atr)
{
	(void)scard_ctx;
	(void)atr;
	/* AT+CSIM gives no access to the ATR. Nothing in the IPAd needs it. */
	return -ENOTSUP;
}

int ipa_scard_at_free(void *scard_ctx)
{
	struct at_ctx *ctx = scard_ctx;

	if (!ctx)
		return 0;
	if (ctx->fd >= 0)
		close(ctx->fd); /* releases the flock */
	free(ctx->dev);
	IPA_FREE(ctx);
	return 0;
}
