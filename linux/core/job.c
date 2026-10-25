/*
 * Rufus for Linux: option defaults/validation and the main write job
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
 *
 * The defaults follow the logic of Windows Rufus' SetPartitionSchemeAndTargetSystem(),
 * SetFSFromISO() and the sequence of its FormatThread().
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
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>

#include "internal.h"

#define MIN_PERSISTENCE_SIZE    (512 * MB)

void rf_options_init(rf_options_t* opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->boot_type = BT_NON_BOOTABLE;
	opts->image_mode = IM_ISO;
	opts->partition_style = PS_MBR;
	opts->target_type = TT_BIOS;
	opts->fs = FS_FAT32;
	opts->quick_format = true;
	opts->use_rufus_mbr = true;
	opts->unattend_flags = 0;
}

void rf_sanitize_label(char* label, rf_fs_t fs, uint64_t disk_size)
{
	static const char unauthorized[] = "*?,;:/\\|+=<>[]\"";
	char out[64];
	size_t i, k = 0, max_len, underscores = 0;
	bool fat = IS_FAT(fs);

	for (i = 0; label[i] != 0 && k < sizeof(out) - 1; i++) {
		unsigned char c = (unsigned char)label[i];
		if (fat) {
			if (strchr(unauthorized, c) != NULL)
				continue;
			/* A FAT label that contains extended chars will be rejected */
			if (c >= 0x80) {
				/* Skip UTF-8 continuation bytes, replace the char with a single '_' */
				if ((c & 0xc0) != 0x80)
					out[k++] = '_';
				continue;
			}
		}
		if (c == '\t' || c == '.') {
			out[k++] = '_';
			continue;
		}
		out[k++] = fat ? (char)toupper(c) : (char)c;
	}
	out[k] = 0;

	switch (fs) {
	case FS_FAT16:
	case FS_FAT32: max_len = 11; break;
	case FS_EXFAT: max_len = 11; break;
	case FS_EXT2:
	case FS_EXT3:
	case FS_EXT4: max_len = 16; break;
	default: max_len = 32; break;
	}
	if (strlen(out) > max_len) {
		out[max_len] = 0;
		/* Don't cut a UTF-8 sequence in half */
		for (size_t j = max_len; j > 0 && ((unsigned char)out[j - 1] & 0xc0) == 0x80; j--)
			out[j - 1] = 0;
		if (max_len > 0 && (unsigned char)out[strlen(out) - 1] >= 0xc0)
			out[strlen(out) - 1] = 0;
	}
	if (fat) {
		for (i = 0; out[i] != 0; i++)
			if (out[i] == '_')
				underscores++;
		if (out[0] == 0 || i < 2 * underscores) {
			/* Mostly underscores: use the size instead (e.g. "7_9 GB") */
			snprintf(out, sizeof(out), "%s", rf_size_to_human(disk_size, true));
			for (i = 0; out[i] != 0; i++)
				if (out[i] == '.')
					out[i] = '_';
		}
	}
	snprintf(label, 64, "%s", out);
}

void rf_default_label(const rf_drive_t* drive, char* out, size_t out_size)
{
	if (drive != NULL && drive->label[0] != 0)
		snprintf(out, out_size, "%s", drive->label);
	else if (drive != NULL)
		snprintf(out, out_size, "%s", rf_size_to_human(drive->size, true));
	else
		snprintf(out, out_size, "USB");
}

