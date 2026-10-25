/*
 * Rufus for Linux: command line interface (also the GUI's privileged helper)
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
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>

#include "rufus.h"

static bool machine_mode = false;
static bool progress_line_active = false;

static void print_machine_escaped(const char* prefix, const char* msg)
{
	/* One message per line: flatten any embedded newlines */
	fputs(prefix, stdout);
	for (const char* p = msg; *p; p++)
		fputc((*p == '\n' || *p == '\r') ? ' ' : *p, stdout);
	fputc('\n', stdout);
	fflush(stdout);
}

static void log_cb(const char* msg, void* ctx)
{
	(void)ctx;
	if (machine_mode) {
		print_machine_escaped("LOG ", msg);
		return;
	}
	if (progress_line_active) {
		fputs("\r\033[K", stderr);
		progress_line_active = false;
	}
	fprintf(stderr, "%s\n", msg);
}

static void progress_cb(rf_op_t op, double fraction, const char* msg, void* ctx)
{
	(void)ctx;
	if (machine_mode) {
		char prefix[64];
		snprintf(prefix, sizeof(prefix), "PROGRESS %d %.4f ", (int)op, fraction);
		print_machine_escaped(prefix, msg);
		return;
	}
	if (!isatty(STDERR_FILENO))
		return;
	if (fraction < 0.0) {
		fprintf(stderr, "\r\033[K%s", msg);
	} else {
		int width = 30, filled = (int)(fraction * width + 0.5);
		fprintf(stderr, "\r\033[K[");
		for (int i = 0; i < width; i++)
			fputc(i < filled ? '#' : '.', stderr);
		fprintf(stderr, "] %5.1f%% %s", fraction * 100.0, msg);
	}
	progress_line_active = true;
	fflush(stderr);
}

static void on_signal(int sig)
{
	(void)sig;
	rf_cancel();
}

/*
 * In machine mode, the frontend asks us to stop by writing "cancel" or closing our stdin.
 * This uses read(2) rather than stdio: a thread blocked in fgets() holds the stdin lock,
 * which glibc's exit() then waits for, so the process could never terminate.
 */
static void* stdin_watch(void* arg)
{
	char buf[64];
	ssize_t n;
	(void)arg;
	while ((n = read(STDIN_FILENO, buf, sizeof(buf) - 1)) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		buf[n] = 0;
		if (strstr(buf, "cancel") != NULL)
			break;
	}
	rf_cancel();
	return NULL;
}

static uint64_t parse_size(const char* s)
{
	char* end;
	double v = strtod(s, &end);
	while (*end == ' ')
		end++;
	switch (toupper((unsigned char)*end)) {
	case 'K': v *= KB; break;
	case 'M': v *= MB; break;
	case 'G': v *= GB; break;
	case 'T': v *= TB; break;
	case 0: v *= MB; break;   /* Default unit: MB */
	default: break;
	}
	return (uint64_t)v;
}

static void json_str(FILE* f, const char* s)
{
	fputc('"', f);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\')
			fprintf(f, "\\%c", *s);
		else if ((unsigned char)*s < 0x20)
			fprintf(f, "\\u%04x", *s);
		else
			fputc(*s, f);
	}
	fputc('"', f);
}

static int cmd_list(bool all, bool json)
{
	rf_drive_t* drives = NULL;
	int n = rf_drive_list(&drives, all);

	if (json) {
		printf("[");
		for (int i = 0; i < n; i++) {
			printf("%s{\"path\":", i ? "," : "");
			json_str(stdout, drives[i].path);
			printf(",\"name\":");
			json_str(stdout, drives[i].display);
			printf(",\"label\":");
			json_str(stdout, drives[i].label);
			printf(",\"vendor\":");
			json_str(stdout, drives[i].vendor);
			printf(",\"model\":");
			json_str(stdout, drives[i].model);
			printf(",\"bus\":");
			json_str(stdout, drives[i].bus);
			printf(",\"size\":%" PRIu64 ",\"sector_size\":%u,\"removable\":%s,\"partitions\":%d}",
				drives[i].size, drives[i].sector_size, drives[i].removable ? "true" : "false",
				drives[i].nb_partitions);
		}
		printf("]\n");
	} else if (n == 0) {
		fprintf(stderr, "No suitable drives found%s\n", all ? "" : " (use --all to include USB hard drives)");
	} else {
		for (int i = 0; i < n; i++)
			printf("%-14s %-10s %s\n", drives[i].path, rf_size_to_human(drives[i].size, true), drives[i].display);
	}
	free(drives);
	return 0;
}

