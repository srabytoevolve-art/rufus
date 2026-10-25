/*
 * Rufus for Linux: raw (DD) image writing, verification, zeroing and saving
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#include "internal.h"
#include "reader.h"

#define DD_BUFFER_SIZE  (4 * MB)
#define DIRECT_ALIGN    4096

typedef struct {
	rf_op_t op;
	uint64_t total;
	uint64_t last_ms;
	uint64_t start_ms;
	uint64_t last_bytes;
	double speed;
} progress_t;

static void progress_init(progress_t* p, rf_op_t op, uint64_t total)
{
	memset(p, 0, sizeof(*p));
	p->op = op;
	p->total = total;
	p->start_ms = rf_monotonic_ms();
	p->last_ms = p->start_ms;
}

static void progress_update(progress_t* p, uint64_t done, uint64_t fraction_num, uint64_t fraction_den)
{
	uint64_t now = rf_monotonic_ms();
	char eta[32] = "";

	if (now - p->last_ms < 250 && fraction_num < fraction_den)
		return;
	if (now > p->last_ms) {
		double inst = (double)(done - p->last_bytes) * 1000.0 / (double)(now - p->last_ms);
		p->speed = (p->speed == 0.0) ? inst : (0.8 * p->speed + 0.2 * inst);
	}
	p->last_ms = now;
	p->last_bytes = done;
	if (p->speed > 0.0 && fraction_den > fraction_num && fraction_num > 0) {
		double elapsed = (double)(now - p->start_ms) / 1000.0;
		double remaining = elapsed * (double)(fraction_den - fraction_num) / (double)fraction_num;
		snprintf(eta, sizeof(eta), ", %d:%02d remaining", (int)remaining / 60, (int)remaining % 60);
	}
	rf_progress(p->op, fraction_den ? (double)fraction_num / (double)fraction_den : 0.0,
		"%s: %s / %s (%s/s%s)", rf_op_name(p->op), rf_size_to_human(done, false),
		p->total ? rf_size_to_human(p->total, false) : "?", rf_size_to_human((uint64_t)p->speed, false), eta);
}

static bool set_direct(int fd, bool enable)
{
	int flags = fcntl(fd, F_GETFL);
	if (flags < 0)
		return false;
	flags = enable ? (flags | O_DIRECT) : (flags & ~O_DIRECT);
	return fcntl(fd, F_SETFL, flags) == 0;
}

static bool write_full(int fd, const uint8_t* buf, size_t len, uint64_t offset)
{
	while (len > 0) {
		ssize_t n = pwrite(fd, buf, len, (off_t)offset);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			uprintf("Write error at offset %" PRIu64 ": %s", offset, strerror(errno));
			if (errno == EIO)
				uprintf("This usually indicates a faulty drive or a bad USB connection");
			return false;
		}
		if (n == 0) {
			uprintf("Write error at offset %" PRIu64 ": no space left on device", offset);
			return false;
		}
		buf += n;
		len -= (size_t)n;
		offset += (uint64_t)n;
	}
	return true;
}

static bool read_full(int fd, uint8_t* buf, size_t len, uint64_t offset)
{
	while (len > 0) {
		ssize_t n = pread(fd, buf, len, (off_t)offset);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			uprintf("Read error at offset %" PRIu64 ": %s", offset, strerror(errno));
			return false;
		}
		if (n == 0)
			return false;
		buf += n;
		len -= (size_t)n;
		offset += (uint64_t)n;
	}
	return true;
}

/* Fills the buffer as much as possible from the reader (compressed streams may return short reads) */
static ssize_t reader_fill(rf_reader_t* r, uint8_t* buf, size_t len)
{
	size_t got = 0;
	while (got < len) {
		ssize_t n = rf_reader_read(r, &buf[got], len - got);
		if (n < 0)
			return -1;
		if (n == 0)
			break;
		got += (size_t)n;
	}
	return (ssize_t)got;
}

