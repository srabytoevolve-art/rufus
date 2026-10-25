/*
 * Rufus for Linux: MBR and GPT partitioning
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
 *
 * Creates the same layout as Windows Rufus' CreatePartition():
 *   [main partition][persistence (optional)][UEFI:NTFS (optional)]
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
#include <unistd.h>
#include <sys/random.h>
#include <zlib.h>

#include "internal.h"

#define ALIGNMENT           (1 * MB)
#define GPT_ENTRIES         128
#define GPT_ENTRY_SIZE      128
#define MBR_UEFI_MARKER     0x49464555  /* 'U', 'E', 'F', 'I' */
#define CLEAR_SIZE          (1 * MB)

#define FLOOR_ALIGN(x, a)   ((x) / (a) * (a))
#define CEILING_ALIGN(x, a) (((x) + (a) - 1) / (a) * (a))

typedef struct {
	uint8_t b[16];
} guid_t;

/* GUIDs in their string form; converted to the on-disk mixed endian form below */
static const char* GUID_MS_BASIC_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
static const char* GUID_LINUX_DATA = "0FC63DAF-8483-4772-8E79-3D69D8477DE4";
#define GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER    0x8000000000000000ULL

#pragma pack(push, 1)
typedef struct {
	uint8_t status;
	uint8_t chs_first[3];
	uint8_t type;
	uint8_t chs_last[3];
	uint32_t lba_first;
	uint32_t nb_sectors;
} mbr_entry_t;

typedef struct {
	char signature[8];
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	guid_t disk_guid;
	uint64_t entries_lba;
	uint32_t nb_entries;
	uint32_t entry_size;
	uint32_t entries_crc32;
} gpt_header_t;

typedef struct {
	guid_t type;
	guid_t unique;
	uint64_t first_lba;
	uint64_t last_lba;
	uint64_t attributes;
	uint16_t name[36];
} gpt_entry_t;
#pragma pack(pop)

_Static_assert(sizeof(mbr_entry_t) == 16, "bad MBR entry size");
_Static_assert(sizeof(gpt_header_t) == 92, "bad GPT header size");
_Static_assert(sizeof(gpt_entry_t) == GPT_ENTRY_SIZE, "bad GPT entry size");

static guid_t guid_from_string(const char* s)
{
	guid_t g = { 0 };
	unsigned int v[16];

	sscanf(s, "%2x%2x%2x%2x-%2x%2x-%2x%2x-%2x%2x-%2x%2x%2x%2x%2x%2x",
		&v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7],
		&v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14], &v[15]);
	/* First three fields are little endian on disk */
	g.b[0] = (uint8_t)v[3]; g.b[1] = (uint8_t)v[2]; g.b[2] = (uint8_t)v[1]; g.b[3] = (uint8_t)v[0];
	g.b[4] = (uint8_t)v[5]; g.b[5] = (uint8_t)v[4];
	g.b[6] = (uint8_t)v[7]; g.b[7] = (uint8_t)v[6];
	for (int i = 8; i < 16; i++)
		g.b[i] = (uint8_t)v[i];
	return g;
}

static guid_t guid_random(void)
{
	guid_t g;

	if (getrandom(g.b, sizeof(g.b), 0) != (ssize_t)sizeof(g.b)) {
		for (size_t i = 0; i < sizeof(g.b); i++)
			g.b[i] = (uint8_t)rand();
	}
	/* RFC 4122 version 4 */
	g.b[7] = (uint8_t)((g.b[7] & 0x0f) | 0x40);
	g.b[8] = (uint8_t)((g.b[8] & 0x3f) | 0x80);
	return g;
}

static void set_name(uint16_t* dst, const char* name)
{
	for (int i = 0; i < 36 && name[i] != 0; i++)
		dst[i] = (uint16_t)(unsigned char)name[i];
}

static bool pwrite_all(int fd, const void* buf, size_t len, uint64_t offset)
{
	const uint8_t* p = buf;
	while (len > 0) {
		ssize_t n = pwrite(fd, p, len, (off_t)offset);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			uprintf("Write error at offset %" PRIu64 ": %s", offset, strerror(errno));
			return false;
		}
		p += n;
		len -= (size_t)n;
		offset += (uint64_t)n;
	}
	return true;
}

