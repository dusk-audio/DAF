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

// A DGL_BACKEND=both build chooses X11 or Wayland when it opens a world. This test works out from
// the environment which one the documented rule picks (dgl/src/runtime-backend.md), checks that pugl
// picked the same for a program and for a module, then opens an OpenGL window on it, draws, checks
// the frame reached the display server, resizes it and sends it an event.
//
// It runs wherever it is started: under Xvfb alone, under a headless Wayland compositor alone, with
// both, and with DAF_WINDOW_BACKEND forcing either. The environment it reads is the contract: a set
// DISPLAY means an X server that answers, a set WAYLAND_DISPLAY or WAYLAND_SOCKET a compositor that
// does, and when the rule leaves no backend, opening the world has to fail. A forced Wayland with
// WAYLAND_DISPLAY unset lets libwayland try wayland-0, so XDG_RUNTIME_DIR must not hold one then.

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "dgl/src/pugl.cpp"

#ifndef DGL_RUNTIME_BACKEND
# error This test is for DGL_BACKEND=both builds
#endif

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace DGL_NAMESPACE;

enum Expected { kNone, kX11, kWayland };

static const char* name(const Expected expected)
{
    return expected == kX11 ? "x11" : expected == kWayland ? "wayland" : "none";
}

static bool isSet(const char* const var)
{
    const char* const value = std::getenv(var);
    return value != nullptr && value[0] != '\0';
}

static Expected expectedBackend(const PuglWorldType type)
{
    const bool x11 = isSet("DISPLAY");
    const bool wayland = isSet("WAYLAND_DISPLAY") || isSet("WAYLAND_SOCKET");

    if (const char* const forced = std::getenv("DAF_WINDOW_BACKEND"))
    {
        if (std::strcmp(forced, "x11") == 0)
            return x11 ? kX11 : kNone;
        if (std::strcmp(forced, "wayland") == 0)
            return wayland ? kWayland : kNone;
    }

    if (type == PUGL_MODULE)
        return x11 ? kX11 : wayland ? kWayland : kNone;

    return wayland ? kWayland : x11 ? kX11 : kNone;
}

// --------------------------------------------------------------------------------------------------------------------

static const uintptr_t kClientData1 = 0xDAF0;
static const uintptr_t kClientData2 = 0x2026;

struct State
{
    bool realized = false;
    bool exposed = false;
    bool contextCurrent = false;
    bool drewColour = false;
    bool gotClient = false;
    unsigned configuredWidth = 0;
    unsigned configuredHeight = 0;
};

static PuglStatus onEvent(PuglView* const view, const PuglEvent* const event)
{
    State* const state = static_cast<State*>(puglGetHandle(view));

    switch (event->type)
    {
    case PUGL_REALIZE:
        state->realized = true;
        break;
    case PUGL_CONFIGURE:
        state->configuredWidth = event->configure.width;
        state->configuredHeight = event->configure.height;
        break;
    case PUGL_EXPOSE:
    {
        // pugl has entered the view's context for this, GLX or EGL depending on the backend
        state->contextCurrent = puglBackendIsCurrent(view, false);

        glViewport(0, 0, static_cast<GLsizei>(state->configuredWidth), static_cast<GLsizei>(state->configuredHeight));
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        GLubyte pixel[4] = {};
        glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        state->drewColour = pixel[0] > 50 && pixel[0] < 80
                         && pixel[1] > 115 && pixel[1] < 140
                         && pixel[2] > 180 && pixel[2] < 205;
        state->exposed = true;
        break;
    }
    case PUGL_CLIENT:
        if (event->client.data1 == kClientData1 && event->client.data2 == kClientData2)
            state->gotClient = true;
        break;
    default:
        break;
    }

    return PUGL_SUCCESS;
}

template <class Condition>
static bool pumpUntil(PuglWorld* const world, Condition condition, const double seconds = 5.0)
{
    const std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(seconds * 1000));

    while (! condition())
    {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        puglUpdate(world, 0.01);
    }

    return true;
}

