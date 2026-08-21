/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Author: Philipp Maier <pmaier@sysmocom.de> / sysmocom - s.f.m.c. GmbH
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#ifdef _WIN32
/* Windows has PC/SC natively: winscard.dll implements the same WinSCard API
 * that pcsc-lite clones on POSIX, so everything below compiles unchanged.
 * Only the headers differ -- <wintypes.h> is a pcsc-lite invention -- and the
 * error-to-string helper has to be supplied (see pcsc_stringify_error). */
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winscard.h>
#else
#include <wintypes.h>
#include <winscard.h>
#ifdef __APPLE__
#include <pcsclite.h>
#endif
#endif
#include <onomondo/ipa/utils.h>
#include <onomondo/ipa/scard.h>
#include <onomondo/ipa/log.h>
#include <onomondo/ipa/mem.h>

#ifdef _WIN32
/* pcsc-lite's reader-name bound; the Windows SDK does not declare one. */
#ifndef MAX_READERNAME
#define MAX_READERNAME 128
#endif

/* WinSCard status code -> text.  Every entry is #ifdef-guarded: these are
 * preprocessor defines in the SDK's scarderr.h, so guarding lets the table
 * build against any SDK version without having to know which codes it
 * happens to declare. */
#define PCSC_ERR(code, text) { code, text },
static const struct {
	LONG rv;
	const char *text;
} pcsc_error_table[] = {
#ifdef SCARD_S_SUCCESS
	PCSC_ERR(SCARD_S_SUCCESS, "Command successful.")
#endif
#ifdef SCARD_E_CANCELLED
	PCSC_ERR(SCARD_E_CANCELLED, "Command cancelled.")
#endif
#ifdef SCARD_E_INVALID_HANDLE
	PCSC_ERR(SCARD_E_INVALID_HANDLE, "Invalid handle.")
#endif
#ifdef SCARD_E_INVALID_PARAMETER
	PCSC_ERR(SCARD_E_INVALID_PARAMETER, "Invalid parameter given.")
#endif
#ifdef SCARD_E_NO_MEMORY
	PCSC_ERR(SCARD_E_NO_MEMORY, "Not enough memory available.")
#endif
#ifdef SCARD_E_INSUFFICIENT_BUFFER
	PCSC_ERR(SCARD_E_INSUFFICIENT_BUFFER, "Buffer is too small.")
#endif
#ifdef SCARD_E_UNKNOWN_READER
	PCSC_ERR(SCARD_E_UNKNOWN_READER, "The reader name is unknown.")
#endif
#ifdef SCARD_E_TIMEOUT
	PCSC_ERR(SCARD_E_TIMEOUT, "The operation timed out.")
#endif
#ifdef SCARD_E_SHARING_VIOLATION
	PCSC_ERR(SCARD_E_SHARING_VIOLATION, "Sharing violation.")
#endif
#ifdef SCARD_E_NO_SMARTCARD
	PCSC_ERR(SCARD_E_NO_SMARTCARD, "No smart card inserted.")
#endif
#ifdef SCARD_E_UNKNOWN_CARD
	PCSC_ERR(SCARD_E_UNKNOWN_CARD, "Unknown card.")
#endif
#ifdef SCARD_E_PROTO_MISMATCH
	PCSC_ERR(SCARD_E_PROTO_MISMATCH, "Requested protocol not available.")
#endif
#ifdef SCARD_E_NOT_READY
	PCSC_ERR(SCARD_E_NOT_READY, "The reader or card is not ready.")
#endif
#ifdef SCARD_E_SYSTEM_CANCELLED
	PCSC_ERR(SCARD_E_SYSTEM_CANCELLED, "The action was cancelled by the system.")
#endif
#ifdef SCARD_E_NOT_TRANSACTED
	PCSC_ERR(SCARD_E_NOT_TRANSACTED, "The transaction failed.")
#endif
#ifdef SCARD_E_READER_UNAVAILABLE
	PCSC_ERR(SCARD_E_READER_UNAVAILABLE, "The reader is unavailable.")
#endif
#ifdef SCARD_E_NO_SERVICE
	PCSC_ERR(SCARD_E_NO_SERVICE, "The smart card resource manager is not running.")
#endif
#ifdef SCARD_E_SERVICE_STOPPED
	PCSC_ERR(SCARD_E_SERVICE_STOPPED, "The smart card resource manager has shut down.")
#endif
#ifdef SCARD_E_NO_READERS_AVAILABLE
	PCSC_ERR(SCARD_E_NO_READERS_AVAILABLE, "No smart card reader available.")
#endif
#ifdef SCARD_W_UNSUPPORTED_CARD
	PCSC_ERR(SCARD_W_UNSUPPORTED_CARD, "The card is not supported.")
#endif
#ifdef SCARD_W_UNRESPONSIVE_CARD
	PCSC_ERR(SCARD_W_UNRESPONSIVE_CARD, "The card is unresponsive.")
#endif
#ifdef SCARD_W_UNPOWERED_CARD
	PCSC_ERR(SCARD_W_UNPOWERED_CARD, "The card is not powered.")
#endif
#ifdef SCARD_W_RESET_CARD
	PCSC_ERR(SCARD_W_RESET_CARD, "The card was reset.")
#endif
#ifdef SCARD_W_REMOVED_CARD
	PCSC_ERR(SCARD_W_REMOVED_CARD, "The card was removed.")
#endif
};
#undef PCSC_ERR

