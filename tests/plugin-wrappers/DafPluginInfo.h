/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2025 Filipe Coelho <falktx@falktx.com>
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

// The plugin the *Wrapper tests link into themselves, each with one format wrapper compiled in and
// main() acting as the host. See WrapperTestPlugin.hpp.

#define DAF_PLUGIN_BRAND     "DAF"
#define DAF_PLUGIN_NAME      "WrapperTest"
#define DAF_PLUGIN_URI       "urn:daf:tests:wrapper"
#define DAF_PLUGIN_CLAP_ID   "audio.dusk.daf.tests.wrapper"
#define DAF_PLUGIN_BRAND_ID  Dusk
#define DAF_PLUGIN_UNIQUE_ID dWrT

#define DAF_PLUGIN_HAS_UI       0
#define DAF_PLUGIN_IS_RT_SAFE   1
#define DAF_PLUGIN_NUM_INPUTS   1
#define DAF_PLUGIN_NUM_OUTPUTS  1
#define DAF_PLUGIN_WANT_TIMEPOS 1

// LADSPA has no time position
#if defined(DAF_PLUGIN_TARGET_LADSPA)
# undef DAF_PLUGIN_WANT_TIMEPOS
# define DAF_PLUGIN_WANT_TIMEPOS 0
#endif

// LV2 state is tested through the worker
#if defined(DAF_PLUGIN_TARGET_LV2)
# define DAF_PLUGIN_WANT_STATE 1
#endif

// VST3 class IDs are tested as they are with the cross-platform layout
#if defined(DAF_PLUGIN_TARGET_VST3)
# define DAF_VST3_CROSS_PLATFORM_UID
#endif

#endif // DAF_PLUGIN_INFO_H_INCLUDED
