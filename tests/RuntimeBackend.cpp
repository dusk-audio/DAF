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

// A DGL_BACKEND=both build chooses X11 or Wayland when it opens a world. This test works out which one
// the documented rule picks (dgl/src/runtime-backend.md, and runtime-backend.hpp for how), checks that
// pugl picked the same for a program and for a module, then opens an OpenGL window on it, draws, checks
// the frame reached the display server, resizes it and sends it an event. It opens a window in the
// module's world as well while the program's is open, which with both servers up puts an X11 and a
// Wayland window in one process.
//
// Last, it announces a Wayland compositor that cannot be used, first a WAYLAND_DISPLAY nothing listens
// on, then a WAYLAND_SOCKET to a stand-in compositor without xdg_wm_base, and checks that a program
// falls back to X11, or opens nothing when DAF_WINDOW_BACKEND=wayland or there is no X server.
//
// It runs wherever it is started: under Xvfb alone, under a headless Wayland compositor alone, with
// both, and with DAF_WINDOW_BACKEND forcing either. A forced Wayland with WAYLAND_DISPLAY unset lets
// libwayland try wayland-0, so XDG_RUNTIME_DIR must not hold one then.

#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "runtime-backend.hpp"

#include "dgl/src/pugl.cpp"

#ifndef DGL_RUNTIME_BACKEND
# error This test is for DGL_BACKEND=both builds
#endif

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

using namespace DGL_NAMESPACE;

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

// Opens a GL window in a world, draws into it, and checks the frame reached the display server.
static int openWindow(PuglWorld* const world, const Expected backend, State& state, const char* const title,
                      PuglView*& view)
{
    view = puglNewView(world);
    DAF_ASSERT_NOT_EQUAL(view, nullptr, "a view is created");
    DAF_ASSERT_EQUAL(puglViewIsX11(view), backend == kX11, "a view is on the backend of its world");
    DAF_ASSERT_EQUAL(puglGetWorld(view), world, "a view hands back the world it was created in");

    puglSetHandle(view, &state);
    puglSetEventFunc(view, onEvent);
    puglSetMatchingBackendForCurrentBuild(view);
    puglSetViewString(view, PUGL_WINDOW_TITLE, title);
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
    if (backend == kX11)
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

    return 0;
}

// A size change goes to the display server and comes back as a configure, and the window is exposed again.
// The new size must be larger: an X server exposes nothing of a window that shrinks.
static int resize(PuglWorld* const world, PuglView* const view, State& state,
                  const unsigned width, const unsigned height)
{
    state.exposed = false;

    DAF_ASSERT_EQUAL(puglSetSizeHint(view, PUGL_CURRENT_SIZE, width, height), PUGL_SUCCESS,
                     "the window can be resized");
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.configuredWidth == width && state.configuredHeight == height; }),
                     true, "the new size is configured");
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.exposed; }), true, "the resized window is exposed again");

    return 0;
}

// A redraw asked for is exposed, and an event sent to the window comes back to it.
static int redrawAndSignal(PuglWorld* const world, PuglView* const view, State& state)
{
    state.exposed = false;
    state.gotClient = false;

    DAF_ASSERT_EQUAL(puglObscureView(view), PUGL_SUCCESS, "a redraw can be asked for");
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.exposed; }), true, "the window is exposed again");

    PuglEvent client = {};
    client.client.type = PUGL_CLIENT;
    client.client.data1 = kClientData1;
    client.client.data2 = kClientData2;
    DAF_ASSERT_EQUAL(puglSendEvent(view, &client), PUGL_SUCCESS, "an event can be sent to the window");
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.gotClient; }), true, "the window receives the event");

    return 0;
}

struct HostEgl
{
    wl_display* connection = nullptr;
    wl_registry* registry = nullptr;
    wl_compositor* compositor = nullptr;
    xdg_wm_base* shell = nullptr;
    xdg_surface* shellSurface = nullptr;
    xdg_toplevel* toplevel = nullptr;
    bool configured = false;
    wl_surface* nativeSurface = nullptr;
    wl_egl_window* window = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    ~HostEgl()
    {
        if (display != EGL_NO_DISPLAY)
        {
            eglBindAPI(EGL_OPENGL_API);
            eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
            if (surface != EGL_NO_SURFACE) eglDestroySurface(display, surface);
            if (context != EGL_NO_CONTEXT) eglDestroyContext(display, context);
            eglTerminate(display);
        }
        if (window) wl_egl_window_destroy(window);
        if (toplevel) xdg_toplevel_destroy(toplevel);
        if (shellSurface) xdg_surface_destroy(shellSurface);
        if (nativeSurface) wl_surface_destroy(nativeSurface);
        if (shell) xdg_wm_base_destroy(shell);
        if (compositor) wl_compositor_destroy(compositor);
        if (registry) wl_registry_destroy(registry);
        if (connection) wl_display_disconnect(connection);
    }

