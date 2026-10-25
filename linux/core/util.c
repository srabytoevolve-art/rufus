/*
 * Rufus for Linux: utility functions
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
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "rufus.h"

extern char** environ;

const char* rf_size_to_human(uint64_t size, bool decimal_units)
{
	static __thread char buf[32];
	static const char* suffix[] = { "bytes", "KB", "MB", "GB", "TB", "PB" };
	double base = decimal_units ? 1000.0 : 1024.0;
	double hr = (double)size;
	int i;

	for (i = 0; i < (int)ARRAYSIZE(suffix) - 1 && hr >= base; i++)
		hr /= base;
	if (i == 0)
		snprintf(buf, sizeof(buf), "%u %s", (unsigned)size, suffix[0]);
	else if (hr - (int)hr < 0.05 || hr >= 100.0)
		snprintf(buf, sizeof(buf), "%d %s", (int)(hr + 0.5), suffix[i]);
	else
		snprintf(buf, sizeof(buf), "%.1f %s", hr, suffix[i]);
	return buf;
}

char* rf_strdup_printf(const char* fmt, ...)
{
	char* s = NULL;
	va_list args;

	va_start(args, fmt);
	if (vasprintf(&s, fmt, args) < 0)
		s = NULL;
	va_end(args);
	return s;
}

bool rf_str_iequal(const char* a, const char* b)
{
	if (a == NULL || b == NULL)
		return a == b;
	return strcasecmp(a, b) == 0;
}

bool rf_str_iprefix(const char* s, const char* prefix)
{
	if (s == NULL || prefix == NULL)
		return false;
	return strncasecmp(s, prefix, strlen(prefix)) == 0;
}

bool rf_str_isuffix(const char* s, const char* suffix)
{
	size_t ls, lx;

	if (s == NULL || suffix == NULL)
		return false;
	ls = strlen(s);
	lx = strlen(suffix);
	return (ls >= lx) && strcasecmp(&s[ls - lx], suffix) == 0;
}

char* rf_read_text_file(const char* path, size_t* len)
{
	FILE* fd = fopen(path, "rb");
	char* buf = NULL;
	long size;

	if (fd == NULL)
		return NULL;
	if (fseek(fd, 0, SEEK_END) != 0 || (size = ftell(fd)) < 0 || fseek(fd, 0, SEEK_SET) != 0)
		goto out;
	buf = malloc((size_t)size + 1);
	if (buf == NULL)
		goto out;
	if (fread(buf, 1, (size_t)size, fd) != (size_t)size) {
		free(buf);
		buf = NULL;
		goto out;
	}
	buf[size] = 0;
	if (len != NULL)
		*len = (size_t)size;
out:
	fclose(fd);
	return buf;
}

bool rf_write_file(const char* path, const void* data, size_t len)
{
	FILE* fd = fopen(path, "wb");
	bool r;

	if (fd == NULL) {
		uprintf("Could not create '%s': %s", path, strerror(errno));
		return false;
	}
	r = (fwrite(data, 1, len, fd) == len);
	if (fclose(fd) != 0)
		r = false;
	if (!r)
		uprintf("Could not write '%s': %s", path, strerror(errno));
	return r;
}

bool rf_mkdir_p(const char* path, int mode)
{
	char tmp[4096];
	size_t len = strlen(path);

	if (len == 0 || len >= sizeof(tmp))
		return false;
	memcpy(tmp, path, len + 1);
	for (char* p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, mode) != 0 && errno != EEXIST)
				return false;
			*p = '/';
		}
	}
	return mkdir(tmp, mode) == 0 || errno == EEXIST;
}

bool rf_which(const char* prog, char* out, size_t out_size)
{
	/* Root's PATH from pkexec is minimal, so also look in the usual sbin dirs */
	static const char* extra_dirs = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
	char paths[8192], candidate[4096];
	const char* env = getenv("PATH");

	if (strchr(prog, '/') != NULL) {
		if (access(prog, X_OK) != 0)
			return false;
		if (out != NULL)
			snprintf(out, out_size, "%s", prog);
		return true;
	}
	snprintf(paths, sizeof(paths), "%s:%s", (env != NULL) ? env : "", extra_dirs);
	for (char *save = NULL, *dir = strtok_r(paths, ":", &save); dir != NULL; dir = strtok_r(NULL, ":", &save)) {
		if (*dir == 0)
			continue;
		snprintf(candidate, sizeof(candidate), "%s/%s", dir, prog);
		if (access(candidate, X_OK) == 0) {
			if (out != NULL)
				snprintf(out, out_size, "%s", candidate);
			return true;
		}
	}
	return false;
}

