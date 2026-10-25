/*
 * Rufus for Linux: file checksums
 * Copyright © 2026 Rufus for Linux contributors
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
#include <sys/stat.h>

#include <openssl/evp.h>

#include "rufus.h"

#define HASH_BUFFER_SIZE (4 * MB)

static void to_hex(const uint8_t* d, unsigned int len, char* out)
{
	static const char hex[] = "0123456789abcdef";
	for (unsigned int i = 0; i < len; i++) {
		out[2 * i] = hex[d[i] >> 4];
		out[2 * i + 1] = hex[d[i] & 0xf];
	}
	out[2 * len] = 0;
}

bool rf_hash_file(const char* path, rf_hashes_t* out)
{
	const EVP_MD* md[4] = { EVP_md5(), EVP_sha1(), EVP_sha256(), EVP_sha512() };
	char* dst[4] = { out->md5, out->sha1, out->sha256, out->sha512 };
	EVP_MD_CTX* ctx[4] = { NULL };
	uint8_t* buf = NULL;
	uint64_t size, done = 0, last = 0;
	struct stat st;
	bool r = false;
	int fd;

	memset(out, 0, sizeof(*out));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open '%s': %s", path, strerror(errno));
		return false;
	}
	if (fstat(fd, &st) != 0)
		goto out;
	size = (uint64_t)st.st_size;
	buf = malloc(HASH_BUFFER_SIZE);
	if (buf == NULL)
		goto out;
	for (int i = 0; i < 4; i++) {
		ctx[i] = EVP_MD_CTX_new();
		if (ctx[i] == NULL || EVP_DigestInit_ex(ctx[i], md[i], NULL) != 1)
			goto out;
	}
	posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	for (;;) {
		ssize_t n = read(fd, buf, HASH_BUFFER_SIZE);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			uprintf("Read error: %s", strerror(errno));
			goto out;
		}
		if (n == 0)
			break;
		for (int i = 0; i < 4; i++)
			EVP_DigestUpdate(ctx[i], buf, (size_t)n);
		done += (uint64_t)n;
		if (rf_monotonic_ms() - last > 250) {
			last = rf_monotonic_ms();
			rf_progress(OP_HASH, size ? (double)done / (double)size : 0.0, "Computing checksums: %s / %s",
				rf_size_to_human(done, false), rf_size_to_human(size, false));
		}
		if (rf_cancelled())
			goto out;
	}
	for (int i = 0; i < 4; i++) {
		uint8_t digest[EVP_MAX_MD_SIZE];
		unsigned int len = 0;
		EVP_DigestFinal_ex(ctx[i], digest, &len);
		to_hex(digest, len, dst[i]);
	}
	r = true;
out:
	for (int i = 0; i < 4; i++)
		EVP_MD_CTX_free(ctx[i]);
	free(buf);
	close(fd);
	return r;
}
