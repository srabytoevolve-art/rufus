/*
 * Rufus for Linux: drive enumeration and handling
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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <sys/sysmacros.h>
#include <linux/fs.h>

#include "rufus.h"

static bool read_sysfs(const char* dir, const char* attr, char* out, size_t out_size)
{
	char path[PATH_MAX];
	FILE* fd;
	size_t len;

	out[0] = 0;
	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	fd = fopen(path, "r");
	if (fd == NULL)
		return false;
	if (fgets(out, (int)out_size, fd) == NULL)
		out[0] = 0;
	fclose(fd);
	/* Trim whitespace */
	len = strlen(out);
	while (len > 0 && isspace((unsigned char)out[len - 1]))
		out[--len] = 0;
	char* p = out;
	while (isspace((unsigned char)*p))
		p++;
	if (p != out)
		memmove(out, p, strlen(p) + 1);
	return true;
}

static uint64_t read_sysfs_u64(const char* dir, const char* attr)
{
	char buf[64];
	if (!read_sysfs(dir, attr, buf, sizeof(buf)))
		return 0;
	return strtoull(buf, NULL, 10);
}

/* Decode the octal escapes (\040 etc.) used in /proc/self/mountinfo */
static void unescape_mount(char* s)
{
	char* d = s;
	while (*s) {
		if (s[0] == '\\' && s[1] >= '0' && s[1] <= '7' && s[2] >= '0' && s[2] <= '7' && s[3] >= '0' && s[3] <= '7') {
			*d++ = (char)(((s[1] - '0') << 6) | ((s[2] - '0') << 3) | (s[3] - '0'));
			s += 4;
		} else {
			*d++ = *s++;
		}
	}
	*d = 0;
}

/* Is dev (e.g. "/dev/sdb1") the disk or one of its partitions? */
static bool dev_belongs_to(const char* dev, const char* disk_name)
{
	const char* base;
	size_t len = strlen(disk_name);
	struct stat st;
	char syspath[PATH_MAX], real[PATH_MAX], needle[64];

	if (strncmp(dev, "/dev/", 5) != 0)
		return false;
	base = &dev[5];
	if (strcmp(base, disk_name) == 0)
		return true;
	/* Resolve through sysfs, which handles symlinks like /dev/disk/by-uuid/... */
	if (stat(dev, &st) == 0 && S_ISBLK(st.st_mode)) {
		snprintf(syspath, sizeof(syspath), "/sys/dev/block/%u:%u", major(st.st_rdev), minor(st.st_rdev));
		if (realpath(syspath, real) != NULL) {
			snprintf(needle, sizeof(needle), "/block/%s/", disk_name);
			if (strstr(real, needle) != NULL)
				return true;
			snprintf(needle, sizeof(needle), "/block/%s", disk_name);
			if (rf_str_isuffix(real, needle))
				return true;
		}
		return false;
	}
	/* Fallback: name prefix match (sdb -> sdb1, mmcblk0 -> mmcblk0p1) */
	if (strncmp(base, disk_name, len) != 0)
		return false;
	base += len;
	if (*base == 'p')
		base++;
	return isdigit((unsigned char)*base);
}

typedef struct {
	char source[PATH_MAX];
	char target[PATH_MAX];
} mount_entry_t;

static int list_mounts(mount_entry_t** entries)
{
	FILE* fd = fopen("/proc/self/mountinfo", "r");
	char line[8192];
	int n = 0, size = 0;

	*entries = NULL;
	if (fd == NULL)
		return 0;
	while (fgets(line, sizeof(line), fd) != NULL) {
		/* id parent maj:min root target options [optional...] - fstype source superoptions */
		char *save = NULL, *tok, *target = NULL, *source = NULL;
		int field = 0;
		bool after_sep = false;
		int sep_field = 0;
		for (tok = strtok_r(line, " \n", &save); tok != NULL; tok = strtok_r(NULL, " \n", &save), field++) {
			if (field == 4)
				target = tok;
			if (!after_sep && strcmp(tok, "-") == 0) {
				after_sep = true;
				sep_field = field;
			} else if (after_sep && field == sep_field + 2) {
				source = tok;
				break;
			}
		}
		if (target == NULL || source == NULL)
			continue;
		if (n >= size) {
			size = (size == 0) ? 64 : size * 2;
			*entries = realloc(*entries, sizeof(mount_entry_t) * (size_t)size);
		}
		snprintf((*entries)[n].source, PATH_MAX, "%s", source);
		snprintf((*entries)[n].target, PATH_MAX, "%s", target);
		unescape_mount((*entries)[n].source);
		unescape_mount((*entries)[n].target);
		n++;
	}
	fclose(fd);
	return n;
}

