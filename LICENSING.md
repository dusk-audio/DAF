# DAF - Dusk Audio Framework

Even though DAF is quite liberally licensed, not all plugin formats follow the same ideals.  
This is usually due to plugin APIs/headers being tied to a specific license or having commercial restrictions.  
This file describes the licensing that applies to each individual plugin format as a way to make it clear what is possible and compatible.

Regardless of target format, DAF itself needs to be mentioned in attribution, with the copyright and permission
notice from the [LICENSE](LICENSE) file. DAF is derived from the DISTRHO Plugin Framework (DPF), and that notice is
its authors'.

| Target          | License(s)            | License restrictions  | Additional attribution                  |
|-----------------|-----------------------|-----------------------|-----------------------------------------|
| JACK/Standalone | MIT (RtAudio, RtMidi) | Copyright attribution | **RtAudio**: 2001-2021 Gary P. Scavone  |
| LADSPA          | LGPLv2.1+             | ??? (*)               | 2000-2002 Richard W. E. Furse, Paul Barton-Davis, Stefan Westerfeld |
| DSSI            | LGPLv2.1+             | ??? (*)               | **DSSI**: 2004, 2009 Chris Cannam, Steve Harris and Sean Bolton;<br/> **ALSA**: 1998-2001 Jaroslav Kysela, Abramo Bagnara, Takashi Iwai |
| LV2             | ISC                   | Copyright attribution | 2006-2020 Steve Harris, David Robillard;<br/> 2000-2002 Richard W.E. Furse, Paul Barton-Davis, Stefan Westerfeld |
| VST2            | BSD-3                 | Copyright attribution | 2020-2022 Michael Fabian 'Xaymar' Dirks |
| VST3            | ISC                   | Copyright attribution | (none, only DAF files used)             |
| CLAP            | MIT                   | Copyright attribution | 2014-2022 Alexandre Bique               |
| AU              | ISC                   | Copyright attribution | (none, only DAF files used)             |

### LADSPA and DSSI special note

The header files on LADSPA and DSSI are LGPLv2.1+ licensed, which is unusual for pure APIs without libraries.  
LADSPA authors mention this on ladspa.org homepage:

> LADSPA has been released under LGPL (GNU Lesser General Public License).
> This is not intended to be the final license for LADSPA.
> In the long term it is hoped that LADSPA will have a public license that is even less restrictive, so that commercial applications can use it (in a protected way) without having to use a derived LGPL library.
> It may be that LGPL is already free enough for this, but we aren't sure.

So the situation for LADSPA/DSSI plugins is unclear for commercial plugins.  
These formats are very limited and not much used anymore anyway, feel free to skip them if this situation is a potential issue for you.

### VST2 special note

DAF's VST2 implementation uses https://github.com/Xaymar/vst2sdk which is a liberally-licensed "clean room" untainted reverse engineered "SDK" for the VST2 interface.  
Previously "vestige" was used, but was problematic due to it being GPLv2 licensed.  
With the Xaymar's work, both open-source and proprietary plugins can be created from the same source, which helps in maintenance on the DAF side.

### VST3 special note

Contrary to most plugins, DAF does not use the official VST3 SDK.  
Instead, the API definitions are provided by the [travesty](daf/src/travesty/) sub-project, licensed in the same way as DAF.
This allows us to freely build plugins without being encumbered by restrictive licensing deals.  
It makes the internal implementation much harder for DAF, but this is not an issue for external developers.

## widgets/

The widget bridge and kit use the ISC terms below; individual source copyright
notices are retained. Dear ImGui is MIT-licensed: see
[LICENSE.txt](widgets/imgui/DearImGui/LICENSE.txt). DearImGuiKnobs is MIT-licensed
([notice](widgets/imgui/DearImGuiKnobs/LICENSE)); DearImGuiToggle uses the permissive
ISC-style terms in its [notice](widgets/imgui/DearImGuiToggle/LICENSE).

DISTRHO Plugin Framework (DPF)
Copyright (C) 2012-2021 Filipe Coelho <falktx@falktx.com>

Permission to use, copy, modify, and/or distribute this software for any purpose with
or without fee is hereby granted, provided that the above copyright notice and this
permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