static bool zero_at(int fd, uint64_t offset, size_t size)
{
	uint8_t* buf = calloc(1, size);
	bool r;

	if (buf == NULL)
		return false;
	r = pwrite_all(fd, buf, size, offset);
	free(buf);
	return r;
}

bool rf_clear_partition_tables(int fd, uint64_t disk_size, uint32_t sector_size)
{
	size_t size = (size_t)MIN((uint64_t)CLEAR_SIZE, disk_size / 2);
	(void)sector_size;

	uprintf("Erasing the partition tables...");
	if (!zero_at(fd, 0, size))
		return false;
	/* Backup GPT */
	if (!zero_at(fd, FLOOR_ALIGN(disk_size - size, 512), size))
		return false;
	return fsync(fd) == 0;
}

static void lba_to_chs(uint64_t lba, uint8_t chs[3])
{
	/* 255 heads, 63 sectors per track. Use the 1023/254/63 marker past the CHS limit. */
	uint64_t c, h, s;
	if (lba >= 1024ULL * 255 * 63) {
		chs[0] = 0xfe;
		chs[1] = 0xff;
		chs[2] = 0xff;
		return;
	}
	c = lba / (255 * 63);
	h = (lba / 63) % 255;
	s = (lba % 63) + 1;
	chs[0] = (uint8_t)h;
	chs[1] = (uint8_t)(s | ((c >> 2) & 0xc0));
	chs[2] = (uint8_t)(c & 0xff);
}

typedef struct {
	const char* name;
	uint64_t offset;
	uint64_t size;
	uint8_t mbr_type;
	const char* gpt_type;
	uint64_t gpt_attributes;
	bool active;
} part_def_t;

static bool write_mbr_table(int fd, uint32_t sector_size, part_def_t* parts, int nb, uint32_t signature)
{
	uint8_t mbr[512];
	mbr_entry_t* e = (mbr_entry_t*)&mbr[0x1be];

	/* Preserve any existing boot code: only rewrite the table area */
	if (pread(fd, mbr, sizeof(mbr), 0) != (ssize_t)sizeof(mbr))
		memset(mbr, 0, sizeof(mbr));
	memset(&mbr[0x1b8], 0, 512 - 0x1b8);
	memcpy(&mbr[0x1b8], &signature, 4);
	for (int i = 0; i < nb && i < 4; i++) {
		uint64_t first = parts[i].offset / sector_size;
		uint64_t count = parts[i].size / sector_size;
		if (first > 0xffffffffULL || count > 0xffffffffULL) {
			uprintf("Partition %d does not fit in an MBR (drive too large) - use GPT", i + 1);
			return false;
		}
		e[i].status = parts[i].active ? 0x80 : 0x00;
		e[i].type = parts[i].mbr_type;
		e[i].lba_first = (uint32_t)first;
		e[i].nb_sectors = (uint32_t)count;
		lba_to_chs(first, e[i].chs_first);
		lba_to_chs(first + count - 1, e[i].chs_last);
	}
	mbr[0x1fe] = 0x55;
	mbr[0x1ff] = 0xaa;
	return pwrite_all(fd, mbr, sizeof(mbr), 0);
}

