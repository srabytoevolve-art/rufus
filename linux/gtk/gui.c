/*
 * Rufus for Linux: GTK 4 user interface
 * Copyright © 2011-2026 Pete Batard <pete@akeo.ie>
 * Linux port © 2026 Rufus for Linux contributors
 *
 * The interface runs unprivileged. The actual write is performed by
 * rufus-cli, started through pkexec, which reports back on its stdout.
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
#include <limits.h>
#include <unistd.h>
#include <gtk/gtk.h>

#include "rufus.h"

#ifndef LIBEXECDIR
#define LIBEXECDIR "/usr/local/libexec"
#endif

/* Boot selection entries */
enum { BOOT_IDX_IMAGE = 0, BOOT_IDX_FREEDOS, BOOT_IDX_NONE, BOOT_IDX_SYSLINUX4, BOOT_IDX_SYSLINUX6, BOOT_IDX_GRUB2 };
static const rf_boot_type_t boot_idx_to_type[] = { BT_IMAGE, BT_FREEDOS, BT_NON_BOOTABLE, BT_SYSLINUX_V4, BT_SYSLINUX_V6, BT_GRUB2 };

#define MAX_CHOICES 16

typedef struct {
	int values[MAX_CHOICES];
	int count;
} choices_t;

typedef struct {
	GtkApplication* app;
	GtkWidget* window;
	GtkWidget* device_dd;
	GtkWidget* boot_dd;
	GtkStringList* boot_list;
	GtkWidget* select_btn;
	GtkWidget* image_mode_row;
	GtkWidget* image_mode_dd;
	GtkWidget* persist_row;
	GtkWidget* persist_scale;
	GtkWidget* persist_value;
	GtkWidget* scheme_dd;
	GtkWidget* target_dd;
	GtkWidget* label_entry;
	GtkWidget* fs_dd;
	GtkWidget* cluster_dd;
	GtkWidget* list_hdd_check;
	GtkWidget* rufus_mbr_check;
	GtkWidget* verify_check;
	GtkWidget* quick_check;
	GtkWidget* badblocks_check;
	GtkWidget* badblocks_dd;
	GtkWidget* progress;
	GtkWidget* status;
	GtkWidget* start_btn;
	GtkWidget* close_btn;
	GtkWidget* tool_box;
	GtkWidget* log_window;
	GtkTextBuffer* log_buf;

	rf_drive_t* drives;
	int nb_drives;
	choices_t scheme_choices, target_choices, fs_choices, cluster_choices;

	rf_image_t* img;
	bool analyzing;
	bool busy;
	bool updating;
	int wue_flags;
	char wue_user[64];
	char wue_locale[32];

	GSubprocess* proc;
	GDataInputStream* proc_out;
	GOutputStream* proc_in;
	bool got_result;
	bool result_ok;
	bool result_cancelled;
	uint64_t op_start;
} app_t;

static app_t A = { 0 };

static void update_controls(void);
static void refresh_devices(bool force);
static void append_log(const char* msg);

/* ---------------------------------------------------------------------- */
/* Logging (core callbacks may come from worker threads)                  */
/* ---------------------------------------------------------------------- */

static gboolean log_idle(gpointer data)
{
	append_log((const char*)data);
	g_free(data);
	return G_SOURCE_REMOVE;
}

static void core_log_cb(const char* msg, void* ctx)
{
	(void)ctx;
	fprintf(stderr, "%s\n", msg);
	g_idle_add(log_idle, g_strdup(msg));
}

static void append_log(const char* msg)
{
	GtkTextIter end;
	gtk_text_buffer_get_end_iter(A.log_buf, &end);
	gtk_text_buffer_insert(A.log_buf, &end, msg, -1);
	gtk_text_buffer_get_end_iter(A.log_buf, &end);
	gtk_text_buffer_insert(A.log_buf, &end, "\n", 1);
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                */
/* ---------------------------------------------------------------------- */

static void set_status(const char* text, double fraction)
{
	if (fraction < 0.0) {
		gtk_progress_bar_pulse(GTK_PROGRESS_BAR(A.progress));
	} else {
		gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(A.progress), fraction);
	}
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(A.progress), text);
}

static GtkWidget* section_header(const char* title)
{
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget* label = gtk_label_new(NULL);
	char* markup = g_markup_printf_escaped("<span size='x-large' weight='bold'>%s</span>", title);
	GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

	gtk_label_set_markup(GTK_LABEL(label), markup);
	g_free(markup);
	gtk_widget_set_valign(sep, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(sep, TRUE);
	gtk_box_append(GTK_BOX(box), label);
	gtk_box_append(GTK_BOX(box), sep);
	gtk_widget_set_margin_top(box, 8);
	return box;
}

static GtkWidget* field(const char* title, GtkWidget* widget)
{
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
	GtkWidget* label = gtk_label_new(title);
	gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
	gtk_box_append(GTK_BOX(box), label);
	gtk_box_append(GTK_BOX(box), widget);
	gtk_widget_set_hexpand(box, TRUE);
	return box;
}

static void set_dd_items(GtkWidget* dd, const char* const* items, int selected)
{
	GtkStringList* list = gtk_string_list_new(items);
	A.updating = true;
	gtk_drop_down_set_model(GTK_DROP_DOWN(dd), G_LIST_MODEL(list));
	g_object_unref(list);
	if (selected >= 0)
		gtk_drop_down_set_selected(GTK_DROP_DOWN(dd), (guint)selected);
	A.updating = false;
}

static int dd_selected(GtkWidget* dd)
{
	guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(dd));
	return (sel == GTK_INVALID_LIST_POSITION) ? -1 : (int)sel;
}

static int choice_value(GtkWidget* dd, const choices_t* c, int def)
{
	int sel = dd_selected(dd);
	return (sel >= 0 && sel < c->count) ? c->values[sel] : def;
}

static int choice_index(const choices_t* c, int value)
{
	for (int i = 0; i < c->count; i++)
		if (c->values[i] == value)
			return i;
	return 0;
}

static const rf_drive_t* selected_drive(void)
{
	int sel = dd_selected(A.device_dd);
	if (sel < 0 || sel >= A.nb_drives)
		return NULL;
	return &A.drives[sel];
}

static rf_boot_type_t selected_boot(void)
{
	int sel = dd_selected(A.boot_dd);
	if (sel < 0 || sel >= (int)G_N_ELEMENTS(boot_idx_to_type))
		return BT_IMAGE;
	return boot_idx_to_type[sel];
}

static bool image_mode_dd(void)
{
	return gtk_widget_get_visible(A.image_mode_row) && dd_selected(A.image_mode_dd) == 1;
}

static void message(GtkMessageType type, const char* title, const char* detail)
{
	GtkAlertDialog* dlg = gtk_alert_dialog_new("%s", title);
	(void)type;
	if (detail != NULL)
		gtk_alert_dialog_set_detail(dlg, detail);
	gtk_alert_dialog_show(dlg, GTK_WINDOW(A.window));
	g_object_unref(dlg);
}

/* ---------------------------------------------------------------------- */
/* Option population (mirrors Windows Rufus' logic)                       */
/* ---------------------------------------------------------------------- */

static void build_options(rf_options_t* opts)
{
	const rf_drive_t* drive = selected_drive();
	rf_options_init(opts);
	if (drive != NULL)
		snprintf(opts->device, sizeof(opts->device), "%s", drive->path);
	opts->boot_type = selected_boot();
	if (opts->boot_type == BT_IMAGE && A.img != NULL)
		snprintf(opts->image, sizeof(opts->image), "%s", A.img->path);
	opts->image_mode = image_mode_dd() ? IM_DD : IM_ISO;
	opts->partition_style = (rf_partition_style_t)choice_value(A.scheme_dd, &A.scheme_choices, PS_MBR);
	opts->target_type = (rf_target_type_t)choice_value(A.target_dd, &A.target_choices, TT_BIOS);
	opts->fs = (rf_fs_t)choice_value(A.fs_dd, &A.fs_choices, FS_FAT32);
	opts->cluster_size = (uint32_t)choice_value(A.cluster_dd, &A.cluster_choices, 0);
	snprintf(opts->label, sizeof(opts->label), "%s", gtk_editable_get_text(GTK_EDITABLE(A.label_entry)));
	opts->quick_format = gtk_check_button_get_active(GTK_CHECK_BUTTON(A.quick_check));
	opts->use_rufus_mbr = gtk_check_button_get_active(GTK_CHECK_BUTTON(A.rufus_mbr_check));
	opts->verify = gtk_check_button_get_active(GTK_CHECK_BUTTON(A.verify_check));
	opts->badblocks_passes = gtk_check_button_get_active(GTK_CHECK_BUTTON(A.badblocks_check)) ?
		dd_selected(A.badblocks_dd) + 1 : 0;
	if (gtk_widget_get_visible(A.persist_row))
		opts->persistence_size = (uint64_t)gtk_range_get_value(GTK_RANGE(A.persist_scale)) * MB;
	opts->unattend_flags = A.wue_flags;
	snprintf(opts->unattend_username, sizeof(opts->unattend_username), "%s", A.wue_user);
	snprintf(opts->unattend_locale, sizeof(opts->unattend_locale), "%s", A.wue_locale);
}