/*! Render a WinSCard status code as text, pcsc-lite's pcsc_stringify_error().
 *  The Windows SDK has no equivalent, and FormatMessage() does not know these
 *  codes either.  Anything not in the table falls back to the numeric form --
 *  which the caller logs in hex alongside this string anyway.
 *  \param[in] rv WinSCard status code.
 *  \returns pointer to a static, human readable description. */
static const char *pcsc_stringify_error(LONG rv)
{
	static char unknown[64];
	size_t i;

	for (i = 0; i < IPA_ARRAY_SIZE(pcsc_error_table); i++) {
		if (pcsc_error_table[i].rv == rv)
			return pcsc_error_table[i].text;
	}

	snprintf(unknown, sizeof(unknown), "Unknown error 0x%08lX", (unsigned long)rv);
	return unknown;
}
#endif /* _WIN32 */

#define PCSC_ERROR(reader_num, rv, text) \
if (rv != SCARD_S_SUCCESS) { \
	IPA_LOGP(SSCARD, LERROR, "PCSC reader #%d error: %s (%s,0x%lX)\n", \
		 reader_num, pcsc_stringify_error(rv), text, rv); \
	goto error; \
}

struct scard_ctx {
	bool initialized;
	unsigned int reader_num;
	SCARDCONTEXT hContext;
	DWORD dwActiveProtocol;
	SCARDHANDLE hCard;
	SCARD_IO_REQUEST pioRecvPci;
	const SCARD_IO_REQUEST *pioSendPci;
};

static const SCARD_IO_REQUEST *select_pio_send_pci(DWORD protocol)
{
	switch (protocol) {
	case SCARD_PROTOCOL_T1:
		return SCARD_PCI_T1;
	case SCARD_PROTOCOL_T0:
	default:
		return SCARD_PCI_T0;
	}
}

/*! Initialize smartcard reader (and card).
 *  \param[in] reader_num device number of the smartcard reader.
 *  \returns pointer to newly allocated smartcard reader context. */