void rf_options_defaults(rf_options_t* opts, const rf_drive_t* drive, const rf_image_t* img)
{
	uint64_t disk_size = (drive != NULL) ? drive->size : 0;

	opts->persistence_size = 0;
	if (opts->boot_type == BT_IMAGE && img != NULL) {
		opts->image_mode = (IS_DD_ONLY(img) || !img->is_iso) ? IM_DD : IM_ISO;
		/* ISOHybrids whose ISO file system has no boot loaders we know of only work in DD mode */
		if (img->is_isohybrid && !IS_BIOS_BOOTABLE(img) && !IS_EFI_BOOTABLE(img))
			opts->image_mode = IM_DD;

		/* Partition scheme */
		if (!IS_EFI_BOOTABLE(img))
			opts->partition_style = PS_MBR;
		else if (HAS_WINDOWS(img) && img->has_efi)
			opts->partition_style = PS_GPT;
		else if (!IS_BIOS_BOOTABLE(img))
			opts->partition_style = PS_GPT;
		else
			opts->partition_style = PS_MBR;
		if (IS_DD_BOOTABLE(img) && !HAS_WINDOWS(img))
			opts->partition_style = PS_MBR;
		if (disk_size > 2 * TB)
			opts->partition_style = PS_GPT;

		/* Target system */
		if (opts->partition_style == PS_GPT)
			opts->target_type = TT_UEFI;
		else
			opts->target_type = IS_BIOS_BOOTABLE(img) ? TT_BIOS : TT_UEFI;

		/* File system */
		if (HAS_SYSLINUX(img) || HAS_REACTOS(img) || HAS_KOLIBRIOS(img) ||
			(IS_EFI_BOOTABLE(img) && opts->target_type == TT_UEFI && !img->has_4GB_file)) {
			opts->fs = FS_FAT32;
		} else if (HAS_BOOTMGR(img) || HAS_WINPE(img) || img->has_4GB_file) {
			opts->fs = rf_fs_available(FS_NTFS) ? FS_NTFS : FS_EXFAT;
		} else {
			opts->fs = FS_FAT32;
		}
		if (opts->fs == FS_FAT32 && img->has_4GB_file)
			opts->fs = rf_fs_available(FS_NTFS) ? FS_NTFS : FS_EXFAT;
		if (img->label[0] != 0)
			snprintf(opts->label, sizeof(opts->label), "%s", img->label);
		else
			rf_default_label(drive, opts->label, sizeof(opts->label));
	} else {
		opts->image_mode = IM_ISO;
		opts->partition_style = (disk_size > 2 * TB) ? PS_GPT : PS_MBR;
		opts->target_type = (opts->partition_style == PS_GPT) ? TT_UEFI : TT_BIOS;
		if (opts->boot_type == BT_FREEDOS) {
			opts->partition_style = PS_MBR;
			opts->target_type = TT_BIOS;
			opts->fs = (disk_size != 0 && disk_size < 512 * MB) ? FS_FAT16 : FS_FAT32;
			snprintf(opts->label, sizeof(opts->label), "FREEDOS");
		} else {
			opts->fs = (disk_size > 32 * GB && rf_fs_available(FS_EXFAT)) ? FS_EXFAT : FS_FAT32;
			rf_default_label(drive, opts->label, sizeof(opts->label));
		}
	}
	opts->cluster_size = 0;
}