static bool is_system_mountpoint(const char* target)
{
	static const char* sys_mounts[] = { "/", "/boot", "/boot/efi", "/efi", "/home", "/usr", "/var",
		"/opt", "/srv", "/nix", "/gnu", "/root", "/tmp" };
	for (size_t i = 0; i < ARRAYSIZE(sys_mounts); i++)
		if (strcmp(target, sys_mounts[i]) == 0)
			return true;
	return false;
}

static bool disk_has_holders(const char* name)
{
	char path[PATH_MAX];
	struct dirent* de;
	DIR* d;
	bool r = false;

	snprintf(path, sizeof(path), "/sys/block/%s", name);
	d = opendir(path);
	if (d == NULL)
		return false;
	while (!r && (de = readdir(d)) != NULL) {
		char hpath[PATH_MAX];
		DIR* h;
		struct dirent* he;
		if (strncmp(de->d_name, name, strlen(name)) != 0 && strcmp(de->d_name, ".") != 0)
			continue;
		if (strcmp(de->d_name, ".") == 0)
			snprintf(hpath, sizeof(hpath), "%s/holders", path);
		else
			snprintf(hpath, sizeof(hpath), "%s/%s/holders", path, de->d_name);
		h = opendir(hpath);
		if (h == NULL)
			continue;
		while ((he = readdir(h)) != NULL) {
			if (he->d_name[0] != '.') {
				r = true;
				break;
			}
		}
		closedir(h);
	}
	closedir(d);
	return r;
}

static bool disk_is_system(const char* name, mount_entry_t* mounts, int nb_mounts)
{
	char line[1024];
	FILE* fd;

	for (int i = 0; i < nb_mounts; i++) {
		if (dev_belongs_to(mounts[i].source, name) && is_system_mountpoint(mounts[i].target))
			return true;
	}
	fd = fopen("/proc/swaps", "r");
	if (fd != NULL) {
		while (fgets(line, sizeof(line), fd) != NULL) {
			char* sp = strchr(line, ' ');
			if (sp == NULL)
				continue;
			*sp = 0;
			if (dev_belongs_to(line, name)) {
				fclose(fd);
				return true;
			}
		}
		fclose(fd);
	}
	/* LVM / LUKS / RAID members: never touch */
	return disk_has_holders(name);
}

static void find_label(const char* name, char* label, size_t label_size)
{
	DIR* d = opendir("/dev/disk/by-label");
	struct dirent* de;
	char path[PATH_MAX], real[PATH_MAX];

	label[0] = 0;
	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/dev/disk/by-label/%s", de->d_name);
		if (realpath(path, real) == NULL)
			continue;
		if (dev_belongs_to(real, name)) {
			snprintf(label, label_size, "%s", de->d_name);
			unescape_mount(label);
			/* udev encodes spaces as \x20 */
			for (char* p = strstr(label, "\\x20"); p != NULL; p = strstr(p, "\\x20")) {
				*p = ' ';
				memmove(p + 1, p + 4, strlen(p + 4) + 1);
			}
			break;
		}
	}
	closedir(d);
}

