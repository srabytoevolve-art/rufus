/*
 * Rufus for Linux: core API
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Linux port additions © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>

#define RUFUS_LINUX_VERSION     "4.12-linux"
#define APPLICATION_NAME        "Rufus"
#define FILES_URL               "https://rufus.ie/files"

#define KB                      1024ULL
#define MB                      1048576ULL
#define GB                      1073741824ULL
#define TB                      1099511627776ULL

#define ARRAYSIZE(a)            (sizeof(a) / sizeof((a)[0]))
#ifndef MIN
#define MIN(a, b)               (((a) < (b)) ? (a) : (b))
#define MAX(a, b)               (((a) > (b)) ? (a) : (b))
#endif

#define SL_MAJOR(x)             ((uint8_t)((x) >> 8))
#define SL_MINOR(x)             ((uint8_t)(x))

/* ---------------------------------------------------------------------- */
/* Logging and progress (log.c)                                            */
/* ---------------------------------------------------------------------- */

typedef enum {
	OP_NONE = 0,
	OP_ANALYZE,
	OP_BADBLOCKS,
	OP_ZERO,
	OP_PARTITION,
	OP_FORMAT,
	OP_BOOTRECORD,
	OP_FILE_COPY,
	OP_WRITE_IMAGE,
	OP_VERIFY,
	OP_FINALIZE,
	OP_SAVE_IMAGE,
	OP_HASH,
	OP_DOWNLOAD,
	OP_MAX
} rf_op_t;

typedef void (*rf_log_cb)(const char* msg, void* ctx);
typedef void (*rf_progress_cb)(rf_op_t op, double fraction, const char* msg, void* ctx);