static int run_internal(const char* const argv[], char** capture)
{
	char exe[4096], cmdline[4096] = "", line[4096];
	int out_pipe[2] = { -1, -1 }, status = -1;
	size_t cap_len = 0, cap_size = 0, line_len = 0;
	posix_spawn_file_actions_t fa;
	pid_t pid;
	ssize_t n;
	char buf[4096];

	if (!rf_which(argv[0], exe, sizeof(exe))) {
		uprintf("Could not find '%s' - please install it", argv[0]);
		return -1;
	}
	for (int i = 0, pos = 0; argv[i] != NULL && pos < (int)sizeof(cmdline) - 1; i++)
		pos += snprintf(&cmdline[pos], sizeof(cmdline) - (size_t)pos, "%s ", argv[i]);
	uprintf("Running: %s", cmdline);

	if (pipe2(out_pipe, O_CLOEXEC) != 0)
		return -1;
	posix_spawn_file_actions_init(&fa);
	posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDOUT_FILENO);
	if (capture == NULL)
		posix_spawn_file_actions_adddup2(&fa, out_pipe[1], STDERR_FILENO);
	posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
	if (posix_spawn(&pid, exe, &fa, NULL, (char* const*)argv, environ) != 0) {
		uprintf("Could not run '%s': %s", exe, strerror(errno));
		posix_spawn_file_actions_destroy(&fa);
		close(out_pipe[0]);
		close(out_pipe[1]);
		return -1;
	}
	posix_spawn_file_actions_destroy(&fa);
	close(out_pipe[1]);

	while ((n = read(out_pipe[0], buf, sizeof(buf))) != 0) {
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (capture != NULL) {
			if (cap_len + (size_t)n + 1 > cap_size) {
				cap_size = (cap_len + (size_t)n + 1) * 2;
				*capture = realloc(*capture, cap_size);
			}
			memcpy(*capture + cap_len, buf, (size_t)n);
			cap_len += (size_t)n;
			(*capture)[cap_len] = 0;
			continue;
		}
		/* Relay the tool's output to our log, line by line */
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || buf[i] == '\r' || line_len == sizeof(line) - 1) {
				line[line_len] = 0;
				if (line_len > 0)
					uprintf("  %s", line);
				line_len = 0;
			} else {
				line[line_len++] = buf[i];
			}
		}
	}
	if (line_len > 0) {
		line[line_len] = 0;
		uprintf("  %s", line);
	}
	close(out_pipe[0]);

	while (waitpid(pid, &status, 0) < 0) {
		if (errno != EINTR)
			return -1;
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	return -1;
}

int rf_run(const char* const argv[])
{
	return run_internal(argv, NULL);
}

int rf_run_capture(const char* const argv[], char** out)
{
	*out = NULL;
	return run_internal(argv, out);
}

void rf_udev_settle(void)
{
	char exe[256];
	const char* argv[] = { "udevadm", "settle", "--timeout=10", NULL };

	if (rf_which("udevadm", exe, sizeof(exe))) {
		char* out = NULL;
		rf_run_capture(argv, &out);
		free(out);
	} else {
		sleep(1);
	}
}

uint64_t rf_monotonic_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
