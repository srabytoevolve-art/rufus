/*
 * Rufus for Linux: file system creation and mounting
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
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#include "internal.h"

const char* rf_fs_name[FS_MAX] = { "FAT", "FAT32", "NTFS", "UDF", "exFAT", "ReFS", "ext2", "ext3", "ext4" };

static const char* mkfs_tool[FS_MAX][2] = {
	[FS_FAT16] = { "mkfs.fat", "mkfs.vfat" },
	[FS_FAT32] = { "mkfs.fat", "mkfs.vfat" },
	[FS_NTFS] = { "mkfs.ntfs", "mkntfs" },
	[FS_UDF] = { "mkudffs", "mkfs.udf" },
	[FS_EXFAT] = { "mkfs.exfat", NULL },
	[FS_REFS] = { NULL, NULL },
	[FS_EXT2] = { "mkfs.ext2", "mke2fs" },
	[FS_EXT3] = { "mkfs.ext3", "mke2fs" },
	[FS_EXT4] = { "mkfs.ext4", "mke2fs" },
};

rf_fs_t rf_fs_from_name(const char* name)
{
	if (rf_str_iequal(name, "FAT16") || rf_str_iequal(name, "FAT"))
		return FS_FAT16;
	for (int i = 0; i < FS_MAX; i++)
		if (rf_str_iequal(name, rf_fs_name[i]))
			return (rf_fs_t)i;
	return FS_UNKNOWN;
}

static const char* find_mkfs(rf_fs_t fs)
{
	if (fs < 0 || fs >= FS_MAX)
		return NULL;
	for (int i = 0; i < 2; i++)
		if (mkfs_tool[fs][i] != NULL && rf_which(mkfs_tool[fs][i], NULL, 0))
			return mkfs_tool[fs][i];
	return NULL;
}

bool rf_fs_available(rf_fs_t fs)
{
	if (fs == FS_REFS || fs == FS_UDF)
		return false;
	return find_mkfs(fs) != NULL;
}

uint32_t rf_default_cluster_size(rf_fs_t fs, uint64_t part_size)
{
	/* Same defaults as Windows for the sizes most people use, so that drives look familiar */
	switch (fs) {
	case FS_FAT16:
		return (part_size < 512 * MB) ? 8 * KB : ((part_size < 1 * GB) ? 16 * KB : ((part_size < 2 * GB) ? 32 * KB : 64 * KB));
	case FS_FAT32:
		return (part_size < 8 * GB) ? 4 * KB : ((part_size < 16 * GB) ? 8 * KB : ((part_size < 32 * GB) ? 16 * KB : 32 * KB));
	case FS_NTFS:
		return (part_size < 16 * TB) ? 4 * KB : 8 * KB;
	case FS_EXFAT:
		return (part_size < 256 * MB) ? 4 * KB : ((part_size < 32 * GB) ? 32 * KB : 128 * KB);
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		return 4 * KB;
	default:
		return 0;
	}
}

static uint64_t partition_start_sector(const char* part_path)
{
	struct stat st;
	char sys[256], buf[64];
	FILE* fd;

	if (stat(part_path, &st) != 0)
		return 0;
	snprintf(sys, sizeof(sys), "/sys/dev/block/%u:%u/start", major(st.st_rdev), minor(st.st_rdev));
	fd = fopen(sys, "r");
	if (fd == NULL)
		return 0;
	if (fgets(buf, sizeof(buf), fd) == NULL)
		buf[0] = 0;
	fclose(fd);
	return strtoull(buf, NULL, 10);
}