static bool write_gpt_table(int fd, uint64_t disk_size, uint32_t sector_size, part_def_t* parts, int nb)
{
	uint64_t total_lba = disk_size / sector_size;
	uint64_t entries_bytes = (uint64_t)GPT_ENTRIES * GPT_ENTRY_SIZE;
	uint64_t entries_lbas = CEILING_ALIGN(entries_bytes, sector_size) / sector_size;
	uint8_t *entries = NULL, *sector = NULL;
	gpt_header_t hdr;
	mbr_entry_t* e;
	bool r = false;

	entries = calloc(1, (size_t)(entries_lbas * sector_size));
	sector = calloc(1, sector_size);
	if (entries == NULL || sector == NULL)
		goto out;

	for (int i = 0; i < nb; i++) {
		gpt_entry_t* ge = (gpt_entry_t*)&entries[i * GPT_ENTRY_SIZE];
		ge->type = guid_from_string(parts[i].gpt_type);
		ge->unique = guid_random();
		ge->first_lba = parts[i].offset / sector_size;
		ge->last_lba = (parts[i].offset + parts[i].size) / sector_size - 1;
		ge->attributes = parts[i].gpt_attributes;
		set_name(ge->name, parts[i].name);
	}

	/* Protective MBR (boot code gets added later) */
	if (pread(fd, sector, 512, 0) != 512)
		memset(sector, 0, 512);
	memset(&sector[0x1b8], 0, 512 - 0x1b8);
	e = (mbr_entry_t*)&sector[0x1be];
	e->type = 0xee;
	e->lba_first = 1;
	e->nb_sectors = (total_lba - 1 > 0xffffffffULL) ? 0xffffffffU : (uint32_t)(total_lba - 1);
	e->chs_first[1] = 0x02;
	lba_to_chs(total_lba - 1, e->chs_last);
	sector[0x1fe] = 0x55;
	sector[0x1ff] = 0xaa;
	if (!pwrite_all(fd, sector, 512, 0))
		goto out;

	memset(&hdr, 0, sizeof(hdr));
	memcpy(hdr.signature, "EFI PART", 8);
	hdr.revision = 0x00010000;
	hdr.header_size = sizeof(hdr);
	hdr.first_usable_lba = 2 + entries_lbas;
	hdr.last_usable_lba = total_lba - 2 - entries_lbas;
	hdr.disk_guid = guid_random();
	hdr.nb_entries = GPT_ENTRIES;
	hdr.entry_size = GPT_ENTRY_SIZE;
	hdr.entries_crc32 = (uint32_t)crc32(0L, entries, (uInt)entries_bytes);

	/* Primary */
	hdr.my_lba = 1;
	hdr.alternate_lba = total_lba - 1;
	hdr.entries_lba = 2;
	hdr.header_crc32 = 0;
	hdr.header_crc32 = (uint32_t)crc32(0L, (const Bytef*)&hdr, sizeof(hdr));
	memset(sector, 0, sector_size);
	memcpy(sector, &hdr, sizeof(hdr));
	if (!pwrite_all(fd, sector, sector_size, 1ULL * sector_size) ||
		!pwrite_all(fd, entries, (size_t)(entries_lbas * sector_size), 2ULL * sector_size))
		goto out;

	/* Backup */
	hdr.my_lba = total_lba - 1;
	hdr.alternate_lba = 1;
	hdr.entries_lba = total_lba - 1 - entries_lbas;
	hdr.header_crc32 = 0;
	hdr.header_crc32 = (uint32_t)crc32(0L, (const Bytef*)&hdr, sizeof(hdr));
	memset(sector, 0, sector_size);
	memcpy(sector, &hdr, sizeof(hdr));
	if (!pwrite_all(fd, entries, (size_t)(entries_lbas * sector_size), hdr.entries_lba * sector_size) ||
		!pwrite_all(fd, sector, sector_size, (total_lba - 1) * sector_size))
		goto out;
	r = true;

out:
	free(entries);
	free(sector);
	return r;
}

