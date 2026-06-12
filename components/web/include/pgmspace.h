// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal Arduino <pgmspace.h> compatibility shim for native ESP-IDF.
// web_assets.h is a generated, do-not-edit file that includes <pgmspace.h>
// and marks its gzip blobs PROGMEM. Under ESP-IDF flash is memory-mapped, so
// PROGMEM is a no-op and the blobs are plain const arrays. This shim lets the
// generated header compile unmodified.
#pragma once

#ifndef PROGMEM
#define PROGMEM
#endif
