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
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define DAF_TEST_POINT_CPP
#define DAF_TEST_WINDOW_CPP
#include "dgl/src/pugl.cpp"
#include "dgl/src/Application.cpp"
#include "dgl/src/ApplicationPrivateData.cpp"
#include "dgl/src/Geometry.cpp"
#include "dgl/src/Widget.cpp"
#include "dgl/src/WidgetPrivateData.cpp"
#include "dgl/src/Window.cpp"

#ifndef DGL_RUNTIME_BACKEND
# error This test is for DGL_BACKEND=both builds
#endif

// What differs per backend in getClipboard is the loop that waits for an offer's types, and on a compositor that
// loop never runs: without a selection offer puglPaste fails first, and with one the data is in hand before it
// starts. So WindowPrivateData.cpp is compiled here with the calls each loop makes counted, and with a puglPaste
// that can report an offer whose types have yet to arrive, which is what the loops wait for.
START_NAMESPACE_DGL

static int x11WaitLoopUpdates = 0;
static int waylandWaitLoopUpdates = 0;
static bool pasteWithTypesToCome = false;

static PuglStatus testPaste(PuglView* const view)
{
    return pasteWithTypesToCome ? PUGL_SUCCESS : puglPaste(view);
}

static PuglStatus testX11UpdateWithoutExposures(PuglWorld* const world)
{
    ++x11WaitLoopUpdates;
    return puglX11UpdateWithoutExposures(world);
}

static PuglStatus testWaylandUpdateWithoutExposures(PuglWorld* const world)
{
    ++waylandWaitLoopUpdates;
    return puglWaylandUpdateWithoutExposures(world);
}

END_NAMESPACE_DGL

#define puglPaste testPaste
#define puglX11UpdateWithoutExposures testX11UpdateWithoutExposures
#define puglWaylandUpdateWithoutExposures testWaylandUpdateWithoutExposures
#include "dgl/src/WindowPrivateData.cpp"
#undef puglPaste
#undef puglX11UpdateWithoutExposures
#undef puglWaylandUpdateWithoutExposures

#ifdef HAVE_DBUS
# include <dbus/dbus.h>
# include <dirent.h>
# include <poll.h>
# include <signal.h>
# include <sys/prctl.h>
# include <sys/wait.h>
# include <unistd.h>
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

    bool fileSelected = false;
    std::string selectedFile;
    uint scaleFactorChanges = 0;
    double lastScaleFactor = 0.0;

protected:
    void onReshape(const uint width, const uint height) override
    {
        reshaped = true;
        Window::onReshape(width, height);
    }

    void onFileSelected(const char* const filename) override
    {
        fileSelected = true;
        selectedFile = filename != nullptr ? filename : "";
    }

    void onScaleFactorChanged(const double scaleFactor) override
    {
        ++scaleFactorChanges;
        lastScaleFactor = scaleFactor;
    }
};

END_NAMESPACE_DAF

#ifdef HAVE_DBUS
// --------------------------------------------------------------------------------------------------------------------
// A session bus of the test's own, and the desktop portal's FileChooser on it

// Its configuration names no service directories, so nothing on it can be activated, the desktop's own
// portal included.
class PrivateSessionBus
{
public:
    std::string address;

    ~PrivateSessionBus()
    {
        if (pid > 0)
        {
            kill(pid, SIGTERM);
            waitpid(pid, nullptr, 0);
        }

        if (dir.empty())
            return;

        if (DIR* const entries = opendir(dir.c_str()))
        {
            while (const dirent* const entry = readdir(entries))
                if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0)
                    unlink((dir + "/" + entry->d_name).c_str());
            closedir(entries);
        }
        rmdir(dir.c_str());
    }

    // false where there is no dbus-daemon to run
    bool start()
    {
        const char* const tmpdir = std::getenv("TMPDIR");
        std::string pattern = std::string(tmpdir != nullptr && tmpdir[0] != '\0' ? tmpdir : "/tmp")
                            + "/daf-test-bus-XXXXXX";
        if (mkdtemp(&pattern[0]) == nullptr)
            return false;
        dir = pattern;

        const std::string config = dir + "/bus.conf";
        if (FILE* const file = std::fopen(config.c_str(), "w"))
        {
            std::fprintf(file,
                "<!DOCTYPE busconfig PUBLIC \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
                " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
                "<busconfig>\n"
                "  <type>session</type>\n"
                "  <listen>unix:dir=%s</listen>\n"
                "  <auth>EXTERNAL</auth>\n"
                "  <policy context=\"default\">\n"
                "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
                "    <allow eavesdrop=\"true\"/>\n"
                "    <allow own=\"*\"/>\n"
                "  </policy>\n"
                "</busconfig>\n", dir.c_str());
            std::fclose(file);
        }
        else
        {
            return false;
        }

        int fds[2];
        if (pipe(fds) != 0)
            return false;

        pid = fork();
        if (pid == 0)
        {
            // the bus goes with the test, however the test ends
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            close(fds[0]);
            const std::string configArg = "--config-file=" + config;
            const std::string addressArg = "--print-address=" + std::to_string(fds[1]);
            execlp("dbus-daemon", "dbus-daemon", "--nofork", "--nopidfile", configArg.c_str(), addressArg.c_str(),
                   static_cast<char*>(nullptr));
            _exit(127);
        }
        close(fds[1]);

        // the address, on a line of its own
        pollfd readable = { fds[0], POLLIN, 0 };
        char buffer[512];
        size_t length = 0;
        while (length < sizeof(buffer) - 1 && poll(&readable, 1, 5000) > 0)
        {
            const ssize_t got = read(fds[0], buffer + length, sizeof(buffer) - 1 - length);
            if (got <= 0)
                break;
            length += static_cast<size_t>(got);
            if (buffer[length - 1] == '\n')
                break;
        }
        close(fds[0]);

        while (length != 0 && (buffer[length - 1] == '\n' || buffer[length - 1] == '\r'))
            --length;
        address.assign(buffer, length);
        return ! address.empty();
    }

