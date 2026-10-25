/* Generated: embedded Rufus resources */
/* Assemble with -Wa,-I<rufus>/res so that .incbin finds the files */
	.section .rodata

	.global rfres_ldlinux_v4_sys
	.global rfres_ldlinux_v4_sys_end
	.balign 16
rfres_ldlinux_v4_sys:
	.incbin "syslinux/ldlinux_v4.sys"
rfres_ldlinux_v4_sys_end:
	.byte 0

	.global rfres_ldlinux_v4_bss
	.global rfres_ldlinux_v4_bss_end
	.balign 16
rfres_ldlinux_v4_bss:
	.incbin "syslinux/ldlinux_v4.bss"
rfres_ldlinux_v4_bss_end:
	.byte 0

	.global rfres_ldlinux_v6_sys
	.global rfres_ldlinux_v6_sys_end
	.balign 16
rfres_ldlinux_v6_sys:
	.incbin "syslinux/ldlinux_v6.sys"
rfres_ldlinux_v6_sys_end:
	.byte 0

	.global rfres_ldlinux_v6_bss
	.global rfres_ldlinux_v6_bss_end
	.balign 16
rfres_ldlinux_v6_bss:
	.incbin "syslinux/ldlinux_v6.bss"
rfres_ldlinux_v6_bss_end:
	.byte 0

	.global rfres_mboot_c32
	.global rfres_mboot_c32_end
	.balign 16
rfres_mboot_c32:
	.incbin "syslinux/mboot.c32"
rfres_mboot_c32_end:
	.byte 0

	.global rfres_grldr_mbr
	.global rfres_grldr_mbr_end
	.balign 16
rfres_grldr_mbr:
	.incbin "grub/grldr.mbr"
rfres_grldr_mbr_end:
	.byte 0

	.global rfres_core_img
	.global rfres_core_img_end
	.balign 16
rfres_core_img:
	.incbin "grub2/core.img"
rfres_core_img_end:
	.byte 0

	.global rfres_msg_txt
	.global rfres_msg_txt_end
	.balign 16
rfres_msg_txt:
	.incbin "mbr/msg.txt"
rfres_msg_txt_end:
	.byte 0

	.global rfres_uefi_ntfs_img
	.global rfres_uefi_ntfs_img_end
	.balign 16
rfres_uefi_ntfs_img:
	.incbin "uefi/uefi-ntfs.img"
rfres_uefi_ntfs_img_end:
	.byte 0

	.global rfres_freedos_COMMAND_COM
	.global rfres_freedos_COMMAND_COM_end
	.balign 16
rfres_freedos_COMMAND_COM:
	.incbin "freedos/COMMAND.COM"
rfres_freedos_COMMAND_COM_end:
	.byte 0

	.global rfres_freedos_KERNEL_SYS
	.global rfres_freedos_KERNEL_SYS_end
	.balign 16
rfres_freedos_KERNEL_SYS:
	.incbin "freedos/KERNEL.SYS"
rfres_freedos_KERNEL_SYS_end:
	.byte 0

	.global rfres_freedos_DISPLAY_EXE
	.global rfres_freedos_DISPLAY_EXE_end
	.balign 16
rfres_freedos_DISPLAY_EXE:
	.incbin "freedos/DISPLAY.EXE"
rfres_freedos_DISPLAY_EXE_end:
	.byte 0

	.global rfres_freedos_KEYB_EXE
	.global rfres_freedos_KEYB_EXE_end
	.balign 16
rfres_freedos_KEYB_EXE:
	.incbin "freedos/KEYB.EXE"
rfres_freedos_KEYB_EXE_end:
	.byte 0

	.global rfres_freedos_MODE_COM
	.global rfres_freedos_MODE_COM_end
	.balign 16
rfres_freedos_MODE_COM:
	.incbin "freedos/MODE.COM"
rfres_freedos_MODE_COM_end:
	.byte 0

	.global rfres_freedos_KEYBOARD_SYS
	.global rfres_freedos_KEYBOARD_SYS_end
	.balign 16
rfres_freedos_KEYBOARD_SYS:
	.incbin "freedos/KEYBOARD.SYS"
rfres_freedos_KEYBOARD_SYS_end:
	.byte 0

	.global rfres_freedos_KEYBRD2_SYS
	.global rfres_freedos_KEYBRD2_SYS_end
	.balign 16
rfres_freedos_KEYBRD2_SYS:
	.incbin "freedos/KEYBRD2.SYS"
rfres_freedos_KEYBRD2_SYS_end:
	.byte 0

	.global rfres_freedos_KEYBRD3_SYS
	.global rfres_freedos_KEYBRD3_SYS_end
	.balign 16
rfres_freedos_KEYBRD3_SYS:
	.incbin "freedos/KEYBRD3.SYS"
rfres_freedos_KEYBRD3_SYS_end:
	.byte 0

	.global rfres_freedos_KEYBRD4_SYS
	.global rfres_freedos_KEYBRD4_SYS_end
	.balign 16
rfres_freedos_KEYBRD4_SYS:
	.incbin "freedos/KEYBRD4.SYS"
