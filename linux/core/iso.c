/*
 * Rufus for Linux: ISO image scanning and extraction
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
 *
 * Scanning uses libcdio (so that it can run unprivileged), and mirrors the
 * detection logic of Windows Rufus' iso.c. Extraction loop-mounts the image
 * through the kernel, which handles ISO9660/Joliet/Rock Ridge/UDF for us.
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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cdio/cdio.h>
#include <cdio/logging.h>
#include <cdio/iso9660.h>
#include <cdio/udf.h>

#include "internal.h"

#define ISO_BLOCKSIZE       2048
#define COPY_BUFFER_SIZE    (4 * MB)

static const char* bootmgr_name = "bootmgr";
static const char* bootmgr_efi_name = "bootmgr.efi";
static const char* grldr_name = "grldr";
static const char* ldlinux_name = "ldlinux.sys";
static const char* ldlinux_c32 = "ldlinux.c32";
static const char* casper_dirname = "/casper";
static const char* proxmox_dirname = "/proxmox";
static const char* efi_dirname = "/efi/boot";
static const char* sources_str = "/sources";
static const char* wininst_name[] = { "install.wim", "install.esd", "install.swm" };
static const char* grub_dirname[] = { "/boot/grub/i386-pc", "/boot/grub2/i386-pc" };
static const char* grub_cfg[] = { "grub.cfg", "loopback.cfg" };
static const char* menu_cfg = "menu.cfg";
static const char* syslinux_cfg[] = { "isolinux.cfg", "syslinux.cfg", "extlinux.conf", "txt.cfg", "live.cfg" };
static const char* isolinux_bin[] = { "isolinux.bin", "boot.bin" };
static const char* pe_dirname[] = { "/i386", "/amd64", "/minint" };
static const char* pe_file[] = { "ntdetect.com", "setupldr.bin", "txtsetup.sif" };
static const char* reactos_name[] = { "setupldr.sys", "freeldr.sys" };
static const char* kolibri_name = "kolibri.img";
static const char* manjaro_marker = ".miso";
static const char* pop_os_name = "pop-os";
static const char* md5sum_name[] = { "md5sum.txt", "MD5SUMS", "md5sum_ext.txt" };
static const char* efi_archname[] = { "ia32", "x64", "arm", "aa64", "ia64", "riscv64", "loongarch64" };
static const char* efi_bootname[] = { "boot", "grub", "mm" };
static const char* suse_derivative[] = { "Install-SUSE", "Install-LEAP", "openSUSE-Tumbleweed" };

/* ---------------------------------------------------------------------- */
/* Scanning                                                               */
/* ---------------------------------------------------------------------- */

#define MAX_LIST 64
typedef struct {
	char* s[MAX_LIST];
	int n;
} strlist_t;

static void list_add(strlist_t* l, const char* s)
{
	for (int i = 0; i < l->n; i++)
		if (strcmp(l->s[i], s) == 0)
			return;
	if (l->n < MAX_LIST)
		l->s[l->n++] = strdup(s);
}

static void list_free(strlist_t* l)
{
	for (int i = 0; i < l->n; i++)
		free(l->s[i]);
	l->n = 0;
}

typedef struct {
	rf_image_t* img;
	strlist_t config_path;
	strlist_t isolinux_path;
	strlist_t grub_efi_path;
	uint64_t total_blocks;
} scan_ctx_t;

