/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2026 Filipe Coelho <falktx@falktx.com>
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

#undef NDEBUG

#include <cassert>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <unistd.h>

#include "dgl/src/pugl.cpp"
#include "wayland-connection.hpp"

#ifndef HAVE_WAYLAND
# error This test requires the native Wayland backend
#endif

// --------------------------------------------------------------------------------------------------------------------

namespace {

using namespace DGL_NAMESPACE;
using namespace dafTestWayland;
#ifdef DGL_RUNTIME_BACKEND
namespace wl = DGL_NAMESPACE::pugl_wl;
#else
namespace wl = DGL_NAMESPACE;
#endif

static void hostGlobal(void* data, wl_registry* registry, uint32_t name,
                       const char* interface, uint32_t)
{
    if (std::strcmp(interface, "wl_compositor") == 0)
        *static_cast<wl_compositor**>(data) = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
}

static void hostGlobalRemove(void*, wl_registry*, uint32_t) {}

static void sendPaste(void* const data, wl_callback* const callback, uint32_t)
{
    const int fd = *static_cast<int*>(data);
    assert(write(fd, "paste", 5) == 5);
    close(fd);
    wl_callback_destroy(callback);
}

static void testActivationProtocol()
{
    for (const bool advertised : {false, true})
    {
        ConnectionServer server(true, true, false, advertised);
        wl_display* const display = server.connect();
        DGL_NAMESPACE::PuglWorld* const world = puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display);
        assert(world);
        const auto* const nativeWorld = static_cast<wl::PuglWorld*>(world);
        assert((nativeWorld->impl->activation != nullptr) == advertised);
        puglFreeWorld(world);
        assert(wl_display_roundtrip(display) >= 0);
        unsigned destroyed = 0;
        for (const auto& request : server.requests())
            if (request.type == "xdg_activation_v1")
            {
                assert(request.opcode == 0);
                ++destroyed;
            }
        assert(destroyed == unsigned(advertised));
        wl_display_disconnect(display);
    }
}

static void testBorrowedConnection()
{
    ConnectionServer server;
    wl_display* const display = server.connect();
    const int fd = wl_display_get_fd(display);
    const wl_callback_listener syncListener = {syncDone};

    const char* const previous = std::getenv("XDG_ACTIVATION_TOKEN");
    const bool hadPrevious = previous != nullptr;
    const std::string saved = previous ? previous : "";
    assert(setenv("XDG_ACTIVATION_TOKEN", "host-launch-token", 1) == 0);
    const char* const previousBackend = std::getenv("DAF_WINDOW_BACKEND");
    const bool hadBackend = previousBackend != nullptr;
    const std::string savedBackend = previousBackend ? previousBackend : "";
    assert(setenv("DAF_WINDOW_BACKEND", "x11", 1) == 0);

    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        unsigned hostCalls = 0;
        queueSync(display, nullptr, &syncListener, &hostCalls);
        DGL_NAMESPACE::PuglWorld* const world =
            puglNewWorldWithWaylandDisplay(PUGL_PROGRAM, 0, display);
        assert(world);
        wl::PuglWorld* const nativeWorld = static_cast<wl::PuglWorld*>(world);
        assert(!nativeWorld->impl->ownsDisplay);
        assert(nativeWorld->type == PUGL_PROGRAM);
        assert(nativeWorld->startTime > 0.0);
        assert(puglGetNativeWorld(world) == display);
        assert(std::strcmp(puglGetWorldString(world, PUGL_CLASS_NAME), "Pugl") == 0);
        assert(puglGetTime(world) >= 0.0 && puglGetTime(world) < 30.0);
        assert(nativeWorld->impl->launcherToken == nullptr);
        assert(std::getenv("XDG_ACTIVATION_TOKEN"));
        assert(std::strcmp(std::getenv("XDG_ACTIVATION_TOKEN"), "host-launch-token") == 0);
        assert(hostCalls == 0);
        assert(nativeWorld->impl->numOutputs == 1U && nativeWorld->impl->outputs[0].scale == 2);

        unsigned privateCalls = 0;
        queueSync(display, nullptr, &syncListener, &hostCalls);
        queueSync(display, nativeWorld->impl->queue, &syncListener, &privateCalls);
        readReplies(display);
        assert(privateCalls == 0 && hostCalls == 0);
        assert(DGL_NAMESPACE::puglUpdate(world, 60.0) == PUGL_SUCCESS);
        assert(privateCalls == 1 && hostCalls == 0);
        assert(DGL_NAMESPACE::puglUpdate(world, -1.0) == PUGL_SUCCESS);
        assert(wl_display_dispatch_pending(display) >= 0);
        assert(hostCalls == 2);

        // A synchronous receive must service its own sender without dispatching the host callback.
        int pipeFds[2];
        assert(pipe(pipeFds) == 0);
        const wl_callback_listener pasteListener = {sendPaste};
        queueSync(display, nullptr, &syncListener, &hostCalls);
        queueSync(display, nativeWorld->impl->queue, &pasteListener, &pipeFds[1]);
        wl::PuglBlob received = {};
        assert(wl::puglWaylandReadPipe(nativeWorld->impl, pipeFds[0], &received) == PUGL_SUCCESS);
        assert(received.len == 5U && std::memcmp(received.data, "paste", 5U) == 0);
        std::free(received.data);
        close(pipeFds[0]);
        assert(hostCalls == 2);

        DGL_NAMESPACE::puglFreeWorld(world);
        assert(fcntl(fd, F_GETFD) >= 0);
        assert(hostCalls == 2);
        assert(wl_display_roundtrip(display) >= 0);
        assert(hostCalls == 3);
    }

    if (hadPrevious)
        assert(setenv("XDG_ACTIVATION_TOKEN", saved.c_str(), 1) == 0);
    else
        assert(unsetenv("XDG_ACTIVATION_TOKEN") == 0);
    if (hadBackend)
        assert(setenv("DAF_WINDOW_BACKEND", savedBackend.c_str(), 1) == 0);
    else
        assert(unsetenv("DAF_WINDOW_BACKEND") == 0);
    wl_display_disconnect(display);
}

