#!/bin/bash
# Rufus for Linux: end-to-end tests on loop devices (never touches real drives).
#
#   sudo tests/loop-test.sh [ISO_DIR]
#
# ISO_DIR is scanned for the optional test images used below (archbang*.iso,
# omarchy*.iso, tails*.img); the tests whose image is missing are skipped.
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)
CLI="$HERE/rufus-cli"
ISO_DIR=${1:-/run/media/${SUDO_USER:-$USER}/Ventoy}
WORK=$(mktemp -d /var/tmp/rufus-test.XXXXXX)
LOOP=""
PASS=0
FAIL=0
SKIP=0
FAILED=()
SHOTS=/var/tmp/rufus-shots
BOOT_MODES=""
rm -rf "$SHOTS"

[ "$(id -u)" = 0 ] || { echo "Run as root (sudo $0)"; exit 1; }
[ -x "$CLI" ] || { echo "Build rufus-cli first (make cli)"; exit 1; }

# Keep udisks automounters (udiskie, desktop file managers) away from our loop devices.
# Rufus only blocks automount while it writes; once it's done, the new partitions would
# otherwise be mounted read-write behind our back and break the checks below.
RULE=/run/udev/rules.d/89-rufus-loop-test.rules

cleanup() {
	[ -n "$LOOP" ] && { umount "$WORK"/mnt 2>/dev/null; umount "$LOOP"p* 2>/dev/null; losetup -d "$LOOP" 2>/dev/null; }
	rm -rf "$WORK"
	rm -f "$RULE"
	udevadm control --reload
}
trap cleanup EXIT
trap 'echo; echo "Interrupted"; exit 130' INT TERM
mkdir -p "$WORK/mnt" /run/udev/rules.d
echo 'KERNEL=="loop*", ENV{UDISKS_IGNORE}="1", ENV{UDISKS_AUTO}="0"' > "$RULE"
udevadm control --reload

find_img() { ls "$ISO_DIR"/$1 2>/dev/null | head -1; }

new_loop() {
	# $1 = size
	rm -f "$WORK/disk.img"
	truncate -s "$1" "$WORK/disk.img"
	LOOP=$(losetup -fP --show "$WORK/disk.img")
}

free_loop() {
	umount "$WORK"/mnt 2>/dev/null
	umount "$LOOP"p* 2>/dev/null
	losetup -d "$LOOP"
	LOOP=""
}

check() {
	# $1 = description, rest = command
	local desc=$1; shift
	if "$@" >/dev/null 2>&1; then
		echo "    ok   $desc"
	else
		echo "    FAIL $desc"
		CUR_FAIL=1
	fi
}

part() { echo "${LOOP}p$1"; }
fstype() { blkid -p -o value -s TYPE "$(part "$1")"; }
label() { blkid -p -o value -s LABEL "$(part "$1")"; }
pttype() { blkid -p -o value -s PTTYPE "$LOOP"; }
has_file() { mount -o ro "$(part "$1")" "$WORK/mnt" && { test -e "$WORK/mnt/$2"; local r=$?; umount "$WORK/mnt"; return $r; }; }
mbr_sig() { [ "$(dd if="$LOOP" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' ')" = "55aa" ]; }
pbr_sig() { [ "$(dd if="$(part 1)" bs=1 skip=510 count=2 2>/dev/null | od -An -tx1 | tr -d ' ')" = "55aa" ]; }
active() { [ "$(dd if="$LOOP" bs=1 skip=446 count=1 2>/dev/null | od -An -tx1 | tr -d ' ')" = "80" ]; }
fsck_ok() { case "$(fstype 1)" in vfat) fsck.vfat -n "$(part 1)";; exfat) fsck.exfat -n "$(part 1)";; ext*) e2fsck -fn "$(part 1)";; *) true;; esac; }

run_test() {
	# $1 = name, $2 = disk size, $3 = verification function, rest = rufus-cli write args
	local name=$1 size=$2 verify=$3; shift 3
	echo "=== $name"
	new_loop "$size"
	CUR_FAIL=0
	if ! "$CLI" write -y -d "$LOOP" "$@" > "$WORK/$name.log" 2>&1; then
		echo "    FAIL rufus-cli returned an error (last lines of the log below)"
		tail -15 "$WORK/$name.log" | sed 's/^/         /'
		CUR_FAIL=1
	else
		udevadm settle
		$verify
	fi
	if [ $CUR_FAIL = 0 ]; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); FAILED+=("$name"); fi
	cp "$WORK/$name.log" /var/tmp/rufus-test-$name.log 2>/dev/null
	free_loop
	# Boot what we wrote in QEMU, and keep screenshots for review
	if [ $CUR_FAIL = 0 ] && [ -n "$BOOT_MODES" ] && command -v qemu-system-x86_64 >/dev/null; then
		"$HERE/tests/boot-test.sh" "$WORK/disk.img" "$name" "$SHOTS" $BOOT_MODES
	fi
	BOOT_MODES=""
}

skip() { echo "=== $1"; echo "    skip ($2)"; SKIP=$((SKIP + 1)); }

# --------------------------------------------------------------------------

v_fat32() {
	check "MBR partition table" test "$(pttype)" = dos
	check "FAT32 file system" test "$(fstype 1)" = vfat
	check "label TESTFAT" test "$(label 1)" = TESTFAT
	check "fsck clean" fsck_ok
}
run_test nonboot-fat32 2G v_fat32 -b none -f fat32 -l testfat