rfres_freedos_KEYBRD4_SYS_end:
	.byte 0

	.global rfres_freedos_EGA_CPX
	.global rfres_freedos_EGA_CPX_end
	.balign 16
rfres_freedos_EGA_CPX:
	.incbin "freedos/EGA.CPX"
rfres_freedos_EGA_CPX_end:
	.byte 0

	.global rfres_freedos_EGA2_CPX
	.global rfres_freedos_EGA2_CPX_end
	.balign 16
rfres_freedos_EGA2_CPX:
	.incbin "freedos/EGA2.CPX"
rfres_freedos_EGA2_CPX_end:
	.byte 0

	.global rfres_freedos_EGA3_CPX
	.global rfres_freedos_EGA3_CPX_end
	.balign 16
rfres_freedos_EGA3_CPX:
	.incbin "freedos/EGA3.CPX"
rfres_freedos_EGA3_CPX_end:
	.byte 0

	.global rfres_freedos_EGA4_CPX
	.global rfres_freedos_EGA4_CPX_end
	.balign 16
rfres_freedos_EGA4_CPX:
	.incbin "freedos/EGA4.CPX"
rfres_freedos_EGA4_CPX_end:
	.byte 0

	.global rfres_freedos_EGA5_CPX
	.global rfres_freedos_EGA5_CPX_end
	.balign 16
rfres_freedos_EGA5_CPX:
	.incbin "freedos/EGA5.CPX"
rfres_freedos_EGA5_CPX_end:
	.byte 0

	.global rfres_freedos_EGA6_CPX
	.global rfres_freedos_EGA6_CPX_end
	.balign 16
rfres_freedos_EGA6_CPX:
	.incbin "freedos/EGA6.CPX"
rfres_freedos_EGA6_CPX_end:
	.byte 0

	.global rfres_freedos_EGA7_CPX
	.global rfres_freedos_EGA7_CPX_end
	.balign 16
rfres_freedos_EGA7_CPX:
	.incbin "freedos/EGA7.CPX"
rfres_freedos_EGA7_CPX_end:
	.byte 0

	.global rfres_freedos_EGA8_CPX
	.global rfres_freedos_EGA8_CPX_end
	.balign 16
rfres_freedos_EGA8_CPX:
	.incbin "freedos/EGA8.CPX"
rfres_freedos_EGA8_CPX_end:
	.byte 0

	.global rfres_freedos_EGA9_CPX
	.global rfres_freedos_EGA9_CPX_end
	.balign 16
rfres_freedos_EGA9_CPX:
	.incbin "freedos/EGA9.CPX"
rfres_freedos_EGA9_CPX_end:
	.byte 0

	.global rfres_freedos_EGA10_CPX
	.global rfres_freedos_EGA10_CPX_end
	.balign 16
rfres_freedos_EGA10_CPX:
	.incbin "freedos/EGA10.CPX"
rfres_freedos_EGA10_CPX_end:
	.byte 0

	.global rfres_freedos_EGA11_CPX
	.global rfres_freedos_EGA11_CPX_end
	.balign 16
rfres_freedos_EGA11_CPX:
	.incbin "freedos/EGA11.CPX"
rfres_freedos_EGA11_CPX_end:
	.byte 0

	.global rfres_freedos_EGA12_CPX
	.global rfres_freedos_EGA12_CPX_end
	.balign 16
rfres_freedos_EGA12_CPX:
	.incbin "freedos/EGA12.CPX"
rfres_freedos_EGA12_CPX_end:
	.byte 0

	.global rfres_freedos_EGA13_CPX
	.global rfres_freedos_EGA13_CPX_end
	.balign 16
rfres_freedos_EGA13_CPX:
	.incbin "freedos/EGA13.CPX"
rfres_freedos_EGA13_CPX_end:
	.byte 0

	.global rfres_freedos_EGA14_CPX
	.global rfres_freedos_EGA14_CPX_end
	.balign 16
rfres_freedos_EGA14_CPX:
	.incbin "freedos/EGA14.CPX"
rfres_freedos_EGA14_CPX_end:
	.byte 0

	.global rfres_freedos_EGA15_CPX
	.global rfres_freedos_EGA15_CPX_end
	.balign 16
rfres_freedos_EGA15_CPX:
	.incbin "freedos/EGA15.CPX"
rfres_freedos_EGA15_CPX_end:
	.byte 0

	.global rfres_freedos_EGA16_CPX
	.global rfres_freedos_EGA16_CPX_end
	.balign 16
rfres_freedos_EGA16_CPX:
	.incbin "freedos/EGA16.CPX"
rfres_freedos_EGA16_CPX_end:
	.byte 0

	.global rfres_freedos_EGA17_CPX
	.global rfres_freedos_EGA17_CPX_end
	.balign 16
rfres_freedos_EGA17_CPX:
	.incbin "freedos/EGA17.CPX"
rfres_freedos_EGA17_CPX_end:
	.byte 0

	.global rfres_freedos_EGA18_CPX
	.global rfres_freedos_EGA18_CPX_end
	.balign 16
rfres_freedos_EGA18_CPX:
	.incbin "freedos/EGA18.CPX"
rfres_freedos_EGA18_CPX_end:
	.byte 0

	.section .note.GNU-stack,"",%progbits
