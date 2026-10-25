/*
 * Rufus for Linux: internal declarations shared between core modules
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include "rufus.h"

/* iso.c */
bool rf_iso_scan(const char* path, rf_image_t* img);
uint16_t rf_get_syslinux_version(char* buf, size_t buf_size, char** ext);

/* boot.c */
bool rf_write_mbr(int fd, const rf_options_t* opts, const rf_image_t* img, uint32_t sector_size);
bool rf_write_sbr(int fd, const rf_options_t* opts, const rf_image_t* img, uint64_t first_partition_offset);
bool rf_write_pbr(const char* part_path, rf_fs_t fs, const rf_options_t* opts, const rf_image_t* img,
	uint32_t sector_size, uint64_t partition_offset);
bool rf_install_syslinux(const char* part_path, const char* mount_dir, const rf_options_t* opts,
	const rf_image_t* img, uint32_t sector_size);
bool rf_install_freedos(const char* mount_dir);

/* partition.c */
#define PI_MAIN         0
#define PI_ESP          1   /* UEFI:NTFS */
#define PI_CASPER       2   /* persistence */
#define PI_MAX          3

typedef struct {
	uint64_t offset;        /* bytes */
	uint64_t size;          /* bytes */
	int index;              /* 1-based partition number, 0 = not present */
} rf_part_t;

typedef struct {
	rf_part_t part[PI_MAX];
	int nb_partitions;
} rf_layout_t;

bool rf_create_partitions(int fd, uint64_t disk_size, uint32_t sector_size, const rf_options_t* opts,
	rf_fs_t fs, bool bootable, bool add_uefi_ntfs, rf_layout_t* layout);
bool rf_clear_partition_tables(int fd, uint64_t disk_size, uint32_t sector_size);

/* format.c */
bool rf_format_partition(const char* part_path, rf_fs_t fs, const char* label, uint32_t cluster_size,
	uint64_t part_size, uint32_t sector_size, bool quick);
bool rf_mount(const char* part_path, rf_fs_t fs, char* mount_dir, size_t mount_dir_size);
bool rf_unmount(const char* mount_dir);

/* dd.c */
bool rf_write_image(int fd, const char* image, const rf_image_t* img, uint64_t disk_size, bool verify);
bool rf_zero_range(int fd, uint64_t offset, uint64_t size, rf_op_t op);

/* badblocks.c */
bool rf_badblocks_check(int fd, uint64_t disk_size, int passes, uint64_t* bad_blocks);

/* wue.c */
char* rf_create_unattend_xml(int flags, const char* arch, const char* username, const char* locale,
	const char* timezone);

/* net.c */
bool rf_verify_signature(const uint8_t* buf, size_t len, const uint8_t* sig, size_t sig_len);

/* Copy a file, with progress */
bool rf_copy_file(const char* src, const char* dst);