static void populate_clusters(void)
{
	rf_fs_t fs = (rf_fs_t)choice_value(A.fs_dd, &A.fs_choices, FS_FAT32);
	const rf_drive_t* drive = selected_drive();
	uint64_t size = drive ? drive->size : 0;
	static const uint32_t fat_sizes[] = { 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
	static const uint32_t ntfs_sizes[] = { 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152 };
	static const uint32_t exfat_sizes[] = { 4096, 8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576, 2097152, 4194304, 8388608, 16777216, 33554432 };
	static const uint32_t ext_sizes[] = { 1024, 2048, 4096 };
	const uint32_t* sizes = fat_sizes;
	size_t n = G_N_ELEMENTS(fat_sizes);
	const char* items[MAX_CHOICES + 1];
	char labels[MAX_CHOICES][64];
	uint32_t def = rf_default_cluster_size(fs, size);
	int k = 0;

	switch (fs) {
	case FS_NTFS: sizes = ntfs_sizes; n = G_N_ELEMENTS(ntfs_sizes); break;
	case FS_EXFAT: sizes = exfat_sizes; n = G_N_ELEMENTS(exfat_sizes); break;
	case FS_EXT2: case FS_EXT3: case FS_EXT4: sizes = ext_sizes; n = G_N_ELEMENTS(ext_sizes); break;
	default: break;
	}
	A.cluster_choices.count = 0;
	/* "Default" lets the formatting tool decide */
	snprintf(labels[k], sizeof(labels[k]), "%s (Default)", def ? rf_size_to_human(def, false) : "Automatic");
	items[k] = labels[k];
	A.cluster_choices.values[k++] = 0;
	for (size_t i = 0; i < n && k < MAX_CHOICES; i++) {
		if (sizes[i] == def)
			continue;
		if (drive != NULL && sizes[i] < drive->sector_size)
			continue;
		snprintf(labels[k], sizeof(labels[k]), "%s", rf_size_to_human(sizes[i], false));
		items[k] = labels[k];
		A.cluster_choices.values[k++] = (int)sizes[i];
	}
	items[k] = NULL;
	A.cluster_choices.count = k;
	set_dd_items(A.cluster_dd, items, 0);
}

static void populate_fs(rf_fs_t preferred)
{
	rf_boot_type_t bt = selected_boot();
	const rf_drive_t* drive = selected_drive();
	uint64_t size = drive ? drive->size : 0;
	bool allowed[FS_MAX] = { false };
	const char* items[FS_MAX + 1];
	char labels[FS_MAX][32];
	int k = 0;

	switch (bt) {
	case BT_FREEDOS:
	case BT_SYSLINUX_V4:
	case BT_SYSLINUX_V6:
		allowed[FS_FAT16] = allowed[FS_FAT32] = true;
		break;
	case BT_GRUB2:
		allowed[FS_FAT16] = allowed[FS_FAT32] = allowed[FS_NTFS] = allowed[FS_EXFAT] = allowed[FS_EXT2] = true;
		break;
	case BT_IMAGE:
		allowed[FS_FAT16] = allowed[FS_FAT32] = allowed[FS_NTFS] = allowed[FS_EXFAT] = true;
		if (A.img != NULL && HAS_SYSLINUX(A.img) && !HAS_BOOTMGR(A.img))
			allowed[FS_NTFS] = allowed[FS_EXFAT] = false;
		if (A.img != NULL && HAS_SYSLINUX(A.img) && A.img->has_4GB_file)
			allowed[FS_NTFS] = allowed[FS_EXFAT] = true;
		break;
	default:
		for (int i = 0; i < FS_MAX; i++)
			allowed[i] = true;
		break;
	}
	if (size > 4 * GB)
		allowed[FS_FAT16] = false;
	allowed[FS_UDF] = allowed[FS_REFS] = false;

	A.fs_choices.count = 0;
	for (int i = 0; i < FS_MAX; i++) {
		if (!allowed[i] || !rf_fs_available((rf_fs_t)i))
			continue;
		snprintf(labels[k], sizeof(labels[k]), "%s%s", (i == FS_FAT32 && size > 32 * GB) ? "Large " : "", rf_fs_name[i]);
		items[k] = labels[k];
		A.fs_choices.values[k++] = i;
	}
	items[k] = NULL;
	A.fs_choices.count = k;
	set_dd_items(A.fs_dd, items, choice_index(&A.fs_choices, preferred));
	populate_clusters();
}

static void populate_scheme_target(rf_partition_style_t pref_ps, rf_target_type_t pref_tt)
{
	rf_boot_type_t bt = selected_boot();
	const rf_image_t* img = (bt == BT_IMAGE) ? A.img : NULL;
	const char* items[4];
	int k = 0;
	bool allow_gpt = true, allow_mbr = true;

	if (bt == BT_FREEDOS || bt == BT_SYSLINUX_V4 || bt == BT_SYSLINUX_V6 || bt == BT_GRUB2)
		allow_gpt = false;
	if (img != NULL && !IS_EFI_BOOTABLE(img))
		allow_gpt = false;
	if (img != NULL && HAS_SYSLINUX(img) && SL_MAJOR(img->sl_version) < 5 && img->has_4GB_file &&
		!HAS_BOOTMGR(img) && !HAS_WINPE(img) && !HAS_GRUB(img))
		allow_mbr = false;
	{
		const rf_drive_t* drive = selected_drive();
		if (drive != NULL && drive->size > 2 * TB)
			allow_mbr = false;
	}

	A.scheme_choices.count = 0;
	if (allow_mbr) {
		items[k] = "MBR";
		A.scheme_choices.values[k++] = PS_MBR;
	}
	if (allow_gpt) {
		items[k] = "GPT";
		A.scheme_choices.values[k++] = PS_GPT;
	}
	items[k] = NULL;
	A.scheme_choices.count = k;
	set_dd_items(A.scheme_dd, items, choice_index(&A.scheme_choices, pref_ps));

	/* Target system */
	{
		rf_partition_style_t ps = (rf_partition_style_t)choice_value(A.scheme_dd, &A.scheme_choices, PS_MBR);
		const char* titems[3];
		int t = 0;
		A.target_choices.count = 0;
		if (bt == BT_NON_BOOTABLE) {
			titems[t] = "BIOS or UEFI";
			A.target_choices.values[t++] = TT_BIOS;
		} else if (img == NULL) {
			titems[t] = "BIOS (or UEFI-CSM)";
			A.target_choices.values[t++] = TT_BIOS;
		} else if (ps == PS_GPT) {
			titems[t] = "UEFI (non CSM)";
			A.target_choices.values[t++] = TT_UEFI;
		} else if (IS_BIOS_BOOTABLE(img) && IS_EFI_BOOTABLE(img) && !HAS_WINDOWS(img)) {
			titems[t] = "BIOS or UEFI";
			A.target_choices.values[t++] = TT_BIOS;
		} else if (IS_BIOS_BOOTABLE(img)) {
			titems[t] = "BIOS (or UEFI-CSM)";
			A.target_choices.values[t++] = TT_BIOS;
		} else {
			titems[t] = "UEFI (non CSM)";
			A.target_choices.values[t++] = TT_UEFI;
		}
		titems[t] = NULL;
		A.target_choices.count = t;
		set_dd_items(A.target_dd, titems, choice_index(&A.target_choices, pref_tt));
	}
}

static void update_persistence_range(void)
{
	const rf_drive_t* drive = selected_drive();
	double max_mb = 0;

	if (drive != NULL && A.img != NULL) {
		int64_t avail = (int64_t)drive->size - (int64_t)A.img->projected_size - (int64_t)(256 * MB);
		max_mb = (avail > 0) ? (double)(avail / (int64_t)MB) : 0;
	}
	if (max_mb < 512) {
		gtk_widget_set_sensitive(A.persist_scale, FALSE);
		gtk_range_set_range(GTK_RANGE(A.persist_scale), 0, 1);
		gtk_range_set_value(GTK_RANGE(A.persist_scale), 0);
	} else {
		gtk_widget_set_sensitive(A.persist_scale, !A.busy);
		gtk_range_set_range(GTK_RANGE(A.persist_scale), 0, max_mb);
		gtk_range_set_increments(GTK_RANGE(A.persist_scale), 256, 1024);
	}
}

/* Apply the core defaults for the current device/boot/image combination */
static void apply_defaults(void)
{
	rf_options_t opts;
	const rf_drive_t* drive = selected_drive();
	rf_boot_type_t bt = selected_boot();
	bool has_img = (bt == BT_IMAGE && A.img != NULL);

	rf_options_init(&opts);
	opts.boot_type = bt;
	rf_options_defaults(&opts, drive, has_img ? A.img : NULL);

	/* Image mode (ISO vs DD) */
	gtk_widget_set_visible(A.image_mode_row, has_img && A.img->is_iso && IS_DD_BOOTABLE(A.img));
	if (has_img) {
		A.updating = true;
		gtk_drop_down_set_selected(GTK_DROP_DOWN(A.image_mode_dd), opts.image_mode == IM_DD ? 1 : 0);
		A.updating = false;
	}

	/* Persistence */
	gtk_widget_set_visible(A.persist_row, has_img && A.img->is_iso && HAS_PERSISTENCE(A.img) && opts.image_mode == IM_ISO);
	update_persistence_range();
	gtk_range_set_value(GTK_RANGE(A.persist_scale), 0);

	populate_scheme_target(opts.partition_style, opts.target_type);
	populate_fs(opts.fs);
	gtk_editable_set_text(GTK_EDITABLE(A.label_entry), opts.label);
	update_controls();
}

static void update_controls(void)
{
	bool busy = A.busy || A.analyzing;
	bool has_drive = selected_drive() != NULL;
	rf_boot_type_t bt = selected_boot();
	bool dd = (bt == BT_IMAGE && A.img != NULL && (image_mode_dd() || !A.img->is_iso));
	bool can_start = has_drive && !busy && (bt != BT_IMAGE || A.img != NULL);

	gtk_widget_set_sensitive(A.device_dd, !busy);
	gtk_widget_set_sensitive(A.boot_dd, !busy);
	gtk_widget_set_sensitive(A.select_btn, !busy && bt == BT_IMAGE);
	gtk_widget_set_sensitive(A.image_mode_dd, !busy);
	gtk_widget_set_sensitive(A.persist_row, !busy);
	gtk_widget_set_sensitive(A.scheme_dd, !busy && !dd);
	gtk_widget_set_sensitive(A.target_dd, !busy && !dd);
	gtk_widget_set_sensitive(A.label_entry, !busy && !dd);
	gtk_widget_set_sensitive(A.fs_dd, !busy && !dd);
	gtk_widget_set_sensitive(A.cluster_dd, !busy && !dd);
	gtk_widget_set_sensitive(A.quick_check, !busy && !dd);
	gtk_widget_set_sensitive(A.verify_check, !busy && dd);
	gtk_widget_set_sensitive(A.list_hdd_check, !busy);
	gtk_widget_set_sensitive(A.rufus_mbr_check, !busy);
	gtk_widget_set_sensitive(A.badblocks_check, !busy);
	gtk_widget_set_sensitive(A.badblocks_dd, !busy && gtk_check_button_get_active(GTK_CHECK_BUTTON(A.badblocks_check)));
	gtk_widget_set_sensitive(A.tool_box, !A.busy);
	gtk_widget_set_sensitive(A.start_btn, can_start);
	gtk_button_set_label(GTK_BUTTON(A.close_btn), A.busy ? "CANCEL" : "CLOSE");
}

/* ---------------------------------------------------------------------- */
/* Devices                                                                */
/* ---------------------------------------------------------------------- */

static bool same_drives(rf_drive_t* a, int na, rf_drive_t* b, int nb)
{
	if (na != nb)
		return false;
	for (int i = 0; i < na; i++)
		if (strcmp(a[i].path, b[i].path) != 0 || a[i].size != b[i].size || strcmp(a[i].label, b[i].label) != 0)
			return false;
	return true;
}

static void refresh_devices(bool force)
{
	rf_drive_t* drives = NULL;
	bool all = gtk_check_button_get_active(GTK_CHECK_BUTTON(A.list_hdd_check));
	int n = rf_drive_list(&drives, all);
	char prev[64] = "";
	const rf_drive_t* cur = selected_drive();
	const char** items;
	int sel = 0;

	if (!force && same_drives(A.drives, A.nb_drives, drives, n)) {
		free(drives);
		return;
	}
	if (cur != NULL)
		snprintf(prev, sizeof(prev), "%s", cur->path);
	free(A.drives);
	A.drives = drives;
	A.nb_drives = n;

	items = g_new0(const char*, (size_t)n + 1);
	for (int i = 0; i < n; i++) {
		items[i] = A.drives[i].display;
		if (strcmp(A.drives[i].path, prev) == 0)
			sel = i;
	}
	set_dd_items(A.device_dd, items, n > 0 ? sel : -1);
	g_free(items);
	if (n == 0)
		set_status("No device found", 0.0);
	else if (!A.busy)
		set_status("READY", 0.0);
	apply_defaults();
}

static gboolean device_poll(gpointer data)
{
	(void)data;
	if (!A.busy && !A.analyzing)
		refresh_devices(false);
	return G_SOURCE_CONTINUE;
}

/* ---------------------------------------------------------------------- */
/* Image selection and analysis                                           */
/* ---------------------------------------------------------------------- */

static void analyze_thread(GTask* task, gpointer source, gpointer task_data, GCancellable* cancellable)
{
	char* path = task_data;
	rf_image_t* img = g_new0(rf_image_t, 1);
	(void)source;
	(void)cancellable;
	if (!rf_image_analyze(path, img)) {
		g_free(img);
		g_task_return_pointer(task, NULL, NULL);
		return;
	}
	g_task_return_pointer(task, img, g_free);
}

static void analyze_done(GObject* source, GAsyncResult* res, gpointer data)
{
	rf_image_t* img = g_task_propagate_pointer(G_TASK(res), NULL);
	char desc[512];
	(void)source;
	(void)data;

	A.analyzing = false;
	if (img == NULL) {
		set_status("Could not open the image", 0.0);
		message(GTK_MESSAGE_ERROR, "Unsupported image", "This image could not be opened or analyzed.");
		update_controls();
		return;
	}
	if (!img->is_iso && !img->is_bootable_img) {
		message(GTK_MESSAGE_WARNING, "Unsupported image",
			"This image is either non-bootable, or it uses a boot or compression method that is not supported by Rufus.");
		g_free(img);
		set_status("READY", 0.0);
		update_controls();
		return;
	}
	g_free(A.img);
	A.img = img;
	{
		char* base = g_path_get_basename(img->path);
		gtk_string_list_splice(A.boot_list, 0, 1, (const char* const[]){ base, NULL });
		g_free(base);
		A.updating = true;
		gtk_drop_down_set_selected(GTK_DROP_DOWN(A.boot_dd), BOOT_IDX_IMAGE);
		A.updating = false;
	}
	rf_image_describe(img, desc, sizeof(desc));
	append_log(desc);
	if (img->is_iso && !IS_BIOS_BOOTABLE(img) && !IS_EFI_BOOTABLE(img) && !IS_DD_BOOTABLE(img))
		message(GTK_MESSAGE_WARNING, "Unsupported ISO",
			"This ISO image does not appear to be bootable: no BIOS or UEFI boot loader was found.");
	set_status("READY", 0.0);
	apply_defaults();
}

static void analyze_image(const char* path)
{
	GTask* task = g_task_new(NULL, NULL, analyze_done, NULL);
	A.analyzing = true;
	update_controls();
	set_status("Analyzing image...", -1.0);
	g_task_set_task_data(task, g_strdup(path), g_free);
	g_task_run_in_thread(task, analyze_thread);
	g_object_unref(task);
}

static void file_opened(GObject* source, GAsyncResult* res, gpointer data)
{
	GFile* file = gtk_file_dialog_open_finish(GTK_FILE_DIALOG(source), res, NULL);
	(void)data;
	if (file == NULL)
		return;
	char* path = g_file_get_path(file);
	if (path != NULL)
		analyze_image(path);
	g_free(path);
	g_object_unref(file);
}

static void on_select(GtkButton* btn, gpointer data)
{
	GtkFileDialog* dlg = gtk_file_dialog_new();
	GListStore* filters = g_list_store_new(GTK_TYPE_FILE_FILTER);
	GtkFileFilter* f = gtk_file_filter_new();
	GtkFileFilter* all = gtk_file_filter_new();
	static const char* patterns[] = { "*.iso", "*.img", "*.bin", "*.raw", "*.vhd", "*.gz", "*.xz", "*.zst",
		"*.bz2", "*.lzma", "*.ISO", "*.IMG" };
	(void)btn;
	(void)data;

	gtk_file_filter_set_name(f, "Disk or ISO images");
	for (size_t i = 0; i < G_N_ELEMENTS(patterns); i++)
		gtk_file_filter_add_pattern(f, patterns[i]);
	gtk_file_filter_set_name(all, "All files");
	gtk_file_filter_add_pattern(all, "*");
	g_list_store_append(filters, f);
	g_list_store_append(filters, all);
	gtk_file_dialog_set_filters(dlg, G_LIST_MODEL(filters));
	gtk_file_dialog_set_default_filter(dlg, f);
	gtk_file_dialog_set_title(dlg, "Select an image");
	if (A.img != NULL) {
		char* dir = g_path_get_dirname(A.img->path);
		GFile* folder = g_file_new_for_path(dir);
		gtk_file_dialog_set_initial_folder(dlg, folder);
		g_object_unref(folder);
		g_free(dir);
	}
	gtk_file_dialog_open(dlg, GTK_WINDOW(A.window), NULL, file_opened, NULL);
	g_object_unref(filters);
	g_object_unref(f);
	g_object_unref(all);
	g_object_unref(dlg);
}

/* Drag and drop of an image onto the window */
static gboolean on_drop(GtkDropTarget* target, const GValue* value, double x, double y, gpointer data)
{
	(void)target; (void)x; (void)y; (void)data;
	if (A.busy || !G_VALUE_HOLDS(value, G_TYPE_FILE))
		return FALSE;
	char* path = g_file_get_path(g_value_get_object(value));
	if (path == NULL)
		return FALSE;
	analyze_image(path);
	g_free(path);
	return TRUE;
}

/* ---------------------------------------------------------------------- */
/* Signals                                                                */
/* ---------------------------------------------------------------------- */

static void on_device_changed(GObject* obj, GParamSpec* pspec, gpointer data)
{
	(void)obj; (void)pspec; (void)data;
	if (A.updating)
		return;
	apply_defaults();
}

static void on_boot_changed(GObject* obj, GParamSpec* pspec, gpointer data)
{
	(void)obj; (void)pspec; (void)data;
	if (A.updating)
		return;
	apply_defaults();
}

static void on_image_mode_changed(GObject* obj, GParamSpec* pspec, gpointer data)
{
	(void)obj; (void)pspec; (void)data;
	if (A.updating)
		return;
	gtk_widget_set_visible(A.persist_row, A.img != NULL && HAS_PERSISTENCE(A.img) && !image_mode_dd());
	update_controls();
}

static void on_scheme_changed(GObject* obj, GParamSpec* pspec, gpointer data)
{
	(void)obj; (void)pspec; (void)data;
	if (A.updating)
		return;
	rf_partition_style_t ps = (rf_partition_style_t)choice_value(A.scheme_dd, &A.scheme_choices, PS_MBR);
	populate_scheme_target(ps, (ps == PS_GPT) ? TT_UEFI : TT_BIOS);
}

static void on_fs_changed(GObject* obj, GParamSpec* pspec, gpointer data)
{
	(void)obj; (void)pspec; (void)data;
	if (A.updating)
		return;
	populate_clusters();
}

static void on_toggle(GtkCheckButton* btn, gpointer data)
{
	(void)data;
	if (btn == GTK_CHECK_BUTTON(A.list_hdd_check))
		refresh_devices(true);
	update_controls();
}

static void on_persist_changed(GtkRange* range, gpointer data)
{
	double v = gtk_range_get_value(range);
	(void)data;
	if (v < 1.0) {
		gtk_label_set_text(GTK_LABEL(A.persist_value), "No persistence");
	} else {
		/* Snap to 512 MB minimum, like the Windows version */
		if (v < 512)
			gtk_range_set_value(range, 512);
		gtk_label_set_text(GTK_LABEL(A.persist_value), rf_size_to_human((uint64_t)gtk_range_get_value(range) * MB, false));
	}
}

/* ---------------------------------------------------------------------- */
/* Running the job through the privileged helper                          */
/* ---------------------------------------------------------------------- */

static char* find_helper(void)
{
	char exe[PATH_MAX], path[PATH_MAX + 32];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);

	if (n > 0) {
		exe[n] = 0;
		char* dir = g_path_get_dirname(exe);
		snprintf(path, sizeof(path), "%s/rufus-cli", dir);
		g_free(dir);
		if (access(path, X_OK) == 0)
			return g_strdup(path);
	}
	char* p = g_find_program_in_path("rufus-cli");
	if (p != NULL)
		return p;
	snprintf(path, sizeof(path), "%s/rufus-cli", LIBEXECDIR);
	if (access(path, X_OK) == 0)
		return g_strdup(path);
	return NULL;
}

