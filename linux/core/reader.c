/*
 * Rufus for Linux: streaming (optionally decompressing) image reader
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

#include <zlib.h>
#include <lzma.h>
#include <zstd.h>
#include <bzlib.h>

#include "reader.h"

#define IN_BUF_SIZE (1 * 1024 * 1024)

struct rf_reader {
	int fd;
	rf_compression_t comp;
	uint64_t in_pos;        /* compressed bytes consumed from the file */
	uint64_t limit;         /* max bytes to return when uncompressed (VHD footer), 0 = none */
	uint64_t out_pos;
	uint8_t* in_buf;
	size_t in_len, in_off;
	bool eof_in, eof_out, error;
	z_stream zs;
	lzma_stream xs;
	ZSTD_DStream* zstd;
	bz_stream bs;
};

static const struct {
	const char* ext;
	rf_compression_t comp;
} file_assoc[] = {
	{ ".gz", COMP_GZIP },
	{ ".tgz", COMP_GZIP },
	{ ".xz", COMP_XZ },
	{ ".txz", COMP_XZ },
	{ ".lzma", COMP_LZMA },
	{ ".zst", COMP_ZSTD },
	{ ".zstd", COMP_ZSTD },
	{ ".bz2", COMP_BZIP2 },
	{ ".tbz2", COMP_BZIP2 },
};

rf_compression_t rf_compression_from_path(const char* path)
{
	for (size_t i = 0; i < ARRAYSIZE(file_assoc); i++)
		if (rf_str_isuffix(path, file_assoc[i].ext))
			return file_assoc[i].comp;
	return COMP_NONE;
}

const char* rf_compression_name(rf_compression_t comp)
{
	switch (comp) {
	case COMP_GZIP: return "gzip";
	case COMP_XZ: return "XZ";
	case COMP_LZMA: return "LZMA";
	case COMP_ZSTD: return "Zstandard";
	case COMP_BZIP2: return "bzip2";
	default: return "none";
	}
}

