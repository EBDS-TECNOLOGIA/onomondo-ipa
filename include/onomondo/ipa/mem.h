/*
 * Copyright (c) 2025 Onomondo ApS & sysmocom - s.f.m.c. GmbH. All rights reserved.
 *
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once

#include <stdlib.h>
#ifdef __APPLE__
#include <malloc/malloc.h>
#define malloc_usable_size(p) malloc_size(p)
#elif defined(_MSC_VER)
#include <malloc.h>
#include <stdio.h>
#include <assert.h>
/* The MS CRT spells it _msize().  Unlike glibc's malloc_usable_size() and
 * macOS's malloc_size(), it does not tolerate NULL -- it trips the CRT
 * invalid-parameter handler -- and IPA_FREE() is routinely called on NULL.
 * Wrap it so the accounting below behaves like it does elsewhere. */
static inline size_t malloc_usable_size(void *p)
{
	return p ? _msize(p) : 0;
}
#else
#include <malloc.h>
#endif

#define IPA_ALLOC(obj) IPA_ALLOC_N(sizeof(obj))

extern long int ___mem_counter;
extern long int ___mem_peak;

#if defined(MEM_EMIT_DEBUG) && defined(_MSC_VER)
/* ------------------------------------------------------------------------ *
 * MEM_EMIT_DEBUG, MSVC variant.
 *
 * The heap accounting below is the same as the GNU version further down; only
 * the shape differs, because MSVC has no statement expressions to return a
 * value from a macro body.  Static inline helpers do that instead, and the
 * macros stay thin wrappers so every call site is unaffected.
 * ------------------------------------------------------------------------ */

static inline void *ipa_mem_dbg_account(void *ptr, const char *what, size_t n)
{
	___mem_counter += malloc_usable_size(ptr);
	if (___mem_counter > ___mem_peak)
		___mem_peak = ___mem_counter;
	printf("====> %p=%s(%zu): %li bytes total, %li bytes peak\n",
	       ptr, what, n, ___mem_counter, ___mem_peak);
	assert(___mem_counter >= 0);
	return ptr;
}

static inline void *ipa_mem_dbg_realloc(void *obj, size_t n)
{
	___mem_counter -= malloc_usable_size(obj);
	return ipa_mem_dbg_account(realloc(obj, n), "realloc", n);
}

static inline void ipa_mem_dbg_free(void *obj)
{
	size_t freed = malloc_usable_size(obj);
	___mem_counter -= freed;
	printf("====> free(%p): %li bytes total, %li bytes peak, %zu bytes freed\n",
	       obj, ___mem_counter, ___mem_peak, freed);
	assert(___mem_counter >= 0);
	free(obj);
}

#define IPA_ALLOC_N(n)        ipa_mem_dbg_account(malloc(n), "malloc", (size_t)(n))
#define IPA_CALLOC(nmemb, n)  ipa_mem_dbg_account(calloc(nmemb, n), "calloc", \
						  (size_t)(nmemb) * (size_t)(n))
#define IPA_REALLOC(obj, n)   ipa_mem_dbg_realloc(obj, (size_t)(n))
#define IPA_FREE(obj)         ipa_mem_dbg_free(obj)

#else /* !(MEM_EMIT_DEBUG && _MSC_VER) */

#ifdef MEM_EMIT_DEBUG
#define IPA_ALLOC_N(n) ({ \
	void *___ptr;	  \
	___ptr = malloc(n); \
	___mem_counter += malloc_usable_size(___ptr); \
	if (___mem_counter > ___mem_peak) ___mem_peak = ___mem_counter;	\
	printf("====> %p=malloc(%zu): %li bytes total, %li bytes peak\n", \
	       ___ptr, (size_t)n, ___mem_counter, ___mem_peak);	\
	assert(___mem_counter >= 0); \
	___ptr; \
})
#else
#define IPA_ALLOC_N(n) malloc(n)
#endif

#ifdef MEM_EMIT_DEBUG
#define IPA_CALLOC(nmemb, n) ({ \
	void *___ptr;	  \
	___ptr = calloc(nmemb, n);		      \
	___mem_counter += malloc_usable_size(___ptr); \
	if (___mem_counter > ___mem_peak) ___mem_peak = ___mem_counter;	\
	printf("====> %p=calloc(%zu, %ld): %li bytes total, %li bytes peak\n", \
	       ___ptr, (size_t)nmemb, (long unsigned int)n, ___mem_counter, ___mem_peak); \
	assert(___mem_counter >= 0); \
	___ptr; \
})
#else
#define IPA_CALLOC(nmemb, n) calloc(nmemb, n)
#endif

#ifdef MEM_EMIT_DEBUG
#define IPA_REALLOC(obj, n) ({			\
	void *___ptr;	  \
	___mem_counter -= malloc_usable_size(obj); \
	___ptr = realloc(obj, n); \
	___mem_counter += malloc_usable_size(___ptr); \
	if (___mem_counter > ___mem_peak) ___mem_peak = ___mem_counter;	\
	printf("====> %p=realloc(%p, %ld): %li bytes total, %li bytes peak\n", \
	       ___ptr, obj, (long unsigned int)n, ___mem_counter, ___mem_peak); \
	assert(___mem_counter >= 0); \
	___ptr; \
})
#else
#define IPA_REALLOC(obj, n) realloc(obj, n)
#endif

#ifdef MEM_EMIT_DEBUG
#define IPA_FREE(obj) ({ \
	___mem_counter -= malloc_usable_size(obj); \
	printf("====> free(%p): %li bytes total, %li bytes peak, %zu bytes freed\n", \
	       obj, ___mem_counter, ___mem_peak, malloc_usable_size(obj)); \
	assert(___mem_counter >= 0); \
	free(obj); \
})
#else
#define IPA_FREE(obj) free(obj)
#endif

#endif /* MEM_EMIT_DEBUG && _MSC_VER */