static void testConnectionOwnership()
{
    const wl_callback_listener syncListener = {syncDone};
    for (unsigned owned = 0; owned < 2; ++owned)
    {
        for (unsigned valid = 0; valid < 2; ++valid)
        {
            ConnectionServer server(valid != 0);
            wl_display* const display = server.connect();
            const int fd = wl_display_get_fd(display);
            unsigned hostCalls = 0;
            if (!owned)
                queueSync(display, nullptr, &syncListener, &hostCalls);
            wl::PuglWorldInternals* const impl =
                wl::puglWaylandInitWorldInternals(display, owned != 0, PUGL_MODULE, nullptr);
            assert(bool(impl) == bool(valid));
            wl::puglWaylandDestroyWorldInternals(impl);
            if (owned)
            {
                errno = 0;
                assert(fcntl(fd, F_GETFD) == -1 && errno == EBADF);
            }
            else
            {
                assert(hostCalls == 0);
                assert(fcntl(fd, F_GETFD) >= 0);
                assert(wl_display_roundtrip(display) >= 0);
                assert(hostCalls == 1);
                if (!valid)
                {
                    assert(puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display) == nullptr);
                    assert(fcntl(fd, F_GETFD) >= 0);
                    assert(wl_display_roundtrip(display) >= 0);
                }
                wl_display_disconnect(display);
            }
        }
    }
}

static void testIndependentSeat()
{
    ConnectionServer server(true, true, true);
    wl_display* const display = server.connect();
    DGL_NAMESPACE::PuglWorld* const world =
        puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display);
    assert(world);
    assert(!puglGetWaylandInput(world).data);
    wl::PuglWorldInternals* const impl = static_cast<wl::PuglWorld*>(world)->impl;
    assert(impl->seat && impl->pointer && impl->keyboard);
    assert(impl->dataDeviceManager && !impl->dataDevice);
    wl::puglWaylandSeatCapabilities(impl, impl->seat,
                                  WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
    DGL_NAMESPACE::puglFreeWorld(world);
    assert(wl_display_roundtrip(display) >= 0);
    unsigned pointers = 0, keyboards = 0, pointerReleases = 0, keyboardReleases = 0, seatReleases = 0;
    for (const auto& request : server.requests())
    {
        if (request.type == "wl_seat" && request.opcode == 0) ++pointers;
        if (request.type == "wl_seat" && request.opcode == 1) ++keyboards;
        if (request.type == "wl_seat" && request.opcode == 3) ++seatReleases;
        if (request.type == "wl_pointer" && request.opcode == 1) ++pointerReleases;
        if (request.type == "wl_keyboard" && request.opcode == 0) ++keyboardReleases;
        assert(!(request.type == "wl_data_device_manager" && request.opcode == 1));
    }
    assert(pointers == 1 && keyboards == 1);
    assert(pointerReleases == 1 && keyboardReleases == 1 && seatReleases == 1);
    wl_display_disconnect(display);
}

static void testHostSeat()
{
    for (unsigned valid = 0; valid < 2; ++valid)
    {
        ConnectionServer server(valid != 0, true, true);
        wl_display* const display = server.connect();
        wl_registry* const registry = wl_display_get_registry(display);
        wl_compositor* compositor = nullptr;
        const wl_registry_listener registryListener = {hostGlobal, hostGlobalRemove};
        assert(wl_registry_add_listener(registry, &registryListener, &compositor) == 0);
        assert(wl_display_roundtrip(display) >= 0);
        wl_seat* const seat = static_cast<wl_seat*>(wl_registry_bind(registry, 5, &wl_seat_interface, 5));
        unsigned hostCalls = 0;
        const wl_seat_listener seatListener = {
            [](void* data, wl_seat*, uint32_t) { ++*static_cast<unsigned*>(data); },
            [](void*, wl_seat*, const char*) {}
        };
        assert(wl_seat_add_listener(seat, &seatListener, &hostCalls) == 0);
        wl_pointer* const pointer = wl_seat_get_pointer(seat);
        wl_keyboard* const keyboard = wl_seat_get_keyboard(seat);
        wl_pointer_set_user_data(pointer, &hostCalls);
        wl_keyboard_set_user_data(keyboard, &hostCalls);
        for (unsigned cycle = 0; cycle < 2; ++cycle)
        {
            DGL_NAMESPACE::PuglWorld* const world =
                puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display, seat);
            assert(bool(world) == bool(valid));
            if (world)
            {
                auto* const impl = static_cast<wl::PuglWorld*>(world)->impl;
                assert(impl->hostManagedInput && impl->seat == seat);
                assert(!impl->pointer && !impl->keyboard);
                assert(impl->dataDeviceManager && !impl->dataDevice);
                impl->pointer = pointer;
                impl->keyboard = keyboard;
                wl::puglWaylandSeatCapabilities(impl, seat, 0);
                assert(impl->pointer == pointer && impl->keyboard == keyboard);
                DGL_NAMESPACE::puglFreeWorld(world);
            }
            assert(hostCalls == 0);
            assert(wl_seat_get_user_data(seat) == &hostCalls);
            assert(wl_pointer_get_user_data(pointer) == &hostCalls);
            assert(wl_keyboard_get_user_data(keyboard) == &hostCalls);
        }
        assert(wl_display_roundtrip(display) >= 0);
        assert(hostCalls == 1);
        unsigned pointers = 0, keyboards = 0, seats = 0;
        for (const auto& request : server.requests())
        {
            if (request.type == "wl_registry" && request.opcode == 0 && request.words[0] == 5U) ++seats;
            if (request.type == "wl_seat" && request.opcode == 0) ++pointers;
            if (request.type == "wl_seat" && request.opcode == 1) ++keyboards;
            assert(!(request.type == "wl_data_device_manager" && request.opcode == 1));
            assert(!(request.type == "wl_seat" && request.opcode == 3));
            assert(!(request.type == "wl_pointer" && request.opcode == 1));
            assert(!(request.type == "wl_keyboard" && request.opcode == 0));
        }
        assert(seats == 1 && pointers == 1 && keyboards == 1);
        wl_keyboard_release(keyboard);
        wl_pointer_release(pointer);
        wl_seat_release(seat);
        if (compositor) wl_compositor_destroy(compositor);
        wl_registry_destroy(registry);
        assert(wl_display_roundtrip(display) >= 0);
        wl_display_disconnect(display);
    }
}