static int cmd_info(const char* image)
{
	rf_image_t img;
	char desc[512];

	if (!rf_image_analyze(image, &img))
		return 1;
	rf_image_describe(&img, desc, sizeof(desc));
	printf("\n%s\n", desc);
	printf("  Size:            %s\n", rf_size_to_human(img.size, false));
	if (img.is_iso) {
		printf("  Label:           %s\n", img.label);
		printf("  Filesystem:      %s\n", img.is_udf ? "UDF" : "ISO9660");
		printf("  Content size:    %s\n", rf_size_to_human(img.projected_size, false));
		printf("  BIOS bootable:   %s\n", IS_BIOS_BOOTABLE(&img) ? "yes" : "no");
		printf("  UEFI bootable:   %s\n", IS_EFI_BOOTABLE(&img) ? "yes" : "no");
		printf("  DD mode:         %s\n", IS_DD_BOOTABLE(&img) ? (img.disable_iso ? "required" : "supported") : "no");
		printf("  Files > 4 GB:    %s\n", img.has_4GB_file ? "yes" : "no");
		printf("  Persistence:     %s\n", HAS_PERSISTENCE(&img) ? "supported" : "no");
		if (HAS_WININST(&img))
			printf("  Windows setup:   %s\n", img.wininst_path[0]);
	} else {
		printf("  Bootable disk:   %s\n", img.is_bootable_img ? "yes" : "no");
	}
	return 0;
}

static int cmd_hash(const char* file)
{
	rf_hashes_t h;
	if (!rf_hash_file(file, &h))
		return 1;
	if (progress_line_active)
		fputs("\r\033[K", stderr);
	printf("MD5:     %s\nSHA1:    %s\nSHA256:  %s\nSHA512:  %s\n", h.md5, h.sha1, h.sha256, h.sha512);
	return 0;
}

static bool confirm(const rf_drive_t* drive)
{
	char line[64];
	fprintf(stderr, "\nWARNING: ALL DATA ON %s WILL BE DESTROYED.\nType 'YES' to continue: ", drive->display);
	if (fgets(line, sizeof(line), stdin) == NULL)
		return false;
	return strncmp(line, "YES", 3) == 0;
}

static void usage(void)
{
	fprintf(stderr,
		"Rufus %s - Create bootable USB drives\n\n"
		"Usage:\n"
		"  rufus-cli list [--all] [--json]       List target drives\n"
		"  rufus-cli info IMAGE                  Analyze an image\n"
		"  rufus-cli hash FILE                   Compute MD5/SHA1/SHA256/SHA512\n"
		"  rufus-cli save -d DEVICE -o FILE      Save a drive to a raw image\n"
		"  rufus-cli write -d DEVICE [options]   Create a bootable drive\n\n"
		"Write options:\n"
		"  -d, --device DEV        Target drive (e.g. /dev/sdb)\n"
		"  -i, --image FILE        ISO or disk image (implies --boot image)\n"
		"  -b, --boot TYPE         image, freedos, syslinux4, syslinux6, grub2 or none\n"
		"  -m, --mode MODE         iso (extract files) or dd (raw write)\n"
		"  -p, --scheme SCHEME     mbr or gpt\n"
		"  -t, --target TARGET     bios (BIOS or UEFI-CSM / BIOS+UEFI) or uefi\n"
		"  -f, --fs FS             fat16, fat32, ntfs, exfat, ext2, ext3, ext4\n"
		"  -c, --cluster SIZE      Cluster size in bytes (default: automatic)\n"
		"  -l, --label LABEL       Volume label\n"
		"  -P, --persistence SIZE  Persistent partition size (e.g. 4G) for Ubuntu/Debian\n"
		"      --full-format       Zero the partition before formatting (slow)\n"
		"      --badblocks N       Check for bad blocks first (N = 1 to 4 passes, destructive)\n"
		"      --verify            Verify written data (DD mode)\n"
		"      --dry-run           Show what would be done, without writing anything\n"
		"      --no-rufus-mbr      Use the Windows 7 MBR rather than the Rufus MBR\n"
		"      --no-download       Don't download matching Syslinux/GRUB boot files\n"
		"  Windows customization:\n"
		"      --wue-bypass        Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0\n"
		"      --wue-no-account    Remove requirement for an online Microsoft account\n"
		"      --wue-user NAME     Create a local account with username NAME\n"
		"      --wue-no-privacy    Set privacy options to 'no' (disable data collection)\n"
		"      --wue-no-bitlocker  Disable BitLocker automatic device encryption\n"
		"      --wue-locale LOC    Set regional options (e.g. en-US)\n"
		"      --wue-timezone TZ   Set the time zone (Windows name, e.g. 'Pacific Standard Time')\n"
		"      --wue-offline       Set internal drives offline during setup\n"
		"      --wue-qol           Quality of life tweaks (no OneDrive/Copilot/news...)\n"
		"  -y, --yes               Don't ask for confirmation\n"
		"      --machine           Machine readable output (for frontends)\n",
		RUFUS_LINUX_VERSION);
}