    int connect()
    {
        connection = wl_display_connect(nullptr);
        DAF_ASSERT_NOT_EQUAL(connection, nullptr, "the foreign host connects to Wayland");
        registry = wl_display_get_registry(connection);
        DAF_ASSERT_NOT_EQUAL(registry, nullptr, "the foreign host has a registry");
        static const wl_registry_listener listener = {
            [](void* data, wl_registry* registry, uint32_t name, const char* interface, uint32_t) {
                if (std::strcmp(interface, "wl_compositor") == 0)
                    static_cast<HostEgl*>(data)->compositor = static_cast<wl_compositor*>(
                        wl_registry_bind(registry, name, &wl_compositor_interface, 1));
                else if (std::strcmp(interface, "xdg_wm_base") == 0)
                {
                    auto* const host = static_cast<HostEgl*>(data);
                    host->shell = static_cast<xdg_wm_base*>(wl_registry_bind(registry, name, &xdg_wm_base_interface, 1));
                    static const xdg_wm_base_listener shellListener = {
                        [](void*, xdg_wm_base* shell, uint32_t serial) { xdg_wm_base_pong(shell, serial); }
                    };
                    xdg_wm_base_add_listener(host->shell, &shellListener, nullptr);
                }
            },
            [](void*, wl_registry*, uint32_t) {}
        };
        DAF_ASSERT_EQUAL(wl_registry_add_listener(registry, &listener, this), 0, "host registry listener attaches");
        DAF_ASSERT_NOT_EQUAL(wl_display_roundtrip(connection), -1, "the host receives globals");
        DAF_ASSERT_NOT_EQUAL(compositor, nullptr, "the host finds the compositor");
        display = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, connection, nullptr);
        DAF_ASSERT_NOT_EQUAL(display, EGL_NO_DISPLAY, "the host gets its EGL display");
        return 0;
    }

    int initialize()
    {
        DAF_ASSERT_EQUAL(eglInitialize(display, nullptr, nullptr), EGL_TRUE, "the host initializes EGL");
        DAF_ASSERT_EQUAL(eglBindAPI(EGL_OPENGL_API), EGL_TRUE, "the host selects OpenGL");
        const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                               EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
        EGLConfig config = nullptr;
        EGLint count = 0;
        DAF_ASSERT_EQUAL(eglChooseConfig(display, attrs, &config, 1, &count), EGL_TRUE, "host EGL config lookup succeeds");
        DAF_ASSERT_EQUAL(count, 1, "the host has a rendering config");
        const EGLint contextAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
        context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttrs);
        DAF_ASSERT_NOT_EQUAL(context, EGL_NO_CONTEXT, "the host has its own context");
        nativeSurface = wl_compositor_create_surface(compositor);
        DAF_ASSERT_NOT_EQUAL(nativeSurface, nullptr, "the host has its own surface");
        window = wl_egl_window_create(nativeSurface, 32, 32);
        DAF_ASSERT_NOT_EQUAL(window, nullptr, "the host has its own EGL window");
        surface = eglCreateWindowSurface(display, config, reinterpret_cast<EGLNativeWindowType>(window), nullptr);
        DAF_ASSERT_NOT_EQUAL(surface, EGL_NO_SURFACE, "the host has its own EGL surface");
        return drawAfterRebind();
    }

    int drawAfterRebind()
    {
        // A context marked for deletion can remain current, so a handle comparison is insufficient.
        DAF_ASSERT_EQUAL(eglBindAPI(EGL_OPENGL_API), EGL_TRUE, "the host selects its API again");
        DAF_ASSERT_EQUAL(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), EGL_TRUE,
                         "the host unbinds its context");
        DAF_ASSERT_EQUAL(eglMakeCurrent(display, surface, surface, context), EGL_TRUE,
                         "the same host context and surface can be rebound");
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        GLubyte pixel[4] = {};
        glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        DAF_ASSERT_EQUAL(pixel[0] > 50 && pixel[0] < 80 && pixel[1] > 115 && pixel[1] < 140 &&
                         pixel[2] > 180 && pixel[2] < 205, true, "the foreign host still renders");
        return 0;
    }

    int checkBinding(const EGLenum api = EGL_OPENGL_ES_API)
    {
        DAF_ASSERT_EQUAL(eglQueryAPI(), api, "the host's selected client API is preserved");
        DAF_ASSERT_EQUAL(eglGetCurrentDisplay(), display, "the host's current display is preserved");
        DAF_ASSERT_EQUAL(eglGetCurrentContext(), context, "the host's current context is preserved");
        DAF_ASSERT_EQUAL(eglGetCurrentSurface(EGL_DRAW), surface, "the host's draw surface is preserved");
        DAF_ASSERT_EQUAL(eglGetCurrentSurface(EGL_READ), surface, "the host's read surface is preserved");
        return 0;
    }

    int map()
    {
        DAF_ASSERT_NOT_EQUAL(shell, nullptr, "the host has xdg-shell support");
        shellSurface = xdg_wm_base_get_xdg_surface(shell, nativeSurface);
        DAF_ASSERT_NOT_EQUAL(shellSurface, nullptr, "the host gets a shell surface");
        static const xdg_surface_listener surfaceListener = {
            [](void* data, xdg_surface* surface, uint32_t serial) {
                xdg_surface_ack_configure(surface, serial);
                static_cast<HostEgl*>(data)->configured = true;
            }
        };
        DAF_ASSERT_EQUAL(xdg_surface_add_listener(shellSurface, &surfaceListener, this), 0, "host configure listener attaches");
        toplevel = xdg_surface_get_toplevel(shellSurface);
        DAF_ASSERT_NOT_EQUAL(toplevel, nullptr, "the host gets a toplevel");
        static const xdg_toplevel_listener toplevelListener = {
            [](void*, xdg_toplevel*, int32_t, int32_t, wl_array*) {}, [](void*, xdg_toplevel*) {}, nullptr, nullptr
        };
        DAF_ASSERT_EQUAL(xdg_toplevel_add_listener(toplevel, &toplevelListener, nullptr), 0, "host toplevel listener attaches");
        xdg_toplevel_set_title(toplevel, "DAF foreign host");
        wl_surface_commit(nativeSurface);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!configured)
        {
            DAF_ASSERT_EQUAL(std::chrono::steady_clock::now() < deadline, true, "the host is configured in time");
            DAF_ASSERT_NOT_EQUAL(wl_display_roundtrip(connection), -1, "the host reads its configure");
        }
        wl_egl_window_resize(window, 256, 192, 0, 0);
        if (drawAfterRebind()) return 1;
        DAF_ASSERT_EQUAL(eglSwapBuffers(display, surface), EGL_TRUE, "the host maps with an attached buffer");
        DAF_ASSERT_NOT_EQUAL(wl_display_roundtrip(connection), -1, "the compositor receives the host buffer");
        return 0;
    }
};