static void testHostFocus()
{
    ConnectionServer server(true, true, true);
    wl_display* const display = server.connect();
    wl_registry* const registry = wl_display_get_registry(display);
    wl_compositor* compositor = nullptr;
    const wl_registry_listener registryListener = {hostGlobal, hostGlobalRemove};
    assert(wl_registry_add_listener(registry, &registryListener, &compositor) == 0);
    assert(wl_display_roundtrip(display) >= 0);
    wl_seat* const seat = static_cast<wl_seat*>(wl_registry_bind(registry, 5, &wl_seat_interface, 5));
    const wl_seat_listener seatListener = {
        [](void*, wl_seat*, uint32_t) {}, [](void*, wl_seat*, const char*) {}
    };
    assert(wl_seat_add_listener(seat, &seatListener, nullptr) == 0);
    wl_surface* const parent = wl_compositor_create_surface(compositor);
    wl_surface* const otherParent = wl_compositor_create_surface(compositor);
    DGL_NAMESPACE::PuglWorld* const world = puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display, seat);
    assert(world);
    auto* const impl = static_cast<wl::PuglWorld*>(world)->impl;
    const auto input = puglGetWaylandInput(world);
    assert(input.data == impl && input.keyboard && input.pointer && input.consumed &&
           input.hasKeyboardFocus && input.releaseFocus && input.reset);
    assert(!input.hasKeyboardFocus(input.data));
    struct FocusLog {
        std::vector<std::pair<DGL_NAMESPACE::PuglView*, PuglEventType>> events;
        wl::PuglView* redirect = nullptr;
        wl_surface* leaveParent = nullptr;
        bool hideOnKey = false;
        bool hideOnFocus = false;
    } log;
    wl::PuglView* views[2];
    for (auto& view : views)
    {
        view = static_cast<wl::PuglView*>(DGL_NAMESPACE::puglNewView(world));
        assert(view);
        wl::puglSetMatchingBackendForCurrentBuild(view);
        wl::puglSetParent(view, reinterpret_cast<PuglNativeView>(parent));
        wl::puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 200, 100);
        wl::puglSetHandle(view, &log);
        wl::puglSetEventFunc(view, [](DGL_NAMESPACE::PuglView* view, const PuglEvent* event) {
            auto* const log = static_cast<FocusLog*>(DGL_NAMESPACE::puglGetHandle(view));
            if (event->type == PUGL_FOCUS_IN || event->type == PUGL_FOCUS_OUT || event->type == PUGL_BUTTON_PRESS ||
                event->type == PUGL_BUTTON_RELEASE || event->type == PUGL_KEY_PRESS ||
                event->type == PUGL_KEY_RELEASE || event->type == PUGL_TEXT)
                log->events.emplace_back(view, event->type);
            if (event->type == PUGL_KEY_PRESS && log->hideOnKey)
            {
                assert(DGL_NAMESPACE::puglHide(view) == PUGL_SUCCESS);
                wl::puglWaylandKeyboardKey(static_cast<wl::PuglView*>(view)->world->impl,
                                           nullptr, 98, 300, 31, WL_KEYBOARD_KEY_STATE_RELEASED);
            }
            if (event->type == PUGL_FOCUS_IN && log->hideOnFocus)
                assert(DGL_NAMESPACE::puglHide(view) == PUGL_SUCCESS);
            if (event->type == PUGL_FOCUS_OUT && log->leaveParent)
            {
                wl::puglWaylandKeyboardLeave(static_cast<wl::PuglView*>(view)->world->impl,
                                             nullptr, 99, log->leaveParent);
                log->leaveParent = nullptr;
            }
            if (event->type == PUGL_FOCUS_OUT && log->redirect)
            {
                auto* const redirect = log->redirect;
                log->redirect = nullptr;
                assert(wl::puglGrabFocus(redirect) == PUGL_SUCCESS);
            }
            return PUGL_SUCCESS;
        });
        assert(wl::puglShow(view, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
    }
    assert(wl::puglGrabFocus(views[0]) == PUGL_SUCCESS);
    assert(!wl::puglHasFocus(views[0]) && log.events.empty());
    wl::puglWaylandKeyboardEnter(impl, nullptr, 1, parent, nullptr);
    assert(wl::puglHasFocus(views[0]));
    assert(log.events.size() == 1 && log.events.back().second == PUGL_FOCUS_IN);
    wl::puglWaylandStartRepeat(impl, views[0], 30, 0.0);
    assert(impl->repeat.view == views[0]);
    wl::puglWaylandKeyboardLeave(impl, nullptr, 2, otherParent);
    assert(wl::puglHasFocus(views[0]));
    wl::puglWaylandKeyboardEnter(impl, nullptr, 3, otherParent, nullptr);
    assert(!wl::puglHasFocus(views[0]) && !impl->repeat.view);
    wl::puglWaylandKeyboardEnter(impl, nullptr, 4, parent, nullptr);
    assert(wl::puglHasFocus(views[0]));
    log.redirect = views[0];
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);
    assert(wl::puglHasFocus(views[0]) && !wl::puglHasFocus(views[1]));
    log.leaveParent = parent;
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);
    assert(!wl::puglHasFocus(views[0]) && !wl::puglHasFocus(views[1]));
    wl::puglWaylandKeyboardEnter(impl, nullptr, 100, parent, nullptr);
    assert(wl::puglHasFocus(views[1]));
    assert(wl::puglGrabFocus(views[0]) == PUGL_SUCCESS);
    log.events.clear();
    wl::puglWaylandPointerEnter(impl, nullptr, 5, views[1]->impl->wlSurface, 0, 0);
    wl::puglWaylandPointerButton(impl, nullptr, 6, 100, PUGL_WAYLAND_BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    assert(log.events.size() == 3);
    assert(log.events[0].first == views[0] && log.events[0].second == PUGL_FOCUS_OUT);
    assert(log.events[1].first == views[1] && log.events[1].second == PUGL_FOCUS_IN);
    assert(log.events[2].first == views[1] && log.events[2].second == PUGL_BUTTON_PRESS);
    assert(wl::puglHide(views[1]) == PUGL_SUCCESS);
    assert(!wl::puglHasFocus(views[1]) && log.events.back().second == PUGL_FOCUS_OUT);
    assert(wl::puglGrabFocus(views[1]) == PUGL_BAD_CALL);
    assert(wl::puglShow(views[1], PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
    assert(!wl::puglHasFocus(views[1]));
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);

    const char* const keymap =
        "xkb_keymap { xkb_keycodes { minimum=8; maximum=255; <AC01>=38; };"
        "xkb_types { type \"ONE_LEVEL\" { modifiers=None; map[None]=Level1; }; };"
        "xkb_compatibility {}; xkb_symbols { key <AC01> { [ a ] }; }; };";
    char keymapPath[] = "/tmp/daf-keymap-XXXXXX";
    const int keymapFd = mkstemp(keymapPath);
    assert(keymapFd >= 0 && unlink(keymapPath) == 0);
    const size_t keymapSize = std::strlen(keymap) + 1U;
    assert(write(keymapFd, keymap, keymapSize) == static_cast<ssize_t>(keymapSize));
    const int copy = fcntl(keymapFd, F_DUPFD_CLOEXEC, 0);
    assert(copy >= 0);
    input.keyboard->keymap(input.data, nullptr, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, copy, uint32_t(keymapSize));
    errno = 0;
    assert(fcntl(copy, F_GETFD) < 0 && errno == EBADF);
    assert(fcntl(keymapFd, F_GETFD) >= 0);
    close(keymapFd);
    assert(impl->xkb.state);
    wl::puglWaylandKeyboardKey(impl, nullptr, 101, 200, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    assert(wl::puglGrabFocus(views[0]) == PUGL_SUCCESS);
    log.events.clear();
    wl::puglWaylandKeyboardKey(impl, nullptr, 102, 201, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
    assert(log.events.empty() && impl->lastEventConsumed);
    wl::puglWaylandKeyboardKey(impl, nullptr, 103, 202, 31, WL_KEYBOARD_KEY_STATE_RELEASED);
    assert(log.events.empty() && !impl->lastEventConsumed);
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);
    log.hideOnKey = true;
    log.events.clear();
    wl::puglWaylandKeyboardKey(impl, nullptr, 7, 200, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    assert(log.events.size() == 2);
    assert(log.events[0].second == PUGL_KEY_PRESS && log.events[1].second == PUGL_FOCUS_OUT);
    assert(!impl->repeat.view && impl->lastEventConsumed);
    assert(wl::puglShow(views[1], PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);
    log.events.clear();
    int repeatPipe[2];
    assert(pipe(repeatPipe) == 0);
    const uint64_t expirations = 3;
    assert(write(repeatPipe[1], &expirations, sizeof(expirations)) == sizeof(expirations));
    const int repeatFd = impl->repeat.fd;
    impl->repeat.fd = repeatPipe[0];
    impl->repeat.view = views[1];
    impl->repeat.key = 30;
    wl::puglWaylandHandleRepeat(impl);
    impl->repeat.fd = repeatFd;
    close(repeatPipe[0]);
    close(repeatPipe[1]);
    assert(log.events.size() == 2 && !impl->repeat.view);
    log.hideOnKey = false;
    assert(wl::puglShow(views[1], PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);
    const auto key = [&](uint32_t code, bool pressed) {
        input.keyboard->key(input.data, nullptr, 200, 300, code,
                            pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
        return input.consumed(input.data);
    };
    const auto button = [&](bool pressed) {
        input.pointer->button(input.data, nullptr, 201, 301, PUGL_WAYLAND_BTN_LEFT,
                              pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        return input.consumed(input.data);
    };
    log.events.clear();
    assert(key(30, false) && button(false) && log.events.empty());
    assert(key(30, true) && key(30, false));
    assert(log.events.size() == 3 && log.events.back().second == PUGL_KEY_RELEASE);
    const auto repeated = [&](uint32_t code) {
        input.keyboard->key(input.data, nullptr, 200, 300, code, 2U); // WL_KEYBOARD_KEY_STATE_REPEATED, v10
        return input.consumed(input.data);
    };
    log.events.clear();
    assert(key(30, true) && repeated(30) && repeated(30) && key(30, false));
    assert(log.events.size() == 7 && impl->heldKeys.count == 0);
    for (size_t i = 0; i < 6; ++i)
        assert(log.events[i].first == views[1] && log.events[i].second != PUGL_KEY_RELEASE);
    assert(log.events[6].second == PUGL_KEY_RELEASE);
    log.events.clear();
    assert(!repeated(31) && log.events.empty() && impl->heldKeys.count == 0);
    assert(key(30, true));
    assert(wl::puglGrabFocus(views[0]) == PUGL_SUCCESS);
    log.events.clear();
    assert(repeated(30) && log.events.empty());
    assert(key(30, false) && log.events.empty() && impl->heldKeys.count == 0);
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);
    assert(input.hasKeyboardFocus(input.data) && key(30, true));
    input.releaseFocus(input.data);
    assert(!input.hasKeyboardFocus(input.data) && !impl->selectedView);
    log.events.clear();
    assert(key(30, false) && log.events.empty());
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);

    uint32_t heldCode = 30;
    wl_array held = {sizeof(heldCode), sizeof(heldCode), &heldCode};
    assert(key(30, true));
    wl::puglWaylandKeyboardLeave(impl, nullptr, 202, parent);
    wl::puglWaylandKeyboardEnter(impl, nullptr, 203, parent, &held);
    log.events.clear();
    assert(key(30, false) && log.events.empty());
    wl::puglWaylandKeyboardLeave(impl, nullptr, 204, parent);
    assert(!key(30, true));
    wl::puglWaylandKeyboardEnter(impl, nullptr, 205, parent, &held);
    log.events.clear();
    assert(!key(30, false) && log.events.empty());
    assert(key(30, true));
    wl::puglWaylandKeyboardLeave(impl, nullptr, 206, parent);
    wl::puglWaylandKeyboardEnter(impl, nullptr, 207, parent, nullptr);
    assert(impl->heldKeys.count == 0);
    assert(key(30, true) && key(30, false));

    for (uint32_t code = 30; code < 158; ++code) assert(key(code, true));
    assert(impl->heldKeys.count == 128);
    assert(wl::puglHide(views[1]) == PUGL_SUCCESS);
    assert(wl::puglGrabFocus(views[0]) == PUGL_SUCCESS);
    log.events.clear();
    for (uint32_t code = 30; code < 158; ++code) assert(key(code, false));
    assert(log.events.empty() && impl->heldKeys.count == 0);

    wl::puglWaylandPointerEnter(impl, nullptr, 208, parent, 0, 0);
    assert(!button(true));
    wl::puglWaylandPointerEnter(impl, nullptr, 209, views[0]->impl->wlSurface, 0, 0);
    assert(!button(false) && log.events.empty());
    assert(button(true));
    assert(wl::puglHide(views[0]) == PUGL_SUCCESS);
    assert(wl::puglShow(views[0], PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
    assert(wl::puglShow(views[1], PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
    wl::puglWaylandPointerEnter(impl, nullptr, 210, views[1]->impl->wlSurface, 0, 0);
    log.events.clear();
    assert(button(false) && log.events.empty());
    log.hideOnFocus = true;
    assert(button(true));
    assert(log.events.size() == 2 && log.events.back().second == PUGL_FOCUS_OUT);
    log.events.clear();
    assert(button(false) && log.events.empty());
    log.hideOnFocus = false;
    assert(wl::puglShow(views[1], PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
    assert(wl::puglGrabFocus(views[1]) == PUGL_SUCCESS);
    wl::puglWaylandPointerEnter(impl, nullptr, 211, views[1]->impl->wlSurface, 0, 0);
    assert(button(true) && key(30, true));
    DGL_NAMESPACE::puglFreeView(views[0]);
    assert(wl::puglHasFocus(views[1]));
    DGL_NAMESPACE::puglFreeView(views[1]);
    assert(!impl->keyboardFocus && !impl->repeat.view);
    assert(log.events.back().second == PUGL_FOCUS_OUT);
    log.events.clear();
    assert(button(false) && key(30, false) && log.events.empty());
    input.reset(input.data);
    assert(!impl->xkb.state && !impl->xkb.keymap && !input.consumed(input.data));
    DGL_NAMESPACE::puglFreeWorld(world);
    wl_surface_destroy(otherParent);
    wl_surface_destroy(parent);
    wl_seat_release(seat);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    assert(wl_display_roundtrip(display) >= 0);
    wl_display_disconnect(display);
}

static void testHostPointer()
{
    ConnectionServer server(true, true, true);
    wl_display* const display = server.connect();
    wl_registry* const registry = wl_display_get_registry(display);
    wl_compositor* compositor = nullptr;
    const wl_registry_listener registryListener = {hostGlobal, hostGlobalRemove};
    assert(wl_registry_add_listener(registry, &registryListener, &compositor) == 0);
    assert(wl_display_roundtrip(display) >= 0);
    wl_seat* const seat = static_cast<wl_seat*>(wl_registry_bind(registry, 5, &wl_seat_interface, 5));
    const wl_seat_listener seatListener = {
        [](void*, wl_seat*, uint32_t) {}, [](void*, wl_seat*, const char*) {}
    };
    assert(wl_seat_add_listener(seat, &seatListener, nullptr) == 0);
    wl_pointer* const pointer = wl_seat_get_pointer(seat);
    wl_surface* const parent = wl_compositor_create_surface(compositor);
    auto* const world = puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display, seat);
    assert(world);
    auto* const impl = static_cast<wl::PuglWorld*>(world)->impl;
    const auto input = puglGetWaylandInput(world);
    struct PointerLog {
        std::vector<std::pair<DGL_NAMESPACE::PuglView*, PuglEventType>> events;
        double x = 0, y = 0;
        bool hideOnScroll = false;
    } log;
    wl_pointer_set_user_data(pointer, &log);
    wl::PuglView* views[2];
    for (auto& view : views)
    {
        view = static_cast<wl::PuglView*>(DGL_NAMESPACE::puglNewView(world));
        assert(view);
        wl::puglSetMatchingBackendForCurrentBuild(view);
        wl::puglSetParent(view, reinterpret_cast<PuglNativeView>(parent));
        wl::puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 200, 100);
        wl::puglSetHandle(view, &log);
        wl::puglSetEventFunc(view, [](DGL_NAMESPACE::PuglView* view, const PuglEvent* event) {
            auto* const log = static_cast<PointerLog*>(DGL_NAMESPACE::puglGetHandle(view));
            if (event->type == PUGL_POINTER_IN || event->type == PUGL_POINTER_OUT ||
                event->type == PUGL_MOTION || event->type == PUGL_SCROLL || event->type == PUGL_BUTTON_PRESS)
                log->events.emplace_back(view, event->type);
            if (event->type == PUGL_MOTION) {
                log->x = event->motion.x;
                log->y = event->motion.y;
            }
            if (event->type == PUGL_SCROLL && log->hideOnScroll)
                assert(DGL_NAMESPACE::puglHide(view) == PUGL_SUCCESS);
            return PUGL_SUCCESS;
        });
        assert(wl::puglShow(view, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
        view->impl->scale = 1.5;
    }
    wl::puglWaylandPointerEnter(impl, pointer, 41, views[0]->impl->wlSurface, 0, 0);
    assert(impl->pointer == pointer);
    assert(wl::puglSetCursor(views[0], PUGL_CURSOR_NONE) == PUGL_SUCCESS);
    wl::puglWaylandPointerMotion(impl, pointer, 100, wl_fixed_from_int(12), wl_fixed_from_int(8));
    assert(std::abs(log.x - 18.0) < 1e-9 && std::abs(log.y - 12.0) < 1e-9);
    log.events.clear();
    wl::puglWaylandPointerAxis(impl, pointer, 101, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    wl::puglWaylandPointerLeave(impl, pointer, 42, views[0]->impl->wlSurface);
    assert(log.events.size() == 2 && log.events[0].second == PUGL_SCROLL &&
           log.events[1].second == PUGL_POINTER_OUT);
    wl::puglWaylandPointerEnter(impl, pointer, 43, views[1]->impl->wlSurface, 0, 0);
    log.events.clear();
    wl::puglWaylandPointerFrame(impl, pointer);
    assert(log.events.empty());
    log.hideOnScroll = true;
    wl::puglWaylandPointerAxis(impl, pointer, 102, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(10));
    wl::puglWaylandPointerLeave(impl, pointer, 44, views[1]->impl->wlSurface);
    assert(log.events.size() == 2 && log.events.back().second == PUGL_POINTER_OUT);
    log.events.clear();
    wl::puglWaylandPointerEnter(impl, pointer, 45, views[1]->impl->wlSurface, 0, 0);
    wl::puglWaylandPointerMotion(impl, pointer, 103, 0, 0);
    wl::puglWaylandPointerButton(impl, pointer, 46, 104, PUGL_WAYLAND_BTN_RIGHT, WL_POINTER_BUTTON_STATE_PRESSED);
    assert(log.events.empty() && !impl->lastEventConsumed);
    log.hideOnScroll = false;
    input.keyboard->enter(input.data, nullptr, 47, parent, nullptr);
    assert(wl::puglGrabFocus(views[0]) == PUGL_SUCCESS);
    input.pointer->enter(input.data, pointer, 48, views[0]->impl->wlSurface, 0, 0);
    input.pointer->button(input.data, pointer, 49, 105, PUGL_WAYLAND_BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
    input.keyboard->key(input.data, nullptr, 50, 106, 30, WL_KEYBOARD_KEY_STATE_PRESSED);
    wl::puglWaylandStartRepeat(impl, views[0], 30, 0.0);
    assert(input.hasKeyboardFocus(input.data) && impl->heldKeys.count && impl->heldButtons.count);
    input.reset(input.data);
    assert(!input.hasKeyboardFocus(input.data) && !impl->pointerFocus && !impl->pointer && !impl->keyboard);
    assert(!impl->heldKeys.count && !impl->heldButtons.count && !impl->repeat.view && !impl->lastSerial);
    assert(impl->seat == seat);
    input.pointer->enter(input.data, pointer, 51, views[0]->impl->wlSurface, 0, 0);
    assert(impl->pointer == pointer && impl->pointerFocus == views[0]);
    for (auto* view : views) DGL_NAMESPACE::puglFreeView(view);
    DGL_NAMESPACE::puglFreeWorld(world);
    assert(wl_pointer_get_user_data(pointer) == &log);
    assert(wl_display_roundtrip(display) >= 0);
    bool cursorHidden = false;
    for (const auto& request : server.requests())
    {
        if (request.type != "wl_pointer") continue;
        assert(request.opcode != 1);
        if (request.opcode == 0 && request.words[0] == 41 && request.words[1] == 0) cursorHidden = true;
    }
    assert(cursorHidden);
    wl_pointer_release(pointer);
    wl_surface_destroy(parent);
    wl_seat_release(seat);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    assert(wl_display_roundtrip(display) >= 0);
    wl_display_disconnect(display);
}

static void testForeignSurface()
{
    using namespace DGL_NAMESPACE;
#ifdef DGL_RUNTIME_BACKEND
    using namespace DGL_NAMESPACE::pugl_wl;
#endif
    ConnectionServer server;
    wl_display* const display = server.connect();
    wl_registry* const registry = wl_display_get_registry(display);
    wl_compositor* compositor = nullptr;
    const wl_registry_listener listener = {hostGlobal, hostGlobalRemove};
    assert(wl_registry_add_listener(registry, &listener, &compositor) == 0);
    assert(wl_display_roundtrip(display) >= 0);
    assert(compositor);
    wl_surface* const surface = wl_compositor_create_surface(compositor);
    assert(surface);
    char foreignData = 0;
    wl_surface_set_user_data(surface, &foreignData);
    assert(puglWaylandViewForSurface(surface) == nullptr);

    DGL_NAMESPACE::PuglWorld* const world =
        puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display);
    assert(world);
    wl::PuglWorldInternals* const impl = static_cast<wl::PuglWorld*>(world)->impl;
    wl::PuglView* const view = static_cast<wl::PuglView*>(DGL_NAMESPACE::puglNewView(world));
    assert(view);
    wl::puglSetMatchingBackendForCurrentBuild(view);
    assert(wl::puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 200, 100) == PUGL_SUCCESS);
    assert(wl::puglSetEventFunc(view, [](DGL_NAMESPACE::PuglView*, const PuglEvent*) {
        return PUGL_SUCCESS;
    }) == PUGL_SUCCESS);
    assert(wl::puglRealize(view) == PUGL_SUCCESS);
    assert(puglWaylandViewForSurface(view->impl->wlSurface) == view);

    for (void* const foreign : {static_cast<void*>(&foreignData), static_cast<void*>(view)})
    {
        wl_surface_set_user_data(surface, foreign);
        assert(puglWaylandViewForSurface(surface) == nullptr);
        puglWaylandPointerEnter(impl, nullptr, 1U, surface, 0, 0);
        puglWaylandKeyboardEnter(impl, nullptr, 2U, surface, nullptr);
        assert(impl->pointerFocus == nullptr && impl->keyboardFocus == nullptr);
        puglWaylandPointerLeave(impl, nullptr, 3U, surface);
        puglWaylandKeyboardLeave(impl, nullptr, 4U, surface);
        assert(wl_surface_get_user_data(surface) == foreign);
    }

    wl_output* const output = static_cast<wl_output*>(
        wl_registry_bind(registry, 3U, &wl_output_interface, 2));
    assert(output);
    wl_output_set_user_data(output, &foreignData);
    puglWaylandSurfaceEnter(view, view->impl->wlSurface, output);
    assert(view->impl->bufferScale == 1 && view->impl->numEnteredOutputs == 0);
    puglWaylandSurfaceEnter(view, view->impl->wlSurface, impl->outputs[0].output);
    assert(view->impl->bufferScale == 2 && view->impl->numEnteredOutputs == 1);
    assert(wl_output_get_user_data(output) == &foreignData);
    wl_output_destroy(output);
    DGL_NAMESPACE::puglFreeView(view);
    DGL_NAMESPACE::puglFreeWorld(world);

    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
}

static void testSubsurfaceLifetime()
{
    for (unsigned supported = 0; supported < 2; ++supported)
    {
        ConnectionServer server(true, supported != 0);
        wl_display* const display = server.connect();
        wl_registry* const registry = wl_display_get_registry(display);
        wl_compositor* compositor = nullptr;
        const wl_registry_listener listener = {hostGlobal, hostGlobalRemove};
        assert(wl_registry_add_listener(registry, &listener, &compositor) == 0);
        assert(wl_display_roundtrip(display) >= 0);
        assert(compositor);
        wl_surface* const parent = wl_compositor_create_surface(compositor);
        assert(parent);
        char hostData = 0;
        const char* const hostTag[] = {"host surface"};
        wl_surface_set_user_data(parent, &hostData);
        wl_proxy_set_tag(reinterpret_cast<wl_proxy*>(parent), hostTag);

        DGL_NAMESPACE::PuglWorld* const world =
            puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, display);
        assert(world);
        for (unsigned cycle = 0; cycle < 2; ++cycle)
        {
            auto* const view = static_cast<wl::PuglView*>(DGL_NAMESPACE::puglNewView(world));
            assert(view);
            wl::puglSetMatchingBackendForCurrentBuild(view);
            assert(wl::puglSetParent(view, reinterpret_cast<PuglNativeView>(parent)) == PUGL_SUCCESS);
            assert(wl::puglSetPositionHint(view, PUGL_DEFAULT_POSITION, -7, 11) == PUGL_SUCCESS);
            assert(wl::puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 200, 100) == PUGL_SUCCESS);
            assert(wl::puglSetEventFunc(view, [](DGL_NAMESPACE::PuglView*, const PuglEvent*) {
                return PUGL_SUCCESS;
            }) == PUGL_SUCCESS);
            if (supported && cycle == 0)
            {
                const wl::PuglBackend* const backend = view->backend;
                wl::PuglBackend failing = *backend;
                failing.create = [](wl::PuglView*) { return PUGL_CREATE_CONTEXT_FAILED; };
                view->backend = &failing;
                assert(wl::puglRealize(view) == PUGL_CREATE_CONTEXT_FAILED);
                assert(!view->impl->wlSurface && !view->impl->subsurface);
                view->backend = backend;
            }
            const PuglStatus status = wl::puglRealize(view);
            assert(status == (supported ? PUGL_SUCCESS : PUGL_UNSUPPORTED));
            if (supported)
            {
                assert(view->impl->wlSurface && view->impl->wlSurface != parent);
                assert(!view->impl->xdgSurface && !view->impl->xdgToplevel);
                assert(view->impl->configured);
                assert(wl::puglShow(view, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
                assert(DGL_NAMESPACE::puglUpdate(world, 0.0) == PUGL_SUCCESS);
                assert(wl::puglGetPositionHint(view, PUGL_CURRENT_POSITION).x == -7);
                assert(wl::puglGetPositionHint(view, PUGL_CURRENT_POSITION).y == 11);
                for (const double scale : {1.0, 2.0, 1.5})
                {
                    view->impl->scale = scale;
                    assert(wl::puglSetPositionHint(view, PUGL_CURRENT_POSITION, -17, 9) == PUGL_SUCCESS);
                    assert(DGL_NAMESPACE::puglUpdate(world, 0.0) == PUGL_SUCCESS);
                    const PuglPoint position = wl::puglGetPositionHint(view, PUGL_CURRENT_POSITION);
                    assert(position.x == -17 && position.y == 9);
                }
                assert(wl::puglSetPositionHint(view, PUGL_CURRENT_POSITION, 0, 0) == PUGL_SUCCESS);
                assert(wl::puglHide(view) == PUGL_SUCCESS);
                assert(wl::puglShow(view, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS);
                assert(view->impl->configured);
                assert(DGL_NAMESPACE::puglUpdate(world, 0.0) == PUGL_SUCCESS);
                assert(wl::puglGetPositionHint(view, PUGL_CURRENT_POSITION).x == 0);
                assert(wl::puglGetPositionHint(view, PUGL_CURRENT_POSITION).y == 0);
            }
            DGL_NAMESPACE::puglFreeView(view);
            assert(wl_surface_get_user_data(parent) == &hostData);
            assert(wl_proxy_get_tag(reinterpret_cast<wl_proxy*>(parent)) == hostTag);
        }
        DGL_NAMESPACE::puglFreeWorld(world);
        assert(wl_display_roundtrip(display) >= 0);

        uint32_t parentId = 0;
        unsigned created = 0, destroyed = 0, desynchronized = 0;
        unsigned initialPositions = 0, movedPositions = 0, zeroPositions = 0;
        const auto requests = server.requests();
        for (const auto& request : requests)
        {
            if (request.type == "wl_compositor" && request.opcode == 0 && !parentId)
                parentId = request.words[0];
            if (request.type == "wl_subcompositor" && request.opcode == 1)
            {
                ++created;
                assert(request.words[1] != parentId && request.words[2] == parentId);
            }
            if (request.type == "wl_subsurface" && request.opcode == 0) ++destroyed;
            if (request.type == "wl_subsurface" && request.opcode == 5) ++desynchronized;
            if (request.type == "wl_subsurface" && request.opcode == 1)
            {
                if (request.words[0] == uint32_t(-7) && request.words[1] == 11U) ++initialPositions;
                else if (request.words[0] == uint32_t(-17) && request.words[1] == 9U) ++movedPositions;
                else if (request.words[0] == 0U && request.words[1] == 0U) ++zeroPositions;
                else assert(false);
            }
            assert(!(request.type == "xdg_wm_base" && request.opcode == 2));
            assert(!(request.type == "wl_surface" && request.object == parentId &&
                     (request.opcode == 0 || request.opcode == 6)));
        }
        assert(parentId && created == (supported ? 3U : 0U));
        assert(destroyed == created && desynchronized == created);
        assert(initialPositions == created);
        assert(movedPositions == (supported ? 6U : 0U));
        assert(zeroPositions == (supported ? 2U : 0U));
        wl_surface_destroy(parent);
        wl_compositor_destroy(compositor);
        wl_registry_destroy(registry);
        assert(wl_display_roundtrip(display) >= 0);
        wl_display_disconnect(display);
    }
}

} // namespace

int main()
{
    using namespace DGL_NAMESPACE;
#ifdef DGL_RUNTIME_BACKEND
    // A DGL_BACKEND=both build compiles the Wayland backend into a namespace of its own, see
    // pugl-dispatch.hpp. Its view type is the one the internals below work with.
    using namespace DGL_NAMESPACE::pugl_wl;
    typedef DGL_NAMESPACE::pugl_wl::PuglView PuglView;
#endif

    // Redraw requests are intersected with the view, including negative and overflowing origins.
    const PuglArea viewSize = {100U, 80U};
    PuglExposeEvent expose = {};

    assert(puglWaylandClipExpose(&expose, viewSize, 90, 70, 20U, 20U));
    assert(expose.x == 90);
    assert(expose.y == 70);
    assert(expose.width == 10U);
    assert(expose.height == 10U);

    assert(puglWaylandClipExpose(&expose, viewSize, -10, -20, 20U, 30U));
    assert(expose.x == 0);
    assert(expose.y == 0);
    assert(expose.width == 10U);
    assert(expose.height == 10U);

    assert(!puglWaylandClipExpose(&expose, viewSize, 100, 0, 1U, 1U));
    assert(!puglWaylandClipExpose(&expose, viewSize, 0, 80, 1U, 1U));

    // A clipboard transfer is successful only after EOF and preserves more specific errors.
    PuglWaylandPipeRecv recv = {};
    assert(puglWaylandReceiveStatus(&recv, PUGL_SUCCESS) == PUGL_FAILURE);
    assert(puglWaylandReceiveStatus(&recv, PUGL_UNKNOWN_ERROR) == PUGL_UNKNOWN_ERROR);

    recv.done = true;
    assert(puglWaylandReceiveStatus(&recv, PUGL_SUCCESS) == PUGL_SUCCESS);
    assert(puglWaylandReceiveStatus(&recv, PUGL_UNKNOWN_ERROR) == PUGL_SUCCESS);

    recv.status = PUGL_NO_MEMORY;
    assert(puglWaylandReceiveStatus(&recv, PUGL_SUCCESS) == PUGL_NO_MEMORY);

    // A payload exactly at the size ceiling succeeds on EOF, while an extra byte is rejected.
    int exactPipe[2] = {};
    assert(pipe(exactPipe) == 0);
    close(exactPipe[1]);

    PuglWaylandPipeRecv exact = {};
    exact.fd = exactPipe[0];
    exact.len = PUGL_WAYLAND_CLIPBOARD_MAX_SIZE;
    exact.capacity = PUGL_WAYLAND_CLIPBOARD_MAX_SIZE;
    puglWaylandRecvRead(&exact);
    assert(exact.done);
    assert(exact.status == PUGL_SUCCESS);
    close(exactPipe[0]);

    int oversizedPipe[2] = {};
    assert(pipe(oversizedPipe) == 0);
    assert(write(oversizedPipe[1], "x", 1U) == 1);

    PuglWaylandPipeRecv oversized = {};
    oversized.fd = oversizedPipe[0];
    oversized.len = PUGL_WAYLAND_CLIPBOARD_MAX_SIZE;
    oversized.capacity = PUGL_WAYLAND_CLIPBOARD_MAX_SIZE;
    puglWaylandRecvRead(&oversized);
    assert(oversized.done);
    assert(oversized.status == PUGL_FAILURE);
    close(oversizedPipe[0]);
    close(oversizedPipe[1]);

    // Output scale changes are latched until wl_output.done completes the atomic event batch.
    PuglWaylandOutput output = {};
    output.scale = 1;
    puglWaylandOutputScale(&output, nullptr, 2);
    assert(output.scale == 2);
    assert(output.scaleChanged);
    puglWaylandOutputDone(&output, nullptr);
    assert(!output.scaleChanged);

    // Removing a view closes all of its timers while preserving timers owned by other views.
    int viewTokenA = 0;
    int viewTokenB = 0;
    PuglView* const viewA = reinterpret_cast<PuglView*>(&viewTokenA);
    PuglView* const viewB = reinterpret_cast<PuglView*>(&viewTokenB);

    int pipes[3][2] = {};
    for (size_t i = 0; i < 3U; ++i) {
        assert(pipe(pipes[i]) == 0);
        close(pipes[i][1]);
    }

    PuglWorldInternals wimpl = {};
    wimpl.numTimers = 3U;
    wimpl.timers = static_cast<PuglWaylandTimer*>(
        std::calloc(wimpl.numTimers, sizeof(PuglWaylandTimer)));
    assert(wimpl.timers);

    wimpl.timers[0] = {viewA, 1U, pipes[0][0]};
    wimpl.timers[1] = {viewB, 2U, pipes[1][0]};
    wimpl.timers[2] = {viewA, 3U, pipes[2][0]};

    puglWaylandRemoveViewTimers(&wimpl, viewA);
    assert(wimpl.numTimers == 1U);
    assert(wimpl.timers[0].view == viewB);
    assert(wimpl.timers[0].fd == pipes[1][0]);

    errno = 0;
    assert(fcntl(pipes[0][0], F_GETFD) == -1);
    assert(errno == EBADF);
    errno = 0;
    assert(fcntl(pipes[2][0], F_GETFD) == -1);
    assert(errno == EBADF);

    close(wimpl.timers[0].fd);
    std::free(wimpl.timers);

#ifdef DGL_RUNTIME_BACKEND
    if (const char* const failure = dglWaylandLoad())
    {
        const char* const expected = std::getenv("DAF_TEST_EXPECT_BACKEND");
        if (expected && std::strcmp(expected, "x11") == 0)
        {
            d_stdout("Wayland: connection checks skipped: %s", failure);
            return 0;
        }
        d_stderr("Wayland: %s", failure);
        return 1;
    }
#endif
    assert(puglNewWorldWithWaylandDisplay(PUGL_MODULE, 0, nullptr) == nullptr);
    assert(!puglGetWaylandInput(nullptr).data);
    alarm(30); // Bound a broken protocol or an accidental blocking borrowed-world update.
    testActivationProtocol();
    testBorrowedConnection();
    testConnectionOwnership();
    testIndependentSeat();
    testHostSeat();
    testHostFocus();
    testHostPointer();
    testForeignSurface();
    testSubsurfaceLifetime();
    alarm(0);

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
