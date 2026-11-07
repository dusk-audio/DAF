/*
 * Dusk Audio Framework (DAF)
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

#ifndef DAF_PLUGIN_INFO_H_INCLUDED
#define DAF_PLUGIN_INFO_H_INCLUDED

// The plugin AUViewDispose hosts as a built AU bundle: a stereo effect whose UI reads the DSP straight off the
// instance pointer on every idle tick and every paint, as plugins read their meters (see ViewDisposeUI.cpp).

#define DAF_PLUGIN_BRAND     "DAF"
#define DAF_PLUGIN_NAME      "ViewDisposeTest"
#define DAF_PLUGIN_URI       "urn:daf:tests:au-view"
#define DAF_PLUGIN_CLAP_ID   "audio.dusk.daf.tests.au-view"

#define DAF_PLUGIN_BRAND_ID  Dusk
#define DAF_PLUGIN_UNIQUE_ID dVwT

#define DAF_PLUGIN_HAS_UI             1
#define DAF_PLUGIN_IS_RT_SAFE         1
#define DAF_PLUGIN_NUM_INPUTS         2
#define DAF_PLUGIN_NUM_OUTPUTS        2
#define DAF_PLUGIN_WANT_DIRECT_ACCESS 1

#endif // DAF_PLUGIN_INFO_H_INCLUDED
