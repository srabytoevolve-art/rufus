/*
 * Rufus for Linux: replacement for src/ms-sys/file.c
 *
 * As with Windows Rufus, the FILE* that is handed to ms-sys is really a
 * FAKE_FD, with _handle holding a file descriptor and _offset a base offset.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include "file.h"

extern unsigned long ulBytesPerSector;

static int fake_fd_to_fd(FILE* fp)
{
	return (int)(intptr_t)((FAKE_FD*)fp)->_handle;
}

int64_t write_sectors(void* hDrive, uint64_t SectorSize, uint64_t StartSector,
	uint64_t nSectors, const void* pBuf)
{
	int fd = (int)(intptr_t)hDrive;
	size_t size = (size_t)(nSectors * SectorSize);
	ssize_t r = pwrite(fd, pBuf, size, (off_t)(StartSector * SectorSize));

	if (r < 0 || (size_t)r != size) {
		uprintf("write_sectors: Write error at sector 0x%08" PRIx64 ": %s",
			StartSector, (r < 0) ? strerror(errno) : "short write");
		return -1;
	}
	return (int64_t)r;
}

int64_t read_sectors(void* hDrive, uint64_t SectorSize, uint64_t StartSector,
	uint64_t nSectors, void* pBuf)
{
	int fd = (int)(intptr_t)hDrive;
	size_t size = (size_t)(nSectors * SectorSize);
	ssize_t r = pread(fd, pBuf, size, (off_t)(StartSector * SectorSize));

	if (r < 0 || (size_t)r != size) {
		uprintf("read_sectors: Read error at sector 0x%08" PRIx64 ": %s",
			StartSector, (r < 0) ? strerror(errno) : "short read");
		return -1;
	}
	return (int64_t)r;
}

int contains_data(FILE* fp, uint64_t Position, const void* pData, uint64_t Len)
{
	unsigned char buf[MAX_DATA_LEN];

	if (Len > sizeof(buf) || !read_data(fp, Position, buf, Len))
		return 0;
	return memcmp(pData, buf, (size_t)Len) == 0;
}

/* Block devices on Linux don't require sector-aligned buffered access */
int read_data(FILE* fp, uint64_t Position, void* pData, uint64_t Len)
{
	int fd = fake_fd_to_fd(fp);
	ssize_t r;

	Position += ((FAKE_FD*)fp)->_offset;
	r = pread(fd, pData, (size_t)Len, (off_t)Position);
	if (r < 0 || (uint64_t)r != Len) {
		uprintf("read_data: Could not read %" PRIu64 " bytes at 0x%" PRIx64 ": %s",
			Len, Position, (r < 0) ? strerror(errno) : "short read");
		return 0;
	}
	return 1;
}

int write_data(FILE* fp, uint64_t Position, const void* pData, uint64_t Len)
{
	int fd = fake_fd_to_fd(fp);
	ssize_t r;

	Position += ((FAKE_FD*)fp)->_offset;
	r = pwrite(fd, pData, (size_t)Len, (off_t)Position);
	if (r < 0 || (uint64_t)r != Len) {
		uprintf("write_data: Could not write %" PRIu64 " bytes at 0x%" PRIx64 ": %s",
			Len, Position, (r < 0) ? strerror(errno) : "short write");
		return 0;
	}
	return 1;
}
