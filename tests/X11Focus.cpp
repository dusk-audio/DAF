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

// An embedded X11 view and its host, as Ardour hosts a VST3 UI: the host owns a window on its own
// connection, the view lives in it on pugl's, and the host has installed a process-wide X error
// handler that exits, as GDK's does. Three regressions, all seen in Ardour 9.8 on XWayland:
//  - the host asks for the focus (VST3 onFocus) after the user has switched to another
//    application, and the view took it with XSetInputFocus(CurrentTime), stealing it back;
//  - the view took the focus with RevertToNone, so closing the plugin window left no window
//    with the keyboard;
//  - a request of the view's failed (BadMatch on XSetInputFocus, the host having unmapped the
//    window), the error reached the host's handler, and the host exited, deadlocking in its own
//    exit-time destructors.
// Needs an X server; CI runs the unit tests under xvfb-run.

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "dgl/src/pugl.cpp"

static int hostErrorCount = 0;

static int hostErrorHandler(Display*, XErrorEvent*)
{
    ++hostErrorCount;
    return 0;
}

static DGL_NAMESPACE::PuglStatus onEvent(DGL_NAMESPACE::PuglView*, const DGL_NAMESPACE::PuglEvent*)
{
    return DGL_NAMESPACE::PUGL_SUCCESS;
}

static ::Window makeTopLevel(Display* const display)
{
    const ::Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 300, 200, 0, 0, 0);
    XMapWindow(display, window);
    XSync(display, False);
    return window;
}

static ::Window focusOf(Display* const display)
{
    ::Window focus = None;
    int revert = RevertToNone;
    XSync(display, False);
    XGetInputFocus(display, &focus, &revert);
    return focus;
}

static bool waitViewable(DGL_NAMESPACE::PuglWorld* const world, Display* const display, const ::Window window)
{
    for (int i = 0; i < 200; ++i)
    {
        DGL_NAMESPACE::puglUpdate(world, 0.005);
        XWindowAttributes attrs = {};
        if (XGetWindowAttributes(display, window, &attrs) && attrs.map_state == IsViewable)
            return true;
    }
    return false;
}

int main()
{
    using namespace DGL_NAMESPACE;

    Display* const host = XOpenDisplay(nullptr);
    DAF_ASSERT_NOT_EQUAL(host, nullptr, "an X server is reachable");

    // the host installs its handler first, as GDK does when it opens its display
    XSetErrorHandler(hostErrorHandler);

    const ::Window hostWindow = makeTopLevel(host);
    const ::Window otherApp   = makeTopLevel(host);

    PuglWorld* const world = puglNewWorld(PUGL_MODULE, 0);
    DAF_ASSERT_NOT_EQUAL(world, nullptr, "world");
    Display* const display = world->impl->display;

    PuglView* const view = puglNewView(world);
    puglSetBackend(view, puglStubBackend());
    puglSetEventFunc(view, onEvent);
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 100, 80);
    puglSetParent(view, static_cast<PuglNativeView>(hostWindow));
    DAF_ASSERT_EQUAL(puglRealize(view), PUGL_SUCCESS, "embedded view realizes");
    DAF_ASSERT_EQUAL(puglShow(view, PUGL_SHOW_PASSIVE), PUGL_SUCCESS, "embedded view shows");
    const ::Window viewWindow = static_cast<::Window>(puglGetNativeView(view));
    DAF_ASSERT_EQUAL(waitViewable(world, display, viewWindow), true, "embedded view becomes viewable");

    // 1. the user is in another application: a host focus request must not take the keyboard back
    XSetInputFocus(host, otherApp, RevertToNone, CurrentTime);
    DAF_ASSERT_EQUAL(focusOf(host), otherApp, "another application has the focus");
    DAF_ASSERT_EQUAL(puglGrabFocus(view), PUGL_FAILURE, "the view declines focus held by another application");
    XSync(display, False); // a request the view left queued reaches the server here at the latest
    DAF_ASSERT_EQUAL(focusOf(host), otherApp, "the other application keeps the focus");

    // 2. the focus is in the host's own window: the view takes it, as before
    XSetInputFocus(host, hostWindow, RevertToNone, CurrentTime);
    DAF_ASSERT_EQUAL(focusOf(host), hostWindow, "the host window has the focus");
    DAF_ASSERT_EQUAL(puglGrabFocus(view), PUGL_SUCCESS, "the view takes focus from its own host window");
    XSync(display, False);
    DAF_ASSERT_EQUAL(focusOf(host), viewWindow, "the view has the focus");

    // 3. closing the plugin window withdraws the host's top level with the view in it: the
    //    keyboard must not be left with no window at all
    XUnmapWindow(host, hostWindow);
    XSync(host, False);
    puglUpdate(world, 0.0);
    DAF_ASSERT_NOT_EQUAL(focusOf(host), static_cast<::Window>(None), "the host closing its window leaves a focus");

    // and the same when only the view is hidden
    XMapWindow(host, hostWindow);
    XSync(host, False);
    DAF_ASSERT_EQUAL(waitViewable(world, display, viewWindow), true, "the view is viewable again");
    XSetInputFocus(host, hostWindow, RevertToNone, CurrentTime);
    DAF_ASSERT_EQUAL(focusOf(host), hostWindow, "the host window has the focus again");
    DAF_ASSERT_EQUAL(puglGrabFocus(view), PUGL_SUCCESS, "the view takes the focus again");
    XSync(display, False);
    DAF_ASSERT_EQUAL(focusOf(host), viewWindow, "the view has the focus again");
    puglHide(view);
    puglUpdate(world, 0.0);
    DAF_ASSERT_NOT_EQUAL(focusOf(host), static_cast<::Window>(None), "hiding the view leaves a focus");

    // 4. a request of the view's that fails is never reported to the host: the hidden window
    //    cannot take the focus (BadMatch), and the next update reads the error
    hostErrorCount = 0;
    XSetInputFocus(display, viewWindow, RevertToParent, CurrentTime);
    puglUpdate(world, 0.0);
    XSync(display, False);
    DAF_ASSERT_EQUAL(hostErrorCount, 0, "the view's X error does not reach the host's handler");

    // 5. while a trap is open, an error on the host's own connection still reaches the host's
    //    handler, which its own error trapping (gdk_error_trap_push) depends on
    {
        hostErrorCount = 0;
        PuglX11ErrorTrap trap;
        puglX11BeginErrorTrap(&trap, display);
        XErrorEvent event = {};
        event.type       = 0;
        event.display    = host;
        event.error_code = BadWindow;
        puglX11TrapError(host, &event);
        event.display = display;
        puglX11TrapError(display, &event);
        const unsigned char caught = puglX11EndErrorTrap(&trap);
        DAF_ASSERT_EQUAL(hostErrorCount, 1, "only the host connection's error goes to the host");
        DAF_ASSERT_EQUAL(caught, static_cast<unsigned char>(BadWindow), "the view's error is caught by the trap");
    }

    // a host may destroy its window, and the view's with it, before letting the view go
    hostErrorCount = 0;
    XDestroyWindow(host, hostWindow);
    XSync(host, False);
    puglFreeView(view);
    puglFreeWorld(world);
    DAF_ASSERT_EQUAL(hostErrorCount, 0, "tearing down after the host destroyed the window reports nothing");

    XDestroyWindow(host, otherApp);
    XCloseDisplay(host);
    return 0;
}
