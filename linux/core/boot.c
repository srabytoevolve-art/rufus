/*
 * Rufus for Linux: boot records and boot loader installation
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
 *
 * Uses the same ms-sys boot records and syslinux installer as Windows Rufus.
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
#include <linux/msdos_fs.h>
/* Conflicts with the syslinux installer variable of the same name */
#undef SECTOR_SIZE

/* ms-sys */
#include "file.h"
#include "br.h"
#include "fat16.h"
#include "fat32.h"
#include "ntfs.h"
#include "partition_info.h"

/* syslinux */
#include "syslinux.h"
#include "syslxfs.h"
#include "setadv.h"
#include "libfat.h"

#include "internal.h"

/* Globals required by the syslinux installer and libfat */
unsigned char* syslinux_ldlinux[2] = { NULL, NULL };
unsigned long syslinux_ldlinux_len[2];
uint32_t SECTOR_SHIFT = 9;
uint32_t SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_SHIFT = 9;
uint32_t LIBFAT_SECTOR_SIZE = 512;
uint32_t LIBFAT_SECTOR_MASK = 511;

static FAKE_FD make_fake_fd(int fd, uint32_t sector_size)
{
	FAKE_FD f = { (void*)(intptr_t)fd, 0 };
	set_bytes_per_sector(sector_size);
	return f;
}

static bool uses_syslinux(const rf_options_t* opts, const rf_image_t* img)
{
	return opts->boot_type == BT_SYSLINUX_V4 || opts->boot_type == BT_SYSLINUX_V6 ||
		(opts->boot_type == BT_IMAGE && img != NULL && HAS_SYSLINUX(img));
}

/*
 * Master Boot Record. The partition table, partition types and active flag were
 * already written by rf_create_partitions(), so we only add the boot code.
 */
bool rf_write_mbr(int fd, const rf_options_t* opts, const rf_image_t* img, uint32_t sector_size)
{
	bool needs_masquerading = (opts->boot_type == BT_IMAGE) && img != NULL && HAS_WINPE(img) && !img->uses_minint;
	FAKE_FD fake_fd = make_fake_fd(fd, sector_size);
	FILE* fp = (FILE*)&fake_fd;
	const char* using_msg = "Using %s MBR";
	uint8_t buf[512];
	int r;

	if (opts->partition_style == PS_GPT) {
		uprintf(using_msg, "Rufus protective");
		r = write_rufus_msg_mbr(fp);
		goto out;
	}

	if (opts->boot_type != BT_NON_BOOTABLE && opts->target_type == TT_BIOS && needs_masquerading) {
		/* Masquerade as second disk for WinPE */
		if (pread(fd, buf, sizeof(buf), 0) == (ssize_t)sizeof(buf)) {
			buf[0x1be] = 0x81;
			uprintf("Set bootable USB partition as 0x81");
			if (pwrite(fd, buf, sizeof(buf), 0) != (ssize_t)sizeof(buf))
				return false;
		}
	}

	if (opts->boot_type == BT_NON_BOOTABLE || opts->target_type == TT_UEFI) {
		uprintf(using_msg, "Zeroed");
		r = write_zero_mbr(fp);
	} else if (uses_syslinux(opts, img)) {
		uprintf(using_msg, "Syslinux");
		r = write_syslinux_mbr(fp);
	} else if ((opts->boot_type == BT_IMAGE && img != NULL && (img->has_grub2 & 0x7f)) || opts->boot_type == BT_GRUB2) {
		uprintf(using_msg, "Grub 2.0");
		r = write_grub2_mbr(fp);
	} else if (opts->boot_type == BT_IMAGE && img != NULL && img->has_grub4dos) {
		uprintf(using_msg, "Grub4DOS");
		r = write_grub4dos_mbr(fp);
	} else if (opts->boot_type == BT_IMAGE && img != NULL && HAS_KOLIBRIOS(img) && IS_FAT(opts->fs)) {
		uprintf(using_msg, "KolibriOS");
		r = write_kolibrios_mbr(fp);
	} else if (needs_masquerading || opts->use_rufus_mbr) {
		uprintf(using_msg, APPLICATION_NAME);
		r = write_rufus_mbr(fp);
	} else {
		uprintf(using_msg, "Windows 7");
		r = write_win7_mbr(fp);
	}
out:
	fsync(fd);
	return r != 0;
}