bool rf_create_partitions(int fd, uint64_t disk_size, uint32_t sector_size, const rf_options_t* opts,
	rf_fs_t fs, bool bootable, bool add_uefi_ntfs, rf_layout_t* layout)
{
	part_def_t parts[PI_MAX];
	int pi = 0, mi, idx[PI_MAX];
	uint64_t last_offset, main_size, cluster;
	const rf_resource_t* uefi_ntfs = NULL;

	memset(layout, 0, sizeof(*layout));
	memset(parts, 0, sizeof(parts));
	for (int i = 0; i < PI_MAX; i++)
		idx[i] = -1;
	uprintf("Partitioning (%s)...", (opts->partition_style == PS_GPT) ? "GPT" : "MBR");

	if (add_uefi_ntfs) {
		uefi_ntfs = rf_resource("uefi-ntfs.img");
		if (uefi_ntfs == NULL) {
			uprintf("Could not access embedded 'uefi-ntfs.img'");
			return false;
		}
	}

	/* Main partition */
	mi = pi;
	idx[PI_MAIN] = pi;
	parts[pi].name = "Main Data Partition";
	parts[pi].offset = ALIGNMENT;
	parts[pi].active = bootable && opts->partition_style == PS_MBR && opts->target_type == TT_BIOS;
	switch (fs) {
	case FS_FAT16: parts[pi].mbr_type = 0x0e; break;
	case FS_FAT32: parts[pi].mbr_type = 0x0c; break;
	case FS_NTFS:
	case FS_EXFAT:
	case FS_UDF: parts[pi].mbr_type = 0x07; break;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4: parts[pi].mbr_type = 0x83; break;
	default:
		uprintf("Unsupported file system");
		return false;
	}
	parts[pi].gpt_type = IS_EXT(fs) ? GUID_LINUX_DATA : GUID_MS_BASIC_DATA;
	pi++;

	if (opts->persistence_size != 0) {
		idx[PI_CASPER] = pi;
		parts[pi].name = "Linux Persistence";
		parts[pi].size = CEILING_ALIGN(opts->persistence_size, ALIGNMENT);
		parts[pi].mbr_type = 0x83;
		parts[pi].gpt_type = GUID_LINUX_DATA;
		pi++;
	}
	if (add_uefi_ntfs) {
		idx[PI_ESP] = pi;
		parts[pi].name = "UEFI:NTFS";
		parts[pi].size = CEILING_ALIGN(uefi_ntfs->size, ALIGNMENT);
		/* UEFI:NTFS as an MBR ESP is fine. For GPT, do NOT declare it as ESP, lest Windows setup fails */
		parts[pi].mbr_type = 0xef;
		parts[pi].gpt_type = GUID_MS_BASIC_DATA;
		parts[pi].gpt_attributes = GPT_BASIC_DATA_ATTRIBUTE_NO_DRIVE_LETTER;
		pi++;
	}

	/* Extra partitions go at the end of the drive */
	last_offset = disk_size;
	if (opts->partition_style == PS_GPT)
		last_offset -= 33ULL * sector_size;
	for (int i = pi - 1; i > mi; i--) {
		if (parts[i].size >= last_offset) {
			uprintf("The drive is too small for the %s partition", parts[i].name);
			return false;
		}
		parts[i].offset = FLOOR_ALIGN(last_offset - parts[i].size, ALIGNMENT);
		last_offset = parts[i].offset;
	}
	if (last_offset <= parts[mi].offset) {
		uprintf("The drive is too small");
		return false;
	}
	main_size = FLOOR_ALIGN(last_offset - parts[mi].offset, sector_size);
	cluster = opts->cluster_size;
	if (cluster != 0 && cluster % sector_size == 0)
		main_size = FLOOR_ALIGN(main_size, cluster);
	parts[mi].size = main_size;

	for (int i = 0; i < pi; i++) {
		uprintf("● Creating %s%s (offset: %" PRIu64 ", size: %s)", parts[i].name,
			(strstr(parts[i].name, "Partition") == NULL) ? " Partition" : "",
			parts[i].offset, rf_size_to_human(parts[i].size, false));
		/* Zero the start of each partition to avoid stale filesystem detection */
		if (!zero_at(fd, parts[i].offset, (size_t)MIN((uint64_t)CLEAR_SIZE, parts[i].size)))
			return false;
	}

	/* Write UEFI:NTFS data before the table */
	if (add_uefi_ntfs) {
		uprintf("Writing UEFI:NTFS data...");
		if (!pwrite_all(fd, uefi_ntfs->data, uefi_ntfs->size, parts[idx[PI_ESP]].offset))
			return false;
	}

	if (opts->partition_style == PS_MBR) {
		uint32_t signature;
		if (opts->target_type == TT_UEFI) {
			signature = MBR_UEFI_MARKER;
		} else if (getrandom(&signature, sizeof(signature), 0) != (ssize_t)sizeof(signature)) {
			signature = (uint32_t)rf_monotonic_ms();
		}
		if (!write_mbr_table(fd, sector_size, parts, pi, signature))
			return false;
	} else {
		if (!write_gpt_table(fd, disk_size, sector_size, parts, pi))
			return false;
	}
	if (fsync(fd) != 0) {
		uprintf("Could not flush partition table: %s", strerror(errno));
		return false;
	}

	layout->nb_partitions = pi;
	for (int i = 0; i < PI_MAX; i++) {
		if (idx[i] < 0)
			continue;
		layout->part[i].offset = parts[idx[i]].offset;
		layout->part[i].size = parts[idx[i]].size;
		layout->part[i].index = idx[i] + 1;
	}
	return true;
}
