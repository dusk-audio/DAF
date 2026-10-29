/*
 * Dusk Audio Framework (DAF)
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

// Which backend the selection rule of dgl/src/runtime-backend.md gives a world in the environment a
// runtime backend test runs in. The environment is the contract: a set DISPLAY means an X server that
// answers, a set WAYLAND_DISPLAY or WAYLAND_SOCKET a compositor that does.
//
// DAF_TEST_EXPECT_BACKEND (x11, wayland or none) names the backend a program world must get instead.
// Set by CI, it makes a display server that failed to start fail the test, rather than turn it into a
// check that nothing opens, and it covers environments outside that contract, such as a compositor EGL
// cannot render on.

#pragma once

#include <cstdlib>
#include <cstring>

enum Expected { kNone, kX11, kWayland, kInvalid };

static inline const char* backendName(const Expected expected)
{
    return expected == kX11 ? "x11"
         : expected == kWayland ? "wayland"
         : expected == kNone ? "none"
         : "invalid";
}

static inline bool isSet(const char* const var)
{
    const char* const value = std::getenv(var);
    return value != nullptr && value[0] != '\0';
}

static inline Expected parseExpected(const char* const value)
{
    if (std::strcmp(value, "x11") == 0)
        return kX11;
    if (std::strcmp(value, "wayland") == 0)
        return kWayland;
    if (std::strcmp(value, "none") == 0)
        return kNone;
    return kInvalid;
}

static inline Expected expectedBackend(const bool program)
{
    const bool x11 = isSet("DISPLAY");
    const bool wayland = isSet("WAYLAND_DISPLAY") || isSet("WAYLAND_SOCKET");

    if (! program)
        return x11 ? kX11 : wayland ? kWayland : kNone;

    if (const char* const required = std::getenv("DAF_TEST_EXPECT_BACKEND"))
        return parseExpected(required);

    if (const char* const forced = std::getenv("DAF_WINDOW_BACKEND"))
    {
        if (std::strcmp(forced, "x11") == 0)
            return x11 ? kX11 : kNone;
        if (std::strcmp(forced, "wayland") == 0)
            return wayland ? kWayland : kNone;
    }

    return wayland ? kWayland : x11 ? kX11 : kNone;
}