/* Secondary Boot Record (right after the MBR) */
bool rf_write_sbr(int fd, const rf_options_t* opts, const rf_image_t* img, uint64_t first_partition_offset)
{
	uint32_t sector_size = 512;
	FAKE_FD fake_fd;
	FILE* fp = (FILE*)&fake_fd;
	uint64_t br_size = 0x200;
	const uint8_t* buf = NULL;
	uint8_t* alloc = NULL;
	size_t size = 0;
	enum { SBR_NONE, SBR_GRUB4DOS, SBR_GRUB2, SBR_MSG } sub_type = SBR_NONE;
	int r;

	struct stat st;
	if (fstat(fd, &st) == 0) {
		int ss = 0;
		if (ioctl(fd, BLKSSZGET, &ss) == 0 && ss > 0)
			sector_size = (uint32_t)ss;
	}
	fake_fd = make_fake_fd(fd, sector_size);

	if (opts->boot_type == BT_GRUB2)
		sub_type = SBR_GRUB2;
	if (opts->boot_type == BT_IMAGE && img != NULL && !HAS_SYSLINUX(img)) {
		if (img->has_grub4dos)
			sub_type = SBR_GRUB4DOS;
		if (img->has_grub2 & 0x7f)
			sub_type = SBR_GRUB2;
	}
	if (opts->boot_type != BT_NON_BOOTABLE && opts->partition_style == PS_GPT)
		sub_type = SBR_MSG;
	if (opts->target_type == TT_UEFI && sub_type != SBR_MSG)
		return true;

	switch (sub_type) {
	case SBR_GRUB4DOS: {
		const rf_resource_t* res = rf_resource("grldr.mbr");
		uprintf("Writing Grub4Dos SBR");
		if (res == NULL || res->size <= br_size)
			return false;
		buf = &res->data[br_size];
		size = res->size - br_size;
		break;
	}
	case SBR_GRUB2:
		if (opts->grub2_core_img[0] != 0) {
			uprintf("Writing Grub 2.0 SBR (from download '%s')", opts->grub2_core_img);
			alloc = (uint8_t*)rf_read_text_file(opts->grub2_core_img, &size);
			buf = alloc;
		}
		if (buf == NULL) {
			const rf_resource_t* res = rf_resource("core.img");
			uprintf("Writing Grub 2.0 SBR (from embedded %s)", embedded_grub2_version);
			if (img != NULL && img->grub2_version[0] != 0 && strncmp(img->grub2_version, embedded_grub2_version,
				strlen(embedded_grub2_version)) != 0)
				uprintf("WARNING: This image uses GRUB %s, but the embedded core.img is from GRUB %s. "
					"BIOS boot may fail.", img->grub2_version, embedded_grub2_version);
			if (res == NULL)
				return false;
			buf = res->data;
			size = res->size;
		}
		break;
	case SBR_MSG: {
		const rf_resource_t* res = rf_resource("msg.txt");
		uprintf("Writing protective message SBR");
		/* 34 sectors are reserved for the protective MBR + primary GPT (more for 4Kn) */
		br_size = MAX(17 * KB, (2ULL + (128 * 128 + sector_size - 1) / sector_size) * sector_size);
		if (res == NULL)
			return false;
		buf = res->data;
		size = res->size;
		break;
	}
	default:
		return true;
	}

	if (br_size + size > first_partition_offset) {
		uprintf("  SBR size is too large");
		free(alloc);
		return false;
	}
	r = write_data(fp, br_size, buf, (uint64_t)size);
	free(alloc);
	fsync(fd);
	return r != 0;
}