// The cleared colour in the window as the X server has it, read back over the X protocol.
static bool x11ServerHasColour(PuglView* const view)
{
    Display* const display = static_cast<Display*>(puglGetNativeWorld(puglGetWorld(view)));
    const ::Window window = static_cast<::Window>(puglGetNativeView(view));

    XSync(display, False);
    XImage* const image = XGetImage(display, window, 2, 2, 1, 1, AllPlanes, ZPixmap);
    if (image == nullptr)
        return false;

    const unsigned long pixel = XGetPixel(image, 0, 0);
    const unsigned red   = static_cast<unsigned>((pixel & image->red_mask) >> 16);
    const unsigned green = static_cast<unsigned>((pixel & image->green_mask) >> 8);
    const unsigned blue  = static_cast<unsigned>(pixel & image->blue_mask);
    XDestroyImage(image);

    return red > 50 && red < 80 && green > 115 && green < 140 && blue > 180 && blue < 205;
}

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    // a module's world follows its own rule, and only which backend it opens on is checked here
    {
        const Expected expected = expectedBackend(PUGL_MODULE);
        PuglWorld* const world = puglNewWorld(PUGL_MODULE, 0);

        if (expected == kNone)
        {
            DAF_ASSERT_EQUAL(world, nullptr, "a module world cannot open without its backend");
        }
        else
        {
            DAF_ASSERT_NOT_EQUAL(world, nullptr, "a module world opens");
            DAF_ASSERT_EQUAL(puglWorldIsX11(world), expected == kX11, "a module world opens on the expected backend");
            puglFreeWorld(world);
        }

        d_stdout("RuntimeBackend: module world on %s", name(expected));
    }

    const Expected expected = expectedBackend(PUGL_PROGRAM);
    PuglWorld* const world = puglNewWorld(PUGL_PROGRAM, 0);

    if (expected == kNone)
    {
        DAF_ASSERT_EQUAL(world, nullptr, "a program world cannot open without its backend");
        d_stdout("RuntimeBackend: no backend available, as expected");
        return 0;
    }

    DAF_ASSERT_NOT_EQUAL(world, nullptr, "a program world opens");
    DAF_ASSERT_EQUAL(puglWorldIsX11(world), expected == kX11, "a program world opens on the expected backend");

    State state;
    PuglView* const view = puglNewView(world);
    DAF_ASSERT_NOT_EQUAL(view, nullptr, "a view is created");
    DAF_ASSERT_EQUAL(puglViewIsX11(view), expected == kX11, "a view is on the backend of its world");
    DAF_ASSERT_EQUAL(puglGetWorld(view), world, "a view hands back the world it was created in");

    puglSetHandle(view, &state);
    puglSetEventFunc(view, onEvent);
    puglSetMatchingBackendForCurrentBuild(view);
    puglSetViewString(view, PUGL_WINDOW_TITLE, "DAF runtime backend test");
    puglSetViewHint(view, PUGL_RESIZABLE, PUGL_TRUE);
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 200, 150);

    DAF_ASSERT_EQUAL(puglRealize(view), PUGL_SUCCESS, "the window realizes");
    DAF_ASSERT_EQUAL(state.realized, true, "the realize event arrives");
    DAF_ASSERT_EQUAL(puglShow(view, PUGL_SHOW_RAISE), PUGL_SUCCESS, "the window shows");
    DAF_ASSERT_NOT_EQUAL(puglGetNativeView(view), static_cast<PuglNativeView>(0), "the window has a native handle");

    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.exposed; }), true, "the window is exposed");
    DAF_ASSERT_EQUAL(state.contextCurrent, true, "the window's GL context is current while it draws");
    DAF_ASSERT_EQUAL(state.drewColour, true, "drawing into the window's GL context works");

    // the frame has to reach the display server, not only the client's own buffer
    if (expected == kX11)
    {
        DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return x11ServerHasColour(view); }), true,
                         "the X server has the drawn frame");
    }
    else
    {
        const pugl_wl::PuglView* const wlView = static_cast<const pugl_wl::PuglView*>(view);
        DAF_ASSERT_EQUAL(wlView->impl->configured, true, "the compositor configured the surface");
        DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return wlView->impl->frameCallbackWorks; }), true,
                         "the compositor presented a frame");
    }

    // a size change goes to the display server and comes back as a configure
    state.exposed = false;
    DAF_ASSERT_EQUAL(puglSetSizeHint(view, PUGL_CURRENT_SIZE, 240, 180), PUGL_SUCCESS, "the window can be resized");
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.configuredWidth == 240 && state.configuredHeight == 180; }),
                     true, "the new size is configured");
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.exposed; }), true, "the resized window is exposed again");

    PuglEvent client = {};
    client.client.type = PUGL_CLIENT;
    client.client.data1 = kClientData1;
    client.client.data2 = kClientData2;
    DAF_ASSERT_EQUAL(puglSendEvent(view, &client), PUGL_SUCCESS, "an event can be sent to the window");
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.gotClient; }), true, "the window receives the event");

    puglHide(view);
    puglFreeView(view);
    puglFreeWorld(world);

    d_stdout("RuntimeBackend: program world on %s, window realized, drew, presented, resized and got its event",
             name(expected));
    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
