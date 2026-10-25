#!/bin/bash
# Boot a disk image in QEMU (BIOS and/or UEFI) and take screenshots of what comes up.
#
#   tests/boot-test.sh DISK_IMAGE NAME OUT_DIR [bios] [uefi]
#
# The disk is opened with snapshot=on, so it is never modified. Screenshots are
# taken at a few points in time, to catch the boot menu as well as what follows.
set -u

DISK=$1
NAME=$2
OUT=$3
shift 3
MODES=${*:-bios uefi}
SHOTS="4 10 25"
OVMF_CODE=/usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS=/usr/share/edk2/x64/OVMF_VARS.4m.fd

command -v qemu-system-x86_64 >/dev/null || { echo "qemu-system-x86_64 not found"; exit 1; }
mkdir -p "$OUT"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

ACCEL=()
[ -w /dev/kvm ] && ACCEL=(-enable-kvm -cpu host)

for mode in $MODES; do
	FW=()
	if [ "$mode" = uefi ]; then
		[ -r "$OVMF_CODE" ] || { echo "    skip uefi boot (no OVMF)"; continue; }
		cp "$OVMF_VARS" "$TMP/vars.fd"
		FW=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" -drive "if=pflash,format=raw,file=$TMP/vars.fd")
	fi
	SOCK="$TMP/mon-$mode"
	qemu-system-x86_64 "${ACCEL[@]}" -machine q35 -m 2048 -smp 2 "${FW[@]}" \
		-drive "file=$DISK,format=raw,if=none,id=usb,snapshot=on" \
		-device qemu-xhci -device usb-storage,drive=usb,bootindex=0 \
		-display none -vga std -monitor "unix:$SOCK,server,nowait" -nic none &
	PID=$!
	elapsed=0
	for t in $SHOTS; do
		sleep $((t - elapsed))
		elapsed=$t
		kill -0 $PID 2>/dev/null || break
		echo "screendump $OUT/$NAME-$mode-${t}s.png -f png" | socat - "UNIX-CONNECT:$SOCK" >/dev/null
	done
	echo "quit" | socat - "UNIX-CONNECT:$SOCK" >/dev/null 2>&1
	wait $PID 2>/dev/null
	echo "    shots $OUT/$NAME-$mode-*.png"
done
[ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER" "$OUT"
exit 0