/* Partition Boot Record */
bool rf_write_pbr(const char* part_path, rf_fs_t fs, const rf_options_t* opts, const rf_image_t* img,
	uint32_t sector_size, uint64_t partition_offset)
{
	bool is_freedos = (opts->boot_type == BT_FREEDOS);
	bool is_kolibri = (opts->boot_type == BT_IMAGE && img != NULL && HAS_KOLIBRIOS(img));
	const char* name = is_freedos ? "FreeDOS" : (is_kolibri ? "KolibriOS" : "Standard");
	const char* using_msg = "Using %s %s partition boot record";
	FAKE_FD fake_fd;
	FILE* fp = (FILE*)&fake_fd;
	bool r = false;
	int fd;
	(void)partition_offset;

	fd = open(part_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open '%s': %s", part_path, strerror(errno));
		return false;
	}
	fake_fd = make_fake_fd(fd, sector_size);

	switch (fs) {
	case FS_FAT16:
		uprintf(using_msg, name, "FAT16");
		if (!is_fat_16_fs(fp)) {
			uprintf("New volume does not have a FAT16 boot sector - aborting");
			break;
		}
		if (is_freedos) {
			if (!write_fat_16_fd_br(fp, 0)) break;
		} else if (is_kolibri) {
			uprintf("FAT16 is not supported for KolibriOS");
			break;
		} else {
			if (!write_fat_16_br(fp, 0)) break;
		}
		if (!write_partition_physical_disk_drive_id_fat16(fp))
			break;
		r = true;
		break;
	case FS_FAT32:
		uprintf(using_msg, name, "FAT32");
		r = true;
		for (int i = 0; i < 2 && r; i++) {
			r = false;
			if (!is_fat_32_fs(fp)) {
				uprintf("New volume does not have a %s FAT32 boot sector - aborting", i ? "secondary" : "primary");
				break;
			}
			uprintf("Setting %s FAT32 boot sector for boot...", i ? "secondary" : "primary");
			if (is_freedos) {
				if (!write_fat_32_fd_br(fp, 0)) break;
			} else if (is_kolibri) {
				if (!write_fat_32_kos_br(fp, 0)) break;
			} else if (opts->boot_type == BT_IMAGE && img != NULL && HAS_BOOTMGR(img)) {
				if (!write_fat_32_pe_br(fp, 0)) break;
			} else if (opts->boot_type == BT_IMAGE && img != NULL && HAS_WINPE(img)) {
				if (!write_fat_32_nt_br(fp, 0)) break;
			} else {
				if (!write_fat_32_br(fp, 0)) break;
			}
			if (!write_partition_physical_disk_drive_id_fat32(fp))
				break;
			fake_fd._offset += 6 * sector_size;
			r = true;
		}
		break;
	case FS_NTFS:
		uprintf(using_msg, name, "NTFS");
		if (!is_ntfs_fs(fp)) {
			uprintf("New volume does not have an NTFS boot sector - aborting");
			break;
		}
		r = write_ntfs_br(fp) != 0;
		break;
	case FS_EXFAT:
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4:
		r = true;
		break;
	default:
		uprintf("Unsupported FS for FS BR processing - aborting");
		break;
	}
	fsync(fd);
	close(fd);
	return r;
}

/* ---------------------------------------------------------------------- */
/* Syslinux                                                               */
/* ---------------------------------------------------------------------- */

static int libfat_readfile(intptr_t pp, void* buf, size_t secsize, libfat_sector_t sector)
{
	ssize_t n = pread((int)pp, buf, secsize, (off_t)(sector * secsize));
	if (n != (ssize_t)secsize) {
		uprintf("Could not read sector %llu", (unsigned long long)sector);
		return 0;
	}
	return (int)secsize;
}

static uint8_t* load_file_or_resource(const char* path, const char* res_name, unsigned long* len)
{
	uint8_t* buf = NULL;

	if (path != NULL && path[0] != 0) {
		size_t size;
		buf = (uint8_t*)rf_read_text_file(path, &size);
		if (buf != NULL) {
			*len = (unsigned long)size;
			uprintf("Using '%s'", path);
			return buf;
		}
		uprintf("Could not read '%s' - using embedded version", path);
	}
	const rf_resource_t* res = rf_resource(res_name);
	if (res == NULL)
		return NULL;
	buf = malloc(res->size);
	if (buf == NULL)
		return NULL;
	memcpy(buf, res->data, res->size);
	*len = (unsigned long)res->size;
	return buf;
}

static void set_fat_attributes(const char* path, uint32_t attr)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (ioctl(fd, FAT_IOCTL_SET_ATTRIBUTES, &attr) != 0)
		uprintf("Could not set attributes on '%s': %s", path, strerror(errno));
	close(fd);
}