void *ipa_scard_init(unsigned int reader_num)
{
	struct scard_ctx *ctx;
	long rc;
	LPSTR mszReaders = NULL;
	DWORD dwReaders;
	unsigned int num_readers;
	char *reader_name;

	ctx = IPA_ALLOC_ZERO(struct scard_ctx);
	ctx->reader_num = reader_num;

	/* Initialize reader */
	rc = SCardEstablishContext(SCARD_SCOPE_SYSTEM, NULL, NULL, &ctx->hContext);
	PCSC_ERROR(reader_num, rc, "SCardEstablishContext");

#ifdef SCARD_AUTOALLOCATE
	dwReaders = SCARD_AUTOALLOCATE;
	rc = SCardListReaders(ctx->hContext, NULL, (LPSTR) & mszReaders, &dwReaders);
#else
	/* macOS PC/SC does not support SCARD_AUTOALLOCATE */
	dwReaders = 0;
	rc = SCardListReaders(ctx->hContext, NULL, NULL, &dwReaders);
	PCSC_ERROR(reader_num, rc, "SCardListReaders(size)");
	mszReaders = malloc(dwReaders);
	rc = SCardListReaders(ctx->hContext, NULL, mszReaders, &dwReaders);
#endif
	PCSC_ERROR(reader_num, rc, "SCardListReaders");

	num_readers = 0;
	reader_name = mszReaders;
	while (*reader_name != '\0' && num_readers != reader_num) {
		reader_name += strlen(reader_name) + 1;
		num_readers++;
	}
	if (reader_num != num_readers) {
		IPA_LOGP(SSCARD, LINFO, "no smartcard reader.\n");
		goto error;
	}

	/* Initialize card */
	rc = SCardConnect(ctx->hContext, reader_name, SCARD_SHARE_SHARED,
			  SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1, &ctx->hCard, &ctx->dwActiveProtocol);
	PCSC_ERROR(reader_num, rc, "SCardConnect");
	ctx->pioSendPci = select_pio_send_pci(ctx->dwActiveProtocol);
#ifdef _WIN32
	/* SCardTransmit writes the response PCI into pioRecvPci.  pcsc-lite is
	 * happy with the all-zero struct IPA_ALLOC_ZERO leaves behind, but the
	 * Windows resource manager reads cbPciLength on the way in and rejects a
	 * zero-length header with SCARD_E_INVALID_PARAMETER.  Fill it in here and
	 * after every reconnect, since the reconnect can renegotiate the protocol.
	 * Doing this on POSIX too would be harmless, but keeping it scoped leaves
	 * the existing platforms with exactly the behaviour they were tested with. */
	ctx->pioRecvPci.dwProtocol = ctx->dwActiveProtocol;
	ctx->pioRecvPci.cbPciLength = sizeof(ctx->pioRecvPci);
#endif

	IPA_LOGP(SSCARD, LINFO, "PCSC reader #%d (%s) initialized.\n", reader_num, reader_name);
	ctx->initialized = true;

#ifdef SCARD_AUTOALLOCATE
	SCardFreeMemory(ctx->hContext, mszReaders);
#else
	free(mszReaders);
#endif
	return ctx;
error:
	IPA_LOGP(SSCARD, LERROR, "PCSC reader #%d initialization failed!\n", reader_num);
#ifdef SCARD_AUTOALLOCATE
	if (mszReaders)
		SCardFreeMemory(ctx->hContext, mszReaders);
#else
	free(mszReaders);
#endif
	IPA_FREE(ctx);
	return NULL;
}

/*! Transceive smartcard APDU.
 *  \param[inout] scard_ctx smartcard reader context.
 *  \param[out] res buffer to store smartcard response.
 *  \param[out] req buffer with smartcard request.
 *  \returns 0 on success, -EIO on failure. */
int ipa_scard_transceive(void *scard_ctx, struct ipa_buf *res, const struct ipa_buf *req)
{
	struct scard_ctx *ctx = scard_ctx;
	DWORD recv_len;
	LONG rc;
	assert(ctx);

	assert(res);
	assert(req);

	/* SCardTransmit reports the response length through a DWORD*.  Use a
	 * real DWORD rather than casting the address of the size_t res->len:
	 * the callee writes 4 bytes into an 8-byte object, which only happens
	 * to work on a little-endian machine with the high half zeroed. */
	recv_len = (DWORD) res->data_len;
	IPA_LOGP(SSCARD, LDEBUG, "PCSC reader #%d TX: \n", ctx->reader_num);
	ipa_buf_hexdump_multiline(req, 64, 1, SSCARD, LINFO);

	rc = SCardTransmit(ctx->hCard, ctx->pioSendPci, req->data, (DWORD) req->len, &ctx->pioRecvPci, res->data,
			   &recv_len);
	PCSC_ERROR(ctx->reader_num, rc, "SCardEndTransaction");
	res->len = recv_len;

	if (res->len) {
		IPA_LOGP(SSCARD, LDEBUG, "PCSC reader #%d RX: \n", ctx->reader_num);
		ipa_buf_hexdump_multiline(res, 64, 1, SSCARD, LINFO);
	} else {
		IPA_LOGP(SSCARD, LDEBUG, "PCSC reader #%d RX: (no data)\n", ctx->reader_num);
	}

	return 0;
error:
	IPA_LOGP(SSCARD, LERROR, "PCSC reader #%d transceive failed!\n", ctx->reader_num);
	return -EIO;
}