using WorldOwner = std::unique_ptr<PuglWorld, decltype(&puglFreeWorld)>;
using ViewOwner = std::unique_ptr<PuglView, decltype(&puglFreeView)>;

static ViewOwner makeGlView(PuglWorld* const world, const bool fail)
{
    ViewOwner view(puglNewView(world), puglFreeView);
    if (!view) return view;
    puglSetMatchingBackendForCurrentBuild(view.get());
    puglSetSizeHint(view.get(), PUGL_DEFAULT_SIZE, 32, 32);
    puglSetEventFunc(view.get(), [](PuglView*, const PuglEvent*) { return PUGL_SUCCESS; });
    if (fail) puglSetViewHint(view.get(), PUGL_CONTEXT_VERSION_MAJOR, 99);
    return view;
}

static int checkOwnedSubsurface(PuglWorld* const world, PuglView* const parent)
{
    State state;
    ViewOwner child = makeGlView(world, false);
    DAF_ASSERT_NOT_EQUAL(child.get(), nullptr, "a child can be allocated in an owned world");
    puglSetHandle(child.get(), &state);
    puglSetEventFunc(child.get(), onEvent);
    DAF_ASSERT_EQUAL(puglSetParent(child.get(), puglGetNativeView(parent)), PUGL_SUCCESS,
                     "another DAF view can be the parent");
    DAF_ASSERT_EQUAL(puglRealize(child.get()), PUGL_SUCCESS, "the owned-world child realizes");
    DAF_ASSERT_EQUAL(puglShow(child.get(), PUGL_SHOW_PASSIVE), PUGL_SUCCESS, "the owned-world child shows");
    DAF_ASSERT_EQUAL(puglObscureView(parent), PUGL_SUCCESS, "the parent renderer commits the child relationship");
    const auto* const native = static_cast<const pugl_wl::PuglView*>(child.get());
    DAF_ASSERT_EQUAL(pumpUntil(world, [&] { return state.exposed && native->impl->frameCallbackWorks; }), true,
                     "the child of a DAF-owned parent is presented");
    DAF_ASSERT_EQUAL(state.contextCurrent && state.drewColour, true, "the owned-world child draws its pixels");
    return 0;
}

