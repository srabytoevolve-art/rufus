/*
 * Rufus for Linux: destructive bad blocks / fake capacity check
 * Copyright © 2026 Rufus for Linux contributors
 *
 * Like the Windows version, each pass writes a pattern across the whole
 * drive and reads it back. Each 4 KB block is also stamped with its own
 * offset, so that drives that wrap around (fake capacity) are caught.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include "internal.h"

#define BB_BUFFER_SIZE  (4 * MB)
#define BB_BLOCK        4096
#define MAX_REPORTED    16

static const uint8_t patterns[4] = { 0xaa, 0x55, 0xff, 0x00 };

static void fill(uint8_t* buf, size_t len, uint8_t pattern, uint64_t offset)
{
	memset(buf, pattern, len);
	for (size_t i = 0; i + BB_BLOCK <= len; i += BB_BLOCK) {
		uint64_t stamp = (offset + i) ^ (0x0101010101010101ULL * pattern);
		memcpy(&buf[i], &stamp, sizeof(stamp));
	}
}

bool rf_badblocks_check(int fd, uint64_t disk_size, int passes, uint64_t* bad_blocks)
{
	uint8_t *wbuf = NULL, *rbuf = NULL, *expected = NULL;
	uint64_t size = disk_size / BB_BLOCK * BB_BLOCK;
	uint64_t bad = 0, now, last = 0;
	bool ok = false;
	int flags;

	*bad_blocks = 0;
	if (passes < 1)
		passes = 1;
	if (passes > 4)
		passes = 4;
	if (posix_memalign((void**)&wbuf, BB_BLOCK, BB_BUFFER_SIZE) != 0 ||
		posix_memalign((void**)&rbuf, BB_BLOCK, BB_BUFFER_SIZE) != 0 ||
		posix_memalign((void**)&expected, BB_BLOCK, BB_BUFFER_SIZE) != 0)
		goto out;
	flags = fcntl(fd, F_GETFL);
	fcntl(fd, F_SETFL, flags | O_DIRECT);

	uprintf("Bad blocks check: %d pass%s over %s", passes, passes > 1 ? "es" : "", rf_size_to_human(size, false));
	for (int pass = 0; pass < passes; pass++) {
		uint8_t pattern = patterns[pass];
		for (int phase = 0; phase < 2; phase++) {
			for (uint64_t off = 0; off < size; off += BB_BUFFER_SIZE) {
				size_t chunk = (size_t)MIN((uint64_t)BB_BUFFER_SIZE, size - off);
				ssize_t n;
				if (phase == 0) {
					fill(wbuf, chunk, pattern, off);
					n = pwrite(fd, wbuf, chunk, (off_t)off);
					if (n != (ssize_t)chunk) {
						uprintf("Write error at offset %" PRIu64 ": %s", off, (n < 0) ? strerror(errno) : "short write");
						bad += (chunk - (size_t)MAX(n, 0)) / BB_BLOCK;
					}
				} else {
					fill(expected, chunk, pattern, off);
					n = pread(fd, rbuf, chunk, (off_t)off);
					if (n != (ssize_t)chunk) {
						uprintf("Read error at offset %" PRIu64 ": %s", off, (n < 0) ? strerror(errno) : "short read");
						bad += chunk / BB_BLOCK;
					} else if (memcmp(rbuf, expected, chunk) != 0) {
						for (size_t i = 0; i < chunk; i += BB_BLOCK) {
							if (memcmp(&rbuf[i], &expected[i], BB_BLOCK) != 0) {
								if (bad < MAX_REPORTED)
									uprintf("  Bad block at offset %" PRIu64, off + i);
								bad++;
							}
						}
					}
				}
				now = rf_monotonic_ms();
				if (now - last > 250) {
					last = now;
					double frac = ((double)pass * 2 + phase + (double)off / (double)size) / (passes * 2.0);
					rf_progress(OP_BADBLOCKS, frac, "Bad blocks: pass %d/%d (%s 0x%02X) - %" PRIu64 " bad blocks",
						pass + 1, passes, phase ? "reading" : "writing", pattern, bad);
				}
				if (rf_cancelled())
					goto out;
			}
			if (phase == 0)
				fsync(fd);
		}
	}
	ok = true;
out:
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_DIRECT);
	free(wbuf);
	free(rbuf);
	free(expected);
	*bad_blocks = bad;
	if (ok)
		uprintf("Bad blocks check completed: %" PRIu64 " bad block%s found", bad, bad == 1 ? "" : "s");
	return ok;
}
