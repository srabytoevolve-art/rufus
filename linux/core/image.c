/*
 * Rufus for Linux: image analysis
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
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

#include <lzma.h>
#include <zstd.h>

#include "internal.h"
#include "reader.h"

static const char* efi_arch_str[] = { "ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64" };

/* xz: read the uncompressed size from the stream index */
static uint64_t xz_uncompressed_size(const char* path)
{
	lzma_stream strm = LZMA_STREAM_INIT;
	lzma_index* idx = NULL;
	uint8_t* buf = NULL;
	uint64_t r = 0;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	struct stat st;

	if (fd < 0 || fstat(fd, &st) != 0)
		goto out;
	if (lzma_file_info_decoder(&strm, &idx, UINT64_MAX, (uint64_t)st.st_size) != LZMA_OK)
		goto out;
	buf = malloc(BUFSIZ);
	if (buf == NULL)
		goto out;
	strm.avail_in = 0;
	for (;;) {
		lzma_ret ret;
		if (strm.avail_in == 0) {
			ssize_t n = read(fd, buf, BUFSIZ);
			if (n < 0)
				goto out;
			strm.next_in = buf;
			strm.avail_in = (size_t)n;
		}
		ret = lzma_code(&strm, LZMA_RUN);
		if (ret == LZMA_SEEK_NEEDED) {
			if (lseek(fd, (off_t)strm.seek_pos, SEEK_SET) < 0)
				goto out;
			strm.avail_in = 0;
			continue;
		}
		if (ret == LZMA_STREAM_END) {
			r = lzma_index_uncompressed_size(idx);
			break;
		}
		if (ret != LZMA_OK)
			break;
	}
out:
	if (idx != NULL)
		lzma_index_end(idx, NULL);
	lzma_end(&strm);
	free(buf);
	if (fd >= 0)
		close(fd);
	return r;
}

static uint64_t zstd_uncompressed_size(const char* path)
{
	uint8_t hdr[18];    /* ZSTD_FRAMEHEADERSIZE_MAX */
	unsigned long long size;
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return 0;
	n = read(fd, hdr, sizeof(hdr));
	close(fd);
	if (n <= 0)
		return 0;
	size = ZSTD_getFrameContentSize(hdr, (size_t)n);
	if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR)
		return 0;
	return size;
}