static void scan_entry(scan_ctx_t* ctx, const char* dirname, const char* basename, const char* fullpath,
	uint64_t file_length, bool is_dir)
{
	rf_image_t* img = ctx->img;
	char bootloader_name[32];
	size_t i, j, k;

	if (is_dir) {
		/* Check for a '/casper#####' directory */
		if (rf_str_iprefix(fullpath, casper_dirname)) {
			img->uses_casper = true;
			if (strstr(fullpath, pop_os_name) != NULL)
				img->disable_iso = true;
		}
		if (rf_str_iequal(fullpath, proxmox_dirname))
			img->disable_iso = true;
		if (rf_str_iequal(fullpath, "/live"))
			img->uses_debian_live = true;
		for (i = 0; i < ARRAYSIZE(grub_dirname); i++)
			if (rf_str_iequal(fullpath, grub_dirname[i]))
				img->has_grub2 = (uint8_t)((img->has_grub2 & 0x80) | (i + 1));
		return;
	}

	/* Check for an isolinux/syslinux config file anywhere */
	for (i = 0; i < ARRAYSIZE(syslinux_cfg); i++) {
		if (rf_str_iequal(basename, syslinux_cfg[i])) {
			img->has_syslinux_cfg = true;
			if (i < 3)
				list_add(&ctx->config_path, fullpath);
			if (i == 1 && rf_str_iequal(dirname, efi_dirname))
				img->has_efi_syslinux = true;
		}
	}

	/* GRUB artifacts */
	for (i = 0; i < ARRAYSIZE(grub_dirname); i++)
		if (rf_str_iequal(dirname, grub_dirname[i]))
			img->has_grub2 = (uint8_t)((img->has_grub2 & 0x80) | (i + 1));

	if (rf_str_iequal(basename, ldlinux_c32))
		img->has_ldlinux_c32 = true;

	if (rf_str_iprefix(dirname, casper_dirname)) {
		img->uses_casper = true;
		if (strstr(dirname, pop_os_name) != NULL)
			img->disable_iso = true;
	}
	if (rf_str_iequal(dirname, proxmox_dirname))
		img->disable_iso = true;
	if (rf_str_iequal(dirname, "/live") && rf_str_iprefix(basename, "filesystem.squashfs"))
		img->uses_debian_live = true;

	/* Root directory files */
	if (dirname[0] == 0) {
		if (rf_str_iequal(basename, bootmgr_name))
			img->has_bootmgr = true;
		if (rf_str_iequal(basename, bootmgr_efi_name)) {
			img->has_efi |= EFI_BOOTMGR;
			img->has_bootmgr_efi = true;
		}
		if (rf_str_iequal(basename, grldr_name))
			img->has_grub4dos = true;
		if (rf_str_iequal(basename, kolibri_name))
			img->has_kolibrios = true;
		if (rf_str_iequal(basename, manjaro_marker))
			img->disable_iso = true;
		for (i = 0; i < ARRAYSIZE(md5sum_name); i++)
			if (rf_str_iequal(basename, md5sum_name[i]))
				img->has_md5sum = true;
	}

	/* ReactOS */
	if (img->reactos_path[0] == 0) {
		for (i = 0; i < ARRAYSIZE(reactos_name); i++)
			if (rf_str_iequal(basename, reactos_name[i]))
				snprintf(img->reactos_path, sizeof(img->reactos_path), "%s", fullpath);
	}

	/* The first 'efi*.img' we find (that hopefully contains EFI boot files) */
	if (img->efi_img_path[0] == 0 && strlen(basename) >= 7 && rf_str_iprefix(basename, "efi") &&
		rf_str_isuffix(basename, ".img"))
		snprintf(img->efi_img_path, sizeof(img->efi_img_path), "%s", fullpath);

	/* EFI bootloaders */
	if (rf_str_iprefix(dirname, "/efi/")) {
		for (k = 0; k < ARRAYSIZE(efi_bootname); k++) {
			for (i = 0; i < ARRAYSIZE(efi_archname); i++) {
				snprintf(bootloader_name, sizeof(bootloader_name), "%s%s.efi", efi_bootname[k], efi_archname[i]);
				if (!rf_str_iequal(basename, bootloader_name))
					continue;
				if (k == 0)
					img->has_efi |= (uint16_t)(2 << i);
				else if (k == 1)
					list_add(&ctx->grub_efi_path, fullpath);
			}
		}
		/* Broken symbolic link for bootx64.efi (Linux Mint) */
		if (rf_str_iequal(basename, "bootx64.efi") && file_length < 256)
			img->has_efi |= 0x4000;
	}

	/* Windows installers */
	if (rf_str_isuffix(dirname, sources_str)) {
		for (i = 0; i < ARRAYSIZE(wininst_name); i++) {
			if (rf_str_iequal(basename, wininst_name[i]) && img->wininst_index < MAX_WININST) {
				snprintf(img->wininst_path[img->wininst_index], sizeof(img->wininst_path[0]), "%s", fullpath);
				img->wininst_index++;
				if (file_length >= 4 * GB)
					img->has_4GB_file |= (uint8_t)(0x10 << i);
			}
		}
	}
	if (rf_str_iequal(dirname, "/sources/$OEM$/$$/Panther") && rf_str_iequal(basename, "unattend.xml"))
		img->has_panther_unattend = true;

	/* PE (XP) specific files */
	for (i = 0; i < ARRAYSIZE(pe_dirname); i++)
		if (rf_str_iequal(dirname, pe_dirname[i]))
			for (j = 0; j < ARRAYSIZE(pe_file); j++)
				if (rf_str_iequal(basename, pe_file[j]))
					img->winpe |= (uint16_t)((1 << j) << (ARRAYSIZE(pe_dirname) * i));

	for (i = 0; i < ARRAYSIZE(isolinux_bin); i++)
		if (rf_str_iequal(basename, isolinux_bin[i]))
			list_add(&ctx->isolinux_path, fullpath);

	if (file_length >= 4 * GB && (img->has_4GB_file & 0x0f) != 0x0f)
		img->has_4GB_file++;
	if (file_length != 0)
		ctx->total_blocks += (file_length + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE;
}

/* ISO9660 directory walk */
static void scan_iso9660_dir(scan_ctx_t* ctx, iso9660_t* p_iso, const char* path, uint8_t joliet, int depth)
{
	CdioList_t* list;
	CdioListNode_t* node;
	char name[4096], fullpath[8192];

	if (depth > 32)
		return;
	list = iso9660_ifs_readdir(p_iso, (path[0] == 0) ? "/" : path);
	if (list == NULL)
		return;
	_CDIO_LIST_FOREACH(node, list) {
		iso9660_stat_t* st = (iso9660_stat_t*)_cdio_list_node_data(node);
		if (strcmp(st->filename, ".") == 0 || strcmp(st->filename, "..") == 0)
			continue;
		if (st->rr.b3_rock != yep)
			iso9660_name_translate_ext(st->filename, name, joliet);
		else
			snprintf(name, sizeof(name), "%s", st->filename);
		if (st->rr.psz_symlink != NULL && st->rr.i_symlink > 0)
			ctx->img->has_symlinks = true;
		snprintf(fullpath, sizeof(fullpath), "%s/%s", path, name);
		if (st->type == _STAT_DIR) {
			scan_entry(ctx, path, name, fullpath, 0, true);
			scan_iso9660_dir(ctx, p_iso, fullpath, joliet, depth + 1);
		} else {
			scan_entry(ctx, path, name, fullpath, st->total_size, false);
		}
	}
	iso9660_filelist_free(list);
}

/* UDF directory walk. Consumes p_dirent. */
static void scan_udf_dir(scan_ctx_t* ctx, udf_dirent_t* p_dirent, const char* path, int depth)
{
	char fullpath[8192];

	if (depth > 32) {
		udf_dirent_free(p_dirent);
		return;
	}
	while ((p_dirent = udf_readdir(p_dirent)) != NULL) {
		const char* name = udf_get_filename(p_dirent);
		if (name == NULL || name[0] == 0)
			continue;
		snprintf(fullpath, sizeof(fullpath), "%s/%s", path, name);
		if (udf_is_dir(p_dirent)) {
			udf_dirent_t* sub;
			scan_entry(ctx, path, name, fullpath, 0, true);
			sub = udf_opendir(p_dirent);
			if (sub != NULL)
				scan_udf_dir(ctx, sub, fullpath, depth + 1);
		} else {
			scan_entry(ctx, path, name, fullpath, udf_get_file_length(p_dirent), false);
		}
	}
}

static void cdio_log_handler(cdio_log_level_t level, const char* message)
{
	if (level >= CDIO_LOG_WARN)
		uprintf("  libcdio: %s", message);
}

size_t rf_iso_read_file(const char* iso, const char* iso_path, uint8_t** buf)
{
	iso9660_t* p_iso = NULL;
	udf_t* p_udf = NULL;
	udf_dirent_t *p_root = NULL, *p_file = NULL;
	iso9660_stat_t* st = NULL;
	uint64_t length = 0;
	size_t nblocks, r = 0;

	*buf = NULL;
	p_udf = udf_open(iso);
	if (p_udf != NULL) {
		p_root = udf_get_root(p_udf, true, 0);
		if (p_root == NULL)
			goto try_iso;
		p_file = udf_fopen(p_root, iso_path);
		if (p_file == NULL)
			goto out;
		length = udf_get_file_length(p_file);
		if (length == 0 || length > 64 * MB)
			goto out;
		nblocks = (size_t)((length + UDF_BLOCKSIZE - 1) / UDF_BLOCKSIZE);
		*buf = calloc(nblocks + 1, UDF_BLOCKSIZE);
		if (*buf == NULL)
			goto out;
		for (size_t done = 0; done < nblocks;) {
			size_t chunk = MIN((size_t)32, nblocks - done);
			ssize_t rd = udf_read_block(p_file, &(*buf)[done * UDF_BLOCKSIZE], chunk);
			if (rd <= 0) {
				free(*buf);
				*buf = NULL;
				goto out;
			}
			done += chunk;
		}
		r = (size_t)length;
		goto out;
	}

try_iso:
	p_iso = iso9660_open_ext(iso, ISO_EXTENSION_ALL);
	if (p_iso == NULL)
		goto out;
	st = iso9660_ifs_stat_translate(p_iso, iso_path);
	if (st == NULL)
		goto out;
	length = st->total_size;
	if (length == 0 || length > 64 * MB)
		goto out;
	nblocks = (size_t)((length + ISO_BLOCKSIZE - 1) / ISO_BLOCKSIZE);
	*buf = calloc(nblocks + 1, ISO_BLOCKSIZE);
	if (*buf == NULL)
		goto out;
	/* libcdio limits the size of a single read, so go in chunks */
	for (size_t done = 0; done < nblocks;) {
		long chunk = (long)MIN((size_t)32, nblocks - done);
		if (iso9660_iso_seek_read(p_iso, &(*buf)[done * ISO_BLOCKSIZE], st->lsn + (lsn_t)done, chunk) !=
			chunk * ISO_BLOCKSIZE) {
			free(*buf);
			*buf = NULL;
			goto out;
		}
		done += (size_t)chunk;
	}
	r = (size_t)length;

out:
	if (st != NULL)
		iso9660_stat_free(st);
	if (p_file != NULL)
		udf_dirent_free(p_file);
	if (p_root != NULL)
		udf_dirent_free(p_root);
	if (p_udf != NULL)
		udf_close(p_udf);
	if (p_iso != NULL)
		iso9660_close(p_iso);
	return r;
}

uint16_t rf_get_syslinux_version(char* buf, size_t buf_size, char** ext)
{
	size_t i, j, k;
	char* p = NULL;
	unsigned long version_ul[2];
	uint16_t version = 0;
	const char LINUX[] = { 'L', 'I', 'N', 'U', 'X', ' ' };
	static char nullstr[] = "";
	char unauthorized[] = { '<', '>', ':', '|', '*', '?', '\\', '/' };

	*ext = nullstr;
	if (buf_size < 256)
		return 0;

	/* Start at 64 to avoid the short incomplete version at the beginning of ldlinux.sys */
	for (i = 64; i < buf_size - 64; i++) {
		if (memcmp(&buf[i], LINUX, sizeof(LINUX)) != 0)
			continue;
		/* Check for ISO or SYS prefix */
		if (!(((buf[i - 3] == 'I') && (buf[i - 2] == 'S') && (buf[i - 1] == 'O'))
			|| ((buf[i - 3] == 'S') && (buf[i - 2] == 'Y') && (buf[i - 1] == 'S'))))
			continue;
		i += sizeof(LINUX);
		version_ul[0] = strtoul(&buf[i], &p, 10);
		if (version_ul[0] < 256) {
			version_ul[1] = strtoul(&p[1], &p, 10);
			if (version_ul[1] < 256)
				version = (uint16_t)((version_ul[0] << 8) + version_ul[1]);
		}
		if (version == 0)
			continue;
		/* Ensure that our extra version string starts with a slash */
		*p = '/';
		/* Remove the x.yz- duplicate if present */
		for (j = 0; (buf[i + j] == p[1 + j]) && (buf[i + j] != ' '); j++);
		if (p[j + 1] == '-')
			j++;
		if (j >= 4) {
			p[j] = '/';
			p = &p[j];
		}
		for (j = strlen(p) - 1; j > 0; j--) {
			/* Arch Linux affixes a star for their version */
			if ((p[j] == ' ') || (p[j] == '*'))
				p[j] = 0;
			else
				break;
		}
		/* Sanitize the string */
		for (j = 1; j < strlen(p); j++)
			for (k = 0; k < sizeof(unauthorized); k++)
				if (p[j] == unauthorized[k])
					p[j] = '_';
		*ext = (p[1] == 0) ? nullstr : p;
		return version;
	}
	return 0;
}

static void get_grub_version(rf_image_t* img, char* buf, size_t buf_size, const char* source)
{
	const char* grub_version_str[] = { "GRUB  version %s", "GRUB version %s" };
	const char* grub_debug_is_enabled_str = "grub_debug_is_enabled";
	const size_t max_string_size = 32;
	char grub_version[96] = { 0 };
	bool has_grub_debug_is_enabled = false;
	size_t i, j;

	if (buf_size > max_string_size) {
		for (i = 0; i < buf_size - max_string_size; i++) {
			for (j = 0; j < ARRAYSIZE(grub_version_str); j++) {
				if (memcmp(&buf[i], grub_version_str[j], strlen(grub_version_str[j])) == 0) {
					/* For CentOS, who decided to add a '\n' after "GRUB  version %s" */
					if (buf[i + strlen(grub_version_str[j]) + 1] == '\0')
						i++;
					snprintf(grub_version, sizeof(grub_version), "%s", &buf[i + strlen(grub_version_str[j]) + 1]);
				}
			}
			if (memcmp(&buf[i], grub_debug_is_enabled_str, strlen(grub_debug_is_enabled_str)) == 0)
				has_grub_debug_is_enabled = true;
		}
	}
	uprintf("  Detected GRUB version: %s (from '%s')", grub_version, source);
	if (img->grub2_version[0] != 0)
		return;
	snprintf(img->grub2_version, sizeof(img->grub2_version), "%s", grub_version);
	if (img->grub2_version[0] == '0')
		img->grub2_version[0] = 0;
	if (img->grub2_version[0] != 0) {
		/* Same suffixes as the ones used on the Rufus server */
		bool append_label = (strcmp(img->grub2_version, "2.06") == 0);
		size_t len = strlen(img->grub2_version), sz = sizeof(img->grub2_version);
		if ((img->has_grub2 & 0x7f) > 1 && len < sz)
			len += (size_t)snprintf(&img->grub2_version[len], sz - len, "-nonstandard");
		if (has_grub_debug_is_enabled && len < sz)
			len += (size_t)snprintf(&img->grub2_version[len], sz - len, "-gdie");
		if (append_label && len < sz)
			snprintf(&img->grub2_version[len], sz - len, "-%s", img->label);
		/* Sanitize */
		for (char* p = img->grub2_version; *p; p++)
			if (*p == ' ' || *p == '/' || *p == '\\' || *p == ':' || *p == '*' || *p == '?' || *p == '"' ||
				*p == '<' || *p == '>' || *p == '|')
				*p = '_';
	}
}

static bool match_rh8(const char* label)
{
	/* Equivalent of the cregex patterns used by Windows Rufus */
	if (strncmp(label, "AlmaLinux-", 10) == 0 && (label[10] == '8' || label[10] == '9'))
		return true;
	if (strncmp(label, "Fedora", 6) == 0) {
		const char* p = strstr(label, "-3");
		if (p != NULL && p[2] >= '3' && p[2] <= '9')
			return true;
	}
	if (strncmp(label, "CentOS", 6) == 0 || strncmp(label, "RHEL-", 5) == 0 ||
		strncmp(label, "Rocky-", 6) == 0 || strncmp(label, "OL-", 3) == 0 ||
		strncmp(label, "EL-", 3) == 0) {
		for (const char* p = strchr(label, '-'); p != NULL; p = strchr(p + 1, '-'))
			if (p[1] == '8' || p[1] == '9')
				return true;
	}
	return false;
}

bool rf_iso_scan(const char* path, rf_image_t* img)
{
	scan_ctx_t ctx = { .img = img };
	iso9660_t* p_iso = NULL;
	udf_t* p_udf = NULL;
	udf_dirent_t* p_root;
	char* volid = NULL;
	uint8_t* buf;
	size_t size;
	char* ext;
	int sl_index = 0;
	bool r = false;

	cdio_log_set_handler(cdio_log_handler);

	/* Try UDF first, as Windows ISOs only carry a stub ISO9660 filesystem */
	p_udf = udf_open(path);
	if (p_udf != NULL) {
		p_root = udf_get_root(p_udf, true, 0);
		if (p_root != NULL) {
			char vol[64] = "";
			img->is_udf = true;
			if (udf_get_logical_volume_id(p_udf, vol, sizeof(vol)) > 0)
				snprintf(img->label, sizeof(img->label), "%s", vol);
			scan_udf_dir(&ctx, p_root, "", 0);
			r = true;
		}
		udf_close(p_udf);
	}

	if (!r) {
		uint8_t joliet;
		/* Prefer Rock Ridge over Joliet when both are present, like Windows Rufus */
		p_iso = iso9660_open_ext(path, ISO_EXTENSION_ALL & ~ISO_EXTENSION_JOLIET);
		if (p_iso == NULL)
			return false;
		{
			bool has_rr = false;
			CdioList_t* list = iso9660_ifs_readdir(p_iso, "/");
			CdioListNode_t* node;
			if (list != NULL) {
				_CDIO_LIST_FOREACH(node, list) {
					iso9660_stat_t* st = (iso9660_stat_t*)_cdio_list_node_data(node);
					if (st->rr.b3_rock == yep) {
						has_rr = true;
						break;
					}
				}
				iso9660_filelist_free(list);
			}
			if (!has_rr) {
				iso9660_close(p_iso);
				p_iso = iso9660_open_ext(path, ISO_EXTENSION_ALL);
				if (p_iso == NULL)
					return false;
			}
		}
		joliet = iso9660_ifs_get_joliet_level(p_iso);
		if (iso9660_ifs_get_volume_id(p_iso, &volid) && volid != NULL) {
			snprintf(img->label, sizeof(img->label), "%s", volid);
			free(volid);
		}
		scan_iso9660_dir(&ctx, p_iso, "", joliet, 0);
		iso9660_close(p_iso);
		r = true;
	}

	/* Remove trailing spaces from the label */
	for (int k = (int)strlen(img->label) - 1; k > 0 && isspace((unsigned char)img->label[k]); k--)
		img->label[k] = 0;

	/* 1% extra for the larger cluster sizes we're likely to use */
	img->projected_size = (uint64_t)((double)ctx.total_blocks * ISO_BLOCKSIZE * 1.01);

	/* If multiple config files exist, choose the one with the shortest path */
	if (ctx.config_path.n > 0) {
		memset(img->cfg_path, '_', sizeof(img->cfg_path) - 1);
		img->cfg_path[sizeof(img->cfg_path) - 1] = 0;
		for (int i = 0; i < ctx.config_path.n; i++) {
			if (rf_str_iequal(ctx.config_path.s[i], "/boot/i386/loader/isolinux.cfg") ||
				rf_str_iequal(ctx.config_path.s[i], "/boot/x86_64/loader/isolinux.cfg")) {
				snprintf(img->cfg_path, sizeof(img->cfg_path), "%s", ctx.config_path.s[i]);
				break;
			}
			if (strlen(img->cfg_path) >= strlen(ctx.config_path.s[i]))
				snprintf(img->cfg_path, sizeof(img->cfg_path), "%s", ctx.config_path.s[i]);
		}
		uprintf("  Will use '%s' for Syslinux", img->cfg_path);
		for (int i = 0; i < ctx.isolinux_path.n; i++) {
			uint16_t sl_version;
			size = rf_iso_read_file(path, ctx.isolinux_path.s[i], &buf);
			if (size == 0) {
				uprintf("  Could not access %s", ctx.isolinux_path.s[i]);
				continue;
			}
			sl_version = rf_get_syslinux_version((char*)buf, size, &ext);
			if (img->sl_version == 0) {
				snprintf(img->sl_version_ext, sizeof(img->sl_version_ext), "%s", ext);
				img->sl_version = sl_version;
				sl_index = i;
			} else if (img->sl_version != sl_version || strcmp(img->sl_version_ext, ext) != 0) {
				/* Prefer the one that resides in the same directory as the config file */
				size_t j;
				for (j = strlen(img->cfg_path); j > 0 && img->cfg_path[j] != '/'; j--);
				if (strncasecmp(img->cfg_path, ctx.isolinux_path.s[i], j) == 0) {
					snprintf(img->sl_version_ext, sizeof(img->sl_version_ext), "%s", ext);
					img->sl_version = sl_version;
					sl_index = i;
				}
			}
			free(buf);
		}
		if (img->sl_version != 0) {
			snprintf(img->sl_version_str, sizeof(img->sl_version_str), "%d.%02d",
				SL_MAJOR(img->sl_version), SL_MINOR(img->sl_version));
			uprintf("  Detected Syslinux version: %s%s (from '%s')", img->sl_version_str, img->sl_version_ext,
				ctx.isolinux_path.n > 0 ? ctx.isolinux_path.s[sl_index] : "?");
		} else {
			img->sl_version = embedded_sl_version[img->has_ldlinux_c32 ? 1 : 0];
			snprintf(img->sl_version_str, sizeof(img->sl_version_str), "%d.%02d",
				SL_MAJOR(img->sl_version), SL_MINOR(img->sl_version));
			uprintf("  Warning: Could not detect Isolinux version - Forcing to %s (embedded)", img->sl_version_str);
		}
	}

	/* WinPE 1.x: check whether /minint is used */
	if (HAS_WINPE(img)) {
		const char* basedir = ((img->winpe & 0x7) == 0x7) ? "i386" : (((img->winpe & 0x38) == 0x38) ? "amd64" : "minint");
		char sif[64];
		snprintf(sif, sizeof(sif), "/%s/txtsetup.sif", basedir);
		size = rf_iso_read_file(path, sif, &buf);
		if (size != 0) {
			for (size_t i = 0; i < size; i++)
				buf[i] = (uint8_t)tolower(buf[i]);
			img->uses_minint = (strstr((char*)buf, "/minint") != NULL);
			free(buf);
		}
	}

	/* GRUB version, from normal.mod */
	if (img->has_grub2 & 0x7f) {
		char grub_path[128];
		snprintf(grub_path, sizeof(grub_path), "%s/normal.mod", grub_dirname[(img->has_grub2 & 0x7f) - 1]);
		img->grub2_version[0] = 0;
		size = rf_iso_read_file(path, grub_path, &buf);
		if (size == 0) {
			uprintf("  Could not read GRUB version from '%s'", grub_path);
		} else {
			get_grub_version(img, (char*)buf, size, grub_path);
			free(buf);
		}
		if (img->grub2_version[0] == 0) {
			uprintf("  Could not detect GRUB version");
			img->has_grub2 &= 0x80;
		}
	}
	for (int i = 0; i < ctx.grub_efi_path.n; i++) {
		size = rf_iso_read_file(path, ctx.grub_efi_path.s[i], &buf);
		if (size != 0) {
			img->has_grub2 |= 0x80;
			get_grub_version(img, (char*)buf, size, ctx.grub_efi_path.s[i]);
			free(buf);
		}
	}

	if (match_rh8(img->label))
		img->rh8_derivative = true;
	for (size_t i = 0; i < ARRAYSIZE(suse_derivative); i++)
		if (strncmp(img->label, suse_derivative[i], strlen(suse_derivative[i])) == 0)
			img->disable_iso = true;

	list_free(&ctx.config_path);
	list_free(&ctx.isolinux_path);
	list_free(&ctx.grub_efi_path);
	return r;
}

/* ---------------------------------------------------------------------- */
/* Config file patching                                                   */
/* ---------------------------------------------------------------------- */

/*
 * For every line whose first token is 'token' (case insensitive),
 * replace all occurrences of 'src' with 'rep'. Returns true if modified.
 */
static bool replace_in_token_data(const char* filename, const char* token, const char* src, const char* rep)
{
	size_t len, src_len = strlen(src), rep_len = strlen(rep), tok_len = strlen(token);
	char* data;
	char* out = NULL;
	size_t out_len = 0, out_size;
	bool modified = false;

	if (src_len == 0 || strcmp(src, rep) == 0)
		return false;
	data = rf_read_text_file(filename, &len);
	if (data == NULL)
		return false;
	out_size = len * 2 + 1024;
	out = malloc(out_size);
	if (out == NULL) {
		free(data);
		return false;
	}

	for (char* line = data; line < data + len;) {
		char* eol = memchr(line, '\n', (size_t)(data + len - line));
		size_t line_len = (eol != NULL) ? (size_t)(eol - line + 1) : (size_t)(data + len - line);
		char* p = line;
		while (p < line + line_len && (*p == ' ' || *p == '\t'))
			p++;
		bool match = (strncasecmp(p, token, tok_len) == 0) && (p + tok_len < line + line_len) &&
			isspace((unsigned char)p[tok_len]);
		if (match) {
			for (char* s = line; s < line + line_len;) {
				if ((size_t)(line + line_len - s) >= src_len && memcmp(s, src, src_len) == 0) {
					if (out_len + rep_len + 1 > out_size) {
						out_size = (out_size + rep_len) * 2;
						out = realloc(out, out_size);
					}
					memcpy(&out[out_len], rep, rep_len);
					out_len += rep_len;
					s += src_len;
					modified = true;
				} else {
					if (out_len + 2 > out_size) {
						out_size *= 2;
						out = realloc(out, out_size);
					}
					out[out_len++] = *s++;
				}
			}
		} else {
			if (out_len + line_len + 1 > out_size) {
				out_size = (out_size + line_len) * 2;
				out = realloc(out, out_size);
			}
			memcpy(&out[out_len], line, line_len);
			out_len += line_len;
		}
		line += line_len;
	}
	if (modified)
		rf_write_file(filename, out, out_len);
	free(out);
	free(data);
	return modified;
}

static char* replace_char(const char* src, char c, const char* rep)
{
	size_t n = 0, rep_len = strlen(rep);
	char* out;
	size_t j = 0;

	for (const char* p = src; *p; p++)
		if (*p == c)
			n++;
	out = malloc(strlen(src) + n * rep_len + 1);
	if (out == NULL)
		return NULL;
	for (const char* p = src; *p; p++) {
		if (*p == c) {
			memcpy(&out[j], rep, rep_len);
			j += rep_len;
		} else {
			out[j++] = *p;
		}
	}
	out[j] = 0;
	return out;
}

static void fix_config(const char* fullpath, const char* iso_dir, const char* basename, const rf_extract_opts_t* opts)
{
	const rf_image_t* img = opts->img;
	bool is_cfg = false, is_grub_cfg = false, is_menu_cfg = false, is_syslinux_cfg = false, is_conf = false;
	size_t len = strlen(basename);

	for (size_t i = 0; i < ARRAYSIZE(syslinux_cfg); i++) {
		if (rf_str_iequal(basename, syslinux_cfg[i])) {
			is_cfg = true;
			is_syslinux_cfg = true;
		}
	}
	if (rf_str_iequal(iso_dir, "/loader/entries"))
		is_conf = (len > 4) && rf_str_isuffix(basename, ".conf");
	if (len >= 4 && rf_str_isuffix(basename, ".cfg")) {
		is_cfg = true;
		for (size_t i = 0; i < ARRAYSIZE(grub_cfg); i++)
			if (rf_str_iequal(basename, grub_cfg[i]))
				is_grub_cfg = true;
		if (rf_str_iequal(basename, menu_cfg))
			is_menu_cfg = true;
	}
	if (!is_cfg && !is_conf)
		return;

	/* Add persistence to the kernel options */
	if (opts->persistence_size != 0 && (is_grub_cfg || is_menu_cfg || is_syslinux_cfg)) {
		const char* tok = is_grub_cfg ? "linux" : "append";
		if (replace_in_token_data(fullpath, tok, "file=/cdrom/preseed", "persistent file=/cdrom/preseed")) {
			uprintf("  Added 'persistent' kernel option to '%s'", fullpath);
			if (is_grub_cfg && replace_in_token_data(fullpath, "linux", "maybe-ubiquity", ""))
				uprintf("  Removed 'maybe-ubiquity' kernel option");
		} else if (replace_in_token_data(fullpath, tok, "boot=casper", "boot=casper persistent")) {
			uprintf("  Added 'persistent' kernel option to '%s'", fullpath);
		} else if (replace_in_token_data(fullpath, "linux", "/casper/vmlinuz", "/casper/vmlinuz persistent")) {
			uprintf("  Added 'persistent' kernel option to '%s'", fullpath);
		} else if (replace_in_token_data(fullpath, "kernel", "/casper/vmlinuz", "/casper/vmlinuz persistent")) {
			uprintf("  Added 'persistent' kernel option to '%s'", fullpath);
		} else if (replace_in_token_data(fullpath, tok, "boot=live", "boot=live persistence")) {
			uprintf("  Added 'persistence' kernel option to '%s'", fullpath);
		}
	}

	/* Config files that require an ISO label that differs from our USB label */
	if (opts->usb_label != NULL && img->label[0] != 0) {
		static const char* cfg_token[] = { "options", "append", "linux", "linuxefi", "$linux", "search", "for" };
		char* iso_label = replace_char(img->label, ' ', "\\x20");
		char* usb_label = replace_char(opts->usb_label, ' ', "\\x20");
		if (iso_label != NULL && usb_label != NULL) {
			bool patched = false;
			for (size_t i = 0; i < ARRAYSIZE(cfg_token); i++)
				if (replace_in_token_data(fullpath, cfg_token[i], iso_label, usb_label))
					patched = true;
			if (patched)
				uprintf("  Patched %s: '%s' ➔ '%s'", fullpath, iso_label, usb_label);
			/* Red Hat 8+ derivatives need 'inst.repo' instead of 'inst.stage2' */
			if (img->rh8_derivative && strstr(img->path, "netinst") == NULL) {
				patched = false;
				for (size_t i = 0; i < ARRAYSIZE(cfg_token); i++)
					if (replace_in_token_data(fullpath, cfg_token[i], "inst.stage2", "inst.repo"))
						patched = true;
				if (patched)
					uprintf("  Patched %s: 'inst.stage2' ➔ 'inst.repo'", fullpath);
			}
		}
		free(iso_label);
		free(usb_label);
	}

	/* Dual BIOS + EFI support for Tails and similar */
	if (is_syslinux_cfg && rf_str_iequal(iso_dir, efi_dirname) && rf_str_iequal(basename, syslinux_cfg[0]) &&
		!img->has_efi_syslinux) {
		char dst[PATH_MAX];
		size_t l = strlen(fullpath);
		snprintf(dst, sizeof(dst), "%.*ssyslinux.cfg", (int)(l - strlen(basename)), fullpath);
		if (access(dst, F_OK) != 0 && rf_copy_file(fullpath, dst))
			uprintf("Duplicated %s to %s", fullpath, dst);
	}

	/* FreeNAS */
	if (is_grub_cfg && opts->usb_label != NULL) {
		char iso_label[300], usb_label[300];
		snprintf(iso_label, sizeof(iso_label), "cd9660:/dev/iso9660/%s", img->label);
		snprintf(usb_label, sizeof(usb_label), "msdosfs:/dev/msdosfs/%s", opts->usb_label);
		if (replace_in_token_data(fullpath, "set", iso_label, usb_label))
			uprintf("  Patched %s: '%s' ➔ '%s'", fullpath, iso_label, usb_label);
	}
}

/* ---------------------------------------------------------------------- */
/* Extraction                                                             */
/* ---------------------------------------------------------------------- */

typedef struct {
	const rf_extract_opts_t* opts;
	const char* src_root;
	const char* dst_root;
	uint64_t total;
	uint64_t done;
	uint64_t last_update;
	uint8_t* buf;
	bool can_symlink;
	int nb_files;
} extract_ctx_t;

static void update_extract_progress(extract_ctx_t* ctx, const char* name)
{
	uint64_t now = rf_monotonic_ms();
	if (ctx->total == 0)
		return;
	if (now - ctx->last_update < 200 && ctx->done < ctx->total)
		return;
	ctx->last_update = now;
	rf_progress(OP_FILE_COPY, ctx->total ? (double)ctx->done / (double)ctx->total : 0.0,
		"Copying ISO files: %s", name);
}

static bool copy_data(extract_ctx_t* ctx, const char* src, const char* dst, const char* rel)
{
	int in = -1, out = -1;
	ssize_t n;
	bool r = false;

	in = open(src, O_RDONLY | O_CLOEXEC);
	if (in < 0) {
		uprintf("Could not open '%s': %s", src, strerror(errno));
		return false;
	}
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (out < 0) {
		uprintf("Could not create '%s': %s", dst, strerror(errno));
		goto out;
	}
	posix_fadvise(in, 0, 0, POSIX_FADV_SEQUENTIAL);
	while ((n = read(in, ctx->buf, COPY_BUFFER_SIZE)) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			uprintf("Read error on '%s': %s", src, strerror(errno));
			goto out;
		}
		for (ssize_t w = 0; w < n;) {
			ssize_t m = write(out, &ctx->buf[w], (size_t)(n - w));
			if (m < 0) {
				if (errno == EINTR)
					continue;
				uprintf("Write error on '%s': %s", dst, strerror(errno));
				if (errno == ENOSPC)
					uprintf("The target drive is full");
				goto out;
			}
			w += m;
		}
		ctx->done += (uint64_t)n;
		update_extract_progress(ctx, rel);
		if (rf_cancelled())
			goto out;
	}
	r = true;
out:
	if (in >= 0)
		close(in);
	if (out >= 0 && close(out) != 0 && r) {
		uprintf("Could not close '%s': %s", dst, strerror(errno));
		r = false;
	}
	return r;
}