v_exfat_gpt() {
	check "GPT partition table" test "$(pttype)" = gpt
	check "exFAT file system" test "$(fstype 1)" = exfat
	check "label" test "$(label 1)" = "My Data"
	check "fsck clean" fsck_ok
}
command -v mkfs.exfat >/dev/null && run_test nonboot-exfat-gpt 2G v_exfat_gpt -b none -p gpt -f exfat -l "My Data" \
	|| skip nonboot-exfat-gpt "no mkfs.exfat"

v_ext4() {
	check "ext4 file system" test "$(fstype 1)" = ext4
	check "label" test "$(label 1)" = "linux-data"
	check "fsck clean" fsck_ok
}
run_test nonboot-ext4 1G v_ext4 -b none -f ext4 -l linux-data

v_freedos() {
	check "MBR boot signature" mbr_sig
	check "partition is active" active
	check "FAT32 file system" test "$(fstype 1)" = vfat
	check "PBR boot signature" pbr_sig
	check "KERNEL.SYS" has_file 1 KERNEL.SYS
	check "COMMAND.COM" has_file 1 COMMAND.COM
	check "AUTOEXEC.BAT" has_file 1 AUTOEXEC.BAT
	check "fsck clean" fsck_ok
}
BOOT_MODES="bios"
run_test freedos 1G v_freedos -b freedos

v_syslinux6() {
	check "partition is active" active
	check "ldlinux.sys" has_file 1 ldlinux.sys
	check "ldlinux.c32" has_file 1 ldlinux.c32
	check "fsck clean" fsck_ok
}
BOOT_MODES="bios"
run_test syslinux6 1G v_syslinux6 -b syslinux6

ARCH=$(find_img 'archbang*.iso')
if [ -n "$ARCH" ]; then
	v_arch() {
		check "MBR partition table" test "$(pttype)" = dos
		check "partition is active" active
		check "FAT32 file system" test "$(fstype 1)" = vfat
		check "label from ISO" test "$(label 1)" = ARCHBANG_18
		check "/syslinux.cfg" has_file 1 syslinux.cfg
		check "ldlinux.sys installed" has_file 1 boot/syslinux/ldlinux.sys
		check "EFI bootloader" has_file 1 EFI/BOOT/BOOTX64.EFI
		check "fsck clean" fsck_ok
	}
	BOOT_MODES="bios uefi"
	run_test iso-syslinux 4G v_arch -i "$ARCH" -m iso

	v_arch_persist() {
		v_arch
		check "persistence partition ext4" test "$(fstype 2)" = ext4
		check "persistence label" test "$(label 2)" = persistence
		check "persistence.conf" has_file 2 persistence.conf
	}
	BOOT_MODES="bios uefi"
	run_test iso-persistence 6G v_arch_persist -i "$ARCH" -m iso -P 1G

	v_arch_gpt() {
		check "GPT partition table" test "$(pttype)" = gpt
		check "EFI bootloader" has_file 1 EFI/BOOT/BOOTX64.EFI
		check "fsck clean" fsck_ok
	}
	BOOT_MODES="uefi"
	run_test iso-gpt-uefi 4G v_arch_gpt -i "$ARCH" -m iso -p gpt -t uefi

	v_arch_dd() {
		check "image written verbatim" cmp -n "$(stat -c %s "$ARCH")" "$ARCH" "$LOOP"
	}
	BOOT_MODES="bios uefi"
	run_test iso-dd 4G v_arch_dd -i "$ARCH" -m dd --verify
else
	skip "iso-*" "no archbang*.iso in $ISO_DIR"
fi

OMA=$(find_img 'omarchy*.iso')
if [ -n "$OMA" ] && command -v mkfs.exfat >/dev/null; then
	v_oma() {
		check "exFAT main partition" test "$(fstype 1)" = exfat
		check "UEFI:NTFS partition" test "$(label 2)" = RUFUS_BOOT
		check "large file copied" has_file 1 arch/x86_64/airootfs.sfs
		check "fsck clean" fsck_ok
	}
	BOOT_MODES="uefi"
	run_test iso-4gb-uefi-ntfs 10G v_oma -i "$OMA" -m iso
else
	skip iso-4gb-uefi-ntfs "no omarchy*.iso or mkfs.exfat"
fi
if [ -n "$OMA" ] && command -v mkfs.ntfs >/dev/null; then
	v_oma_ntfs() {
		check "NTFS main partition" test "$(fstype 1)" = ntfs
		check "UEFI:NTFS partition" test "$(label 2)" = RUFUS_BOOT
		check "large file copied" has_file 1 arch/x86_64/airootfs.sfs
	}
	BOOT_MODES="uefi"
	run_test iso-4gb-ntfs 10G v_oma_ntfs -i "$OMA" -m iso -f ntfs
else
	skip iso-4gb-ntfs "no omarchy*.iso or mkfs.ntfs (ntfsprogs)"
fi

TAILS=$(find_img 'tails*.img')
if [ -n "$TAILS" ]; then
	v_tails() { check "image written verbatim" cmp -n "$(stat -c %s "$TAILS")" "$TAILS" "$LOOP"; }
	BOOT_MODES="bios uefi"
	run_test dd-img 8G v_tails -i "$TAILS"
else
	skip dd-img "no tails*.img"
fi

echo
echo "Passed: $PASS  Failed: $FAIL  Skipped: $SKIP"
[ $FAIL = 0 ] || echo "Failed: ${FAILED[*]}"
echo "Logs: /var/tmp/rufus-test-*.log   Boot screenshots: $SHOTS"
[ -n "${SUDO_USER:-}" ] && chown "$SUDO_USER" /var/tmp/rufus-test-*.log 2>/dev/null
[ $FAIL = 0 ]