bool rf_install_syslinux(const char* part_path, const char* unused, const rf_options_t* opts,
	const rf_image_t* img, uint32_t sector_size)
{
	bool use_v5 = (opts->boot_type == BT_SYSLINUX_V6) ||
		(opts->boot_type == BT_IMAGE && img != NULL && SL_MAJOR(img->sl_version) >= 5);
	char mount_dir[256], path[512], cfg_dir[256] = "";
	uint8_t* sectbuf = NULL;
	libfat_sector_t* sectors = NULL;
	struct libfat_filesystem* lf_fs;
	const char* errmsg;
	int fd = -1, nsectors = 0, ldlinux_sectors, sl_fs_stype;
	uint32_t ldlinux_cluster;
	libfat_sector_t s;
	FILE* f;
	bool r = false;
	(void)unused;

	if (!IS_FAT(opts->fs)) {
		uprintf("Syslinux installation is only supported on FAT file systems");
		return false;
	}
	uprintf("Installing Syslinux %s...", (opts->boot_type == BT_IMAGE && img != NULL) ? img->sl_version_str :
		embedded_sl_version_str[use_v5 ? 1 : 0]);

	SECTOR_SHIFT = 0;
	for (uint32_t ss = sector_size; ss > 1; ss >>= 1)
		SECTOR_SHIFT++;
	SECTOR_SIZE = sector_size;
	LIBFAT_SECTOR_SHIFT = SECTOR_SHIFT;
	LIBFAT_SECTOR_SIZE = SECTOR_SIZE;
	LIBFAT_SECTOR_MASK = SECTOR_SIZE - 1;

	syslinux_ldlinux[0] = load_file_or_resource(opts->ldlinux_sys, use_v5 ? "ldlinux_v6.sys" : "ldlinux_v4.sys",
		&syslinux_ldlinux_len[0]);
	syslinux_ldlinux[1] = load_file_or_resource(opts->ldlinux_bss, use_v5 ? "ldlinux_v6.bss" : "ldlinux_v4.bss",
		&syslinux_ldlinux_len[1]);
	if (syslinux_ldlinux[0] == NULL || syslinux_ldlinux[1] == NULL)
		goto out;
	if (opts->boot_type == BT_IMAGE && img != NULL && use_v5 && opts->ldlinux_sys[0] == 0 &&
		img->sl_version != embedded_sl_version[1])
		uprintf("WARNING: This image uses Syslinux %s%s but only %s is available - boot may fail",
			img->sl_version_str, img->sl_version_ext, embedded_sl_version_str[1]);

	sectbuf = aligned_alloc(16, SECTOR_SIZE);
	if (sectbuf == NULL)
		goto out;

	/* Check the boot sector */
	fd = open(part_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open '%s': %s", part_path, strerror(errno));
		goto out;
	}
	if (pread(fd, sectbuf, SECTOR_SIZE, 0) != (ssize_t)SECTOR_SIZE) {
		uprintf("Could not read VBR");
		goto out;
	}
	if ((errmsg = syslinux_check_bootsect(sectbuf, &sl_fs_stype)) != NULL) {
		uprintf("Error: %s", errmsg);
		goto out;
	}
	close(fd);
	fd = -1;

	/* Write ldlinux.sys (and ldlinux.c32 / ReactOS files) through the file system */
	syslinux_reset_adv(syslinux_adv);
	if (!rf_mount(part_path, opts->fs, mount_dir, sizeof(mount_dir)))
		goto out;
	snprintf(path, sizeof(path), "%s/ldlinux.sys", mount_dir);
	unlink(path);
	f = fopen(path, "wb");
	if (f == NULL || fwrite(syslinux_ldlinux[0], 1, syslinux_ldlinux_len[0], f) != syslinux_ldlinux_len[0] ||
		fwrite(syslinux_adv, 1, 2 * ADV_SIZE, f) != 2 * ADV_SIZE) {
		uprintf("Could not write '%s': %s", path, strerror(errno));
		if (f != NULL)
			fclose(f);
		rf_unmount(mount_dir);
		goto out;
	}
	fflush(f);
	fsync(fileno(f));
	fclose(f);
	set_fat_attributes(path, ATTR_RO | ATTR_HIDDEN | ATTR_SYS);
	uprintf("Successfully wrote 'ldlinux.sys'");

	if (opts->boot_type == BT_SYSLINUX_V6) {
		snprintf(path, sizeof(path), "%s/ldlinux.c32", mount_dir);
		if (opts->ldlinux_c32[0] != 0 && rf_copy_file(opts->ldlinux_c32, path))
			uprintf("Created '%s'", path);
		else
			uprintf("Caution: No 'ldlinux.c32' was provided. The target will be missing a required Syslinux file!");
	} else if (opts->boot_type == BT_IMAGE && img != NULL && HAS_REACTOS(img) && !HAS_SYSLINUX(img)) {
		const rf_resource_t* res = rf_resource("mboot.c32");
		uprintf("Setting up ReactOS...");
		snprintf(path, sizeof(path), "%s/mboot.c32", mount_dir);
		if (res == NULL || !rf_write_file(path, res->data, res->size)) {
			rf_unmount(mount_dir);
			goto out;
		}
		snprintf(path, sizeof(path), "%s/syslinux.cfg", mount_dir);
		f = fopen(path, "w");
		if (f == NULL) {
			uprintf("Could not create ReactOS 'syslinux.cfg'");
			rf_unmount(mount_dir);
			goto out;
		}
		fprintf(f, "DEFAULT ReactOS\nLABEL ReactOS\n  KERNEL mboot.c32\n  APPEND %s", img->reactos_path);
		fclose(f);
	}
	if (!rf_unmount(mount_dir))
		goto out;

	/* Map ldlinux.sys sectors through libfat, then patch */
	fd = open(part_path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not reopen '%s': %s", part_path, strerror(errno));
		goto out;
	}
	ldlinux_sectors = (int)((syslinux_ldlinux_len[0] + 2 * ADV_SIZE + SECTOR_SIZE - 1) >> SECTOR_SHIFT);
	sectors = calloc((size_t)ldlinux_sectors, sizeof(*sectors));
	if (sectors == NULL)
		goto out;
	lf_fs = libfat_open(libfat_readfile, (intptr_t)fd);
	if (lf_fs == NULL) {
		uprintf("Syslinux FAT access error");
		goto out;
	}
	{
		int32_t cluster = libfat_searchdir(lf_fs, 0, "LDLINUX SYS", NULL);
		if (cluster < 0) {
			uprintf("Could not locate 'ldlinux.sys' on the FAT volume");
			libfat_close(lf_fs);
			goto out;
		}
		ldlinux_cluster = (uint32_t)cluster;
	}
	s = libfat_clustertosector(lf_fs, (int32_t)ldlinux_cluster);
	while (s && nsectors < ldlinux_sectors) {
		sectors[nsectors++] = s;
		s = libfat_nextsector(lf_fs, s);
	}
	libfat_close(lf_fs);
	if (nsectors < ldlinux_sectors) {
		uprintf("Could not map all of 'ldlinux.sys' (%d/%d sectors)", nsectors, ldlinux_sectors);
		goto out;
	}

	/* Set the base directory (the config file's directory) and patch ldlinux.sys */
	if (opts->boot_type == BT_IMAGE && img != NULL && HAS_SYSLINUX(img)) {
		const char* slash = strrchr(img->cfg_path, '/');
		if (slash != NULL && slash != img->cfg_path)
			snprintf(cfg_dir, sizeof(cfg_dir), "%.*s", (int)(slash - img->cfg_path), img->cfg_path);
	}
	if (syslinux_patch(sectors, nsectors, 0, 0, cfg_dir, NULL) < 0) {
		uprintf("WARNING: Could not patch Syslinux files.");
		goto out;
	}

	/* Rewrite the patched ldlinux.sys data in place (read-modify-write, to keep the ADV intact) */
	for (int i = 0; (unsigned long)i * SECTOR_SIZE < syslinux_ldlinux_len[0]; i++) {
		size_t chunk = MIN((size_t)SECTOR_SIZE, (size_t)(syslinux_ldlinux_len[0] - (unsigned long)i * SECTOR_SIZE));
		off_t off = (off_t)(sectors[i] << SECTOR_SHIFT);
		if (pread(fd, sectbuf, SECTOR_SIZE, off) != (ssize_t)SECTOR_SIZE)
			goto out;
		memcpy(sectbuf, &syslinux_ldlinux[0][(size_t)i * SECTOR_SIZE], chunk);
		if (pwrite(fd, sectbuf, SECTOR_SIZE, off) != (ssize_t)SECTOR_SIZE) {
			uprintf("Could not rewrite 'ldlinux.sys': %s", strerror(errno));
			goto out;
		}
	}

	/* Make the syslinux boot sector */
	if (pread(fd, sectbuf, SECTOR_SIZE, 0) != (ssize_t)SECTOR_SIZE) {
		uprintf("Could not read Syslinux boot record");
		goto out;
	}
	syslinux_make_bootsect(sectbuf, VFAT);
	if (pwrite(fd, sectbuf, SECTOR_SIZE, 0) != (ssize_t)SECTOR_SIZE) {
		uprintf("Could not write Syslinux boot record: %s", strerror(errno));
		goto out;
	}
	fsync(fd);
	uprintf("Successfully wrote Syslinux boot record");
	r = true;

out:
	if (fd >= 0)
		close(fd);
	free(sectbuf);
	free(sectors);
	free(syslinux_ldlinux[0]);
	free(syslinux_ldlinux[1]);
	syslinux_ldlinux[0] = syslinux_ldlinux[1] = NULL;
	return r;
}