bool rf_write_image(int fd, const char* image, const rf_image_t* img, uint64_t disk_size, bool verify)
{
	uint8_t *buf = NULL, *vbuf = NULL;
	rf_reader_t* r = NULL;
	progress_t p;
	uint64_t written = 0, total, compressed_size = img->size;
	uint64_t limit = img->is_vhd ? img->size - 512 : 0;
	bool direct, ok = false;

	total = (img->compression == COMP_NONE) ? (limit ? limit : img->size) : img->uncompressed_size;
	if (total != 0 && total > disk_size) {
		uprintf("The image (%s) is larger than the target drive (%s)", rf_size_to_human(total, false),
			rf_size_to_human(disk_size, false));
		return false;
	}

	if (posix_memalign((void**)&buf, DIRECT_ALIGN, DD_BUFFER_SIZE) != 0)
		return false;
	r = rf_reader_open(image, img->compression, limit);
	if (r == NULL)
		goto out;

	direct = set_direct(fd, true);
	uprintf("Writing image '%s' (%s I/O)...", image, direct ? "direct" : "buffered");
	progress_init(&p, OP_WRITE_IMAGE, total);
	for (;;) {
		ssize_t n = reader_fill(r, buf, DD_BUFFER_SIZE);
		size_t to_write;
		if (n < 0)
			goto out;
		if (n == 0)
			break;
		to_write = (size_t)n;
		/* Pad the last block to a full sector (as Windows Rufus does) */
		if (to_write % 512 != 0 || (direct && to_write % DIRECT_ALIGN != 0)) {
			size_t pad = direct ? DIRECT_ALIGN : 512;
			size_t padded = (to_write + pad - 1) / pad * pad;
			memset(&buf[to_write], 0, padded - to_write);
			if (written + padded > disk_size)
				padded = (to_write + 511) / 512 * 512;
			if (padded % DIRECT_ALIGN != 0 && direct) {
				set_direct(fd, false);
				direct = false;
			}
			to_write = padded;
		}
		if (written + to_write > disk_size) {
			uprintf("The image is larger than the target drive");
			goto out;
		}
		if (!write_full(fd, buf, to_write, written))
			goto out;
		written += (uint64_t)n;
		if (total != 0)
			progress_update(&p, written, written, total);
		else
			progress_update(&p, written, rf_reader_input_pos(r), compressed_size);
		if (rf_cancelled())
			goto out;
	}
	rf_progress(OP_FINALIZE, -1.0, "Flushing data to the drive...");
	if (fsync(fd) != 0) {
		uprintf("Could not flush data: %s", strerror(errno));
		goto out;
	}
	uprintf("Wrote %s", rf_size_to_human(written, false));
	rf_reader_close(r);
	r = NULL;

	if (verify) {
		uint64_t verified = 0;
		if (posix_memalign((void**)&vbuf, DIRECT_ALIGN, DD_BUFFER_SIZE) != 0)
			goto out;
		uprintf("Verifying...");
		/* Make sure we read the media, not the page cache */
		ioctl(fd, BLKFLSBUF, 0);
		posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
		r = rf_reader_open(image, img->compression, limit);
		if (r == NULL)
			goto out;
		progress_init(&p, OP_VERIFY, written);
		while (verified < written) {
			ssize_t n = reader_fill(r, buf, DD_BUFFER_SIZE);
			if (n <= 0) {
				uprintf("Could not re-read the image");
				goto out;
			}
			if (!read_full(fd, vbuf, (size_t)n, verified))
				goto out;
			if (memcmp(buf, vbuf, (size_t)n) != 0) {
				for (ssize_t i = 0; i < n; i++) {
					if (buf[i] != vbuf[i]) {
						uprintf("Verification FAILED: mismatch at offset %" PRIu64, verified + (uint64_t)i);
						break;
					}
				}
				uprintf("The drive may be faulty or counterfeit (fake capacity)");
				goto out;
			}
			verified += (uint64_t)n;
			progress_update(&p, verified, verified, written);
			if (rf_cancelled())
				goto out;
		}
		uprintf("Verification successful ✓");
	}
	ok = true;

out:
	set_direct(fd, false);
	rf_reader_close(r);
	free(buf);
	free(vbuf);
	return ok;
}

bool rf_zero_range(int fd, uint64_t offset, uint64_t size, rf_op_t op)
{
	uint8_t* buf = NULL;
	progress_t p;
	uint64_t done = 0;
	bool direct, ok = false;

	if (posix_memalign((void**)&buf, DIRECT_ALIGN, DD_BUFFER_SIZE) != 0)
		return false;
	memset(buf, 0, DD_BUFFER_SIZE);
	direct = (offset % DIRECT_ALIGN == 0) && set_direct(fd, true);
	progress_init(&p, op, size);
	while (done < size) {
		size_t chunk = (size_t)MIN((uint64_t)DD_BUFFER_SIZE, size - done);
		if (direct && chunk % DIRECT_ALIGN != 0) {
			set_direct(fd, false);
			direct = false;
		}
		if (!write_full(fd, buf, chunk, offset + done))
			goto out;
		done += chunk;
		progress_update(&p, done, done, size);
		if (rf_cancelled())
			goto out;
	}
	ok = (fsync(fd) == 0);
out:
	set_direct(fd, false);
	free(buf);
	return ok;
}

bool rf_save_drive_image(const char* device, const char* dest)
{
	uint8_t* buf = NULL;
	uint64_t size = 0, done = 0;
	progress_t p;
	int in = -1, out = -1;
	bool ok = false;

	in = open(device, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		uprintf("Could not open '%s': %s", device, strerror(errno));
		return false;
	}
	if (ioctl(in, BLKGETSIZE64, &size) != 0 || size == 0) {
		uprintf("Could not get the size of '%s'", device);
		goto out;
	}
	out = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		uprintf("Could not create '%s': %s", dest, strerror(errno));
		goto out;
	}
	if (posix_memalign((void**)&buf, DIRECT_ALIGN, DD_BUFFER_SIZE) != 0)
		goto out;
	uprintf("Saving %s (%s) to '%s'...", device, rf_size_to_human(size, false), dest);
	posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
	progress_init(&p, OP_SAVE_IMAGE, size);
	while (done < size) {
		size_t chunk = (size_t)MIN((uint64_t)DD_BUFFER_SIZE, size - done);
		if (!read_full(in, buf, chunk, done))
			goto out;
		for (size_t w = 0; w < chunk;) {
			ssize_t n = write(out, &buf[w], chunk - w);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				uprintf("Write error on '%s': %s", dest, strerror(errno));
				goto out;
			}
			w += (size_t)n;
		}
		done += chunk;
		progress_update(&p, done, done, size);
		if (rf_cancelled())
			goto out;
	}
	if (fsync(out) != 0)
		goto out;
	uprintf("Saved %s", rf_size_to_human(done, false));
	ok = true;
out:
	if (in >= 0)
		close(in);
	if (out >= 0)
		close(out);
	if (!ok && out >= 0)
		unlink(dest);
	free(buf);
	return ok;
}