private:
    pid_t pid = -1;
    std::string dir;
};

static const char kChosenUri[] = "file:///daf%20runtime%20backend/chosen%5Bfile%5D.txt";
static const char kChosenPath[] = "/daf runtime backend/chosen[file].txt";

// The portal's FileChooser as far as OpenFile goes: it answers with a Response that picks kChosenUri, sent to the
// caller alone, as xdg-desktop-portal sends it.
//
// It leaves out the reply to OpenFile, which carries the request handle that DGL has no use for. A real portal
// answers the call at once and the Response only after the user has chosen, so the two never reach DGL together;
// sent back to back here, they could, and fileBrowserIdle hands everything it reads after the first message to
// dbus_connection_dispatch, which drops a signal nothing handles.
class StandInFileChooser
{
public:
    int openFileCalls = 0;
    std::string parentWindow;

    ~StandInFileChooser()
    {
        if (connection == nullptr)
            return;
        dbus_connection_close(connection);
        dbus_connection_unref(connection);
    }

    bool start(const char* const address)
    {
        DBusError error;
        dbus_error_init(&error);

        connection = dbus_connection_open_private(address, &error);
        if (connection == nullptr)
        {
            dbus_error_free(&error);
            return false;
        }
        dbus_connection_set_exit_on_disconnect(connection, false);

        const bool owned = dbus_bus_register(connection, &error)
                        && dbus_bus_request_name(connection, "org.freedesktop.portal.Desktop",
                                                 DBUS_NAME_FLAG_DO_NOT_QUEUE, &error)
                           == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER;
        dbus_error_free(&error);
        return owned;
    }

    void serve()
    {
        dbus_connection_read_write(connection, 0);

        while (DBusMessage* const message = dbus_connection_pop_message(connection))
        {
            if (dbus_message_is_method_call(message, "org.freedesktop.portal.FileChooser", "OpenFile"))
                respond(message);
            dbus_message_unref(message);
        }

        dbus_connection_flush(connection);
    }

private:
    DBusConnection* connection = nullptr;

