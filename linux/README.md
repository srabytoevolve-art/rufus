# Rufus for Linux

A native Linux port of [Rufus](https://rufus.ie). It provides a GTK 4 interface (`rufus`) and a
command-line tool (`rufus-cli`). The CLI also serves as the privileged helper the GUI starts
through `pkexec`.

The port reuses the portable parts of the Windows tree directly: ms-sys (boot records), the
Syslinux installer and libfat, and the embedded resources in `../res` (Syslinux, GRUB, FreeDOS,
UEFI:NTFS). The Win32-specific layers are reimplemented on Linux APIs:

| Windows Rufus                      | Linux port                                      |
|------------------------------------|-------------------------------------------------|
| SetupAPI / VDS drive enumeration   | sysfs (`/sys/block`), mountinfo                 |
| DeviceIoControl partitioning       | MBR/GPT written directly (`core/partition.c`)   |
| fmifs / VDS formatting             | `mkfs.fat`, `mkfs.exfat`, `mkfs.ntfs`, `mkfs.ext4` |
| Volume mount points                | `mount(8)` on temporary directories             |
| libcdio extraction                 | kernel loop mount of the ISO + file copy        |
| WinHTTP downloads                  | libcurl (signed files from rufus.ie, verified)  |
| Automount suppression              | temporary udev rule (`UDISKS_IGNORE`)           |

## Features

- ISO mode (file copy) and DD mode (raw write) for ISOHybrid and disk images.
- Compressed images: `.gz`, `.xz`, `.zst`, `.bz2`, `.lzma`. Fixed `.vhd` is also supported.
- MBR or GPT; BIOS, UEFI or both.
- FAT16, FAT32, exFAT, NTFS (with ntfsprogs), ext2/3/4.
- Boot records matching Windows Rufus: Rufus MBR, Syslinux, GRUB 2, Grub4DOS, ms-sys PBRs.
- Syslinux 4/6, with the version-matched `ldlinux.sys` downloaded the way Windows Rufus does it.
- GRUB 2 `core.img` matching the ISO.
- UEFI:NTFS partition for NTFS/exFAT media, so UEFI can boot files larger than 4 GB.
- Splitting `install.wim` for FAT32 when wimlib is installed.
- The same config fixups Windows Rufus applies to ISOs.
- FreeDOS, Syslinux and GRUB "boot selection" modes.
- Persistence for Ubuntu (casper) and Debian live media.
- Windows customization through `autounattend.xml`: TPM/Secure Boot/RAM bypass, local account,
  no online account, privacy, BitLocker, locale.
- Bad block check, write verification, MD5/SHA1/SHA256/SHA512, saving a drive to an image.

## Building

Dependencies (Arch package names): `gtk4 libcdio curl openssl zlib xz zstd bzip2`

At runtime: `dosfstools`, `exfatprogs`, `e2fsprogs`, `util-linux`, `polkit`. Optional:
`ntfsprogs` + `ntfs-3g` (NTFS) and `wimlib` (splitting install.wim for FAT32).

```sh
make            # builds ./rufus and ./rufus-cli
make cli        # CLI only, no GTK needed
sudo make install
```

`make install` also installs a polkit policy, so the GUI's `pkexec` prompt is specific to Rufus.
Without it, the GUI still works when run from the build directory, with a generic
"run as root" prompt.

## Command line

```sh
rufus-cli list [--all]                         # removable drives (--all adds USB HDDs and loop devices)
rufus-cli info image.iso                       # what Rufus sees in an image
sudo rufus-cli write -d /dev/sdX -i image.iso  # sensible defaults, like the GUI
sudo rufus-cli write -d /dev/sdX -i ubuntu.iso -P 4G
sudo rufus-cli write -d /dev/sdX -i Win11.iso --wue-bypass --wue-no-account --wue-user me
sudo rufus-cli write -d /dev/sdX -b freedos
rufus-cli write -d /dev/sdX -i image.iso --dry-run   # show the plan without writing
```

Run `rufus-cli --help` for every option.

## Testing

`tests/loop-test.sh` runs complete write scenarios against loop devices backed by sparse files.
It never touches real drives, and checks partition tables, file systems, boot sectors and copied
files. When QEMU and OVMF are installed, it also boots every bootable result in BIOS and UEFI mode
(`tests/boot-test.sh`) and saves screenshots to `/var/tmp/rufus-shots`:

```sh
sudo tests/loop-test.sh /path/to/iso/dir
```

## Known limitations

- Syslinux is only installed on FAT. A Syslinux ISO that needs NTFS/exFAT, because it contains a
  file over 4 GB, gets UEFI boot only.
- Windows To Go, ReFS/UDF formatting, Windows 7 UEFI (`bootmgfw.efi` extraction from
  `install.wim`) and XP setup patching are not implemented.
- The exFAT UEFI:NTFS driver is not Secure Boot signed. Use NTFS (install ntfsprogs) when Secure
  Boot must stay enabled.