static int checkBorrowedEgl()
{
    HostEgl host;
    if (host.connect() || host.initialize()) return 1;
    DAF_ASSERT_EQUAL(eglBindAPI(EGL_OPENGL_ES_API), EGL_TRUE, "the host selects a different client API");
    {
        WorldOwner probe(puglNewWorldOn(kPuglWindowSystemWayland, PUGL_MODULE, 0), puglFreeWorld);
        DAF_ASSERT_NOT_EQUAL(probe.get(), nullptr, "the runtime probe opens a separate Wayland connection");
        if (host.checkBinding()) return 1;
    }
    WorldOwner world(puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, host.connection), puglFreeWorld);
    DAF_ASSERT_NOT_EQUAL(world.get(), nullptr, "the DAF world borrows the host connection");
    {
        ViewOwner failed = makeGlView(world.get(), true);
        DAF_ASSERT_NOT_EQUAL(failed.get(), nullptr, "a view exists before failed realization");
        DAF_ASSERT_NOT_EQUAL(puglRealize(failed.get()), PUGL_SUCCESS, "an impossible GL version fails");
    }
    if (host.checkBinding()) return 1;
    if (host.drawAfterRebind()) return 1;
    DAF_ASSERT_EQUAL(eglBindAPI(EGL_OPENGL_ES_API), EGL_TRUE, "the host restores its selected API");
    ViewOwner first = makeGlView(world.get(), false);
    ViewOwner second = makeGlView(world.get(), false);
    DAF_ASSERT_NOT_EQUAL(first.get(), nullptr, "the first DAF child exists");
    DAF_ASSERT_NOT_EQUAL(second.get(), nullptr, "the second DAF child exists");
    DAF_ASSERT_EQUAL(puglRealize(first.get()), PUGL_SUCCESS, "the first DAF child realizes");
    DAF_ASSERT_EQUAL(puglRealize(second.get()), PUGL_SUCCESS, "the second DAF child realizes");
    if (host.checkBinding()) return 1;
    if (host.drawAfterRebind()) return 1;
    DAF_ASSERT_EQUAL(eglBindAPI(EGL_OPENGL_ES_API), EGL_TRUE, "the host selects its API before rendering");
    DAF_ASSERT_EQUAL(puglBackendEnter(first.get()), true, "the first child context can be entered");
    DAF_ASSERT_EQUAL(puglBackendEnter(second.get()), true, "a sibling context can be nested");
    DAF_ASSERT_EQUAL(puglBackendEnter(first.get()), true, "the outer child can be re-entered through a sibling");
    DAF_ASSERT_EQUAL(puglBackendEnter(first.get()), true, "the same current child can be entered again");
    DAF_ASSERT_EQUAL(puglBackendLeave(first.get()), true, "the same-child nested entry can be left");
    DAF_ASSERT_EQUAL(puglBackendIsCurrent(first.get(), false), true, "the repeated child remains current");
    DAF_ASSERT_EQUAL(puglBackendLeave(first.get()), true, "the re-entered outer child can be left");
    DAF_ASSERT_EQUAL(puglBackendIsCurrent(second.get(), false), true, "the sibling context is restored");
    DAF_ASSERT_EQUAL(puglBackendLeave(second.get()), true, "the nested sibling context can be left");
    DAF_ASSERT_EQUAL(puglBackendIsCurrent(first.get(), false), true, "the outer child context is restored");
    auto* const firstGl = static_cast<pugl_wl::PuglWaylandGlSurface*>(
        static_cast<pugl_wl::PuglView*>(first.get())->impl->surface);
    const EGLDisplay savedDisplay = firstGl->display;
    firstGl->display = EGL_NO_DISPLAY;
    const bool failedEntry = puglBackendEnter(first.get());
    firstGl->display = savedDisplay;
    DAF_ASSERT_EQUAL(failedEntry, false, "an invalid EGL display fails context entry");
    DAF_ASSERT_EQUAL(puglBackendIsCurrent(first.get(), false), true, "failed entry preserves the outer context");
    DAF_ASSERT_EQUAL(puglBackendLeave(first.get()), true, "the outer child context can be left");
    if (host.checkBinding()) return 1;
    DAF_ASSERT_EQUAL(puglBackendLeave(first.get()), false, "an unmatched leave is rejected");
    if (host.checkBinding()) return 1;
    DAF_ASSERT_EQUAL(puglBackendEnter(first.get()), true, "the first view is current during sibling creation");
    {
        ViewOwner temporary = makeGlView(world.get(), false);
        DAF_ASSERT_NOT_EQUAL(temporary.get(), nullptr, "a temporary sibling exists");
        DAF_ASSERT_EQUAL(puglRealize(temporary.get()), PUGL_SUCCESS, "a sibling realizes inside another context");
        DAF_ASSERT_EQUAL(puglBackendIsCurrent(first.get(), false), true, "sibling creation preserves the current view");
    }
    DAF_ASSERT_EQUAL(puglBackendIsCurrent(first.get(), false), true, "sibling destruction preserves the current view");
    DAF_ASSERT_EQUAL(puglBackendLeave(first.get()), true, "the first view can leave after sibling destruction");
    if (host.checkBinding()) return 1;
    first.reset();
    if (host.checkBinding()) return 1;
    DAF_ASSERT_EQUAL(puglBackendEnter(second.get()), true, "the sibling context survives");
    glClearColor(0.75f, 0.25f, 0.5f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    GLubyte pixel[4] = {};
    glReadPixels(2, 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
    DAF_ASSERT_EQUAL(pixel[0] > 180 && pixel[0] < 205, true, "the sibling still renders");
    DAF_ASSERT_EQUAL(puglBackendLeave(second.get()), true, "the sibling context can be left");
    if (host.checkBinding()) return 1;
    second.reset();
    if (host.checkBinding()) return 1;
    if (host.drawAfterRebind()) return 1;
    world.reset();
    if (host.drawAfterRebind()) return 1;
    DAF_ASSERT_EQUAL(eglMakeCurrent(host.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT), EGL_TRUE,
                     "the host can also leave no context current");
    DAF_ASSERT_EQUAL(eglBindAPI(EGL_OPENGL_ES_API), EGL_TRUE, "the unbound host selects its client API");

    HostEgl softwareHost;
    if (softwareHost.connect()) return 1;
    WorldOwner softwareWorld(puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, softwareHost.connection), puglFreeWorld);
    DAF_ASSERT_NOT_EQUAL(softwareWorld.get(), nullptr, "a software host also permits borrowing");
    for (unsigned fail = 0; fail < 2; ++fail)
    {
        ViewOwner view = makeGlView(softwareWorld.get(), fail != 0);
        DAF_ASSERT_NOT_EQUAL(view.get(), nullptr, "a DAF GL view can be allocated for a software host");
        DAF_ASSERT_EQUAL(puglRealize(view.get()) == PUGL_SUCCESS, fail == 0, "the requested GL version determines success");
        DAF_ASSERT_EQUAL(eglGetCurrentContext(), EGL_NO_CONTEXT, "creation restores an unbound host");
        view.reset();
        DAF_ASSERT_EQUAL(eglQueryAPI(), EGL_OPENGL_ES_API, "teardown preserves an unbound host's selected API");
        DAF_ASSERT_EQUAL(eglQueryString(softwareHost.display, EGL_VERSION), nullptr,
                         "DAF releases the EGL initialization it owns");
        DAF_ASSERT_EQUAL(eglGetError(), EGL_NOT_INITIALIZED, "the software host's EGL display is uninitialized");
    }
    softwareWorld.reset();
    DAF_ASSERT_NOT_EQUAL(wl_display_roundtrip(softwareHost.connection), -1, "the software host connection survives");
    d_stdout("RuntimeBackend: borrowed EGL preserves host resources and releases DAF-owned initialization");
    return 0;
}