/*! Reset smartcard.
 *  \param[inout] scard_ctx smartcard reader context.
 *  \returns 0 on success, -EIO on failure. */
int ipa_scard_reset(void *scard_ctx)
{
	struct scard_ctx *ctx = scard_ctx;
	LONG rc;
	assert(ctx);

	rc = SCardReconnect(ctx->hCard, SCARD_SHARE_SHARED, SCARD_PROTOCOL_T0 | SCARD_PROTOCOL_T1,
			    SCARD_RESET_CARD, &ctx->dwActiveProtocol);
	PCSC_ERROR(ctx->reader_num, rc, "SCardReconnect");
	ctx->pioSendPci = select_pio_send_pci(ctx->dwActiveProtocol);
#ifdef _WIN32
	/* SCardTransmit writes the response PCI into pioRecvPci.  pcsc-lite is
	 * happy with the all-zero struct IPA_ALLOC_ZERO leaves behind, but the
	 * Windows resource manager reads cbPciLength on the way in and rejects a
	 * zero-length header with SCARD_E_INVALID_PARAMETER.  Fill it in here and
	 * after every reconnect, since the reconnect can renegotiate the protocol.
	 * Doing this on POSIX too would be harmless, but keeping it scoped leaves
	 * the existing platforms with exactly the behaviour they were tested with. */
	ctx->pioRecvPci.dwProtocol = ctx->dwActiveProtocol;
	ctx->pioRecvPci.cbPciLength = sizeof(ctx->pioRecvPci);
#endif
	IPA_LOGP(SSCARD, LINFO, "PCSC reader #%d card reset\n", ctx->reader_num);
	return 0;
error:
	IPA_LOGP(SSCARD, LERROR, "PCSC reader #%d reset failed!\n", ctx->reader_num);
	return -EIO;
}

/*! Read smartcard ATR.
 *  \param[inout] scard_ctx smartcard reader context.
 *  \param[out] res buffer to store the ATR.
 *  \returns 0 on success, -EIO on failure. */
int ipa_scard_atr(void *scard_ctx, struct ipa_buf *atr)
{
	/* Needed by SCardStatus, but we are not interested in those values here */
	char pbReader[MAX_READERNAME];
	DWORD dwReaderLen = sizeof(pbReader);
	DWORD dwState;
	DWORD dwProt;
	DWORD atr_len;

	struct scard_ctx *ctx = scard_ctx;
	LONG rc;
	assert(ctx);

	atr_len = (DWORD) atr->data_len;
	rc = SCardStatus(ctx->hCard, pbReader, &dwReaderLen, &dwState, &dwProt, atr->data, &atr_len);
	PCSC_ERROR(ctx->reader_num, rc, "SCardStatus");
	atr->len = atr_len;
	IPA_LOGP(SSCARD, LINFO, "PCSC reader #%d ATR:%s\n", ctx->reader_num, ipa_buf_hexdump(atr));
	return 0;
error:
	IPA_LOGP(SSCARD, LERROR, "PCSC reader #%d atr query failed!\n", ctx->reader_num);
	return -EIO;
}

/*! Free smartcard reader (and card).
 *  \param[inout] scard_ctx smartcard reader context.
 *  \returns 0 on success, -EIO on failure. */
int ipa_scard_free(void *scard_ctx)
{
	struct scard_ctx *ctx = scard_ctx;
	LONG rc;

	if (!scard_ctx)
		return 0;

	rc = SCardDisconnect(ctx->hCard, SCARD_UNPOWER_CARD);
	PCSC_ERROR(ctx->reader_num, rc, "SCardDisconnect");

	rc = SCardReleaseContext(ctx->hContext);
	PCSC_ERROR(ctx->reader_num, rc, "SCardReleaseContext");

	IPA_LOGP(SSCARD, LINFO, "PCSC reader #%d freed\n", ctx->reader_num);

	IPA_FREE(ctx);
	return 0;
error:
	IPA_LOGP(SSCARD, LERROR, "PCSC reader #%d free failed!\n", ctx->reader_num);
	return -EIO;
}