void rf_set_log_callback(rf_log_cb cb, void* ctx);
void rf_set_progress_callback(rf_progress_cb cb, void* ctx);
void rf_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void uprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void rf_progress(rf_op_t op, double fraction, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
const char* rf_op_name(rf_op_t op);

/* Cancellation */
void rf_cancel(void);
bool rf_cancelled(void);
void rf_reset_cancel(void);

/* ---------------------------------------------------------------------- */
/* Utilities (util.c)                                                      */
/* ---------------------------------------------------------------------- */

const char* rf_size_to_human(uint64_t size, bool decimal_units);
char* rf_strdup_printf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
bool rf_str_iequal(const char* a, const char* b);
bool rf_str_iprefix(const char* s, const char* prefix);
bool rf_str_isuffix(const char* s, const char* suffix);
char* rf_read_text_file(const char* path, size_t* len);
bool rf_write_file(const char* path, const void* data, size_t len);
bool rf_mkdir_p(const char* path, int mode);
bool rf_which(const char* prog, char* out, size_t out_size);
/* Run a program (argv[0] looked up in PATH). Output goes to the log. Returns exit status or -1 */
int rf_run(const char* const argv[]);
/* Same as rf_run, but captures stdout into *out (malloc'ed, NUL terminated) */
int rf_run_capture(const char* const argv[], char** out);
void rf_udev_settle(void);
uint64_t rf_monotonic_ms(void);

/* ---------------------------------------------------------------------- */
/* Drives (drive.c)                                                        */
/* ---------------------------------------------------------------------- */

typedef struct {
	char name[32];          /* "sdb" */
	char path[64];          /* "/dev/sdb" */
	char vendor[64];
	char model[128];
	char serial[64];
	char bus[16];           /* "usb", "mmc", "nvme", "ata", ... */
	char label[64];         /* label of the first partition that has one */
	char display[256];      /* human readable name for UIs */
	uint64_t size;
	uint32_t sector_size;
	bool removable;
	bool is_usb;
	bool is_system;         /* hosts a mounted system path (/, /boot, /home, swap...) */
	bool read_only;
	int nb_partitions;
} rf_drive_t;

/* Returns the number of drives found, with *drives malloc'ed. */
int rf_drive_list(rf_drive_t** drives, bool list_non_removable);
bool rf_drive_get(const char* path, rf_drive_t* drive);
/* Path of partition #n (1-based) of drive, e.g. /dev/sdb1 or /dev/mmcblk0p1 */
void rf_drive_partition_path(const char* disk_path, int n, char* out, size_t out_size);
/* Unmounts all partitions of a drive. */
bool rf_drive_unmount_all(const char* disk_path);
/* Disable udisks automounting for this drive while we work on it */
void rf_drive_inhibit_automount(const char* disk_name, bool inhibit);
/* Ask the kernel to re-read the partition table, and wait for the partition nodes */
bool rf_drive_reread_partitions(int fd, const char* disk_path, int expected_partitions);
/* Is the file at path stored on the drive? */
bool rf_drive_hosts_file(const char* disk_path, const char* file_path);

/* ---------------------------------------------------------------------- */
/* Images (image.c)                                                        */
/* ---------------------------------------------------------------------- */

typedef enum {
	COMP_NONE = 0,
	COMP_GZIP,
	COMP_XZ,
	COMP_ZSTD,
	COMP_BZIP2,
	COMP_LZMA,
} rf_compression_t;

#define EFI_BOOTMGR     0x0001
#define EFI_IA32        0x0002
#define EFI_X64         0x0004
#define EFI_ARM         0x0008
#define EFI_AA64        0x0010
#define EFI_IA64        0x0020
#define EFI_RISCV64     0x0040
#define EFI_LOONGARCH64 0x0080
#define EFI_IMG         0x8000

#define MAX_WININST     16

typedef struct {
	char path[4096];
	uint64_t size;
	uint64_t projected_size;        /* size needed for extracted content */

	/* Image type */
	bool is_iso;
	bool is_udf;
	bool is_bootable_img;           /* raw disk image with an MBR/GPT (DD-able) */
	bool is_isohybrid;              /* ISO that can also be DD'ed */
	bool is_vhd;
	rf_compression_t compression;
	uint64_t uncompressed_size;     /* 0 if unknown */

	char label[64];                 /* ISO volume label */

	/* Boot properties */
	bool has_bootmgr;
	bool has_bootmgr_efi;
	uint16_t has_efi;               /* EFI_* bits */
	char efi_img_path[256];
	bool has_grub4dos;
	uint8_t has_grub2;              /* 1 = /boot/grub, 2 = /boot/grub2 (nonstandard); bit 7: EFI GRUB */
	char grub2_version[96];
	bool has_kolibrios;
	char reactos_path[256];
	uint16_t winpe;
	bool uses_minint;
	int wininst_index;
	char wininst_path[MAX_WININST][256];
	uint8_t has_4GB_file;
	bool has_panther_unattend;
	bool has_symlinks;

	/* Syslinux */
	bool has_syslinux_cfg;
	bool has_efi_syslinux;
	bool has_ldlinux_c32;
	uint16_t sl_version;
	char sl_version_str[16];
	char sl_version_ext[64];
	char cfg_path[256];

	/* Linux specifics */
	bool uses_casper;
	bool uses_debian_live;
	bool disable_iso;               /* ISO mode known not to work (Manjaro, Pop!_OS, Proxmox) */
	bool rh8_derivative;
	bool has_md5sum;
} rf_image_t;

#define HAS_SYSLINUX(r)     ((r)->sl_version != 0)
#define HAS_BOOTMGR_BIOS(r) ((r)->has_bootmgr)
#define HAS_BOOTMGR_EFI(r)  ((r)->has_bootmgr_efi)
#define HAS_BOOTMGR(r)      (HAS_BOOTMGR_BIOS(r) || HAS_BOOTMGR_EFI(r))
#define HAS_WININST(r)      ((r)->wininst_index != 0)
#define HAS_WINPE(r)        (((r)->winpe & 0x7) == 0x7 || ((r)->winpe & 0x38) == 0x38 || ((r)->winpe & 0x1c0) == 0x1c0)
#define HAS_WINDOWS(r)      (HAS_BOOTMGR(r) || (r)->uses_minint || HAS_WINPE(r))
#define HAS_WIN7_EFI(r)     (((r)->has_efi == EFI_BOOTMGR) && HAS_WININST(r))
#define HAS_REACTOS(r)      ((r)->reactos_path[0] != 0)
#define HAS_GRUB(r)         (((r)->has_grub2 & 0x7f) || (r)->has_grub4dos)
#define HAS_KOLIBRIOS(r)    ((r)->has_kolibrios)
#define HAS_PERSISTENCE(r)  ((HAS_SYSLINUX(r) || HAS_GRUB(r)) && !(HAS_WINDOWS(r) || HAS_REACTOS(r) || HAS_KOLIBRIOS(r)))
#define IS_EFI_BOOTABLE(r)  ((r)->has_efi != 0)
#define IS_BIOS_BOOTABLE(r) (HAS_BOOTMGR_BIOS(r) || HAS_SYSLINUX(r) || HAS_WINPE(r) || ((r)->has_grub2 & 0x7f) || \
                             (r)->has_grub4dos || HAS_REACTOS(r) || HAS_KOLIBRIOS(r))