static void job_finished(void)
{
	char msg[128];
	uint64_t secs = (rf_monotonic_ms() - A.op_start) / 1000;

	A.busy = false;
	g_clear_object(&A.proc_out);
	g_clear_object(&A.proc_in);
	g_clear_object(&A.proc);
	if (A.result_ok) {
		set_status("READY", 1.0);
		snprintf(msg, sizeof(msg), "Operation completed in %d:%02d", (int)(secs / 60), (int)(secs % 60));
		append_log(msg);
	} else if (A.result_cancelled) {
		set_status("Cancelled", 0.0);
		message(GTK_MESSAGE_INFO, "Operation cancelled", "The drive may be left in an unusable state.");
	} else {
		set_status("FAILED", 0.0);
		message(GTK_MESSAGE_ERROR, "The operation failed", "See the log (☰ button) for details.");
	}
	refresh_devices(true);
	update_controls();
}

static void read_line_cb(GObject* source, GAsyncResult* res, gpointer data)
{
	gsize len;
	GError* err = NULL;
	char* line = g_data_input_stream_read_line_finish_utf8(G_DATA_INPUT_STREAM(source), res, &len, &err);
	(void)data;

	if (line == NULL) {
		g_clear_error(&err);
		/* EOF: wait for the process to exit */
		if (A.proc != NULL)
			g_subprocess_wait(A.proc, NULL, NULL);
		if (!A.got_result && A.proc != NULL && g_subprocess_get_if_exited(A.proc)) {
			int status = g_subprocess_get_exit_status(A.proc);
			if (status == 126 || status == 127)
				append_log("Authorization was refused or pkexec could not run the helper");
			A.result_ok = (status == 0);
		}
		job_finished();
		return;
	}
	if (strncmp(line, "LOG ", 4) == 0) {
		fprintf(stderr, "%s\n", &line[4]);
		append_log(&line[4]);
	} else if (strncmp(line, "PROGRESS ", 9) == 0) {
		int op = 0;
		double frac = 0.0;
		int consumed = 0;
		if (sscanf(&line[9], "%d %lf %n", &op, &frac, &consumed) >= 2)
			set_status(&line[9 + consumed], frac);
	} else if (strncmp(line, "RESULT ", 7) == 0) {
		A.got_result = true;
		A.result_ok = (strcmp(&line[7], "ok") == 0);
		A.result_cancelled = (strcmp(&line[7], "cancelled") == 0);
		/* The helper is done: closing its stdin lets it exit */
		if (A.proc_in != NULL)
			g_output_stream_close(A.proc_in, NULL, NULL);
	} else {
		append_log(line);   /* pkexec or other stderr output */
	}
	g_free(line);
	g_data_input_stream_read_line_async(A.proc_out, G_PRIORITY_DEFAULT, NULL, read_line_cb, NULL);
}