bool rf_image_analyze(const char* path, rf_image_t* img)
{
	uint8_t sector[512], cd001[8];
	struct stat st;
	int fd;
	ssize_t n;

	memset(img, 0, sizeof(*img));
	snprintf(img->path, sizeof(img->path), "%s", path);

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open image '%s': %s", path, strerror(errno));
		return false;
	}
	if (fstat(fd, &st) != 0) {
		close(fd);
		return false;
	}
	if (S_ISBLK(st.st_mode)) {
		uint64_t bsize = 0;
		ioctl(fd, BLKGETSIZE64, &bsize);
		img->size = bsize;
	} else {
		img->size = (uint64_t)st.st_size;
	}
	uprintf("Image analysis of '%s' (%s):", path, rf_size_to_human(img->size, false));

	img->compression = rf_compression_from_path(path);
	if (img->compression != COMP_NONE) {
		rf_reader_t* r;
		close(fd);
		uprintf("  Image is %s compressed", rf_compression_name(img->compression));
		r = rf_reader_open(path, img->compression, 0);
		if (r == NULL)
			return false;
		n = rf_reader_read(r, sector, sizeof(sector));
		rf_reader_close(r);
		if (n == (ssize_t)sizeof(sector) && sector[0x1FE] == 0x55 && sector[0x1FF] == 0xAA)
			img->is_bootable_img = true;
		if (img->compression == COMP_XZ)
			img->uncompressed_size = xz_uncompressed_size(path);
		else if (img->compression == COMP_ZSTD)
			img->uncompressed_size = zstd_uncompressed_size(path);
		if (img->uncompressed_size != 0)
			uprintf("  Uncompressed size: %s", rf_size_to_human(img->uncompressed_size, false));
		img->projected_size = img->uncompressed_size ? img->uncompressed_size : img->size;
		uprintf("  Image %s a bootable disk image", img->is_bootable_img ? "is" : "is NOT");
		return true;
	}

	n = pread(fd, sector, sizeof(sector), 0);
	if (n == (ssize_t)sizeof(sector) && sector[0x1FE] == 0x55 && sector[0x1FF] == 0xAA)
		img->is_bootable_img = true;

	/* Fixed VHD: raw disk image with a 512 byte 'conectix' footer */
	if (img->size > 512) {
		char footer[8];
		if (pread(fd, footer, sizeof(footer), (off_t)(img->size - 512)) == (ssize_t)sizeof(footer) &&
			memcmp(footer, "conectix", 8) == 0) {
			uint32_t disk_type;
			if (pread(fd, &disk_type, 4, (off_t)(img->size - 512 + 0x3C)) == 4 &&
				__builtin_bswap32(disk_type) == 2) {
				img->is_vhd = true;
				uprintf("  Image is a fixed VHD");
			} else {
				uprintf("  Image is a dynamic/differencing VHD, which is not supported");
				img->is_bootable_img = false;
			}
		}
	}

	if (pread(fd, cd001, sizeof(cd001), 0x8001) == (ssize_t)sizeof(cd001) && memcmp(cd001, "CD001", 5) == 0)
		img->is_iso = true;
	if (!img->is_iso) {
		/* UDF only images have a BEA01/NSR0x descriptor */
		uint8_t vrs[8];
		for (off_t off = 0x8000; off < 0x10000 && !img->is_iso; off += 0x800) {
			if (pread(fd, vrs, sizeof(vrs), off + 1) == (ssize_t)sizeof(vrs) &&
				(memcmp(vrs, "NSR02", 5) == 0 || memcmp(vrs, "NSR03", 5) == 0))
				img->is_iso = true;
		}
	}
	close(fd);

	img->projected_size = img->is_vhd ? img->size - 512 : img->size;
	if (img->is_iso) {
		uprintf("  Image is an ISO image");
		if (!rf_iso_scan(path, img)) {
			uprintf("  Could not scan ISO content");
			img->is_iso = false;
		}
		if (img->is_bootable_img) {
			img->is_isohybrid = true;
			uprintf("  Image is an ISOHybrid (can be written in DD mode)");
		}
		if (img->disable_iso)
			uprintf("  This ISO is known to only work in DD mode");
	} else {
		uprintf("  Image %s a bootable disk image", img->is_bootable_img ? "is" : "is NOT");
	}
	return true;
}

void rf_image_describe(const rf_image_t* img, char* out, size_t out_size)
{
	char boot[512] = "";
	size_t len;

#define ADD(...) do { len = strlen(boot); snprintf(&boot[len], sizeof(boot) - len, __VA_ARGS__); } while (0)
	if (img->is_iso) {
		if (HAS_WININST(img) || HAS_BOOTMGR(img))
			ADD("Windows, ");
		else if (HAS_WINPE(img))
			ADD("WinPE, ");
		if (HAS_SYSLINUX(img))
			ADD("Syslinux %s, ", img->sl_version_str);
		if (img->has_grub2 & 0x7f)
			ADD("GRUB %s, ", img->grub2_version[0] ? img->grub2_version : "2");
		if (img->has_grub4dos)
			ADD("Grub4DOS, ");
		if (HAS_REACTOS(img))
			ADD("ReactOS, ");
		if (HAS_KOLIBRIOS(img))
			ADD("KolibriOS, ");
		if (IS_EFI_BOOTABLE(img)) {
			ADD("UEFI");
			for (size_t i = 0; i < ARRAYSIZE(efi_arch_str); i++)
				if (img->has_efi & (2 << i))
					ADD(" %s", efi_arch_str[i]);
			ADD(", ");
		}
		len = strlen(boot);
		if (len >= 2)
			boot[len - 2] = 0;
		snprintf(out, out_size, "%s ISO%s%s%s", img->label[0] ? img->label : "Unlabeled",
			boot[0] ? " (" : "", boot, boot[0] ? ")" : "");
	} else if (img->is_bootable_img) {
		snprintf(out, out_size, "Bootable disk image%s%s", img->compression ? ", " : "",
			img->compression ? rf_compression_name(img->compression) : "");
	} else {
		snprintf(out, out_size, "Non-bootable image");
	}
#undef ADD
}