enum {
	OPT_FULL = 1000, OPT_BADBLOCKS, OPT_VERIFY, OPT_NO_RUFUS_MBR, OPT_NO_DOWNLOAD, OPT_MACHINE,
	OPT_WUE_BYPASS, OPT_WUE_NO_ACCOUNT, OPT_WUE_USER, OPT_WUE_NO_PRIVACY, OPT_WUE_NO_BITLOCKER,
	OPT_WUE_LOCALE, OPT_WUE_TIMEZONE, OPT_WUE_OFFLINE, OPT_WUE_QOL, OPT_ALL, OPT_JSON,
	OPT_DRY_RUN, OPT_LDLINUX_SYS, OPT_LDLINUX_BSS, OPT_LDLINUX_C32, OPT_CORE_IMG,
};

int main(int argc, char** argv)
{
	static const struct option long_opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "image", required_argument, NULL, 'i' },
		{ "boot", required_argument, NULL, 'b' },
		{ "mode", required_argument, NULL, 'm' },
		{ "scheme", required_argument, NULL, 'p' },
		{ "target", required_argument, NULL, 't' },
		{ "fs", required_argument, NULL, 'f' },
		{ "cluster", required_argument, NULL, 'c' },
		{ "label", required_argument, NULL, 'l' },
		{ "persistence", required_argument, NULL, 'P' },
		{ "output", required_argument, NULL, 'o' },
		{ "yes", no_argument, NULL, 'y' },
		{ "help", no_argument, NULL, 'h' },
		{ "full-format", no_argument, NULL, OPT_FULL },
		{ "badblocks", required_argument, NULL, OPT_BADBLOCKS },
		{ "verify", no_argument, NULL, OPT_VERIFY },
		{ "dry-run", no_argument, NULL, OPT_DRY_RUN },
		{ "no-rufus-mbr", no_argument, NULL, OPT_NO_RUFUS_MBR },
		{ "no-download", no_argument, NULL, OPT_NO_DOWNLOAD },
		{ "machine", no_argument, NULL, OPT_MACHINE },
		{ "wue-bypass", no_argument, NULL, OPT_WUE_BYPASS },
		{ "wue-no-account", no_argument, NULL, OPT_WUE_NO_ACCOUNT },
		{ "wue-user", required_argument, NULL, OPT_WUE_USER },
		{ "wue-no-privacy", no_argument, NULL, OPT_WUE_NO_PRIVACY },
		{ "wue-no-bitlocker", no_argument, NULL, OPT_WUE_NO_BITLOCKER },
		{ "wue-locale", required_argument, NULL, OPT_WUE_LOCALE },
		{ "wue-timezone", required_argument, NULL, OPT_WUE_TIMEZONE },
		{ "wue-offline", no_argument, NULL, OPT_WUE_OFFLINE },
		{ "wue-qol", no_argument, NULL, OPT_WUE_QOL },
		{ "all", no_argument, NULL, OPT_ALL },
		{ "json", no_argument, NULL, OPT_JSON },
		{ "ldlinux-sys", required_argument, NULL, OPT_LDLINUX_SYS },
		{ "ldlinux-bss", required_argument, NULL, OPT_LDLINUX_BSS },
		{ "ldlinux-c32", required_argument, NULL, OPT_LDLINUX_C32 },
		{ "core-img", required_argument, NULL, OPT_CORE_IMG },
		{ NULL, 0, NULL, 0 }
	};
	rf_options_t opts;
	rf_drive_t drive;
	rf_image_t img, *pimg = NULL;
	const char* cmd;
	const char *mode = NULL, *scheme = NULL, *target = NULL, *fs = NULL, *label = NULL, *output = NULL;
	const char* boot = NULL;
	bool yes = false, all = false, json = false, no_download = false;
	bool full_format = false, verify = false, no_rufus_mbr = false, dry_run = false;
	uint64_t persistence = 0;
	uint32_t cluster = 0;
	int badblocks = 0, wue = 0, c;
	char wue_user[64] = "", wue_locale[32] = "", wue_tz[64] = "";
	char ldsys[4096] = "", ldbss[4096] = "", ldc32[4096] = "", coreimg[4096] = "";
	const char* err;
	bool ok;

	if (argc < 2) {
		usage();
		return 1;
	}
	cmd = argv[1];
	if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
		usage();
		return 0;
	}
	if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "version") == 0) {
		printf("Rufus %s\n", RUFUS_LINUX_VERSION);
		return 0;
	}

	rf_options_init(&opts);
	optind = 2;
	while ((c = getopt_long(argc, argv, "d:i:b:m:p:t:f:c:l:P:o:yh", long_opts, NULL)) != -1) {
		switch (c) {
		case 'd': snprintf(opts.device, sizeof(opts.device), "%s", optarg); break;
		case 'i': snprintf(opts.image, sizeof(opts.image), "%s", optarg); break;
		case 'b': boot = optarg; break;
		case 'm': mode = optarg; break;
		case 'p': scheme = optarg; break;
		case 't': target = optarg; break;
		case 'f': fs = optarg; break;
		case 'c':
			/* Plain numbers are bytes, otherwise use the K/M suffix */
			if (strspn(optarg, "0123456789") == strlen(optarg))
				cluster = (uint32_t)strtoul(optarg, NULL, 10);
			else
				cluster = (uint32_t)parse_size(optarg);
			break;
		case 'l': label = optarg; break;
		case 'P': persistence = parse_size(optarg); break;
		case 'o': output = optarg; break;
		case 'y': yes = true; break;
		case 'h': usage(); return 0;
		case OPT_FULL: full_format = true; break;
		case OPT_BADBLOCKS: badblocks = atoi(optarg); break;
		case OPT_VERIFY: verify = true; break;
		case OPT_DRY_RUN: dry_run = true; break;
		case OPT_NO_RUFUS_MBR: no_rufus_mbr = true; break;
		case OPT_NO_DOWNLOAD: no_download = true; break;
		case OPT_MACHINE: machine_mode = true; break;
		case OPT_WUE_BYPASS: wue |= UNATTEND_SECUREBOOT_TPM_MINRAM; break;
		case OPT_WUE_NO_ACCOUNT: wue |= UNATTEND_NO_ONLINE_ACCOUNT; break;
		case OPT_WUE_USER: wue |= UNATTEND_SET_USER; snprintf(wue_user, sizeof(wue_user), "%s", optarg); break;
		case OPT_WUE_NO_PRIVACY: wue |= UNATTEND_NO_DATA_COLLECTION; break;
		case OPT_WUE_NO_BITLOCKER: wue |= UNATTEND_DISABLE_BITLOCKER; break;
		case OPT_WUE_LOCALE: wue |= UNATTEND_DUPLICATE_LOCALE; snprintf(wue_locale, sizeof(wue_locale), "%s", optarg); break;
		case OPT_WUE_TIMEZONE: wue |= UNATTEND_DUPLICATE_LOCALE; snprintf(wue_tz, sizeof(wue_tz), "%s", optarg); break;
		case OPT_WUE_OFFLINE: wue |= UNATTEND_OFFLINE_INTERNAL_DRIVES; break;
		case OPT_WUE_QOL: wue |= UNATTEND_QOL_ENHANCEMENTS; break;
		case OPT_ALL: all = true; break;
		case OPT_JSON: json = true; break;
		case OPT_LDLINUX_SYS: snprintf(ldsys, sizeof(ldsys), "%s", optarg); break;
		case OPT_LDLINUX_BSS: snprintf(ldbss, sizeof(ldbss), "%s", optarg); break;
		case OPT_LDLINUX_C32: snprintf(ldc32, sizeof(ldc32), "%s", optarg); break;
		case OPT_CORE_IMG: snprintf(coreimg, sizeof(coreimg), "%s", optarg); break;
		default: usage(); return 1;
		}
	}

	rf_set_log_callback(log_cb, NULL);
	rf_set_progress_callback(progress_cb, NULL);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGHUP, on_signal);
	signal(SIGPIPE, SIG_IGN);
	if (machine_mode) {
		pthread_t t;
		pthread_create(&t, NULL, stdin_watch, NULL);
		pthread_detach(t);
		yes = true;
	}

	if (strcmp(cmd, "list") == 0)
		return cmd_list(all, json);
	if (strcmp(cmd, "info") == 0) {
		if (optind >= argc && opts.image[0] == 0) {
			usage();
			return 1;
		}
		return cmd_info(opts.image[0] ? opts.image : argv[optind]);
	}
	if (strcmp(cmd, "hash") == 0) {
		if (optind >= argc) {
			usage();
			return 1;
		}
		return cmd_hash(argv[optind]);
	}
	if (strcmp(cmd, "save") == 0) {
		if (opts.device[0] == 0 || output == NULL) {
			usage();
			return 1;
		}
		ok = rf_save_drive_image(opts.device, output);
		if (machine_mode) {
			printf("RESULT %s\n", ok ? "ok" : "fail");
			fflush(stdout);
		}
		return ok ? 0 : 1;
	}
	if (strcmp(cmd, "write") != 0) {
		fprintf(stderr, "Unknown command '%s'\n\n", cmd);
		usage();
		return 1;
	}

	/* write */
	if (opts.device[0] == 0) {
		fprintf(stderr, "No device specified (use --device)\n");
		return 1;
	}
	if (!rf_drive_get(opts.device, &drive)) {
		fprintf(stderr, "'%s' is not a whole disk device\n", opts.device);
		return 1;
	}
	if (boot == NULL)
		opts.boot_type = (opts.image[0] != 0) ? BT_IMAGE : BT_NON_BOOTABLE;
	else if (strcasecmp(boot, "image") == 0 || strcasecmp(boot, "iso") == 0)
		opts.boot_type = BT_IMAGE;
	else if (strcasecmp(boot, "freedos") == 0)
		opts.boot_type = BT_FREEDOS;
	else if (strcasecmp(boot, "syslinux4") == 0)
		opts.boot_type = BT_SYSLINUX_V4;
	else if (strcasecmp(boot, "syslinux6") == 0)
		opts.boot_type = BT_SYSLINUX_V6;
	else if (strcasecmp(boot, "grub2") == 0)
		opts.boot_type = BT_GRUB2;
	else if (strcasecmp(boot, "none") == 0)
		opts.boot_type = BT_NON_BOOTABLE;
	else {
		fprintf(stderr, "Unknown boot type '%s'\n", boot);
		return 1;
	}
	if (opts.boot_type == BT_IMAGE) {
		if (opts.image[0] == 0) {
			fprintf(stderr, "No image specified (use --image)\n");
			return 1;
		}
		if (!rf_image_analyze(opts.image, &img))
			return 1;
		pimg = &img;
	}

	rf_options_defaults(&opts, &drive, pimg);
	if (mode != NULL)
		opts.image_mode = (strcasecmp(mode, "dd") == 0) ? IM_DD : IM_ISO;
	if (scheme != NULL) {
		opts.partition_style = (strcasecmp(scheme, "gpt") == 0) ? PS_GPT : PS_MBR;
		if (target == NULL)
			opts.target_type = (opts.partition_style == PS_GPT) ? TT_UEFI :
				((pimg != NULL && !IS_BIOS_BOOTABLE(pimg)) ? TT_UEFI : TT_BIOS);
	}
	if (target != NULL)
		opts.target_type = (strcasecmp(target, "uefi") == 0) ? TT_UEFI : TT_BIOS;
	if (fs != NULL) {
		opts.fs = rf_fs_from_name(fs);
		if (opts.fs == FS_UNKNOWN) {
			fprintf(stderr, "Unknown file system '%s'\n", fs);
			return 1;
		}
	}
	if (label != NULL)
		snprintf(opts.label, sizeof(opts.label), "%s", label);
	opts.cluster_size = cluster;
	opts.persistence_size = persistence;
	opts.quick_format = !full_format;
	opts.badblocks_passes = badblocks;
	opts.verify = verify;
	opts.use_rufus_mbr = !no_rufus_mbr;
	opts.unattend_flags = wue;
	snprintf(opts.unattend_username, sizeof(opts.unattend_username), "%s", wue_user);
	snprintf(opts.unattend_locale, sizeof(opts.unattend_locale), "%s", wue_locale);
	snprintf(opts.unattend_timezone, sizeof(opts.unattend_timezone), "%s", wue_tz);
	snprintf(opts.ldlinux_sys, sizeof(opts.ldlinux_sys), "%s", ldsys);
	snprintf(opts.ldlinux_bss, sizeof(opts.ldlinux_bss), "%s", ldbss);
	snprintf(opts.ldlinux_c32, sizeof(opts.ldlinux_c32), "%s", ldc32);
	snprintf(opts.grub2_core_img, sizeof(opts.grub2_core_img), "%s", coreimg);

	err = rf_options_validate(&opts, &drive, pimg);
	if (err != NULL) {
		rf_log("ERROR: %s", err);
		if (machine_mode) {
			printf("RESULT fail\n");
			fflush(stdout);
		}
		return 1;
	}

	if (!machine_mode) {
		fprintf(stderr, "\nDevice:           %s\n", drive.display);
		if (pimg != NULL) {
			char desc[512];
			rf_image_describe(pimg, desc, sizeof(desc));
			fprintf(stderr, "Image:            %s\n", desc);
			fprintf(stderr, "Write mode:       %s\n", opts.image_mode == IM_DD ? "DD (raw)" : "ISO (file copy)");
		} else {
			fprintf(stderr, "Boot selection:   %s\n", opts.boot_type == BT_FREEDOS ? "FreeDOS" :
				opts.boot_type == BT_NON_BOOTABLE ? "Non bootable" : boot);
		}
		if (pimg == NULL || opts.image_mode == IM_ISO) {
			fprintf(stderr, "Partition scheme: %s\n", opts.partition_style == PS_GPT ? "GPT" : "MBR");
			fprintf(stderr, "Target system:    %s\n", opts.target_type == TT_UEFI ? "UEFI (non CSM)" :
				((pimg != NULL && IS_EFI_BOOTABLE(pimg)) ? "BIOS or UEFI" : "BIOS (or UEFI-CSM)"));
			fprintf(stderr, "File system:      %s\n", rf_fs_name[opts.fs]);
			fprintf(stderr, "Volume label:     %s\n", opts.label);
			if (opts.persistence_size)
				fprintf(stderr, "Persistence:      %s\n", rf_size_to_human(opts.persistence_size, false));
		}
	}

	if (dry_run) {
		fprintf(stderr, "\nDry run: nothing was written.\n");
		return 0;
	}
	if (!yes && !confirm(&drive)) {
		fprintf(stderr, "Cancelled\n");
		return 1;
	}
	if (geteuid() != 0) {
		fprintf(stderr, "Writing to drives requires root privileges (try: sudo rufus-cli ...)\n");
		return 1;
	}
	if (!no_download)
		rf_fetch_boot_files(pimg, &opts);

	ok = rf_job_run(&opts);
	if (progress_line_active)
		fputs("\r\033[K", stderr);
	if (machine_mode) {
		printf("RESULT %s\n", ok ? "ok" : (rf_cancelled() ? "cancelled" : "fail"));
		fflush(stdout);
	} else
		fprintf(stderr, "%s\n", ok ? "\nDone. The drive is ready." : "\nThe operation failed. See the messages above.");
	return ok ? 0 : 1;
}
