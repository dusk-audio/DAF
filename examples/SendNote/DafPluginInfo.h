/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2024 Filipe Coelho <falktx@falktx.com>
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

#define DAF_PLUGIN_BRAND   "Dusk Audio"
#define DAF_PLUGIN_NAME    "SendNote"
#define DAF_PLUGIN_URI     "http://distrho.sf.net/examples/SendNote"
#define DAF_PLUGIN_CLAP_ID "studio.kx.daf.examples.send-note"

#define DAF_PLUGIN_BRAND_ID  Dstr
#define DAF_PLUGIN_UNIQUE_ID dSNo

#define DAF_PLUGIN_HAS_UI           1
#define DAF_PLUGIN_IS_RT_SAFE       1
#define DAF_PLUGIN_IS_SYNTH         1
#define DAF_PLUGIN_NUM_INPUTS       0
#define DAF_PLUGIN_NUM_OUTPUTS      1
#define DAF_PLUGIN_WANT_MIDI_INPUT  1
#define DAF_PLUGIN_WANT_MIDI_OUTPUT 0
#define DAF_UI_FILE_BROWSER         0

#endif // DAF_PLUGIN_INFO_H_INCLUDED
