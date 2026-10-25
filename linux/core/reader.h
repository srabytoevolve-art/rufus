/*
 * Rufus for Linux: streaming (optionally decompressing) image reader
 * Copyright © 2026 Rufus for Linux contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */
#pragma once

#include <sys/types.h>
#include "rufus.h"

typedef struct rf_reader rf_reader_t;

rf_compression_t rf_compression_from_path(const char* path);
const char* rf_compression_name(rf_compression_t comp);
/* limit: for uncompressed images, the number of bytes to return (0 = whole file) */
rf_reader_t* rf_reader_open(const char* path, rf_compression_t comp, uint64_t limit);
ssize_t rf_reader_read(rf_reader_t* r, void* buf, size_t len);
/* Number of bytes consumed from the underlying file (for progress) */
uint64_t rf_reader_input_pos(const rf_reader_t* r);
void rf_reader_close(rf_reader_t* r);