static void spawn_helper(rf_options_t* opts)
{
	GPtrArray* argv = g_ptr_array_new_with_free_func(g_free);
	GError* err = NULL;
	char* helper = find_helper();
	char buf[64];

	if (helper == NULL) {
		message(GTK_MESSAGE_ERROR, "rufus-cli not found", "The rufus-cli helper must be installed next to rufus.");
		A.busy = false;
		update_controls();
		return;
	}
	if (geteuid() != 0)
		g_ptr_array_add(argv, g_strdup("pkexec"));
	g_ptr_array_add(argv, helper);
	g_ptr_array_add(argv, g_strdup("write"));
	g_ptr_array_add(argv, g_strdup("--machine"));
	g_ptr_array_add(argv, g_strdup("--no-download"));
#define ADD2(a, b) do { g_ptr_array_add(argv, g_strdup(a)); g_ptr_array_add(argv, g_strdup(b)); } while (0)
	ADD2("--device", opts->device);
	switch (opts->boot_type) {
	case BT_IMAGE: ADD2("--boot", "image"); ADD2("--image", opts->image); break;
	case BT_FREEDOS: ADD2("--boot", "freedos"); break;
	case BT_SYSLINUX_V4: ADD2("--boot", "syslinux4"); break;
	case BT_SYSLINUX_V6: ADD2("--boot", "syslinux6"); break;
	case BT_GRUB2: ADD2("--boot", "grub2"); break;
	default: ADD2("--boot", "none"); break;
	}
	ADD2("--mode", opts->image_mode == IM_DD ? "dd" : "iso");
	ADD2("--scheme", opts->partition_style == PS_GPT ? "gpt" : "mbr");
	ADD2("--target", opts->target_type == TT_UEFI ? "uefi" : "bios");
	ADD2("--fs", rf_fs_name[opts->fs]);
	ADD2("--label", opts->label);
	if (opts->cluster_size != 0) {
		snprintf(buf, sizeof(buf), "%u", opts->cluster_size);
		ADD2("--cluster", buf);
	}
	if (opts->persistence_size != 0) {
		snprintf(buf, sizeof(buf), "%" PRIu64 "M", (uint64_t)(opts->persistence_size / MB));
		ADD2("--persistence", buf);
	}
	if (!opts->quick_format)
		g_ptr_array_add(argv, g_strdup("--full-format"));
	if (opts->verify)
		g_ptr_array_add(argv, g_strdup("--verify"));
	if (!opts->use_rufus_mbr)
		g_ptr_array_add(argv, g_strdup("--no-rufus-mbr"));
	if (opts->badblocks_passes > 0) {
		snprintf(buf, sizeof(buf), "%d", opts->badblocks_passes);
		ADD2("--badblocks", buf);
	}
	if (opts->unattend_flags & UNATTEND_SECUREBOOT_TPM_MINRAM)
		g_ptr_array_add(argv, g_strdup("--wue-bypass"));
	if (opts->unattend_flags & UNATTEND_NO_ONLINE_ACCOUNT)
		g_ptr_array_add(argv, g_strdup("--wue-no-account"));
	if (opts->unattend_flags & UNATTEND_NO_DATA_COLLECTION)
		g_ptr_array_add(argv, g_strdup("--wue-no-privacy"));
	if (opts->unattend_flags & UNATTEND_DISABLE_BITLOCKER)
		g_ptr_array_add(argv, g_strdup("--wue-no-bitlocker"));
	if (opts->unattend_flags & UNATTEND_OFFLINE_INTERNAL_DRIVES)
		g_ptr_array_add(argv, g_strdup("--wue-offline"));
	if ((opts->unattend_flags & UNATTEND_SET_USER) && opts->unattend_username[0])
		ADD2("--wue-user", opts->unattend_username);
	if ((opts->unattend_flags & UNATTEND_DUPLICATE_LOCALE) && opts->unattend_locale[0])
		ADD2("--wue-locale", opts->unattend_locale);
	if (opts->ldlinux_sys[0]) ADD2("--ldlinux-sys", opts->ldlinux_sys);
	if (opts->ldlinux_bss[0]) ADD2("--ldlinux-bss", opts->ldlinux_bss);
	if (opts->ldlinux_c32[0]) ADD2("--ldlinux-c32", opts->ldlinux_c32);
	if (opts->grub2_core_img[0]) ADD2("--core-img", opts->grub2_core_img);
#undef ADD2
	g_ptr_array_add(argv, NULL);

	A.got_result = A.result_ok = A.result_cancelled = false;
	A.proc = g_subprocess_newv((const char* const*)argv->pdata,
		G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_MERGE, &err);
	g_ptr_array_free(argv, TRUE);
	if (A.proc == NULL) {
		message(GTK_MESSAGE_ERROR, "Could not start the operation", err ? err->message : NULL);
		g_clear_error(&err);
		A.busy = false;
		update_controls();
		return;
	}
	A.proc_in = g_object_ref(g_subprocess_get_stdin_pipe(A.proc));
	A.proc_out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(A.proc));
	set_status("Waiting for authorization...", -1.0);
	g_data_input_stream_read_line_async(A.proc_out, G_PRIORITY_DEFAULT, NULL, read_line_cb, NULL);
}

