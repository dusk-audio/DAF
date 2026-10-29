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

// Above pugl, a DGL_BACKEND=both build decides per window what a single-backend build fixes when it
// compiles: the portal parent handle, the file browser, the web view, the clipboard and following pugl's
// scale factor each take the X11 or the Wayland branch by the window's backend. This test opens a DGL
// window on whichever backend the environment gives a program (see runtime-backend.hpp) and checks that
// each of them took that backend's branch.

#include "tests.hpp"
#include "runtime-backend.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>

#define DAF_TEST_POINT_CPP
#define DAF_TEST_WINDOW_CPP
#include "dgl/src/pugl.cpp"
#include "dgl/src/Application.cpp"
#include "dgl/src/ApplicationPrivateData.cpp"
#include "dgl/src/Geometry.cpp"
#include "dgl/src/Widget.cpp"
#include "dgl/src/WidgetPrivateData.cpp"
#include "dgl/src/Window.cpp"
#include "dgl/src/WindowPrivateData.cpp"

#ifndef DGL_RUNTIME_BACKEND
# error This test is for DGL_BACKEND=both builds
#endif

// ScopedGraphicsContext creates the renderer's drawing state, which lives with the renderers this test does
// not build; only the window around it matters here.
START_NAMESPACE_DGL
void Window::PrivateData::createContextIfNeeded() {}
END_NAMESPACE_DGL

// Window befriends the plugin wrappers' PluginWindow; this one stands in for it to look at the view.
START_NAMESPACE_DAF

class PluginWindow : public DGL_NAMESPACE::Window
{
public:
    bool reshaped;

    explicit PluginWindow(DGL_NAMESPACE::Application& app)
        : Window(app, 0, 240, 180, 0.0, true),
          reshaped(false) {}

    bool isX11() const
    {
        return DGL_NAMESPACE::puglViewIsX11(pData->view);
    }

    bool followsPuglScaleFactor() const
    {
        return pData->followsPuglScaleFactor;
    }

    double puglScaleFactor() const
    {
        return DGL_NAMESPACE::puglGetScaleFactor(pData->view);
    }

    // a Wayland clipboard is held through the seat's data device, which a compositor without a seat lacks
    bool hasWaylandDataDevice() const
    {
        const DGL_NAMESPACE::pugl_wl::PuglWorld* const world
            = static_cast<const DGL_NAMESPACE::pugl_wl::PuglWorld*>(DGL_NAMESPACE::puglGetWorld(pData->view));
        return world->impl->dataDevice != nullptr;
    }

protected:
    void onReshape(const uint width, const uint height) override
    {
        reshaped = true;
        Window::onReshape(width, height);
    }
};

END_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    // The file browser below must not reach the session's portal, which would put its dialog on the desktop.
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/daf-runtime-backend-test", 1);

    const Expected expected = expectedBackend(true);
    DAF_ASSERT_NOT_EQUAL(expected, kInvalid, "DAF_TEST_EXPECT_BACKEND is x11, wayland or none");

    // RuntimeBackend checks that no world opens then; a DGL Application expects one
    if (expected == kNone)
    {
        d_stdout("RuntimeBackendWindow: no backend available, nothing to check");
        return 0;
    }

    DGL_NAMESPACE::Application app(true);
    DAF_NAMESPACE::PluginWindow win(app);
    win.show();

    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (! win.reshaped && std::chrono::steady_clock::now() < deadline)
        app.idle();

    DAF_ASSERT_EQUAL(win.reshaped, true, "the window is configured");
    DAF_ASSERT_EQUAL(win.isX11(), expected == kX11, "the window opens on the expected backend");

    // a window created without a scale factor keeps following pugl's, on configure as well
    DAF_ASSERT_EQUAL(win.followsPuglScaleFactor(), true, "the window follows pugl's scale factor");
    DAF_ASSERT_SAFE_EQUAL(win.getScaleFactor(), win.puglScaleFactor(), "the window has pugl's scale factor");

    // a portal is handed the window in its own backend's form, never an X11 id for a Wayland window
    const char* const parentHandle = win.getPortalParentHandle();
    if (expected == kX11)
    {
        DAF_ASSERT_EQUAL(std::strncmp(parentHandle, "x11:0x", 6), 0, "an X11 window is an x11: portal parent");
    }
    else
    {
        DAF_ASSERT_EQUAL(parentHandle[0] == '\0' || std::strncmp(parentHandle, "wayland:", 8) == 0, true,
                         "a Wayland window is a wayland: portal parent, or none");
    }

    // With no portal, an X11 window gets the X11 file dialog. A Wayland window gets none rather than an
    // X11 dialog of its own, even with an X server there to show one.
    DGL_NAMESPACE::FileBrowserOptions fileBrowserOptions;
    fileBrowserOptions.title = "DAF runtime backend test";
    DAF_ASSERT_EQUAL(win.openFileBrowser(fileBrowserOptions), expected == kX11,
                     "only an X11 window falls back to the X11 file dialog");

    // the web view is an X11 window reparented into this one, which a Wayland window cannot host
    if (expected == kWayland)
    {
        DAF_ASSERT_EQUAL(win.createWebView("about:blank"), false, "a Wayland window gets no web view");
    }

    // What the window puts on the clipboard comes back through its own backend's paste loop. A compositor
    // only offers the selection to the client with keyboard focus, which no window gets on a headless one,
    // so on Wayland the loop has to come back empty-handed rather than with the text.
    static const char kClipboardText[] = "DAF runtime backend test";
    DAF_ASSERT_EQUAL(win.setClipboard("text/plain", kClipboardText, sizeof(kClipboardText)),
                     expected == kX11 || win.hasWaylandDataDevice(),
                     "the window takes the clipboard, unless a compositor has no seat to hold it");
    size_t clipboardSize = 0;
    const void* const clipboardData = win.getClipboard(clipboardSize);
    const bool gotText = clipboardData != nullptr && clipboardSize == sizeof(kClipboardText)
                      && std::memcmp(clipboardData, kClipboardText, clipboardSize) == 0;
    if (expected == kX11)
    {
        DAF_ASSERT_EQUAL(gotText, true, "the X11 window pastes what it put on the clipboard");
    }
    else
    {
        DAF_ASSERT_EQUAL(clipboardSize == 0 || gotText, true, "the Wayland window pastes nothing but its own text");
    }

    win.close();

    d_stdout("RuntimeBackendWindow: window on %s took that backend's portal handle, file browser, web view, "
             "clipboard and scale", backendName(expected));
    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
