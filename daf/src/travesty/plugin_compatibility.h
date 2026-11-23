/*
 * travesty, pure C VST3-compatible interface
 * Copyright (C) 2021-2022 Filipe Coelho <falktx@falktx.com>
 * Copyright (C) 2026 Dusk Audio
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with
 * or without fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include "bstream.h"

/**
 * plugin compatibility
 *
 * A factory lists one class of this category, next to its audio module classes, to tell the host
 * which older class IDs its current classes replace. The class writes UTF-8 JSON to the stream:
 * an array of { "New": "<class ID>", "Old": ["<class ID>", ...] }, each ID as the 32 hex digits
 * the host shows for it on this platform (COM GUID field order on Windows).
 */

#define V3_PLUGIN_COMPATIBILITY_CLASS "Plugin Compatibility Class"

struct v3_plugin_compatibility {
#ifndef __cplusplus
	struct v3_funknown;
#endif
	v3_result (V3_API* get_compatibility_json)(void* self, struct v3_bstream** stream);
};

static constexpr const v3_tuid v3_plugin_compatibility_iid =
	V3_ID(0x4AFD4B6A, 0x35D7C240, 0xA5C31414, 0xFB7D15E6);

#ifdef __cplusplus

/**
 * C++ variants
 */

struct v3_plugin_compatibility_cpp : v3_funknown {
	v3_plugin_compatibility compat;
};

#endif