static bool split_wim(const char* src, const char* dst)
{
	char swm[PATH_MAX];
	size_t len = strlen(dst);
	const char* argv[] = { "wimlib-imagex", "split", src, swm, "4000", NULL };

	if (len < 4)
		return false;
	snprintf(swm, sizeof(swm), "%.*s.swm", (int)(len - 4), dst);
	if (!rf_which("wimlib-imagex", NULL, 0)) {
		uprintf("'%s' is larger than 4 GB and must be split for FAT32, but wimlib-imagex is not installed.", src);
		uprintf("Install wimlib (e.g. 'pacman -S wimlib' or 'apt install wimtools'), or use NTFS/exFAT.");
		return false;
	}
	uprintf("Splitting '%s' into '%s'...", src, swm);
	return rf_run(argv) == 0;
}

static bool extract_dir(extract_ctx_t* ctx, const char* rel, int depth)
{
	char src[PATH_MAX], dst[PATH_MAX], child_rel[PATH_MAX];
	struct dirent** entries = NULL;
	int n;
	bool r = true;

	if (depth > 40) {
		uprintf("Directory nesting too deep at '%s' (symlink loop?) - skipping", rel);
		return true;
	}
	snprintf(src, sizeof(src), "%s%s", ctx->src_root, rel);
	n = scandir(src, &entries, NULL, alphasort);
	if (n < 0) {
		uprintf("Could not read directory '%s': %s", src, strerror(errno));
		return false;
	}
	for (int i = 0; i < n && r; i++) {
		const char* name = entries[i]->d_name;
		struct stat lst, st;
		if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
			continue;
		if (rf_cancelled()) {
			r = false;
			break;
		}
		snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, name);
		snprintf(src, sizeof(src), "%s%s", ctx->src_root, child_rel);
		snprintf(dst, sizeof(dst), "%s%s", ctx->dst_root, child_rel);
		if (lstat(src, &lst) != 0)
			continue;

		if (S_ISLNK(lst.st_mode)) {
			char target[PATH_MAX];
			ssize_t tl = readlink(src, target, sizeof(target) - 1);
			if (tl > 0)
				target[tl] = 0;
			if (ctx->can_symlink && tl > 0) {
				if (symlink(target, dst) != 0)
					uprintf("Could not create symlink '%s': %s", dst, strerror(errno));
				continue;
			}
			/* Target filesystem has no symlinks: copy what the link points to, if anything */
			if (stat(src, &st) != 0) {
				uprintf("  Skipping dangling symbolic link '%s'", child_rel);
				continue;
			}
			uprintf("  Resolving symbolic link '%s' ➔ '%s'", child_rel, (tl > 0) ? target : "?");
		} else {
			st = lst;
		}

		if (S_ISDIR(st.st_mode)) {
			if (mkdir(dst, 0755) != 0 && errno != EEXIST) {
				uprintf("Could not create directory '%s': %s", dst, strerror(errno));
				r = false;
				break;
			}
			r = extract_dir(ctx, child_rel, depth + 1);
			continue;
		}
		if (!S_ISREG(st.st_mode))
			continue;

		/* In case there's an ldlinux.sys on the ISO, prevent it from overwriting ours */
		if (ctx->opts->skip_root_ldlinux_sys && rel[0] == 0 && rf_str_iequal(name, ldlinux_name)) {
			uprintf("Skipping '%s' file from ISO image", name);
			continue;
		}

		if (ctx->opts->fat_target && (uint64_t)st.st_size >= 4 * GB) {
			if (rf_str_isuffix(rel, sources_str) &&
				(rf_str_iequal(name, wininst_name[0]) || rf_str_iequal(name, wininst_name[1]))) {
				r = split_wim(src, dst);
				ctx->done += (uint64_t)st.st_size;
				continue;
			}
			uprintf("'%s' is larger than 4 GB and cannot be copied to a FAT filesystem", child_rel);
			r = false;
			break;
		}

		ctx->nb_files++;
		r = copy_data(ctx, src, dst, child_rel);
		if (r)
			fix_config(dst, rel, name, ctx->opts);
	}
	for (int i = 0; i < n; i++)
		free(entries[i]);
	free(entries);
	return r;
}