static int checkSubsurfaceRendering()
{
    HostEgl host;
    if (host.connect() || host.initialize() || host.map()) return 1;
    WorldOwner world(puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, host.connection), puglFreeWorld);
    DAF_ASSERT_NOT_EQUAL(world.get(), nullptr, "the embedded renderer borrows the host display");
    State states[2];
    ViewOwner views[] = {makeGlView(world.get(), false), makeGlView(world.get(), false)};
    const auto presented = [&](const unsigned index) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        const auto* const view = static_cast<const pugl_wl::PuglView*>(views[index].get());
        while (!states[index].exposed || !view->impl->frameCallbackWorks)
        {
            if (std::chrono::steady_clock::now() >= deadline || wl_display_roundtrip(host.connection) < 0)
                return false;
            if (puglUpdate(world.get(), 0.0) != PUGL_SUCCESS) return false;
        }
        return states[index].contextCurrent && states[index].drewColour;
    };
    for (unsigned i = 0; i < 2; ++i)
    {
        DAF_ASSERT_NOT_EQUAL(views[i].get(), nullptr, "an embedded view is allocated");
        puglSetHandle(views[i].get(), &states[i]);
        puglSetEventFunc(views[i].get(), onEvent);
        DAF_ASSERT_EQUAL(puglSetParent(views[i].get(), reinterpret_cast<PuglNativeView>(host.nativeSurface)), PUGL_SUCCESS,
                         "the child uses the foreign parent surface");
        puglSetSizeHint(views[i].get(), PUGL_DEFAULT_SIZE, 64, 48);
        DAF_ASSERT_EQUAL(puglRealize(views[i].get()), PUGL_SUCCESS, "the embedded view realizes");
        DAF_ASSERT_EQUAL(puglSetPositionHint(views[i].get(), PUGL_CURRENT_POSITION, 16 + int(i) * 100, 24), PUGL_SUCCESS,
                         "the embedded view is positioned in its parent");
        DAF_ASSERT_EQUAL(puglShow(views[i].get(), PUGL_SHOW_PASSIVE), PUGL_SUCCESS, "the embedded view shows");
        wl_surface_commit(host.nativeSurface);
        DAF_ASSERT_EQUAL(presented(i), true, "the compositor presents the embedded child's pixels");
        if (const char* const scale = std::getenv("DAF_TEST_WAYLAND_SCALE"))
            DAF_ASSERT_SAFE_EQUAL(puglGetScaleFactor(views[i].get()), std::atof(scale),
                                  "the embedded child follows the compositor's scale");
        if (host.checkBinding(EGL_OPENGL_API)) return 1;
    }
    auto* const first = static_cast<pugl_wl::PuglView*>(views[0].get());
    states[0].exposed = false;
    first->impl->frameCallbackWorks = false;
    DAF_ASSERT_EQUAL(puglSetSizeHint(views[0].get(), PUGL_CURRENT_SIZE, 96, 60), PUGL_SUCCESS, "the child resizes");
    DAF_ASSERT_EQUAL(presented(0), true, "the resized child presents another frame");
    DAF_ASSERT_EQUAL(states[0].configuredWidth, 96u, "the resized child reports its pixel width");
    DAF_ASSERT_EQUAL(states[0].configuredHeight, 60u, "the resized child reports its pixel height");
    DAF_ASSERT_EQUAL(puglHide(views[0].get()), PUGL_SUCCESS, "the child hides");
    states[0].exposed = false;
    first->impl->frameCallbackWorks = false;
    DAF_ASSERT_EQUAL(puglShow(views[0].get(), PUGL_SHOW_PASSIVE), PUGL_SUCCESS, "the child remaps without xdg negotiation");
    DAF_ASSERT_EQUAL(presented(0), true, "the remapped child presents a new frame");

    puglSetViewHint(views[0].get(), PUGL_RESIZABLE, PUGL_FALSE);
    DAF_ASSERT_EQUAL(puglWaylandSetEmbeddedGeometry(views[0].get(), -12, 21, 125, 50), PUGL_SUCCESS,
                     "the host assigns a non-resizable child's logical rectangle");
    wl_surface_commit(host.nativeSurface);
    DAF_ASSERT_EQUAL(puglWaylandSetEmbeddedGeometry(views[0].get(), 10, 20, 0, 50), PUGL_BAD_PARAMETER,
                     "invalid geometry is rejected without changing the rectangle");
    DAF_ASSERT_EQUAL(puglWaylandSetEmbeddedGeometry(views[0].get(), 32768, 20, 125, 50), PUGL_BAD_PARAMETER,
                     "out-of-range parent coordinates cannot wrap");
    if (first->impl->viewport && first->impl->fractionalScale)
    {
        for (const uint32_t scale : {120u, 180u, 240u, 120u})
        {
            states[0].exposed = false;
            first->impl->frameCallbackWorks = false;
            pugl_wl::puglWaylandPreferredScale(first, first->impl->fractionalScale, scale);
            DAF_ASSERT_EQUAL(puglObscureView(views[0].get()), PUGL_SUCCESS, "a scale update can redraw");
            DAF_ASSERT_EQUAL(presented(0), true, "the scaled child presents through its viewport");
            DAF_ASSERT_EQUAL(first->impl->logicalSize.width, 125u, "native scale preserves the host width");
            DAF_ASSERT_EQUAL(first->impl->logicalSize.height, 50u, "native scale preserves the host height");
            DAF_ASSERT_EQUAL(states[0].configuredWidth, unsigned(std::lround(125.0 * scale / 120.0)),
                             "buffer width follows native scale despite fixed pixel hints");
            DAF_ASSERT_EQUAL(states[0].configuredHeight, unsigned(std::lround(50.0 * scale / 120.0)),
                             "buffer height follows native scale despite fixed pixel hints");
            DAF_ASSERT_EQUAL(first->impl->parentPosition.x, -12, "native scale preserves the parent x offset");
            DAF_ASSERT_EQUAL(first->impl->parentPosition.y, 21, "native scale preserves the parent y offset");
        }
    }
    else
    {
        d_stdout("RuntimeBackend: SKIP fractional geometry events (compositor lacks fractional scale/viewport)");
    }
    DAF_ASSERT_EQUAL(puglHide(views[0].get()), PUGL_SUCCESS, "the logically sized child hides");
    DAF_ASSERT_EQUAL(puglShow(views[0].get(), PUGL_SHOW_PASSIVE), PUGL_SUCCESS, "the logically sized child remaps");
    states[0].exposed = false;
    first->impl->frameCallbackWorks = false;
    DAF_ASSERT_EQUAL(presented(0), true, "logical geometry survives a remap");
    DAF_ASSERT_EQUAL(first->impl->logicalSize.width, 125u, "remapping preserves the requested logical width");
    views[0].reset();
    states[1].exposed = false;
    static_cast<pugl_wl::PuglView*>(views[1].get())->impl->frameCallbackWorks = false;
    DAF_ASSERT_EQUAL(puglObscureView(views[1].get()), PUGL_SUCCESS, "the surviving sibling can redraw");
    DAF_ASSERT_EQUAL(presented(1), true, "the sibling still presents after another child is destroyed");
    views[1].reset();
    world.reset();
    if (host.drawAfterRebind()) return 1;
    DAF_ASSERT_EQUAL(eglSwapBuffers(host.display, host.surface), EGL_TRUE, "the host presents after all children are gone");
    DAF_ASSERT_NOT_EQUAL(wl_display_roundtrip(host.connection), -1, "the host remains connected after child teardown");

    WorldOwner owned(puglNewWorldOn(kPuglWindowSystemWayland, PUGL_MODULE, 0), puglFreeWorld);
    DAF_ASSERT_NOT_EQUAL(owned.get(), nullptr, "an owned world has a separate connection");
    for (const PuglNativeView foreignParent : {reinterpret_cast<PuglNativeView>(host.nativeSurface),
                                               static_cast<PuglNativeView>(0x2a00007)})
    {
        ViewOwner floating = makeGlView(owned.get(), false);
        DAF_ASSERT_NOT_EQUAL(floating.get(), nullptr, "a view can be allocated on the owned world");
        puglSetParent(floating.get(), foreignParent);
        DAF_ASSERT_EQUAL(puglRealize(floating.get()), PUGL_SUCCESS,
                         "a parent from another connection or window system still realizes");
        const auto* const native = static_cast<const pugl_wl::PuglView*>(floating.get());
        DAF_ASSERT_EQUAL(native->impl->xdgToplevel != nullptr && native->impl->subsurface == nullptr, true,
                         "an owned connection opens a toplevel window for a foreign parent");
    }
    d_stdout("RuntimeBackend: borrowed subsurfaces render, resize, remap and preserve the foreign host");
    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
