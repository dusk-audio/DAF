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

/* The static target serves a host that links a plugin into its own process and embeds its UI in a
 * view of its own. That host owns the editor's size and places the editor inside its parent, so the
 * exporter must offer both. Checked at build time, since calling them needs a native parent window:
 * this translation unit compiling is the test, and it is never linked.
 */

#define DAF_PLUGIN_TARGET_STATIC

#include "tests.hpp"

#include "daf/src/DafUIInternal.hpp"

#include <type_traits>

static_assert(DAF_UI_USES_SIZE_REQUEST, "the static target must leave the editor's size to the host");

static_assert(std::is_same<decltype(&DAF_NAMESPACE::UIExporter::setWindowSizeFromHost),
                           void (DAF_NAMESPACE::UIExporter::*)(uint, uint)>::value,
              "a static-target host must be able to size the editor");

static_assert(std::is_same<decltype(&DAF_NAMESPACE::UIExporter::setWindowEmbeddedOffset),
                           bool (DAF_NAMESPACE::UIExporter::*)(int, int)>::value,
              "a static-target host must be able to place the embedded editor");
