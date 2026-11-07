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

#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <cstddef>

// What the plugin binary AUViewDispose loads calls, watched from libAUViewDisposeWatch, which the test links: dyld
// applies the __DATA,__interpose tuples of a library loaded at launch to every image bound after it, the plugin
// bundle's included, and ignores those of a main executable. The library's own calls go to the real functions.

// exported from the library, which the tests build with hidden visibility
#define AU_VIEW_WATCH_API __attribute__((visibility("default")))

extern "C" {

// The plugin binary, by its mach header: calls made from any other image are not watched.
AU_VIEW_WATCH_API void auViewWatchPluginImage(const void* header);

// Units the host disposed of, which the plugin must not call again. A new unit can come back with the same handle.
AU_VIEW_WATCH_API void auViewWatchDisposed(AudioUnit unit);
AU_VIEW_WATCH_API void auViewWatchCreated(AudioUnit unit);

// How many calls the plugin made on disposed units, and the first such function.
AU_VIEW_WATCH_API int auViewWatchCallsOnDisposedUnits(const char** first);

// How many run loop timers the plugin made since the last forget, and how many of those are still valid.
AU_VIEW_WATCH_API size_t auViewWatchPluginTimers(size_t* valid);
AU_VIEW_WATCH_API void auViewWatchForgetTimers(void);

}