static bool target_supports_symlinks(const char* dir)
{
	char test[PATH_MAX];
	bool r;

	snprintf(test, sizeof(test), "%s/.rufus_symlink_test", dir);
	r = (symlink("/", test) == 0);
	unlink(test);
	return r;
}

bool rf_iso_extract(const char* iso, const char* dest_dir, const rf_extract_opts_t* opts)
{
	char mnt[] = "/tmp/rufus-iso-XXXXXX";
	extract_ctx_t ctx = { .opts = opts, .dst_root = dest_dir };
	const char* fstypes[] = { opts->img->is_udf ? "udf" : "iso9660", opts->img->is_udf ? "iso9660" : "udf" };
	bool mounted = false, r = false;
	char path[PATH_MAX];

	if (mkdtemp(mnt) == NULL) {
		uprintf("Could not create temporary mount point: %s", strerror(errno));
		return false;
	}
	for (size_t i = 0; i < ARRAYSIZE(fstypes) && !mounted; i++) {
		const char* argv[] = { "mount", "-o", "loop,ro", "-t", fstypes[i], iso, mnt, NULL };
		mounted = (rf_run(argv) == 0);
	}
	if (!mounted) {
		uprintf("Could not mount the ISO image");
		rmdir(mnt);
		return false;
	}

	ctx.src_root = mnt;
	ctx.total = opts->img->projected_size ? opts->img->projected_size : opts->img->size;
	ctx.buf = malloc(COPY_BUFFER_SIZE);
	ctx.can_symlink = target_supports_symlinks(dest_dir);
	if (ctx.buf == NULL)
		goto out;

	uprintf("Extracting files from '%s' to '%s'...", iso, dest_dir);
	r = extract_dir(&ctx, "", 0);
	if (!r)
		goto out;
	uprintf("Extracted %d files", ctx.nb_files);

	/* Broken bootx64.efi symlink (Linux Mint): we resolved what we could above */
	if (HAS_SYSLINUX(opts->img)) {
		/* Create a /syslinux.cfg (if none exists) that points to the existing isolinux cfg */
		FILE* fd;
		snprintf(path, sizeof(path), "%s/syslinux.cfg", dest_dir);
		if (rf_str_iequal(opts->img->cfg_path, "/boot/i386/loader/isolinux.cfg") ||
			rf_str_iequal(opts->img->cfg_path, "/boot/x86_64/loader/isolinux.cfg")) {
			char org[PATH_MAX];
			snprintf(org, sizeof(org), "%s/syslinux.org", dest_dir);
			if (access(path, F_OK) == 0) {
				uprintf("Renaming: %s ➔ %s", path, org);
				rename(path, org);
			}
		}
		if (access(path, F_OK) != 0) {
			fd = fopen(path, "w");
			if (fd == NULL) {
				uprintf("Unable to create %s - booting from USB will not work", path);
				r = false;
				goto out;
			}
			fprintf(fd, "DEFAULT loadconfig\n\nLABEL loadconfig\n  CONFIG %s\n", opts->img->cfg_path);
			const char* slash = strrchr(opts->img->cfg_path, '/');
			if (slash != NULL && slash != opts->img->cfg_path)
				fprintf(fd, "  APPEND %.*s/\n", (int)(slash - opts->img->cfg_path), opts->img->cfg_path);
			fclose(fd);
			uprintf("Created: %s → %s", path, opts->img->cfg_path);
		}
	}

	/* Exception for Slax Syslinux UEFI bootloaders */
	snprintf(path, sizeof(path), "%s/slax/boot/EFI", dest_dir);
	if (access(path, F_OK) == 0) {
		char dst_path[PATH_MAX];
		snprintf(dst_path, sizeof(dst_path), "%s/EFI", dest_dir);
		if (access(dst_path, F_OK) != 0 && rename(path, dst_path) == 0)
			uprintf("Moved: %s → %s", path, dst_path);
	}
	rf_progress(OP_FILE_COPY, 1.0, "Copying ISO files: done");

out:
	free(ctx.buf);
	{
		const char* argv[] = { "umount", mnt, NULL };
		if (rf_run(argv) != 0) {
			const char* argv2[] = { "umount", "-l", mnt, NULL };
			rf_run(argv2);
		}
	}
	rmdir(mnt);
	return r && !rf_cancelled();
}

bool rf_copy_file(const char* src, const char* dst)
{
	extract_ctx_t ctx = { 0 };
	rf_extract_opts_t opts = { 0 };
	bool r;

	ctx.opts = &opts;
	ctx.buf = malloc(COPY_BUFFER_SIZE);
	if (ctx.buf == NULL)
		return false;
	ctx.total = 0;
	r = copy_data(&ctx, src, dst, src);
	free(ctx.buf);
	return r;
}