/* Fetch the boot files we may need (as the regular user), then spawn the helper */
static void fetch_thread(GTask* task, gpointer source, gpointer task_data, GCancellable* cancellable)
{
	rf_options_t* opts = task_data;
	(void)source; (void)cancellable;
	rf_fetch_boot_files(A.img, opts);
	g_task_return_boolean(task, TRUE);
}

static void fetch_done(GObject* source, GAsyncResult* res, gpointer data)
{
	rf_options_t* opts = g_task_get_task_data(G_TASK(res));
	(void)source; (void)data;
	if (rf_cancelled()) {
		A.result_cancelled = true;
		job_finished();
		return;
	}
	spawn_helper(opts);
}

static void start_job(void)
{
	rf_options_t* opts = g_new0(rf_options_t, 1);
	GTask* task;

	build_options(opts);
	A.busy = true;
	A.op_start = rf_monotonic_ms();
	rf_reset_cancel();
	update_controls();
	append_log("--------------------------------------------------");
	set_status("Preparing...", -1.0);
	task = g_task_new(NULL, NULL, fetch_done, NULL);
	g_task_set_task_data(task, opts, g_free);
	g_task_run_in_thread(task, fetch_thread);
	g_object_unref(task);
}

static void confirm_done(GObject* source, GAsyncResult* res, gpointer data)
{
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL);
	(void)data;
	if (button == 1)
		start_job();
}

static void confirm_and_start(void)
{
	const rf_drive_t* drive = selected_drive();
	GtkAlertDialog* dlg;
	char* text;

	if (drive == NULL)
		return;
	text = g_strdup_printf("WARNING: ALL DATA ON DEVICE '%s' WILL BE DESTROYED.\n"
		"To continue with this operation, click OK. To quit click CANCEL.", drive->display);
	dlg = gtk_alert_dialog_new("Rufus");
	gtk_alert_dialog_set_detail(dlg, text);
	gtk_alert_dialog_set_buttons(dlg, (const char* const[]){ "Cancel", "OK", NULL });
	gtk_alert_dialog_set_cancel_button(dlg, 0);
	gtk_alert_dialog_set_default_button(dlg, 0);
	gtk_alert_dialog_choose(dlg, GTK_WINDOW(A.window), NULL, confirm_done, NULL);
	g_object_unref(dlg);
	g_free(text);
}

/* Windows User Experience dialog */
typedef struct {
	GtkWidget* win;
	GtkWidget* checks[6];
	GtkWidget* user_entry;
} wue_dlg_t;

static void wue_ok(GtkButton* btn, gpointer data)
{
	wue_dlg_t* d = data;
	static const int flags[6] = { UNATTEND_SECUREBOOT_TPM_MINRAM, UNATTEND_NO_ONLINE_ACCOUNT, UNATTEND_SET_USER,
		UNATTEND_DUPLICATE_LOCALE, UNATTEND_NO_DATA_COLLECTION, UNATTEND_DISABLE_BITLOCKER };
	(void)btn;
	A.wue_flags = 0;
	for (int i = 0; i < 6; i++)
		if (gtk_check_button_get_active(GTK_CHECK_BUTTON(d->checks[i])))
			A.wue_flags |= flags[i];
	snprintf(A.wue_user, sizeof(A.wue_user), "%s", gtk_editable_get_text(GTK_EDITABLE(d->user_entry)));
	{
		/* en_US.UTF-8 -> en-US */
		const char* lang = g_getenv("LC_ALL");
		if (lang == NULL || *lang == 0)
			lang = g_getenv("LANG");
		A.wue_locale[0] = 0;
		if (lang != NULL && strlen(lang) >= 5 && lang[2] == '_')
			snprintf(A.wue_locale, sizeof(A.wue_locale), "%c%c-%c%c", lang[0], lang[1], lang[3], lang[4]);
	}
	gtk_window_destroy(GTK_WINDOW(d->win));
	g_free(d);
	confirm_and_start();
}

static void wue_cancel(GtkButton* btn, gpointer data)
{
	wue_dlg_t* d = data;
	(void)btn;
	gtk_window_destroy(GTK_WINDOW(d->win));
	g_free(d);
}

static void show_wue_dialog(void)
{
	static const char* labels[6] = {
		"Remove requirement for 4GB+ RAM, Secure Boot and TPM 2.0",
		"Remove requirement for an online Microsoft account",
		"Create a local account with username:",
		"Set regional options to the same values as this user's",
		"Disable data collection (Skip privacy questions)",
		"Disable BitLocker automatic device encryption",
	};
	static const bool defaults[6] = { true, true, false, false, false, false };
	wue_dlg_t* d = g_new0(wue_dlg_t, 1);
	GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	GtkWidget* title = gtk_label_new("Customize Windows installation?");
	GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	GtkWidget* ok = gtk_button_new_with_label("OK");
	GtkWidget* cancel = gtk_button_new_with_label("Cancel");
	const char* user = g_get_user_name();

	d->win = gtk_window_new();
	gtk_window_set_title(GTK_WINDOW(d->win), "Windows User Experience");
	gtk_window_set_transient_for(GTK_WINDOW(d->win), GTK_WINDOW(A.window));
	gtk_window_set_modal(GTK_WINDOW(d->win), TRUE);
	gtk_window_set_resizable(GTK_WINDOW(d->win), FALSE);
	gtk_widget_set_margin_start(box, 16);
	gtk_widget_set_margin_end(box, 16);
	gtk_widget_set_margin_top(box, 16);
	gtk_widget_set_margin_bottom(box, 16);
	gtk_label_set_xalign(GTK_LABEL(title), 0.0f);
	gtk_widget_add_css_class(title, "title-4");
	gtk_box_append(GTK_BOX(box), title);
	for (int i = 0; i < 6; i++) {
		d->checks[i] = gtk_check_button_new_with_label(labels[i]);
		gtk_check_button_set_active(GTK_CHECK_BUTTON(d->checks[i]), defaults[i]);
		gtk_box_append(GTK_BOX(box), d->checks[i]);
		if (i == 2) {
			d->user_entry = gtk_entry_new();
			gtk_editable_set_text(GTK_EDITABLE(d->user_entry), user ? user : "");
			gtk_widget_set_margin_start(d->user_entry, 28);
			gtk_box_append(GTK_BOX(box), d->user_entry);
		}
	}
	gtk_widget_set_halign(buttons, GTK_ALIGN_END);
	gtk_box_append(GTK_BOX(buttons), ok);
	gtk_box_append(GTK_BOX(buttons), cancel);
	gtk_widget_set_margin_top(buttons, 8);
	gtk_box_append(GTK_BOX(box), buttons);
	g_signal_connect(ok, "clicked", G_CALLBACK(wue_ok), d);
	g_signal_connect(cancel, "clicked", G_CALLBACK(wue_cancel), d);
	gtk_window_set_child(GTK_WINDOW(d->win), box);
	gtk_window_present(GTK_WINDOW(d->win));
}