bool rf_format_partition(const char* part_path, rf_fs_t fs, const char* label, uint32_t cluster_size,
	uint64_t part_size, uint32_t sector_size, bool quick)
{
	const char* tool = find_mkfs(fs);
	const char* argv[32];
	char cluster_str[32], start_str[32];
	int argc = 0;
	int ret;

	if (tool == NULL) {
		uprintf("No tool to create %s file systems was found - please install %s", rf_fs_name[fs],
			(fs == FS_NTFS) ? "ntfs-3g" : (fs == FS_EXFAT) ? "exfatprogs" : IS_EXT(fs) ? "e2fsprogs" : "dosfstools");
		return false;
	}
	uprintf("Formatting %s as %s, label '%s'%s%s", part_path, rf_fs_name[fs], label,
		cluster_size ? ", cluster size " : "", cluster_size ? rf_size_to_human(cluster_size, false) : "");
	rf_progress(OP_FORMAT, 0.0, "Formatting (%s)...", rf_fs_name[fs]);

	if (!quick) {
		int fd = open(part_path, O_WRONLY | O_CLOEXEC);
		if (fd < 0) {
			uprintf("Could not open '%s': %s", part_path, strerror(errno));
			return false;
		}
		uprintf("Full format: zeroing the partition");
		if (!rf_zero_range(fd, 0, part_size, OP_FORMAT)) {
			close(fd);
			return false;
		}
		close(fd);
	}

	argv[argc++] = tool;
	switch (fs) {
	case FS_FAT16:
	case FS_FAT32:
		argv[argc++] = "-F";
		argv[argc++] = (fs == FS_FAT16) ? "16" : "32";
		argv[argc++] = "-n";
		argv[argc++] = label;
		if (cluster_size != 0 && sector_size != 0) {
			snprintf(cluster_str, sizeof(cluster_str), "%u", cluster_size / sector_size);
			argv[argc++] = "-s";
			argv[argc++] = cluster_str;
		}
		/* Windows-compatible geometry, so that the BPB matches what ms-sys expects */
		argv[argc++] = "-h";
		snprintf(start_str, sizeof(start_str), "%" PRIu64, partition_start_sector(part_path));
		argv[argc++] = start_str;
		argv[argc++] = part_path;
		break;
	case FS_NTFS:
		argv[argc++] = "-Q";    /* We did our own zeroing above, if requested */
		argv[argc++] = "-F";
		argv[argc++] = "-L";
		argv[argc++] = label;
		if (cluster_size != 0) {
			snprintf(cluster_str, sizeof(cluster_str), "%u", cluster_size);
			argv[argc++] = "-c";
			argv[argc++] = cluster_str;
		}
		snprintf(start_str, sizeof(start_str), "%" PRIu64, partition_start_sector(part_path));
		argv[argc++] = "-p";
		argv[argc++] = start_str;
		argv[argc++] = "-H";
		argv[argc++] = "255";
		argv[argc++] = "-S";
		argv[argc++] = "63";
		argv[argc++] = part_path;
		break;
	case FS_EXFAT:
		argv[argc++] = "-L";
		argv[argc++] = label;
		if (cluster_size != 0) {
			snprintf(cluster_str, sizeof(cluster_str), "%u", cluster_size);
			argv[argc++] = "-c";
			argv[argc++] = cluster_str;
		}
		argv[argc++] = part_path;
		break;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		if (strcmp(tool, "mke2fs") == 0) {
			argv[argc++] = "-t";
			argv[argc++] = (fs == FS_EXT2) ? "ext2" : (fs == FS_EXT3) ? "ext3" : "ext4";
		}
		argv[argc++] = "-F";
		argv[argc++] = "-q";
		argv[argc++] = "-L";
		argv[argc++] = label;
		if (cluster_size != 0) {
			snprintf(cluster_str, sizeof(cluster_str), "%u", cluster_size);
			argv[argc++] = "-b";
			argv[argc++] = cluster_str;
		}
		argv[argc++] = part_path;
		break;
	default:
		uprintf("Unsupported file system");
		return false;
	}
	argv[argc] = NULL;

	ret = rf_run(argv);
	if (ret != 0) {
		uprintf("Formatting failed (%s returned %d)", tool, ret);
		return false;
	}
	rf_progress(OP_FORMAT, 1.0, "Formatting (%s)...", rf_fs_name[fs]);
	return true;
}

bool rf_mount(const char* part_path, rf_fs_t fs, char* mount_dir, size_t mount_dir_size)
{
	char tmpl[] = "/tmp/rufus-mnt-XXXXXX";
	const char* types[3] = { NULL, NULL, NULL };
	const char* options = "rw";

	if (mkdtemp(tmpl) == NULL) {
		uprintf("Could not create temporary mount point: %s", strerror(errno));
		return false;
	}
	switch (fs) {
	case FS_FAT16:
	case FS_FAT32:
		types[0] = "vfat";
		options = "rw,utf8,shortname=mixed,flush";
		break;
	case FS_NTFS:
		/* Prefer the in-kernel driver, fall back to ntfs-3g (FUSE) */
		types[0] = "ntfs3";
		types[1] = "ntfs-3g";
		break;
	case FS_EXFAT:
		types[0] = "exfat";
		options = "rw,iocharset=utf8";
		break;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		types[0] = rf_fs_name[fs];
		break;
	default:
		rmdir(tmpl);
		return false;
	}
	for (int i = 0; i < 3 && types[i] != NULL; i++) {
		const char* argv[] = { "mount", "-t", types[i], "-o", options, part_path, tmpl, NULL };
		if (rf_run(argv) == 0) {
			snprintf(mount_dir, mount_dir_size, "%s", tmpl);
			return true;
		}
	}
	uprintf("Could not mount %s", part_path);
	rmdir(tmpl);
	return false;
}

bool rf_unmount(const char* mount_dir)
{
	const char* sync_argv[] = { "sync", "-f", mount_dir, NULL };
	const char* argv[] = { "umount", mount_dir, NULL };
	bool r = false;

	rf_progress(OP_FINALIZE, -1.0, "Flushing data to the drive (this may take a while)...");
	uprintf("Flushing and unmounting '%s'...", mount_dir);
	rf_run(sync_argv);
	for (int i = 0; i < 10 && !r; i++) {
		r = (rf_run(argv) == 0);
		if (!r)
			sleep(1);
	}
	if (!r) {
		const char* lazy[] = { "umount", "-l", mount_dir, NULL };
		r = (rf_run(lazy) == 0);
	}
	if (r)
		rmdir(mount_dir);
	return r;
}