const char* rf_options_validate(const rf_options_t* opts, const rf_drive_t* drive, const rf_image_t* img)
{
	static char err[512];

	if (drive == NULL || drive->size == 0)
		return "No device selected";
	if (drive->is_system)
		return "This drive hosts system partitions and cannot be used";
	if (drive->read_only)
		return "This drive is write-protected";
	if (opts->boot_type == BT_IMAGE) {
		if (img == NULL || img->path[0] == 0)
			return "No image selected";
		if (opts->image_mode == IM_DD) {
			uint64_t size = img->compression ? img->uncompressed_size : (img->is_vhd ? img->size - 512 : img->size);
			if (!img->is_bootable_img && !img->is_iso)
				return "This image is not a bootable disk image and cannot be written in DD mode";
			if (size > drive->size) {
				snprintf(err, sizeof(err), "The image (%s) is too large for the selected device (%s)",
					rf_size_to_human(size, false), rf_size_to_human(drive->size, true));
				return err;
			}
			return NULL;
		}
		if (!img->is_iso)
			return "This image can only be written in DD mode";
		if (img->disable_iso)
			return "This ISO only works when written in DD mode";
		if (img->projected_size + opts->persistence_size + 2 * MB > drive->size) {
			snprintf(err, sizeof(err), "The image content (%s) is too large for the selected device (%s)",
				rf_size_to_human(img->projected_size, false), rf_size_to_human(drive->size, true));
			return err;
		}
		if (IS_FAT(opts->fs) && img->has_4GB_file) {
			bool only_wim = true;
			if ((img->has_4GB_file & 0x0f) > ((img->has_4GB_file & 0x30) ? 1 : 0))
				only_wim = false;
			if (!only_wim || !(img->has_4GB_file & 0x30))
				return "This image contains files larger than 4 GB, which FAT32 cannot store. Use NTFS or exFAT.";
			if (!rf_which("wimlib-imagex", NULL, 0))
				return "This image has an install.wim larger than 4 GB. Use NTFS/exFAT, "
					"or install wimlib so that it can be split for FAT32.";
		}
		if (opts->target_type == TT_BIOS && HAS_SYSLINUX(img) && !IS_FAT(opts->fs) && !IS_EFI_BOOTABLE(img))
			return "Syslinux based images require FAT32 (or FAT16) for BIOS boot on Linux";
		if (opts->target_type == TT_UEFI && !IS_EFI_BOOTABLE(img))
			return "This image does not support UEFI boot. Use MBR with a BIOS target.";
		if (opts->persistence_size != 0) {
			if (!HAS_PERSISTENCE(img))
				return "This image does not support persistence";
			if (opts->persistence_size < MIN_PERSISTENCE_SIZE)
				return "The persistent partition must be at least 512 MB";
			if (!rf_fs_available(FS_EXT4))
				return "Persistence requires mkfs.ext4 (e2fsprogs)";
		}
	}
	if (opts->partition_style == PS_MBR && drive->size > 2 * TB)
		return "MBR cannot address drives larger than 2 TB. Use GPT.";
	if (opts->partition_style == PS_GPT && opts->target_type == TT_BIOS && opts->boot_type != BT_NON_BOOTABLE)
		return "GPT can only be used with UEFI targets";
	if (opts->fs == FS_FAT16 && drive->size > 4 * GB)
		return "FAT16 cannot be used on drives larger than 4 GB";
	if (!rf_fs_available(opts->fs)) {
		snprintf(err, sizeof(err), "Cannot create %s file systems: please install %s", rf_fs_name[opts->fs],
			(opts->fs == FS_NTFS) ? "ntfsprogs" : (opts->fs == FS_EXFAT) ? "exfatprogs" :
			IS_EXT(opts->fs) ? "e2fsprogs" : "dosfstools");
		return err;
	}
	if (opts->boot_type == BT_FREEDOS && !IS_FAT(opts->fs))
		return "FreeDOS requires a FAT file system";
	return NULL;
}

/* ---------------------------------------------------------------------- */
/* Job                                                                    */
/* ---------------------------------------------------------------------- */

static bool write_persistence_conf(const char* part_path, rf_fs_t fs)
{
	char mnt[256], path[512];
	const char data[] = "/ union\n";
	bool r;

	if (!rf_mount(part_path, fs, mnt, sizeof(mnt)))
		return false;
	snprintf(path, sizeof(path), "%s/persistence.conf", mnt);
	r = rf_write_file(path, data, strlen(data));
	if (r)
		uprintf("Created '/persistence.conf' on the persistent partition");
	return rf_unmount(mnt) && r;
}

static bool write_unattend(const char* mount_dir, const rf_options_t* opts, const rf_image_t* img)
{
	char path[512];
	const char* arch = "amd64";
	char* xml;
	bool r;

	if (opts->unattend_flags == 0 || !HAS_WININST(img))
		return true;
	/* Pick the architecture from the EFI bootloaders */
	if ((img->has_efi & EFI_AA64) && !(img->has_efi & EFI_X64))
		arch = "arm64";
	else if ((img->has_efi & EFI_IA32) && !(img->has_efi & EFI_X64))
		arch = "x86";
	xml = rf_create_unattend_xml(opts->unattend_flags, arch, opts->unattend_username, opts->unattend_locale,
		opts->unattend_timezone);
	if (xml == NULL)
		return false;
	if (img->has_panther_unattend)
		uprintf("Note: this image already provides an unattend.xml - ours will take precedence for setup");
	snprintf(path, sizeof(path), "%s/autounattend.xml", mount_dir);
	r = rf_write_file(path, xml, strlen(xml));
	if (r)
		uprintf("Created '/autounattend.xml'");
	free(xml);
	return r;
}