/* ---------------------------------------------------------------------- */
/* FreeDOS                                                                */
/* ---------------------------------------------------------------------- */

bool rf_install_freedos(const char* mount_dir)
{
	static const char* res_name[] = { "COMMAND.COM", "KERNEL.SYS", "DISPLAY.EXE", "KEYB.EXE",
		"MODE.COM", "KEYBOARD.SYS", "KEYBRD2.SYS", "KEYBRD3.SYS", "KEYBRD4.SYS", "EGA.CPX",
		"EGA2.CPX", "EGA3.CPX", "EGA4.CPX", "EGA5.CPX", "EGA6.CPX",
		"EGA7.CPX", "EGA8.CPX", "EGA9.CPX", "EGA10.CPX", "EGA11.CPX",
		"EGA12.CPX", "EGA13.CPX", "EGA14.CPX", "EGA15.CPX", "EGA16.CPX",
		"EGA17.CPX", "EGA18.CPX" };
	char path[512], name[64];
	FILE* f;

	snprintf(path, sizeof(path), "%s/LOCALE", mount_dir);
	mkdir(path, 0755);
	for (size_t i = 0; i < ARRAYSIZE(res_name); i++) {
		const rf_resource_t* res;
		snprintf(name, sizeof(name), "freedos/%s", res_name[i]);
		res = rf_resource(name);
		if (res == NULL) {
			uprintf("Missing embedded FreeDOS file '%s'", res_name[i]);
			return false;
		}
		snprintf(path, sizeof(path), "%s/%s%s", mount_dir, (i < 2) ? "" : "LOCALE/", res_name[i]);
		if (!rf_write_file(path, res->data, res->size))
			return false;
		if (i < 2)
			set_fat_attributes(path, ATTR_HIDDEN | ATTR_SYS);
		uprintf("Successfully wrote '%s' (%zu bytes)", path, res->size);
		rf_progress(OP_FILE_COPY, (double)(i + 1) / ARRAYSIZE(res_name), "Copying FreeDOS files...");
	}
	/* US keyboard, US codepage */
	snprintf(path, sizeof(path), "%s/AUTOEXEC.BAT", mount_dir);
	f = fopen(path, "w");
	if (f == NULL) {
		uprintf("Unable to create 'AUTOEXEC.BAT': %s", strerror(errno));
		return false;
	}
	fprintf(f, "@echo off\r\n");
	fprintf(f, "set PATH=.;\\;\\LOCALE\r\n");
	fprintf(f, "echo Using US keyboard with US codepage [437]\r\n");
	fclose(f);
	uprintf("Successfully wrote 'AUTOEXEC.BAT'");
	return true;
}
