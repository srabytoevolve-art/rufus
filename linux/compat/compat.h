/*
 * Rufus for Linux: shims that let the portable parts of the Windows Rufus
 * source tree (ms-sys, syslinux libinstaller/libfat) build on Linux.
 * This header is force-included (-include) when compiling those sources.
 */
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

/* Aligned allocations, as used by libfat and ms-sys */
static inline void* _rl_aligned_malloc(size_t size, size_t align)
{
	void* p = NULL;
	if (align < sizeof(void*))
		align = sizeof(void*);
	if (posix_memalign(&p, align, size) != 0)
		return NULL;
	return p;
}
#define _mm_malloc(size, align) _rl_aligned_malloc((size), (align))
#define _mm_free(p) free(p)

/* Logging, provided by core/log.c */
void uprintf(const char* format, ...) __attribute__((format(printf, 1, 2)));