static void after_mode_choice(void)
{
	rf_options_t opts;
	build_options(&opts);
	if (opts.boot_type == BT_IMAGE && A.img != NULL && opts.image_mode == IM_ISO && HAS_WININST(A.img)) {
		show_wue_dialog();
		return;
	}
	A.wue_flags = 0;
	confirm_and_start();
}

static void hybrid_done(GObject* source, GAsyncResult* res, gpointer data)
{
	int button = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), res, NULL);
	(void)data;
	if (button < 0 || button == 2)
		return;
	A.updating = true;
	gtk_drop_down_set_selected(GTK_DROP_DOWN(A.image_mode_dd), (guint)button);
	A.updating = false;
	gtk_widget_set_visible(A.persist_row, A.img != NULL && HAS_PERSISTENCE(A.img) && button == 0);
	update_controls();
	/* Revalidate with the new mode */
	{
		rf_options_t opts;
		const char* err;
		build_options(&opts);
		err = rf_options_validate(&opts, selected_drive(), A.img);
		if (err != NULL) {
			message(GTK_MESSAGE_ERROR, "Cannot proceed", err);
			return;
		}
	}
	after_mode_choice();
}

static void on_start(GtkButton* btn, gpointer data)
{
	rf_options_t opts;
	const char* err;
	(void)btn; (void)data;

	build_options(&opts);
	if (opts.boot_type == BT_IMAGE && A.img == NULL) {
		message(GTK_MESSAGE_ERROR, "No image selected", "Please click SELECT to choose an image.");
		return;
	}
	if (opts.boot_type == BT_IMAGE && A.img != NULL && rf_drive_hosts_file(opts.device, A.img->path)) {
		message(GTK_MESSAGE_ERROR, "Image is on the target drive",
			"The image you selected is stored on the drive you want to write to. Copy it elsewhere first.");
		return;
	}
	err = rf_options_validate(&opts, selected_drive(), A.img);
	if (err != NULL && !(opts.boot_type == BT_IMAGE && A.img != NULL && A.img->is_isohybrid && opts.image_mode == IM_ISO)) {
		message(GTK_MESSAGE_ERROR, "Cannot proceed", err);
		return;
	}
	/* ISOHybrid: ask which mode to use, like Windows Rufus */
	if (opts.boot_type == BT_IMAGE && A.img != NULL && A.img->is_isohybrid && !A.img->disable_iso &&
		gtk_widget_get_visible(A.image_mode_row) && opts.image_mode == IM_ISO) {
		GtkAlertDialog* dlg = gtk_alert_dialog_new("ISOHybrid image detected");
		gtk_alert_dialog_set_detail(dlg,
			"The image you have selected is an 'ISOHybrid' image. This means it can be written either in "
			"ISO Image (file copy) mode or DD Image (disk image) mode.\n\n"
			"Rufus recommends using ISO Image mode, so that you always have full access to the drive after "
			"writing it.\nHowever, if you encounter issues during boot, you can try writing this image again "
			"in DD Image mode.");
		gtk_alert_dialog_set_buttons(dlg, (const char* const[]){ "Write in ISO Image mode (Recommended)",
			"Write in DD Image mode", "Cancel", NULL });
		gtk_alert_dialog_set_cancel_button(dlg, 2);
		gtk_alert_dialog_set_default_button(dlg, 0);
		gtk_alert_dialog_choose(dlg, GTK_WINDOW(A.window), NULL, hybrid_done, NULL);
		g_object_unref(dlg);
		return;
	}
	if (err != NULL) {
		message(GTK_MESSAGE_ERROR, "Cannot proceed", err);
		return;
	}
	after_mode_choice();
}

static void on_close(GtkButton* btn, gpointer data)
{
	(void)btn; (void)data;
	if (A.busy) {
		rf_cancel();
		if (A.proc_in != NULL) {
			g_output_stream_write_all(A.proc_in, "cancel\n", 7, NULL, NULL, NULL);
			g_output_stream_flush(A.proc_in, NULL, NULL);
		}
		set_status("Cancelling...", -1.0);
		return;
	}
	gtk_window_destroy(GTK_WINDOW(A.window));
}

static gboolean on_close_request(GtkWindow* win, gpointer data)
{
	(void)win; (void)data;
	if (A.busy) {
		message(GTK_MESSAGE_WARNING, "An operation is in progress", "Please cancel it first, or wait for it to complete.");
		return TRUE;
	}
	return FALSE;
}

/* ---------------------------------------------------------------------- */
/* Tool buttons: log, checksums, save drive, about                        */
/* ---------------------------------------------------------------------- */

static void log_clear(GtkButton* b, gpointer d) { (void)b; (void)d; gtk_text_buffer_set_text(A.log_buf, "", 0); }

static void log_saved(GObject* source, GAsyncResult* res, gpointer data)
{
	GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, NULL);
	GtkTextIter s, e;
	char* text;
	(void)data;
	if (file == NULL)
		return;
	gtk_text_buffer_get_bounds(A.log_buf, &s, &e);
	text = gtk_text_buffer_get_text(A.log_buf, &s, &e, FALSE);
	g_file_replace_contents(file, text, strlen(text), NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, NULL);
	g_free(text);
	g_object_unref(file);
}

static void log_save(GtkButton* b, gpointer d)
{
	GtkFileDialog* dlg = gtk_file_dialog_new();
	(void)b; (void)d;
	gtk_file_dialog_set_initial_name(dlg, "rufus.log");
	gtk_file_dialog_save(dlg, GTK_WINDOW(A.log_window), NULL, log_saved, NULL);
	g_object_unref(dlg);
}

static void on_log(GtkButton* btn, gpointer data)
{
	(void)btn; (void)data;
	if (A.log_window == NULL) {
		GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
		GtkWidget* sw = gtk_scrolled_window_new();
		GtkWidget* tv = gtk_text_view_new_with_buffer(A.log_buf);
		GtkWidget* row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
		GtkWidget* clear = gtk_button_new_with_label("Clear");
		GtkWidget* save = gtk_button_new_with_label("Save");

		A.log_window = gtk_window_new();
		gtk_window_set_title(GTK_WINDOW(A.log_window), "Log");
		gtk_window_set_default_size(GTK_WINDOW(A.log_window), 720, 480);
		gtk_window_set_hide_on_close(GTK_WINDOW(A.log_window), TRUE);
		gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
		gtk_text_view_set_monospace(GTK_TEXT_VIEW(tv), TRUE);
		gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
		gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), tv);
		gtk_widget_set_vexpand(sw, TRUE);
		gtk_widget_set_halign(row, GTK_ALIGN_END);
		gtk_box_append(GTK_BOX(row), clear);
		gtk_box_append(GTK_BOX(row), save);
		gtk_box_append(GTK_BOX(box), sw);
		gtk_box_append(GTK_BOX(box), row);
		gtk_widget_set_margin_start(box, 8);
		gtk_widget_set_margin_end(box, 8);
		gtk_widget_set_margin_top(box, 8);
		gtk_widget_set_margin_bottom(box, 8);
		g_signal_connect(clear, "clicked", G_CALLBACK(log_clear), NULL);
		g_signal_connect(save, "clicked", G_CALLBACK(log_save), NULL);
		gtk_window_set_child(GTK_WINDOW(A.log_window), box);
	}
	gtk_window_present(GTK_WINDOW(A.log_window));
}

static void hash_thread(GTask* task, gpointer source, gpointer task_data, GCancellable* cancellable)
{
	rf_hashes_t* h = g_new0(rf_hashes_t, 1);
	(void)source; (void)cancellable;
	if (!rf_hash_file((const char*)task_data, h)) {
		g_free(h);
		h = NULL;
	}
	g_task_return_pointer(task, h, g_free);
}

