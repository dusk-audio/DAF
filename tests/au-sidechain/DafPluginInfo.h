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

// The plugin AUSidechainLayout links into itself: a stereo effect with a stereo sidechain on ports 2-3, stereo and
// mono main layouts, and a sidechain allowed on top of either (DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO).

#define DAF_PLUGIN_BRAND     "DAF"
#define DAF_PLUGIN_NAME      "SidechainLayoutTest"
#define DAF_PLUGIN_URI       "urn:daf:tests:sidechain-layout"
#define DAF_PLUGIN_CLAP_ID   "audio.dusk.daf.tests.sidechain-layout"
#define DAF_PLUGIN_BRAND_ID  Dusk
#define DAF_PLUGIN_UNIQUE_ID dScL

#define DAF_PLUGIN_HAS_UI      0
#define DAF_PLUGIN_IS_RT_SAFE  1
#define DAF_PLUGIN_NUM_INPUTS  4
#define DAF_PLUGIN_NUM_OUTPUTS 2
#define DAF_PLUGIN_EXTRA_IO    { 2, 2 }, { 1, 1 },
#define DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO 1

#endif // DAF_PLUGIN_INFO_H_INCLUDED