#define IS_DD_BOOTABLE(r)   ((r)->is_bootable_img)
#define IS_DD_ONLY(r)       (IS_DD_BOOTABLE(r) && (!(r)->is_iso || (r)->disable_iso))

/* Analyzes an image. Returns false if it could not be opened at all. */
bool rf_image_analyze(const char* path, rf_image_t* img);
/* Short human readable description, e.g. "ISO image (Syslinux 6.04, UEFI)" */
void rf_image_describe(const rf_image_t* img, char* out, size_t out_size);

/* ---------------------------------------------------------------------- */
/* ISO access (iso.c)                                                      */
/* ---------------------------------------------------------------------- */

/* Reads a whole file from an ISO (ISO9660 or UDF). Returns size, 0 on error. */
size_t rf_iso_read_file(const char* iso, const char* iso_path, uint8_t** buf);

typedef struct {
	const rf_image_t* img;
	const char* usb_label;          /* label of the target filesystem */
	uint64_t persistence_size;      /* add persistence options when non zero */
	bool fat_target;                /* target is FAT: split/flag > 4 GB files */
	bool skip_root_ldlinux_sys;
} rf_extract_opts_t;

/* Extracts all of an ISO into dest_dir, patching configs as needed */
bool rf_iso_extract(const char* iso, const char* dest_dir, const rf_extract_opts_t* opts);

/* ---------------------------------------------------------------------- */
/* Write job (job.c)                                                       */
/* ---------------------------------------------------------------------- */

typedef enum {
	BT_NON_BOOTABLE = 0,
	BT_IMAGE,
	BT_FREEDOS,
	BT_SYSLINUX_V4,
	BT_SYSLINUX_V6,
	BT_GRUB2,
	BT_MAX
} rf_boot_type_t;

typedef enum { PS_MBR = 0, PS_GPT } rf_partition_style_t;
typedef enum { TT_BIOS = 0, TT_UEFI } rf_target_type_t;
typedef enum { IM_ISO = 0, IM_DD } rf_image_mode_t;

typedef enum {
	FS_UNKNOWN = -1,
	FS_FAT16 = 0,
	FS_FAT32,
	FS_NTFS,
	FS_UDF,
	FS_EXFAT,
	FS_REFS,
	FS_EXT2,
	FS_EXT3,
	FS_EXT4,
	FS_MAX
} rf_fs_t;

#define IS_FAT(fs)  ((fs) == FS_FAT16 || (fs) == FS_FAT32)
#define IS_EXT(fs)  ((fs) >= FS_EXT2 && (fs) <= FS_EXT4)

extern const char* rf_fs_name[FS_MAX];
rf_fs_t rf_fs_from_name(const char* name);
/* Is the filesystem creatable on this system (mkfs tool present)? */
bool rf_fs_available(rf_fs_t fs);
uint32_t rf_default_cluster_size(rf_fs_t fs, uint64_t part_size);