static void hash_done(GObject* source, GAsyncResult* res, gpointer data)
{
	rf_hashes_t* h = g_task_propagate_pointer(G_TASK(res), NULL);
	(void)source; (void)data;
	A.busy = false;
	update_controls();
	set_status("READY", 0.0);
	if (h == NULL)
		return;
	{
		GtkWidget* win = gtk_window_new();
		GtkWidget* grid = gtk_grid_new();
		const char* names[4] = { "MD5", "SHA1", "SHA256", "SHA512" };
		const char* vals[4] = { h->md5, h->sha1, h->sha256, h->sha512 };
		gtk_window_set_title(GTK_WINDOW(win), "Checksums");
		gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(A.window));
		gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
		gtk_grid_set_column_spacing(GTK_GRID(grid), 12);
		gtk_widget_set_margin_start(grid, 16);
		gtk_widget_set_margin_end(grid, 16);
		gtk_widget_set_margin_top(grid, 16);
		gtk_widget_set_margin_bottom(grid, 16);
		for (int i = 0; i < 4; i++) {
			GtkWidget* l = gtk_label_new(names[i]);
			GtkWidget* e = gtk_entry_new();
			gtk_label_set_xalign(GTK_LABEL(l), 0.0f);
			gtk_editable_set_text(GTK_EDITABLE(e), vals[i]);
			gtk_editable_set_editable(GTK_EDITABLE(e), FALSE);
			gtk_editable_set_width_chars(GTK_EDITABLE(e), i == 3 ? 64 : 64);
			gtk_widget_set_hexpand(e, TRUE);
			gtk_grid_attach(GTK_GRID(grid), l, 0, i, 1, 1);
			gtk_grid_attach(GTK_GRID(grid), e, 1, i, 1, 1);
			append_log(names[i]);
			append_log(vals[i]);
		}
		gtk_window_set_child(GTK_WINDOW(win), grid);
		gtk_window_present(GTK_WINDOW(win));
	}
	g_free(h);
}

static void on_hash(GtkButton* btn, gpointer data)
{
	GTask* task;
	(void)btn; (void)data;
	if (A.img == NULL) {
		message(GTK_MESSAGE_INFO, "No image selected", "Select an image first to compute its checksums.");
		return;
	}
	A.busy = true;
	rf_reset_cancel();
	update_controls();
	task = g_task_new(NULL, NULL, hash_done, NULL);
	g_task_set_task_data(task, g_strdup(A.img->path), g_free);
	g_task_run_in_thread(task, hash_thread);
	g_object_unref(task);
}

static void save_chosen(GObject* source, GAsyncResult* res, gpointer data)
{
	GFile* file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), res, NULL);
	const rf_drive_t* drive = selected_drive();
	GError* err = NULL;
	char* helper;
	char* path;
	(void)data;
	if (file == NULL || drive == NULL)
		return;
	path = g_file_get_path(file);
	g_object_unref(file);
	helper = find_helper();
	if (helper == NULL || path == NULL) {
		g_free(helper);
		g_free(path);
		return;
	}
	{
		const char* argv[] = { geteuid() != 0 ? "pkexec" : helper, helper, "save", "--machine", "-d", drive->path,
			"-o", path, NULL };
		const char* const* av = (geteuid() != 0) ? argv : &argv[1];
		A.busy = true;
		A.op_start = rf_monotonic_ms();
		update_controls();
		A.got_result = A.result_ok = A.result_cancelled = false;
		A.proc = g_subprocess_newv(av, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
			G_SUBPROCESS_FLAGS_STDERR_MERGE, &err);
	}
	g_free(helper);
	g_free(path);
	if (A.proc == NULL) {
		message(GTK_MESSAGE_ERROR, "Could not start the operation", err ? err->message : NULL);
		g_clear_error(&err);
		A.busy = false;
		update_controls();
		return;
	}
	A.proc_in = g_object_ref(g_subprocess_get_stdin_pipe(A.proc));
	A.proc_out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(A.proc));
	set_status("Waiting for authorization...", -1.0);
	g_data_input_stream_read_line_async(A.proc_out, G_PRIORITY_DEFAULT, NULL, read_line_cb, NULL);
}

static void on_save(GtkButton* btn, gpointer data)
{
	const rf_drive_t* drive = selected_drive();
	GtkFileDialog* dlg;
	char name[128];
	(void)btn; (void)data;
	if (drive == NULL)
		return;
	dlg = gtk_file_dialog_new();
	snprintf(name, sizeof(name), "%s.img", drive->label[0] ? drive->label : drive->name);
	gtk_file_dialog_set_initial_name(dlg, name);
	gtk_file_dialog_set_title(dlg, "Save drive to image");
	gtk_file_dialog_save(dlg, GTK_WINDOW(A.window), NULL, save_chosen, NULL);
	g_object_unref(dlg);
}

static void on_about(GtkButton* btn, gpointer data)
{
	static const char* authors[] = { "Pete Batard (Rufus)", "Rufus for Linux contributors", NULL };
	(void)btn; (void)data;
	gtk_show_about_dialog(GTK_WINDOW(A.window),
		"program-name", "Rufus",
		"version", RUFUS_LINUX_VERSION,
		"comments", "The Reliable USB Formatting Utility, for Linux",
		"website", "https://rufus.ie",
		"copyright", "© 2011-2026 Pete Batard",
		"license-type", GTK_LICENSE_GPL_3_0,
		"authors", authors,
		"logo-icon-name", "drive-removable-media",
		NULL);
}

typedef struct {
	double fraction;
	char* msg;
} progress_msg_t;

static gboolean progress_idle(gpointer data)
{
	progress_msg_t* p = data;
	set_status(p->msg, p->fraction);
	g_free(p->msg);
	g_free(p);
	return G_SOURCE_REMOVE;
}

static void core_progress_thread_cb(rf_op_t op, double fraction, const char* msg, void* ctx)
{
	progress_msg_t* p = g_new(progress_msg_t, 1);
	(void)op; (void)ctx;
	p->fraction = fraction;
	p->msg = g_strdup(msg);
	g_idle_add(progress_idle, p);
}

/* ---------------------------------------------------------------------- */
/* Window                                                                 */
/* ---------------------------------------------------------------------- */

static GtkWidget* icon_button(const char* icon, const char* tooltip, GCallback cb)
{
	GtkWidget* b = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(b, tooltip);
	g_signal_connect(b, "clicked", cb, NULL);
	return b;
}