rf_reader_t* rf_reader_open(const char* path, rf_compression_t comp, uint64_t limit)
{
	rf_reader_t* r = calloc(1, sizeof(rf_reader_t));

	if (r == NULL)
		return NULL;
	r->comp = comp;
	r->limit = limit;
	r->fd = open(path, O_RDONLY | O_CLOEXEC);
	if (r->fd < 0) {
		uprintf("Could not open '%s': %s", path, strerror(errno));
		free(r);
		return NULL;
	}
	posix_fadvise(r->fd, 0, 0, POSIX_FADV_SEQUENTIAL);
	if (comp == COMP_NONE)
		return r;

	r->in_buf = malloc(IN_BUF_SIZE);
	if (r->in_buf == NULL)
		goto fail;
	switch (comp) {
	case COMP_GZIP:
		/* 15 + 32: auto-detect zlib or gzip headers */
		if (inflateInit2(&r->zs, 15 + 32) != Z_OK)
			goto fail;
		break;
	case COMP_XZ:
		if (lzma_stream_decoder(&r->xs, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
			goto fail;
		break;
	case COMP_LZMA:
		if (lzma_alone_decoder(&r->xs, UINT64_MAX) != LZMA_OK)
			goto fail;
		break;
	case COMP_ZSTD:
		r->zstd = ZSTD_createDStream();
		if (r->zstd == NULL)
			goto fail;
		ZSTD_initDStream(r->zstd);
		break;
	case COMP_BZIP2:
		if (BZ2_bzDecompressInit(&r->bs, 0, 0) != BZ_OK)
			goto fail;
		break;
	default:
		goto fail;
	}
	return r;

fail:
	uprintf("Could not initialize %s decompression", rf_compression_name(comp));
	close(r->fd);
	free(r->in_buf);
	free(r);
	return NULL;
}

static bool fill_input(rf_reader_t* r)
{
	ssize_t n;

	if (r->in_off < r->in_len || r->eof_in)
		return true;
	do {
		n = read(r->fd, r->in_buf, IN_BUF_SIZE);
	} while (n < 0 && errno == EINTR);
	if (n < 0) {
		uprintf("Read error: %s", strerror(errno));
		r->error = true;
		return false;
	}
	r->in_len = (size_t)n;
	r->in_off = 0;
	r->in_pos += (uint64_t)n;
	if (n == 0)
		r->eof_in = true;
	return true;
}

ssize_t rf_reader_read(rf_reader_t* r, void* buf, size_t len)
{
	size_t produced = 0;

	if (r->error)
		return -1;
	if (r->comp == COMP_NONE) {
		ssize_t n;
		if (r->limit != 0) {
			if (r->out_pos >= r->limit)
				return 0;
			if (len > r->limit - r->out_pos)
				len = (size_t)(r->limit - r->out_pos);
		}
		do {
			n = read(r->fd, buf, len);
		} while (n < 0 && errno == EINTR);
		if (n > 0) {
			r->out_pos += (uint64_t)n;
			r->in_pos += (uint64_t)n;
		}
		return n;
	}

	while (produced < len && !r->eof_out) {
		size_t avail_in;
		if (!fill_input(r))
			return -1;
		avail_in = r->in_len - r->in_off;
		switch (r->comp) {
		case COMP_GZIP: {
			int ret;
			r->zs.next_in = &r->in_buf[r->in_off];
			r->zs.avail_in = (uInt)avail_in;
			r->zs.next_out = (Bytef*)buf + produced;
			r->zs.avail_out = (uInt)(len - produced);
			ret = inflate(&r->zs, Z_NO_FLUSH);
			r->in_off = r->in_len - r->zs.avail_in;
			produced = len - r->zs.avail_out;
			if (ret == Z_STREAM_END) {
				/* Handle concatenated gzip members */
				if (r->in_off < r->in_len || !r->eof_in) {
					inflateReset(&r->zs);
					if (r->in_off >= r->in_len && !fill_input(r))
						return -1;
					if (r->eof_in && r->in_off >= r->in_len)
						r->eof_out = true;
				} else {
					r->eof_out = true;
				}
			} else if (ret != Z_OK && ret != Z_BUF_ERROR) {
				uprintf("gzip decompression error: %s", (r->zs.msg != NULL) ? r->zs.msg : "unknown");
				r->error = true;
				return -1;
			} else if (ret == Z_BUF_ERROR && r->eof_in) {
				uprintf("gzip stream is truncated");
				r->error = true;
				return -1;
			}
			break;
		}
		case COMP_XZ:
		case COMP_LZMA: {
			lzma_ret ret;
			r->xs.next_in = &r->in_buf[r->in_off];
			r->xs.avail_in = avail_in;
			r->xs.next_out = (uint8_t*)buf + produced;
			r->xs.avail_out = len - produced;
			ret = lzma_code(&r->xs, r->eof_in ? LZMA_FINISH : LZMA_RUN);
			r->in_off = r->in_len - r->xs.avail_in;
			produced = len - r->xs.avail_out;
			if (ret == LZMA_STREAM_END) {
				r->eof_out = true;
			} else if (ret != LZMA_OK && ret != LZMA_BUF_ERROR) {
				uprintf("XZ/LZMA decompression error %d", (int)ret);
				r->error = true;
				return -1;
			} else if (ret == LZMA_BUF_ERROR && r->eof_in) {
				uprintf("XZ/LZMA stream is truncated");
				r->error = true;
				return -1;
			}
			break;
		}
		case COMP_ZSTD: {
			ZSTD_inBuffer in = { &r->in_buf[r->in_off], avail_in, 0 };
			ZSTD_outBuffer out = { buf, len, produced };
			size_t ret = ZSTD_decompressStream(r->zstd, &out, &in);
			if (ZSTD_isError(ret)) {
				uprintf("Zstandard decompression error: %s", ZSTD_getErrorName(ret));
				r->error = true;
				return -1;
			}
			r->in_off += in.pos;
			produced = out.pos;
			if (r->eof_in && r->in_off >= r->in_len && out.pos < out.size)
				r->eof_out = true;
			break;
		}
		case COMP_BZIP2: {
			int ret;
			r->bs.next_in = (char*)&r->in_buf[r->in_off];
			r->bs.avail_in = (unsigned int)avail_in;
			r->bs.next_out = (char*)buf + produced;
			r->bs.avail_out = (unsigned int)(len - produced);
			ret = BZ2_bzDecompress(&r->bs);
			r->in_off = r->in_len - r->bs.avail_in;
			produced = len - r->bs.avail_out;
			if (ret == BZ_STREAM_END) {
				/* Concatenated bzip2 streams (pbzip2) */
				if (r->in_off < r->in_len || !r->eof_in) {
					BZ2_bzDecompressEnd(&r->bs);
					memset(&r->bs, 0, sizeof(r->bs));
					BZ2_bzDecompressInit(&r->bs, 0, 0);
					if (r->in_off >= r->in_len && !fill_input(r))
						return -1;
					if (r->eof_in && r->in_off >= r->in_len)
						r->eof_out = true;
				} else {
					r->eof_out = true;
				}
			} else if (ret != BZ_OK) {
				uprintf("bzip2 decompression error %d", ret);
				r->error = true;
				return -1;
			} else if (r->eof_in && r->in_off >= r->in_len && produced < len) {
				uprintf("bzip2 stream is truncated");
				r->error = true;
				return -1;
			}
			break;
		}
		default:
			return -1;
		}
	}
	r->out_pos += produced;
	return (ssize_t)produced;
}

uint64_t rf_reader_input_pos(const rf_reader_t* r)
{
	return r->in_pos;
}

void rf_reader_close(rf_reader_t* r)
{
	if (r == NULL)
		return;
	switch (r->comp) {
	case COMP_GZIP: inflateEnd(&r->zs); break;
	case COMP_XZ:
	case COMP_LZMA: lzma_end(&r->xs); break;
	case COMP_ZSTD: ZSTD_freeDStream(r->zstd); break;
	case COMP_BZIP2: BZ2_bzDecompressEnd(&r->bs); break;
	default: break;
	}
	close(r->fd);
	free(r->in_buf);
	free(r);
}