/* Recompute md5sum.txt entries for the files we modified, so that integrity checks pass */
static void update_md5sum(const char* mount_dir)
{
	static const char* names[] = { "md5sum.txt", "MD5SUMS" };
	char path[512];

	for (size_t i = 0; i < ARRAYSIZE(names); i++) {
		size_t len;
		char* data;
		FILE* out;
		snprintf(path, sizeof(path), "%s/%s", mount_dir, names[i]);
		data = rf_read_text_file(path, &len);
		if (data == NULL)
			continue;
		out = fopen(path, "w");
		if (out == NULL) {
			free(data);
			continue;
		}
		int updated = 0;
		for (char *save = NULL, *line = strtok_r(data, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save)) {
			char hash[33], file[1024], full[2048];
			if (strlen(line) > 34 && sscanf(line, "%32s %1023[^\n]", hash, file) == 2) {
				const char* f = file;
				while (*f == ' ' || *f == '*')
					f++;
				if (f[0] == '.' && f[1] == '/')
					f += 2;
				/* Only rehash config files, which are the ones we may patch */
				if (rf_str_isuffix(f, ".cfg") || rf_str_isuffix(f, ".conf")) {
					rf_hashes_t h;
					snprintf(full, sizeof(full), "%s/%s", mount_dir, f);
					if (access(full, F_OK) == 0 && rf_hash_file(full, &h) && strcmp(h.md5, hash) != 0) {
						fprintf(out, "%s  %s\n", h.md5, file);
						updated++;
						continue;
					}
				}
			}
			fprintf(out, "%s\n", line);
		}
		fclose(out);
		free(data);
		if (updated)
			uprintf("Updated %d entr%s in '%s'", updated, updated == 1 ? "y" : "ies", names[i]);
	}
}