    void respond(DBusMessage* const call)
    {
        ++openFileCalls;

        const char* parent = nullptr;
        const char* title = nullptr;
        if (dbus_message_get_args(call, nullptr, DBUS_TYPE_STRING, &parent, DBUS_TYPE_STRING, &title,
                                  DBUS_TYPE_INVALID))
            parentWindow = parent;

        DBusMessage* const response = dbus_message_new_signal("/org/freedesktop/portal/desktop/request/daf/test",
                                                              "org.freedesktop.portal.Request", "Response");
        if (response == nullptr)
            return;

        dbus_message_set_destination(response, dbus_message_get_sender(call));

        // (u response, a{sv} results), with results["uris"] = ["file://..."]
        DBusMessageIter args, results, entry, variant, uris;
        dbus_message_iter_init_append(response, &args);
        const dbus_uint32_t success = 0;
        dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &success);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &results);
        dbus_message_iter_open_container(&results, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
        const char* const key = "uris";
        dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
        dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "as", &variant);
        dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &uris);
        const char* const uri = kChosenUri;
        dbus_message_iter_append_basic(&uris, DBUS_TYPE_STRING, &uri);
        dbus_message_iter_close_container(&variant, &uris);
        dbus_message_iter_close_container(&entry, &variant);
        dbus_message_iter_close_container(&results, &entry);
        dbus_message_iter_close_container(&args, &results);

        dbus_connection_send(connection, response, nullptr);
        dbus_message_unref(response);
    }
};
#endif

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    // The file browser below must not reach the session's portal, which would put its dialog on the desktop: it
    // gets a bus of the test's own, or none.
   #ifdef HAVE_DBUS
    PrivateSessionBus bus;
    const bool haveBus = bus.start();
    setenv("DBUS_SESSION_BUS_ADDRESS",
           haveBus ? bus.address.c_str() : "unix:path=/nonexistent/daf-runtime-backend-test", 1);
   #else
    setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/daf-runtime-backend-test", 1);
   #endif

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

    // A window created without a scale factor follows its backend's. On Wayland that is the scale of the output its
    // surface entered, which pugl only learns after the first configure, and which reaches the window through
    // onPuglConfigure's Wayland branch alone. DAF_TEST_WAYLAND_SCALE names that scale where the compositor's output
    // has one other than 1; an X11 window keeps the X server's all the same.
    DAF_ASSERT_EQUAL(win.followsPuglScaleFactor(), true, "the window follows pugl's scale factor");
    if (const char* const outputScaleString = std::getenv("DAF_TEST_WAYLAND_SCALE"))
    {
        const double outputScale = std::atof(outputScaleString);
        if (expected == kWayland)
        {
            const std::chrono::steady_clock::time_point scaleDeadline
                = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (d_isNotEqual(win.getScaleFactor(), outputScale) && std::chrono::steady_clock::now() < scaleDeadline)
                app.idle();

            DAF_ASSERT_SAFE_EQUAL(win.getScaleFactor(), outputScale, "a Wayland window follows its output's scale");
            DAF_ASSERT_SAFE_EQUAL(win.lastScaleFactor, outputScale, "a Wayland window is told of its output's scale");
        }
        else
        {
            DAF_ASSERT_SAFE_EQUAL(win.getScaleFactor(), 1.0, "an X11 window keeps the X server's scale");
            DAF_ASSERT_EQUAL(win.scaleFactorChanges, 0u, "an X11 window hears of no scale change");
        }
    }
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

   #ifdef HAVE_DBUS
    // With a portal, a window of either backend gets its dialog, and the file chosen there. The portal is told
    // which X11 window to put it over, and of a Wayland window nothing, as in a Wayland-only build.
    if (haveBus)
    {
        StandInFileChooser portal;
        DAF_ASSERT_EQUAL(portal.start(bus.address.c_str()), true, "the stand-in portal takes its name on the bus");
        DAF_ASSERT_EQUAL(win.openFileBrowser(fileBrowserOptions), true, "the window opens the portal's file dialog");

        const std::chrono::steady_clock::time_point portalDeadline
            = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (! win.fileSelected && std::chrono::steady_clock::now() < portalDeadline)
        {
            portal.serve();
            app.idle();
        }

        DAF_ASSERT_EQUAL(portal.openFileCalls, 1, "the portal is asked for a file once");
        if (expected == kX11)
        {
            DAF_ASSERT_EQUAL(portal.parentWindow.compare(0, 4, "x11:"), 0, "an X11 window is the dialog's x11: parent");
        }
        else
        {
            DAF_ASSERT_EQUAL(portal.parentWindow.empty(), true, "a Wayland window gives the portal no parent");
        }
        DAF_ASSERT_EQUAL(win.fileSelected, true, "the window hears back about the file chosen in the portal");
        DAF_ASSERT_EQUAL(win.selectedFile == kChosenPath, true, "the window gets the path of the portal's file URI");

        d_stdout("RuntimeBackendWindow: the file chosen in the portal reached the %s window", backendName(expected));
    }
    else
    {
        d_stdout("RuntimeBackendWindow: no dbus-daemon to run a portal on, the portal's file dialog is not checked");
    }
   #endif

    // the web view is an X11 window reparented into this one, which a Wayland window cannot host
    if (expected == kWayland)
    {
        DAF_ASSERT_EQUAL(win.createWebView("about:blank"), false, "a Wayland window gets no web view");
    }

    // A paste whose offer types have yet to arrive waits on the window's own backend, never the other's.
    DGL_NAMESPACE::pasteWithTypesToCome = true;
    size_t pendingSize = 0;
    DAF_ASSERT_EQUAL(win.getClipboard(pendingSize), static_cast<const void*>(nullptr),
                     "a paste whose types never arrive gives nothing");
    DGL_NAMESPACE::pasteWithTypesToCome = false;
    if (expected == kX11)
    {
        DAF_ASSERT_EQUAL(DGL_NAMESPACE::x11WaitLoopUpdates > 0, true, "an X11 window waits on X11 events");
        DAF_ASSERT_EQUAL(DGL_NAMESPACE::waylandWaitLoopUpdates, 0, "an X11 window never waits on Wayland events");
    }
    else
    {
        DAF_ASSERT_EQUAL(DGL_NAMESPACE::waylandWaitLoopUpdates > 0, true, "a Wayland window waits on Wayland events");
        DAF_ASSERT_EQUAL(DGL_NAMESPACE::x11WaitLoopUpdates, 0, "a Wayland window never waits on X11 events");
    }

    // What the window puts on the clipboard comes back through its own backend's paste. A compositor only
    // offers the selection to the client with keyboard focus, which no window gets on a headless one, so on
    // Wayland it has to come back empty-handed rather than with the text.
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