/* Windows User Experience flags (same values as Windows Rufus) */
#define UNATTEND_SECUREBOOT_TPM_MINRAM      0x00001
#define UNATTEND_NO_ONLINE_ACCOUNT          0x00004
#define UNATTEND_NO_DATA_COLLECTION         0x00008
#define UNATTEND_OFFLINE_INTERNAL_DRIVES    0x00010
#define UNATTEND_DUPLICATE_LOCALE           0x00020
#define UNATTEND_SET_USER                   0x00040
#define UNATTEND_DISABLE_BITLOCKER          0x00080
#define UNATTEND_QOL_ENHANCEMENTS           0x01000

typedef struct {
	char device[64];                /* /dev/sdX */
	char image[4096];               /* source image, if any */
	rf_boot_type_t boot_type;
	rf_image_mode_t image_mode;
	rf_partition_style_t partition_style;
	rf_target_type_t target_type;
	rf_fs_t fs;
	uint32_t cluster_size;          /* 0 = default */
	char label[64];
	bool quick_format;
	bool verify;                    /* read back and compare after DD write */
	int badblocks_passes;           /* 0 = no bad blocks check */
	uint64_t persistence_size;
	bool old_bios_fixes;
	bool use_rufus_mbr;
	int unattend_flags;
	char unattend_username[64];
	char unattend_locale[32];       /* e.g. "en-US" */
	char unattend_timezone[64];     /* Windows timezone name */
	/* Optional overrides, as downloaded by the frontend (non-root) */
	char ldlinux_sys[4096];
	char ldlinux_bss[4096];
	char ldlinux_c32[4096];
	char grub2_core_img[4096];
} rf_options_t;

void rf_options_init(rf_options_t* opts);
/* Fills in defaults derived from the drive and image (partition scheme, fs, label...) */
void rf_options_defaults(rf_options_t* opts, const rf_drive_t* drive, const rf_image_t* img);
/* Checks an option set for consistency. Returns NULL if OK, or an error string. */
const char* rf_options_validate(const rf_options_t* opts, const rf_drive_t* drive, const rf_image_t* img);
/* Executes the whole operation. Must be run as root. */
bool rf_job_run(const rf_options_t* opts);
/* Saves a drive to a raw image */
bool rf_save_drive_image(const char* device, const char* dest);

/* Suggested label for a drive with no image */
void rf_default_label(const rf_drive_t* drive, char* out, size_t out_size);
/* Sanitize a label for a filesystem (FAT: uppercase, 11 chars) */
void rf_sanitize_label(char* label, rf_fs_t fs, uint64_t disk_size);

/* ---------------------------------------------------------------------- */
/* Hashing (hash.c)                                                        */
/* ---------------------------------------------------------------------- */

typedef struct {
	char md5[33];
	char sha1[41];
	char sha256[65];
	char sha512[129];
} rf_hashes_t;

bool rf_hash_file(const char* path, rf_hashes_t* out);

/* ---------------------------------------------------------------------- */
/* Downloads (net.c)                                                       */
/* ---------------------------------------------------------------------- */

/* Cache directory for downloaded boot files (~/.cache/rufus) */
const char* rf_cache_dir(void);
/* Download FILES_URL/<rel_path> into the cache, validating the Rufus signature. */
bool rf_download_signed(const char* rel_path, char* out_path, size_t out_size);
/* Fetches the boot files an image needs that aren't embedded. Fills in opts overrides. */
void rf_fetch_boot_files(const rf_image_t* img, rf_options_t* opts);

/* ---------------------------------------------------------------------- */
/* Embedded resources (resources.S)                                        */
/* ---------------------------------------------------------------------- */

typedef struct {
	const char* name;
	const uint8_t* data;
	size_t size;
} rf_resource_t;

const rf_resource_t* rf_resource(const char* name);
extern const char* embedded_sl_version_str[2];
extern const uint16_t embedded_sl_version[2];
extern const char* embedded_grub2_version;