// Fallback from a Wayland compositor that is announced but cannot be used

// Just enough of a Wayland compositor for a client to connect and list the globals, on one end of a socket
// pair whose other end a client gets through WAYLAND_SOCKET. It offers wl_compositor and no xdg_wm_base,
// so no window could open there.
class CompositorWithoutShell
{
public:
    int clientFd = -1;

    // read once stop() returns
    bool listedGlobals = false;
    bool clientHungUp = false;

    bool start()
    {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            return false;

        serverFd = fds[0];
        clientFd = fds[1];

        // a client that neither talks nor hangs up must not stall the test
        const timeval timeout = { 5, 0 };
        setsockopt(serverFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        thread = std::thread(&CompositorWithoutShell::serve, this);
        return true;
    }

    void stop()
    {
        thread.join();
        close(serverFd);
    }

private:
    int serverFd = -1;
    std::thread thread;

    void post(const uint32_t object, const uint32_t opcode, const std::vector<uint32_t>& args)
    {
        std::vector<uint32_t> message;
        message.push_back(object);
        message.push_back(static_cast<uint32_t>((2 + args.size()) * 4) << 16 | opcode);
        message.insert(message.end(), args.begin(), args.end());
        ::send(serverFd, message.data(), message.size() * 4, MSG_NOSIGNAL);
    }

    void serve()
    {
        std::vector<uint8_t> received;

        for (;;)
        {
            uint8_t chunk[4096];
            const ssize_t length = recv(serverFd, chunk, sizeof(chunk), 0);

            if (length <= 0)
            {
                clientHungUp = length == 0;
                // after a timeout, a client still waiting on a reply sees the connection end instead
                shutdown(serverFd, SHUT_RDWR);
                return;
            }

            received.insert(received.end(), chunk, chunk + length);

            // requests: a header of object id and size << 16 | opcode, then the arguments
            while (received.size() >= 8)
            {
                uint32_t header[2];
                std::memcpy(header, received.data(), sizeof(header));

                const uint32_t size = header[1] >> 16;
                if (size < 8 || received.size() < size)
                    break;

                uint32_t newId = 0;
                if (size >= 12)
                    std::memcpy(&newId, received.data() + 8, sizeof(newId));

                const uint32_t opcode = header[1] & 0xffff;

                if (header[0] == 1 && opcode == 0)
                {
                    // wl_display.sync: wl_callback.done, then wl_display.delete_id for the callback
                    post(newId, 0, { 0 });
                    post(1, 1, { newId });
                }
                else if (header[0] == 1 && opcode == 1)
                {
                    // wl_display.get_registry: wl_registry.global(name, interface, version) for each global
                    static const char kInterface[16] = "wl_compositor";
                    std::vector<uint32_t> args = { 1, static_cast<uint32_t>(std::strlen(kInterface) + 1) };
                    for (size_t i = 0; i < sizeof(kInterface); i += 4)
                    {
                        uint32_t word;
                        std::memcpy(&word, kInterface + i, sizeof(word));
                        args.push_back(word);
                    }
                    args.push_back(4);
                    post(newId, 0, args);
                    listedGlobals = true;
                }

                received.erase(received.begin(), received.begin() + size);
            }
        }
    }
};

// whether the Wayland libraries DGL loads when it tries Wayland are there to load
static bool waylandLibrariesLoad()
{
    static const char* const kLibraries[] = {
        "libwayland-client.so.0", "libwayland-cursor.so.0", "libxkbcommon.so.0",
        "libwayland-egl.so.1", "libEGL.so.1",
    };

    for (const char* const library : kLibraries)
    {
        void* const handle = dlopen(library, RTLD_LAZY | RTLD_LOCAL);
        if (handle == nullptr)
            return false;
        dlclose(handle);
    }

    return true;
}

static bool waylandForced(const char* const value)
{
    const char* const forced = std::getenv("DAF_WINDOW_BACKEND");
    return forced != nullptr && std::strcmp(forced, value) == 0;
}

// a program world where the announced compositor cannot be used
static int checkProgramFallsBack(const char* const what)
{
    const Expected expected = ! waylandForced("wayland") && isSet("DISPLAY") ? kX11 : kNone;
    PuglWorld* const world = puglNewWorld(PUGL_PROGRAM, 0);

    if (expected == kNone)
    {
        DAF_ASSERT_EQUAL(world, nullptr, "a program world opens on nothing then");
    }
    else
    {
        DAF_ASSERT_NOT_EQUAL(world, nullptr, "a program world falls back to X11");
        DAF_ASSERT_EQUAL(puglWorldIsX11(world), true, "a program world falls back to X11");
        puglFreeWorld(world);
    }

    d_stdout("RuntimeBackend: with %s, a program world opens on %s", what, backendName(expected));
    return 0;
}

// These change the environment, so they come last.
static int checkFallbacks()
{
    unsetenv("WAYLAND_SOCKET");
    setenv("WAYLAND_DISPLAY", "daf-runtime-backend-test-no-compositor", 1);

    if (checkProgramFallsBack("no compositor at WAYLAND_DISPLAY") != 0)
        return 1;

    CompositorWithoutShell compositor;
    DAF_ASSERT_EQUAL(compositor.start(), true, "the stand-in compositor starts");

    char clientFd[16];
    std::snprintf(clientFd, sizeof(clientFd), "%d", compositor.clientFd);
    unsetenv("WAYLAND_DISPLAY");
    setenv("WAYLAND_SOCKET", clientFd, 1);

    const int status = checkProgramFallsBack("a compositor without xdg_wm_base at WAYLAND_SOCKET");

    // libwayland takes the socket over, unsetting WAYLAND_SOCKET, and closes it with the failed world
    const bool socketTaken = std::getenv("WAYLAND_SOCKET") == nullptr;
    if (! socketTaken)
        close(compositor.clientFd);
    compositor.stop();

    if (status != 0)
        return status;

    // unless the libraries were missing, or DAF_WINDOW_BACKEND=x11 kept it from trying
    if (! waylandForced("x11") && waylandLibrariesLoad())
    {
        DAF_ASSERT_EQUAL(socketTaken, true, "Wayland was tried through WAYLAND_SOCKET");
        DAF_ASSERT_EQUAL(compositor.listedGlobals, true, "the client asked the compositor for its globals");
        DAF_ASSERT_EQUAL(compositor.clientHungUp, true, "the client let go of the compositor it could not use");
    }

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    const Expected expected = expectedBackend(true);
    DAF_ASSERT_NOT_EQUAL(expected, kInvalid, "DAF_TEST_EXPECT_BACKEND is x11, wayland or none");

    // A module's world follows its own rule. It stays open until the end, so with both servers up an X11
    // world and a Wayland world share the process. That also keeps a lone Xvfb from resetting, which it
    // does when its last client leaves, while the program's world connects again.
    const Expected moduleExpected = expectedBackend(false);
    PuglWorld* const moduleWorld = puglNewWorld(PUGL_MODULE, 0);

    if (moduleExpected == kNone)
    {
        DAF_ASSERT_EQUAL(moduleWorld, nullptr, "a module world cannot open without its backend");
    }
    else
    {
        DAF_ASSERT_NOT_EQUAL(moduleWorld, nullptr, "a module world opens");
        DAF_ASSERT_EQUAL(puglWorldIsX11(moduleWorld), moduleExpected == kX11,
                         "a module world opens on the expected backend");
    }

    d_stdout("RuntimeBackend: module world on %s", backendName(moduleExpected));

    PuglWorld* const world = puglNewWorld(PUGL_PROGRAM, 0);

    if (expected == kNone)
    {
        DAF_ASSERT_EQUAL(world, nullptr, "a program world cannot open without its backend");
        d_stdout("RuntimeBackend: program world on none, as expected");
    }
    else
    {
        DAF_ASSERT_NOT_EQUAL(world, nullptr, "a program world opens");
        DAF_ASSERT_EQUAL(puglWorldIsX11(world), expected == kX11, "a program world opens on the expected backend");

        State state;
        PuglView* view = nullptr;
        if (openWindow(world, expected, state, "DAF runtime backend test", view) != 0)
            return 1;
        if (resize(world, view, state, 240, 180) != 0 || redrawAndSignal(world, view, state) != 0)
            return 1;

        d_stdout("RuntimeBackend: program world on %s, window realized, drew, presented, resized and got its event",
                 backendName(expected));

        // A module's window next to the program's, then both still answer. The program's is not resized
        // again: on a compositor, the configure that takes its focus away for the module's window carries
        // its old size, and can arrive after a resize.
        if (moduleWorld != nullptr)
        {
            State moduleState;
            PuglView* moduleView = nullptr;
            if (openWindow(moduleWorld, moduleExpected, moduleState, "DAF runtime backend test, module", moduleView) != 0)
                return 1;
            if (resize(moduleWorld, moduleView, moduleState, 264, 192) != 0)
                return 1;
            if (redrawAndSignal(moduleWorld, moduleView, moduleState) != 0 || redrawAndSignal(world, view, state) != 0)
                return 1;

            puglHide(moduleView);
            puglFreeView(moduleView);

            d_stdout("RuntimeBackend: a %s window and a %s window open at once", backendName(expected),
                     backendName(moduleExpected));
        }

        if (expected == kWayland && checkOwnedSubsurface(world, view)) return 1;
        puglHide(view);
        puglFreeView(view);
        puglFreeWorld(world);
    }

    int status = expected == kWayland ? checkBorrowedEgl() : 0;
    if (!status && expected == kWayland) status = checkSubsurfaceRendering();
    if (!status) status = checkFallbacks();

    if (moduleWorld != nullptr)
        puglFreeWorld(moduleWorld);

    return status;
}

// --------------------------------------------------------------------------------------------------------------------
