/*
 * SPDX-FileCopyrightText: Stone Tickle <lattis@mochiro.moe>
 * SPDX-License-Identifier: GPL-3.0-only
 */

#ifndef MUON_EXTERNAL_LIBARCHIVE_H
#define MUON_EXTERNAL_LIBARCHIVE_H

#include <stdbool.h>
#include <stddef.h>

extern const bool have_libarchive;

bool muon_archive_extract(const char *buf, size_t size, const char *dest_path);
#endif