static void detect_bus(const char* sysdir, rf_drive_t* drive)
{
	char real[PATH_MAX];

	snprintf(drive->bus, sizeof(drive->bus), "%s", "unknown");
	if (realpath(sysdir, real) == NULL)
		return;
	if (strstr(real, "/usb") != NULL) {
		snprintf(drive->bus, sizeof(drive->bus), "usb");
		drive->is_usb = true;
	} else if (strstr(real, "/mmc") != NULL) {
		snprintf(drive->bus, sizeof(drive->bus), "mmc");
	} else if (strstr(real, "/nvme") != NULL) {
		snprintf(drive->bus, sizeof(drive->bus), "nvme");
	} else if (strstr(real, "/ata") != NULL) {
		snprintf(drive->bus, sizeof(drive->bus), "ata");
	} else if (strstr(real, "/virtio") != NULL) {
		snprintf(drive->bus, sizeof(drive->bus), "virtio");
	} else if (strstr(real, "/virtual/block/loop") != NULL) {
		snprintf(drive->bus, sizeof(drive->bus), "loop");
	}
}

static bool fill_drive(const char* name, rf_drive_t* drive, mount_entry_t* mounts, int nb_mounts)
{
	char sysdir[PATH_MAX], devdir[PATH_MAX], buf[256];
	char what[300];
	DIR* d;
	struct dirent* de;

	memset(drive, 0, sizeof(*drive));
	snprintf(drive->name, sizeof(drive->name), "%s", name);
	snprintf(drive->path, sizeof(drive->path), "/dev/%s", name);
	snprintf(sysdir, sizeof(sysdir), "/sys/block/%s", name);
	snprintf(devdir, sizeof(devdir), "%s/device", sysdir);

	drive->size = read_sysfs_u64(sysdir, "size") * 512ULL;
	drive->sector_size = (uint32_t)read_sysfs_u64(sysdir, "queue/logical_block_size");
	if (drive->sector_size == 0)
		drive->sector_size = 512;
	drive->removable = read_sysfs_u64(sysdir, "removable") != 0;
	drive->read_only = read_sysfs_u64(sysdir, "ro") != 0;
	detect_bus(sysdir, drive);

	read_sysfs(devdir, "vendor", drive->vendor, sizeof(drive->vendor));
	if (!read_sysfs(devdir, "model", drive->model, sizeof(drive->model)) || drive->model[0] == 0)
		read_sysfs(devdir, "name", drive->model, sizeof(drive->model));    /* mmc */
	read_sysfs(devdir, "serial", drive->serial, sizeof(drive->serial));
	if (strcmp(drive->bus, "mmc") == 0)
		drive->removable = true;
	if (strcmp(drive->bus, "loop") == 0) {
		read_sysfs(sysdir, "loop/backing_file", buf, sizeof(buf));
		snprintf(drive->vendor, sizeof(drive->vendor), "Loop");
		snprintf(drive->model, sizeof(drive->model), "%s", buf);
	}

	d = opendir(sysdir);
	if (d != NULL) {
		while ((de = readdir(d)) != NULL) {
			char pdir[PATH_MAX];
			struct stat st;
			if (strncmp(de->d_name, name, strlen(name)) != 0)
				continue;
			snprintf(pdir, sizeof(pdir), "%s/%s/partition", sysdir, de->d_name);
			if (stat(pdir, &st) == 0)
				drive->nb_partitions++;
		}
		closedir(d);
	}

	find_label(name, drive->label, sizeof(drive->label));
	drive->is_system = disk_is_system(name, mounts, nb_mounts);

	if (drive->label[0] != 0)
		snprintf(what, sizeof(what), "%s", drive->label);
	else if (drive->vendor[0] != 0 || drive->model[0] != 0)
		snprintf(what, sizeof(what), "%s%s%s", drive->vendor, (drive->vendor[0] && drive->model[0]) ? " " : "", drive->model);
	else
		snprintf(what, sizeof(what), "%s Drive", drive->is_usb ? "USB" : "Disk");
	snprintf(drive->display, sizeof(drive->display), "%s (%s) [%s]", what, drive->path,
		rf_size_to_human(drive->size, true));
	return drive->size != 0;
}

static int compare_drives(const void* a, const void* b)
{
	return strcmp(((const rf_drive_t*)a)->name, ((const rf_drive_t*)b)->name);
}

