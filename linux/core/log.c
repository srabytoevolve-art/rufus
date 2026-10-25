/*
 * Rufus for Linux: logging, progress and cancellation
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

#include "rufus.h"

static rf_log_cb log_cb = NULL;
static void* log_ctx = NULL;
static rf_progress_cb progress_cb = NULL;
static void* progress_ctx = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static atomic_bool cancelled = false;

static const char* op_names[OP_MAX] = {
	[OP_NONE] = "",
	[OP_ANALYZE] = "Analyzing",
	[OP_BADBLOCKS] = "Checking for bad blocks",
	[OP_ZERO] = "Clearing the drive",
	[OP_PARTITION] = "Partitioning",
	[OP_FORMAT] = "Formatting",
	[OP_BOOTRECORD] = "Writing boot records",
	[OP_FILE_COPY] = "Copying files",
	[OP_WRITE_IMAGE] = "Writing image",
	[OP_VERIFY] = "Verifying",
	[OP_FINALIZE] = "Finalizing",
	[OP_SAVE_IMAGE] = "Saving image",
	[OP_HASH] = "Computing checksums",
	[OP_DOWNLOAD] = "Downloading",
};

const char* rf_op_name(rf_op_t op)
{
	return (op >= 0 && op < OP_MAX) ? op_names[op] : "";
}

void rf_set_log_callback(rf_log_cb cb, void* ctx)
{
	log_cb = cb;
	log_ctx = ctx;
}

void rf_set_progress_callback(rf_progress_cb cb, void* ctx)
{
	progress_cb = cb;
	progress_ctx = ctx;
}

static void vlog(const char* fmt, va_list args)
{
	char buf[4096];
	size_t len;

	vsnprintf(buf, sizeof(buf), fmt, args);
	/* ms-sys and syslinux messages may carry their own line endings */
	len = strlen(buf);
	while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
		buf[--len] = 0;

	pthread_mutex_lock(&log_mutex);
	if (log_cb != NULL)
		log_cb(buf, log_ctx);
	else
		fprintf(stderr, "%s\n", buf);
	pthread_mutex_unlock(&log_mutex);
}

void rf_log(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vlog(fmt, args);
	va_end(args);
}

void uprintf(const char* fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	vlog(fmt, args);
	va_end(args);
}

void rf_progress(rf_op_t op, double fraction, const char* fmt, ...)
{
	char buf[512] = "";
	va_list args;

	if (fmt != NULL) {
		va_start(args, fmt);
		vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
	}
	if (fraction > 1.0)
		fraction = 1.0;
	if (progress_cb != NULL)
		progress_cb(op, fraction, buf[0] ? buf : rf_op_name(op), progress_ctx);
}

void rf_cancel(void)
{
	atomic_store(&cancelled, true);
}

bool rf_cancelled(void)
{
	return atomic_load(&cancelled);
}

void rf_reset_cancel(void)
{
	atomic_store(&cancelled, false);
}