static void activate(GtkApplication* app, gpointer data)
{
	GtkWidget *main_box, *row, *box, *exp, *exp_box, *bottom, *spacer;
	static const char* boot_items[] = { "Disk or ISO image (Please select)", "FreeDOS", "Non bootable",
		"Syslinux 4.07", "Syslinux 6.04", "GRUB 2.16", NULL };
	static const char* mode_items[] = { "ISO Image mode (Recommended)", "DD Image mode", NULL };
	static const char* passes_items[] = { "1 pass", "2 passes", "3 passes", "4 passes", NULL };
	GtkDropTarget* drop;
	(void)data;

	if (A.window != NULL) {
		gtk_window_present(GTK_WINDOW(A.window));
		return;
	}
	A.log_buf = gtk_text_buffer_new(NULL);
	rf_set_log_callback(core_log_cb, NULL);
	rf_set_progress_callback(core_progress_thread_cb, NULL);

	A.window = gtk_application_window_new(app);
	gtk_window_set_title(GTK_WINDOW(A.window), "Rufus " RUFUS_LINUX_VERSION);
	gtk_window_set_default_size(GTK_WINDOW(A.window), 480, -1);
	gtk_window_set_resizable(GTK_WINDOW(A.window), FALSE);
	gtk_window_set_icon_name(GTK_WINDOW(A.window), "ie.akeo.rufus");
	g_signal_connect(A.window, "close-request", G_CALLBACK(on_close_request), NULL);

	main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	gtk_widget_set_margin_start(main_box, 14);
	gtk_widget_set_margin_end(main_box, 14);
	gtk_widget_set_margin_top(main_box, 6);
	gtk_widget_set_margin_bottom(main_box, 12);

	/* Drive Properties */
	gtk_box_append(GTK_BOX(main_box), section_header("Drive Properties"));
	A.device_dd = gtk_drop_down_new(NULL, NULL);
	gtk_widget_set_hexpand(A.device_dd, TRUE);
	gtk_box_append(GTK_BOX(main_box), field("Device", A.device_dd));

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	A.boot_list = gtk_string_list_new(boot_items);
	A.boot_dd = gtk_drop_down_new(G_LIST_MODEL(A.boot_list), NULL);
	gtk_widget_set_hexpand(A.boot_dd, TRUE);
	A.select_btn = gtk_button_new_with_label("SELECT");
	gtk_box_append(GTK_BOX(row), A.boot_dd);
	gtk_box_append(GTK_BOX(row), A.select_btn);
	gtk_box_append(GTK_BOX(main_box), field("Boot selection", row));

	A.image_mode_dd = gtk_drop_down_new_from_strings(mode_items);
	A.image_mode_row = field("Image option", A.image_mode_dd);
	gtk_box_append(GTK_BOX(main_box), A.image_mode_row);

	box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	A.persist_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1);
	gtk_scale_set_draw_value(GTK_SCALE(A.persist_scale), FALSE);
	gtk_widget_set_hexpand(A.persist_scale, TRUE);
	A.persist_value = gtk_label_new("No persistence");
	gtk_label_set_width_chars(GTK_LABEL(A.persist_value), 14);
	gtk_box_append(GTK_BOX(box), A.persist_scale);
	gtk_box_append(GTK_BOX(box), A.persist_value);
	A.persist_row = field("Persistent partition size", box);
	gtk_box_append(GTK_BOX(main_box), A.persist_row);

	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_widget_set_hexpand(row, TRUE);
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	A.scheme_dd = gtk_drop_down_new(NULL, NULL);
	A.target_dd = gtk_drop_down_new(NULL, NULL);
	gtk_box_append(GTK_BOX(row), field("Partition scheme", A.scheme_dd));
	gtk_box_append(GTK_BOX(row), field("Target system", A.target_dd));
	gtk_box_append(GTK_BOX(main_box), row);

	exp = gtk_expander_new("Show advanced drive properties");
	exp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	A.list_hdd_check = gtk_check_button_new_with_label("List USB Hard Drives");
	A.rufus_mbr_check = gtk_check_button_new_with_label("Use Rufus MBR with BIOS ID");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(A.rufus_mbr_check), TRUE);
	A.verify_check = gtk_check_button_new_with_label("Verify the written data (DD mode)");
	gtk_box_append(GTK_BOX(exp_box), A.list_hdd_check);
	gtk_box_append(GTK_BOX(exp_box), A.rufus_mbr_check);
	gtk_box_append(GTK_BOX(exp_box), A.verify_check);
	gtk_widget_set_margin_start(exp_box, 12);
	gtk_expander_set_child(GTK_EXPANDER(exp), exp_box);
	gtk_box_append(GTK_BOX(main_box), exp);

	/* Format Options */
	gtk_box_append(GTK_BOX(main_box), section_header("Format Options"));
	A.label_entry = gtk_entry_new();
	gtk_box_append(GTK_BOX(main_box), field("Volume label", A.label_entry));
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
	gtk_box_set_homogeneous(GTK_BOX(row), TRUE);
	A.fs_dd = gtk_drop_down_new(NULL, NULL);
	A.cluster_dd = gtk_drop_down_new(NULL, NULL);
	gtk_box_append(GTK_BOX(row), field("File system", A.fs_dd));
	gtk_box_append(GTK_BOX(row), field("Cluster size", A.cluster_dd));
	gtk_box_append(GTK_BOX(main_box), row);

	exp = gtk_expander_new("Show advanced format options");
	exp_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	A.quick_check = gtk_check_button_new_with_label("Quick format");
	gtk_check_button_set_active(GTK_CHECK_BUTTON(A.quick_check), TRUE);
	row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
	A.badblocks_check = gtk_check_button_new_with_label("Check device for bad blocks");
	A.badblocks_dd = gtk_drop_down_new_from_strings(passes_items);
	gtk_box_append(GTK_BOX(row), A.badblocks_check);
	gtk_box_append(GTK_BOX(row), A.badblocks_dd);
	gtk_box_append(GTK_BOX(exp_box), A.quick_check);
	gtk_box_append(GTK_BOX(exp_box), row);
	gtk_widget_set_margin_start(exp_box, 12);
	gtk_expander_set_child(GTK_EXPANDER(exp), exp_box);
	gtk_box_append(GTK_BOX(main_box), exp);

	/* Status */
	gtk_box_append(GTK_BOX(main_box), section_header("Status"));
	A.progress = gtk_progress_bar_new();
	gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(A.progress), TRUE);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(A.progress), "READY");
	gtk_progress_bar_set_pulse_step(GTK_PROGRESS_BAR(A.progress), 0.05);
	gtk_box_append(GTK_BOX(main_box), A.progress);

	bottom = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	gtk_widget_set_margin_top(bottom, 6);
	A.tool_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	gtk_box_append(GTK_BOX(A.tool_box), icon_button("help-about-symbolic", "About", G_CALLBACK(on_about)));
	gtk_box_append(GTK_BOX(A.tool_box), icon_button("document-save-symbolic", "Save drive to image", G_CALLBACK(on_save)));
	gtk_box_append(GTK_BOX(A.tool_box), icon_button("dialog-password-symbolic", "Compute image checksums", G_CALLBACK(on_hash)));
	gtk_box_append(GTK_BOX(A.tool_box), icon_button("format-justify-left-symbolic", "Show log", G_CALLBACK(on_log)));
	gtk_box_append(GTK_BOX(bottom), A.tool_box);
	spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(bottom), spacer);
	A.start_btn = gtk_button_new_with_label("START");
	A.close_btn = gtk_button_new_with_label("CLOSE");
	gtk_widget_add_css_class(A.start_btn, "suggested-action");
	gtk_widget_set_size_request(A.start_btn, 100, -1);
	gtk_widget_set_size_request(A.close_btn, 100, -1);
	gtk_box_append(GTK_BOX(bottom), A.start_btn);
	gtk_box_append(GTK_BOX(bottom), A.close_btn);
	gtk_box_append(GTK_BOX(main_box), bottom);

	gtk_window_set_child(GTK_WINDOW(A.window), main_box);

	g_signal_connect(A.device_dd, "notify::selected", G_CALLBACK(on_device_changed), NULL);
	g_signal_connect(A.boot_dd, "notify::selected", G_CALLBACK(on_boot_changed), NULL);
	g_signal_connect(A.image_mode_dd, "notify::selected", G_CALLBACK(on_image_mode_changed), NULL);
	g_signal_connect(A.scheme_dd, "notify::selected", G_CALLBACK(on_scheme_changed), NULL);
	g_signal_connect(A.fs_dd, "notify::selected", G_CALLBACK(on_fs_changed), NULL);
	g_signal_connect(A.select_btn, "clicked", G_CALLBACK(on_select), NULL);
	g_signal_connect(A.list_hdd_check, "toggled", G_CALLBACK(on_toggle), NULL);
	g_signal_connect(A.badblocks_check, "toggled", G_CALLBACK(on_toggle), NULL);
	g_signal_connect(A.persist_scale, "value-changed", G_CALLBACK(on_persist_changed), NULL);
	g_signal_connect(A.start_btn, "clicked", G_CALLBACK(on_start), NULL);
	g_signal_connect(A.close_btn, "clicked", G_CALLBACK(on_close), NULL);

	drop = gtk_drop_target_new(G_TYPE_FILE, GDK_ACTION_COPY);
	g_signal_connect(drop, "drop", G_CALLBACK(on_drop), NULL);
	gtk_widget_add_controller(A.window, GTK_EVENT_CONTROLLER(drop));

	append_log("Rufus " RUFUS_LINUX_VERSION " (Linux)");
	if (!rf_fs_available(FS_NTFS))
		append_log("Note: install ntfsprogs to enable NTFS formatting");
	if (!rf_fs_available(FS_EXFAT))
		append_log("Note: install exfatprogs to enable exFAT formatting");

	gtk_widget_set_visible(A.image_mode_row, FALSE);
	gtk_widget_set_visible(A.persist_row, FALSE);
	refresh_devices(true);
	g_timeout_add_seconds(2, device_poll, NULL);
	gtk_window_present(GTK_WINDOW(A.window));

	/* Allow passing an image on the command line */
	{
		const char* img = g_object_get_data(G_OBJECT(app), "initial-image");
		if (img != NULL)
			analyze_image(img);
	}
}

static int command_line(GApplication* app, GApplicationCommandLine* cmdline, gpointer data)
{
	int argc;
	char** argv = g_application_command_line_get_arguments(cmdline, &argc);
	(void)data;
	if (argc > 1) {
		GFile* f = g_application_command_line_create_file_for_arg(cmdline, argv[1]);
		char* path = g_file_get_path(f);
		g_object_set_data_full(G_OBJECT(app), "initial-image", path, g_free);
		g_object_unref(f);
		if (A.window != NULL && path != NULL)
			analyze_image(path);
	}
	g_strfreev(argv);
	g_application_activate(app);
	return 0;
}

int main(int argc, char** argv)
{
	int status;
	A.app = gtk_application_new("ie.akeo.rufus", G_APPLICATION_HANDLES_COMMAND_LINE);
	g_signal_connect(A.app, "activate", G_CALLBACK(activate), NULL);
	g_signal_connect(A.app, "command-line", G_CALLBACK(command_line), NULL);
	status = g_application_run(G_APPLICATION(A.app), argc, argv);
	g_object_unref(A.app);
	free(A.drives);
	g_free(A.img);
	return status;
}