int rf_drive_list(rf_drive_t** drives, bool list_non_removable)
{
	DIR* d = opendir("/sys/block");
	struct dirent* de;
	mount_entry_t* mounts = NULL;
	int nb_mounts, n = 0, size = 0;

	*drives = NULL;
	if (d == NULL)
		return 0;
	nb_mounts = list_mounts(&mounts);
	while ((de = readdir(d)) != NULL) {
		rf_drive_t drive;
		if (de->d_name[0] == '.')
			continue;
		/* Skip virtual and optical devices */
		if (strncmp(de->d_name, "ram", 3) == 0 || strncmp(de->d_name, "zram", 4) == 0 ||
			strncmp(de->d_name, "dm-", 3) == 0 || strncmp(de->d_name, "md", 2) == 0 ||
			strncmp(de->d_name, "sr", 2) == 0 || strncmp(de->d_name, "nbd", 3) == 0 ||
			strncmp(de->d_name, "fd", 2) == 0 || strncmp(de->d_name, "zd", 2) == 0)
			continue;
		if (strncmp(de->d_name, "loop", 4) == 0) {
			char backing[PATH_MAX], sysdir[PATH_MAX];
			snprintf(sysdir, sizeof(sysdir), "/sys/block/%s", de->d_name);
			/* Only list loop devices with a backing file, and only in "list all" mode */
			if (!list_non_removable || !read_sysfs(sysdir, "loop/backing_file", backing, sizeof(backing)))
				continue;
		}
		if (!fill_drive(de->d_name, &drive, mounts, nb_mounts))
			continue;
		if (drive.is_system || drive.read_only)
			continue;
		/*
		 * Same policy as Windows Rufus: removable USB and SD media by default,
		 * USB HDDs (and loop devices, handy for testing) when asked for.
		 */
		if (!list_non_removable) {
			if (!((drive.is_usb && drive.removable) || strcmp(drive.bus, "mmc") == 0))
				continue;
		} else {
			if (!(drive.is_usb || drive.removable || strcmp(drive.bus, "mmc") == 0 ||
				strcmp(drive.bus, "loop") == 0))
				continue;
		}
		if (n >= size) {
			size = (size == 0) ? 8 : size * 2;
			*drives = realloc(*drives, sizeof(rf_drive_t) * (size_t)size);
		}
		(*drives)[n++] = drive;
	}
	closedir(d);
	free(mounts);
	if (n > 1)
		qsort(*drives, (size_t)n, sizeof(rf_drive_t), compare_drives);
	return n;
}

bool rf_drive_get(const char* path, rf_drive_t* drive)
{
	char real[PATH_MAX];
	const char* name;
	mount_entry_t* mounts = NULL;
	int nb_mounts;
	struct stat st;
	char sysdir[PATH_MAX];
	bool r;

	if (realpath(path, real) == NULL || strncmp(real, "/dev/", 5) != 0)
		return false;
	name = &real[5];
	snprintf(sysdir, sizeof(sysdir), "/sys/block/%s", name);
	if (stat(sysdir, &st) != 0)
		return false;   /* Not a whole disk */
	nb_mounts = list_mounts(&mounts);
	r = fill_drive(name, drive, mounts, nb_mounts);
	free(mounts);
	return r;
}

void rf_drive_partition_path(const char* disk_path, int n, char* out, size_t out_size)
{
	size_t len = strlen(disk_path);
	/* Disks whose names end with a digit use a 'p' separator (nvme0n1p1, mmcblk0p1, loop0p1) */
	bool needs_p = (len > 0 && isdigit((unsigned char)disk_path[len - 1]));
	snprintf(out, out_size, "%s%s%d", disk_path, needs_p ? "p" : "", n);
}