bool rf_job_run(const rf_options_t* opts_in)
{
	rf_options_t opts = *opts_in;
	rf_drive_t drive;
	rf_image_t* img = NULL;
	rf_layout_t layout;
	char part_main[128], part_casper[128], label[64], mount_dir[256];
	uint64_t disk_size = 0;
	int ss = 512, fd = -1;
	bool bootable, add_uefi_ntfs, ok = false, inhibited = false, mounted = false;
	const char* err;
	uint64_t start = rf_monotonic_ms();

	if (geteuid() != 0) {
		uprintf("Rufus must run as root to write to drives");
		return false;
	}
	if (!rf_drive_get(opts.device, &drive)) {
		uprintf("'%s' is not a valid disk device", opts.device);
		return false;
	}
	uprintf("Target: %s", drive.display);

	if (opts.boot_type == BT_IMAGE) {
		img = calloc(1, sizeof(*img));
		if (img == NULL || !rf_image_analyze(opts.image, img))
			goto out;
		if (rf_drive_hosts_file(drive.path, opts.image)) {
			uprintf("The image is located on the target drive!");
			goto out;
		}
	}
	err = rf_options_validate(&opts, &drive, img);
	if (err != NULL) {
		uprintf("ERROR: %s", err);
		goto out;
	}

	rf_drive_inhibit_automount(drive.name, true);
	inhibited = true;
	if (!rf_drive_unmount_all(drive.path)) {
		uprintf("Could not unmount all the partitions of %s", drive.path);
		goto out;
	}
	/* Make sure nobody else holds the drive, then work on a regular handle */
	fd = open(drive.path, O_RDWR | O_EXCL | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open %s for exclusive access: %s", drive.path, strerror(errno));
		if (errno == EBUSY)
			uprintf("The drive is in use by another process (mounted, LVM/LUKS, or another application)");
		goto out;
	}
	close(fd);
	fd = open(drive.path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		uprintf("Could not open %s: %s", drive.path, strerror(errno));
		goto out;
	}
	if (ioctl(fd, BLKGETSIZE64, &disk_size) != 0 || disk_size == 0) {
		uprintf("Could not get the size of %s", drive.path);
		goto out;
	}
	if (ioctl(fd, BLKSSZGET, &ss) != 0 || ss < 512)
		ss = 512;
	uprintf("Drive size: %s, sector size: %d", rf_size_to_human(disk_size, false), ss);

	if (opts.badblocks_passes > 0) {
		uint64_t bad = 0;
		if (!rf_badblocks_check(fd, disk_size, opts.badblocks_passes, &bad))
			goto out;
		if (bad > 0) {
			uprintf("ERROR: %" PRIu64 " bad blocks were found. This drive is faulty or uses fake capacity.", bad);
			goto out;
		}
	}
	if (rf_cancelled())
		goto out;

	rf_progress(OP_ZERO, -1.0, "Clearing partition tables...");
	if (!rf_clear_partition_tables(fd, disk_size, (uint32_t)ss))
		goto out;

	/* DD mode */
	if (opts.boot_type == BT_IMAGE && opts.image_mode == IM_DD) {
		if (!rf_write_image(fd, opts.image, img, disk_size, opts.verify))
			goto out;
		rf_progress(OP_FINALIZE, -1.0, "Finalizing...");
		rf_drive_reread_partitions(fd, drive.path, 0);
		ok = true;
		goto out;
	}

	/* Partition */
	bootable = (opts.boot_type != BT_NON_BOOTABLE);
	add_uefi_ntfs = (opts.boot_type == BT_IMAGE && IS_EFI_BOOTABLE(img) && (opts.fs == FS_NTFS || opts.fs == FS_EXFAT));
	if (add_uefi_ntfs && HAS_BOOTMGR_BIOS(img) && opts.target_type == TT_BIOS)
		add_uefi_ntfs = false;
	if (opts.persistence_size != 0 && !(img != NULL && HAS_PERSISTENCE(img)))
		opts.persistence_size = 0;

	rf_progress(OP_PARTITION, -1.0, "Creating partitions...");
	if (!rf_create_partitions(fd, disk_size, (uint32_t)ss, &opts, opts.fs, bootable, add_uefi_ntfs, &layout))
		goto out;
	if (add_uefi_ntfs && opts.fs == FS_EXFAT)
		uprintf("Note: The UEFI:NTFS exFAT driver is not Secure Boot signed - Secure Boot must be disabled to boot");

	rf_progress(OP_BOOTRECORD, -1.0, "Writing master boot record...");
	if (!rf_write_mbr(fd, &opts, img, (uint32_t)ss) || !rf_write_sbr(fd, &opts, img, layout.part[PI_MAIN].offset)) {
		uprintf("Could not write the master boot record");
		goto out;
	}
	if (!rf_drive_reread_partitions(fd, drive.path, layout.nb_partitions))
		goto out;
	rf_drive_partition_path(drive.path, layout.part[PI_MAIN].index, part_main, sizeof(part_main));
	if (rf_cancelled())
		goto out;

	/* Format the persistence partition first */
	if (opts.persistence_size != 0) {
		const char* plabel = img->uses_casper ? "casper-rw" : "persistence";
		rf_drive_partition_path(drive.path, layout.part[PI_CASPER].index, part_casper, sizeof(part_casper));
		uprintf("Using %s-like method to enable persistence", img->uses_casper ? "Ubuntu" : "Debian");
		if (!rf_format_partition(part_casper, FS_EXT4, plabel, 0, layout.part[PI_CASPER].size, (uint32_t)ss,
			opts.quick_format))
			goto out;
		if (!img->uses_casper && !write_persistence_conf(part_casper, FS_EXT4))
			goto out;
	}

	/* Format the main partition */
	snprintf(label, sizeof(label), "%s", opts.label);
	rf_sanitize_label(label, opts.fs, disk_size);
	if (!rf_format_partition(part_main, opts.fs, label, opts.cluster_size, layout.part[PI_MAIN].size, (uint32_t)ss,
		opts.quick_format))
		goto out;
	if (rf_cancelled())
		goto out;

	/* Partition boot record or Syslinux */
	if (bootable && opts.target_type == TT_BIOS) {
		rf_progress(OP_BOOTRECORD, -1.0, "Writing partition boot record...");
		if (opts.boot_type == BT_IMAGE && (HAS_SYSLINUX(img) || HAS_REACTOS(img)) && !IS_FAT(opts.fs)) {
			uprintf("WARNING: Syslinux can only be installed on FAT by Rufus for Linux. "
				"This drive will boot in UEFI mode only.");
		} else if (opts.boot_type == BT_SYSLINUX_V4 || opts.boot_type == BT_SYSLINUX_V6 ||
			(opts.boot_type == BT_IMAGE && (HAS_SYSLINUX(img) || HAS_REACTOS(img)))) {
			if (!rf_install_syslinux(part_main, NULL, &opts, img, (uint32_t)ss))
				goto out;
		} else if (!rf_write_pbr(part_main, opts.fs, &opts, img, (uint32_t)ss, layout.part[PI_MAIN].offset)) {
			goto out;
		}
	}
	if (rf_cancelled())
		goto out;

	/* Copy files */
	if (opts.boot_type == BT_FREEDOS || opts.boot_type == BT_IMAGE) {
		if (!rf_mount(part_main, opts.fs, mount_dir, sizeof(mount_dir)))
			goto out;
		mounted = true;
		if (opts.boot_type == BT_FREEDOS) {
			rf_progress(OP_FILE_COPY, 0.0, "Copying FreeDOS files...");
			if (!rf_install_freedos(mount_dir))
				goto out;
		} else {
			rf_extract_opts_t xo = {
				.img = img,
				.usb_label = label,
				.persistence_size = opts.persistence_size,
				.fat_target = IS_FAT(opts.fs),
				.skip_root_ldlinux_sys = HAS_SYSLINUX(img),
			};
			rf_progress(OP_FILE_COPY, 0.0, "Copying ISO files...");
			if (!rf_iso_extract(opts.image, mount_dir, &xo))
				goto out;
			if (HAS_KOLIBRIOS(img)) {
				char src[512], dst[512];
				snprintf(src, sizeof(src), "%s/HD_Load/USB_Boot/MTLD_F32", mount_dir);
				snprintf(dst, sizeof(dst), "%s/MTLD_F32", mount_dir);
				uprintf("Installing: %s (KolibriOS loader)", dst);
				if (!rf_copy_file(src, dst))
					uprintf("WARNING: Loader installation failed - KolibriOS will not boot!");
			}
			if (opts.target_type == TT_UEFI && HAS_WIN7_EFI(img))
				uprintf("WARNING: Windows 7 UEFI boot requires extracting bootmgfw.efi from install.wim, "
					"which is not supported yet. Use a BIOS target for this image.");
			if (opts.target_type == TT_BIOS && HAS_WINPE(img))
				uprintf("WARNING: Windows XP/WinPE 1.x setup patching is not supported - BIOS boot may fail");
			if (!write_unattend(mount_dir, &opts, img))
				goto out;
			if (img->has_md5sum)
				update_md5sum(mount_dir);
		}
		if (!rf_unmount(mount_dir))
			goto out;
		mounted = false;
	}

	rf_progress(OP_FINALIZE, -1.0, "Finalizing...");
	fsync(fd);
	ok = true;

out:
	if (mounted)
		rf_unmount(mount_dir);
	if (fd >= 0) {
		if (ok)
			rf_drive_reread_partitions(fd, drive.path, 0);
		close(fd);
	}
	if (inhibited)
		rf_drive_inhibit_automount(drive.name, false);
	free(img);
	if (rf_cancelled()) {
		uprintf("Operation cancelled by the user");
		ok = false;
	}
	if (ok) {
		uint64_t secs = (rf_monotonic_ms() - start) / 1000;
		uprintf("Operation completed successfully in %d:%02d", (int)(secs / 60), (int)(secs % 60));
		rf_progress(OP_FINALIZE, 1.0, "READY");
	}
	return ok;
}