bool rf_drive_unmount_all(const char* disk_path)
{
	char real[PATH_MAX], line[1024];
	const char* name;
	mount_entry_t* mounts = NULL;
	int nb_mounts, failures = 0;
	FILE* fd;

	if (realpath(disk_path, real) == NULL)
		return false;
	name = &real[5];

	/* Swap partitions first */
	fd = fopen("/proc/swaps", "r");
	if (fd != NULL) {
		while (fgets(line, sizeof(line), fd) != NULL) {
			char* sp = strchr(line, ' ');
			if (sp == NULL)
				continue;
			*sp = 0;
			if (dev_belongs_to(line, name)) {
				uprintf("Disabling swap on %s", line);
				if (swapoff(line) != 0)
					uprintf("  Could not disable swap: %s", strerror(errno));
			}
		}
		fclose(fd);
	}

	/* Unmount, deepest mount points first (the list is in mount order) */
	nb_mounts = list_mounts(&mounts);
	for (int i = nb_mounts - 1; i >= 0; i--) {
		if (!dev_belongs_to(mounts[i].source, name))
			continue;
		uprintf("Unmounting %s from '%s'", mounts[i].source, mounts[i].target);
		if (umount2(mounts[i].target, 0) != 0) {
			uprintf("  Regular unmount failed (%s) - using lazy unmount", strerror(errno));
			if (umount2(mounts[i].target, MNT_DETACH) != 0) {
				uprintf("  Could not unmount '%s': %s", mounts[i].target, strerror(errno));
				failures++;
				continue;
			}
		}
		/* Clean up udisks-style mount points */
		if (strncmp(mounts[i].target, "/run/media/", 11) == 0 || strncmp(mounts[i].target, "/media/", 7) == 0)
			rmdir(mounts[i].target);
	}
	free(mounts);
	return failures == 0;
}

void rf_drive_inhibit_automount(const char* disk_name, bool inhibit)
{
	char path[PATH_MAX], rule[512], match[64];
	const char* reload[] = { "udevadm", "control", "--reload", NULL };

	if (access("/run/udev", F_OK) != 0)
		return;
	snprintf(path, sizeof(path), "/run/udev/rules.d/90-rufus-%s.rules", disk_name);
	if (inhibit) {
		rf_mkdir_p("/run/udev/rules.d", 0755);
		snprintf(rule, sizeof(rule),
			"# Temporary rule created by Rufus while it writes /dev/%s\n"
			"SUBSYSTEM==\"block\", KERNEL==\"%s*\", ENV{UDISKS_IGNORE}=\"1\", ENV{UDISKS_AUTO}=\"0\"\n",
			disk_name, disk_name);
		if (!rf_write_file(path, rule, strlen(rule)))
			return;
	} else {
		if (unlink(path) != 0)
			return;
	}
	if (rf_which("udevadm", NULL, 0)) {
		char* out = NULL;
		const char* trigger[] = { "udevadm", "trigger", "--action=change", "--subsystem-match=block",
			"--sysname-match", match, NULL };
		snprintf(match, sizeof(match), "%s*", disk_name);
		rf_run_capture(reload, &out);
		free(out);
		out = NULL;
		/* Only retrigger on release, so that udisks picks the new partitions up */
		if (!inhibit) {
			rf_run_capture(trigger, &out);
			free(out);
		}
	}
}

bool rf_drive_reread_partitions(int fd, const char* disk_path, int expected_partitions)
{
	char part[PATH_MAX];
	int i;

	fsync(fd);
	for (i = 0; i < 20; i++) {
		if (ioctl(fd, BLKRRPART, NULL) == 0)
			break;
		if (errno != EBUSY) {
			uprintf("Could not re-read partition table: %s", strerror(errno));
			break;
		}
		usleep(250000);
	}
	if (i == 20)
		uprintf("Partition table re-read failed: device is busy");

	rf_udev_settle();
	if (expected_partitions <= 0)
		return true;
	rf_drive_partition_path(disk_path, expected_partitions, part, sizeof(part));
	for (i = 0; i < 100; i++) {
		if (access(part, F_OK) == 0)
			return true;
		usleep(100000);
	}
	uprintf("Partition '%s' did not appear", part);
	return false;
}

bool rf_drive_hosts_file(const char* disk_path, const char* file_path)
{
	struct stat st;
	char syspath[PATH_MAX], real[PATH_MAX], real_disk[PATH_MAX], needle[96];

	if (stat(file_path, &st) != 0 || realpath(disk_path, real_disk) == NULL)
		return false;
	snprintf(syspath, sizeof(syspath), "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
	if (realpath(syspath, real) == NULL)
		return false;
	snprintf(needle, sizeof(needle), "/block/%s/", &real_disk[5]);
	if (strstr(real, needle) != NULL)
		return true;
	snprintf(needle, sizeof(needle), "/block/%s", &real_disk[5]);
	return rf_str_isuffix(real, needle);
}
