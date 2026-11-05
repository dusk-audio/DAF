/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2024 Filipe Coelho <falktx@falktx.com>
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

#include "tests.hpp"
#include <functional>
#include <memory>

#define DAF_TEST_APPLICATION_CPP
#include "dgl/src/pugl.cpp"
#include "dgl/src/Application.cpp"
#include "dgl/src/ApplicationPrivateData.cpp"

#ifdef DGL_USING_WAYLAND
# include "wayland-connection.hpp"
# include <fcntl.h>
#endif

START_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------

struct IdleCallbackCounter : IdleCallback
{
    int counter;

    IdleCallbackCounter()
        : counter(0) {}

    void idleCallback() override
    {
        ++counter;
    }
};

struct MutatingIdleCallback : IdleCallback
{
    std::function<void()> action;
    void idleCallback() override { action(); }
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL

int main()
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::ApplicationQuitter;
    using DGL_NAMESPACE::IdleCallbackCounter;

    {
        Application app(Application::ExternalWaylandDisplay{nullptr});
        DAF_ASSERT_EQUAL(app.getWindowSystem(), Application::kWindowSystemAuto,
                         "a null borrowed display must not fall back to another connection");
        DAF_ASSERT_EQUAL(app.isStandalone(), false, "a borrowed display belongs to a module");
        DAF_ASSERT_EQUAL(app.getClassName(), nullptr, "a failed application has no class name");
        DAF_ASSERT_EQUAL(app.getWaylandInput().data, nullptr, "a failed application exposes no input callbacks");
        DAF_ASSERT_EQUAL(app.activateWaylandSurface(&app, "unused-token"), false,
                         "failed initialization cannot activate a foreign surface");
        app.idle();
        app.setClassName("unavailable");
        app.quit();
    }

#ifdef DGL_USING_WAYLAND
    bool canBorrow = true;
# ifdef DGL_RUNTIME_BACKEND
    if (const char* const failure = dglWaylandLoad())
    {
        const char* const expected = std::getenv("DAF_TEST_EXPECT_BACKEND");
        DAF_ASSERT_NOT_EQUAL(expected, nullptr, "missing Wayland libraries must be an explicit test case");
        DAF_ASSERT_EQUAL(std::strcmp(expected, "x11"), 0, "only the X11-only runtime permits missing Wayland libraries");
        d_stdout("Application: borrowed Wayland checks skipped: %s", failure);
        canBorrow = false;
    }
# endif
    if (canBorrow)
    {
        alarm(30);
        dafTestWayland::ConnectionServer server;
        wl_display* const display = server.connect();
        const int fd = wl_display_get_fd(display);
        const wl_callback_listener listener = {dafTestWayland::syncDone};
        for (unsigned cycle = 0; cycle < 2; ++cycle)
        {
            unsigned hostCalls = 0;
            dafTestWayland::queueSync(display, nullptr, &listener, &hostCalls);
            {
                Application app(Application::ExternalWaylandDisplay{display});
                DAF_ASSERT_EQUAL(app.getWaylandInput().data, nullptr, "display-only borrowing retains its own input stream");
                DAF_ASSERT_EQUAL(app.getWindowSystem(), Application::kWindowSystemWayland,
                                 "a borrowed application must use Wayland");
                DAF_ASSERT_EQUAL(app.isStandalone(), false, "the host owns the event loop");
                DAF_ASSERT_NOT_EQUAL(app.getClassName(), nullptr, "the borrowed world has a class name");
                app.setClassName("BorrowedDAF");
                DAF_ASSERT_EQUAL(std::strcmp(app.getClassName(), "BorrowedDAF"), 0,
                                 "class names still use the selected world");
                DAF_ASSERT_EQUAL(hostCalls, 0u, "construction must not dispatch the host queue");
                IdleCallbackCounter idleCounter;
                app.addIdleCallback(&idleCounter);
                app.idle();
                DAF_ASSERT_EQUAL(idleCounter.counter, 1, "borrowed applications still run idle callbacks");
                app.removeIdleCallback(&idleCounter);
                DAF_ASSERT_EQUAL(hostCalls, 0u, "idle must not dispatch the host queue");
                assert(wl_display_dispatch_pending(display) >= 0);
                DAF_ASSERT_EQUAL(hostCalls, 1u, "initialization must have read the supplied connection");
                dafTestWayland::queueSync(display, nullptr, &listener, &hostCalls);
                dafTestWayland::readReplies(display);
            }
            DAF_ASSERT_EQUAL(hostCalls, 1u, "teardown must not dispatch the host queue");
            assert(fcntl(fd, F_GETFD) >= 0);
            assert(wl_display_roundtrip(display) >= 0);
            DAF_ASSERT_EQUAL(hostCalls, 2u, "the host display remains usable after application teardown");
        }
        wl_display_disconnect(display);
        {
            dafTestWayland::ConnectionServer incompleteServer(false);
            wl_display* const incompleteDisplay = incompleteServer.connect();
            const int incompleteFd = wl_display_get_fd(incompleteDisplay);
            {
                Application app(Application::ExternalWaylandDisplay{incompleteDisplay});
                DAF_ASSERT_EQUAL(app.getWindowSystem(), Application::kWindowSystemAuto,
                                 "missing Wayland globals must not select another connection");
                app.idle();
            }
            assert(fcntl(incompleteFd, F_GETFD) >= 0);
            assert(wl_display_roundtrip(incompleteDisplay) >= 0);
            wl_display_disconnect(incompleteDisplay);
        }
        {
            dafTestWayland::ConnectionServer inputServer(true, true, true);
            wl_display* const hostDisplay = inputServer.connect();
            wl_registry* const registry = wl_display_get_registry(hostDisplay);
            const wl_registry_listener registryListener = {
                [](void*, wl_registry*, uint32_t, const char*, uint32_t) {},
                [](void*, wl_registry*, uint32_t) {}
            };
            assert(wl_registry_add_listener(registry, &registryListener, nullptr) == 0);
            assert(wl_display_roundtrip(hostDisplay) >= 0);
            wl_seat* const seat = static_cast<wl_seat*>(wl_registry_bind(registry, 5, &wl_seat_interface, 5));
            const wl_seat_listener seatListener = {
                [](void*, wl_seat*, uint32_t) {}, [](void*, wl_seat*, const char*) {}
            };
            assert(wl_seat_add_listener(seat, &seatListener, nullptr) == 0);
            {
                Application app(Application::ExternalWaylandDisplay{hostDisplay, seat});
                DAF_ASSERT_EQUAL(app.getWindowSystem(), Application::kWindowSystemWayland,
                                 "a host-managed input application uses Wayland");
                const auto input = app.getWaylandInput();
                DAF_ASSERT_NOT_EQUAL(input.data, nullptr, "the public application exposes its input adapter");
                assert(input.keyboard && input.pointer && input.consumed && input.hasKeyboardFocus &&
                       input.releaseFocus && input.reset);
                DAF_ASSERT_EQUAL(app.getWaylandInput().data, input.data, "the adapter remains bound to this application");
                DAF_ASSERT_EQUAL(input.hasKeyboardFocus(input.data), false, "a new adapter has no child focus");
                input.keyboard->key(input.data, nullptr, 1, 1, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
                DAF_ASSERT_EQUAL(input.consumed(input.data), false, "unowned releases remain with the host");
                input.releaseFocus(input.data);
                input.reset(input.data);
                app.idle();
            }
            assert(wl_display_roundtrip(hostDisplay) >= 0);
            unsigned seatBindings = 0;
            for (const auto& request : inputServer.requests())
            {
                if (request.type == "wl_registry" && request.opcode == 0 && request.words[0] == 5U) ++seatBindings;
                DAF_ASSERT_NOT_EQUAL(request.type, std::string("wl_seat"),
                                     "DAF must neither create input streams nor release the host seat");
            }
            DAF_ASSERT_EQUAL(seatBindings, 1u, "only the host binds the input seat");
            wl_seat_release(seat);
            wl_registry_destroy(registry);
            assert(wl_display_roundtrip(hostDisplay) >= 0);
            wl_display_disconnect(hostDisplay);
        }
        for (const bool advertised : {false, true})
        {
            dafTestWayland::ConnectionServer activationServer(true, true, false, advertised);
            wl_display* const hostDisplay = activationServer.connect();
            wl_registry* const registry = wl_display_get_registry(hostDisplay);
            const wl_registry_listener registryListener = {
                [](void*, wl_registry*, uint32_t, const char*, uint32_t) {},
                [](void*, wl_registry*, uint32_t) {}
            };
            assert(wl_registry_add_listener(registry, &registryListener, nullptr) == 0);
            assert(wl_display_roundtrip(hostDisplay) >= 0);
            wl_compositor* const compositor = static_cast<wl_compositor*>(
                wl_registry_bind(registry, 1, &wl_compositor_interface, 4));
            wl_surface* const parent = wl_compositor_create_surface(compositor);
            assert(wl_display_roundtrip(hostDisplay) >= 0);
            uint32_t parentId = 0;
            for (const auto& request : activationServer.requests())
                if (request.type == "wl_compositor" && request.opcode == 0)
                    parentId = request.words[0];
            DAF_ASSERT_NOT_EQUAL(parentId, 0u, "the host creates the activation target");
            {
                Application app(Application::ExternalWaylandDisplay{hostDisplay});
                DAF_ASSERT_EQUAL(app.activateWaylandSurface(nullptr, "unused-token"), false,
                                 "activation requires a parent surface");
                DAF_ASSERT_EQUAL(app.activateWaylandSurface(parent, nullptr), false,
                                 "activation requires a token");
                DAF_ASSERT_EQUAL(app.activateWaylandSurface(parent, ""), false,
                                 "an empty token cannot activate a surface");
                unsigned hostCalls = 0;
                dafTestWayland::queueSync(hostDisplay, nullptr, &listener, &hostCalls);
                dafTestWayland::readReplies(hostDisplay);
                DAF_ASSERT_EQUAL(app.activateWaylandSurface(parent, "parent-launch-token"), advertised,
                                 "foreign activation depends on compositor support");
                DAF_ASSERT_EQUAL(hostCalls, 0u, "activation never dispatches the host queue");
                assert(wl_display_roundtrip(hostDisplay) >= 0);
                DAF_ASSERT_EQUAL(hostCalls, 1u, "the host retains its queued callbacks");
                unsigned activations = 0;
                for (const auto& request : activationServer.requests())
                {
                    DAF_ASSERT_NOT_EQUAL(request.type, std::string("xdg_surface"),
                                         "activation never creates a new toplevel");
                    if (request.type != "xdg_activation_v1") continue;
                    DAF_ASSERT_EQUAL(request.opcode, uint16_t(2), "only explicit token submission is requested");
                    DAF_ASSERT_EQUAL(request.words[0], 20u, "the supplied token length is preserved");
                    DAF_ASSERT_EQUAL(std::strcmp(reinterpret_cast<const char*>(request.words.data() + 1),
                                                 "parent-launch-token"), 0, "the supplied token reaches the wire");
                    DAF_ASSERT_EQUAL(request.words.back(), parentId, "activation targets the host's original surface");
                    ++activations;
                }
                DAF_ASSERT_EQUAL(activations, unsigned(advertised), "one supported request produces one activation");
            }
            wl_surface_destroy(parent);
            wl_compositor_destroy(compositor);
            wl_registry_destroy(registry);
            assert(wl_display_roundtrip(hostDisplay) >= 0);
            wl_display_disconnect(hostDisplay);
        }
        alarm(0);
    }
#else
    {
        char sentinel = 0;
        Application app(Application::ExternalWaylandDisplay{&sentinel});
        DAF_ASSERT_EQUAL(app.getWindowSystem(), Application::kWindowSystemAuto,
                         "an unavailable backend must reject a borrowed handle without dereferencing it");
        app.idle();
        app.quit();
    }
#endif

    // regular usage
    {
        Application app(true);
        DAF_ASSERT_EQUAL(app.getWaylandInput().data, nullptr, "standalone input belongs to its own event loop");
        if (app.getWindowSystem() != Application::kWindowSystemWayland)
            DAF_ASSERT_EQUAL(app.activateWaylandSurface(&app, "unused-token"), false,
                             "another backend never interprets the supplied native handle");
        IdleCallbackCounter idleCounter;
        app.addIdleCallback(&idleCounter);
        DAF_ASSERT_EQUAL(app.isQuitting(), false, "app MUST NOT be set as quitting during init");
        DAF_ASSERT_EQUAL(idleCounter.counter, 0, "app MUST NOT have triggered idle callbacks yet");
        app.idle();
        DAF_ASSERT_EQUAL(app.isQuitting(), false, "app MUST NOT be set as quitting after idle()");
        DAF_ASSERT_EQUAL(idleCounter.counter, 1, "app MUST have triggered 1 idle callback");
        app.idle();
        DAF_ASSERT_EQUAL(idleCounter.counter, 2, "app MUST have triggered 2 idle callbacks");
        app.quit();
        DAF_ASSERT_EQUAL(app.isQuitting(), true, "app MUST be set as quitting after quit()");
        DAF_ASSERT_EQUAL(idleCounter.counter, 2, "app MUST have triggered only 2 idle callbacks in its lifetime");
    }

    // A UI can unregister and delete its Window while handling an idle call.
    // Exercise the public Application path, not only the registry in isolation.
    {
        Application app(false);
        std::unique_ptr<DGL_NAMESPACE::MutatingIdleCallback> self(new DGL_NAMESPACE::MutatingIdleCallback);
        IdleCallbackCounter survivor;
        unsigned calls = 0;
        self->action = [&] { ++calls; app.removeIdleCallback(self.get()); self.reset(); };
        app.addIdleCallback(self.get()); app.addIdleCallback(&survivor);
        app.idle(); app.idle();
        DAF_ASSERT_EQUAL(calls, 1u, "a registered callback can unregister and delete itself");
        DAF_ASSERT_EQUAL(survivor.counter, 2, "self-deletion preserves later public callbacks");
        app.removeIdleCallback(&survivor);
    }

    // Embedded UIs can also destroy their Application from a native callback.
    {
        std::unique_ptr<Application> app(new Application(false));
        DGL_NAMESPACE::MutatingIdleCallback closer;
        IdleCallbackCounter survivor;
        closer.action = [&] { app.reset(); };
        app->addIdleCallback(&closer); app->addIdleCallback(&survivor);
        app->idle();
        DAF_ASSERT_EQUAL(app.get(), nullptr, "the Application really was destroyed during idle");
        DAF_ASSERT_EQUAL(survivor.counter, 0, "destroying the Application cancels pending callbacks");
    }

    // standalone exec, must not block forever due to quit() called from another thread
    {
        Application app(true);
        ApplicationQuitter appQuitter(app);
        IdleCallbackCounter idleCounter;
        app.addIdleCallback(&idleCounter);
        app.exec();
        DAF_ASSERT_EQUAL(appQuitter.isThreadRunning(), false, "app quit triggered because we told it so");
        DAF_ASSERT_NOT_EQUAL(idleCounter.counter, 0, "app idle callbacks MUST have been triggered");
    }

    // standalone exec, but with 0 as timeout
    {
        Application app(true);
        ApplicationQuitter appQuitter(app);
        IdleCallbackCounter idleCounter;
        app.addIdleCallback(&idleCounter);
        app.exec(0);
        DAF_ASSERT_EQUAL(appQuitter.isThreadRunning(), false, "app quit triggered because we told it so");
        DAF_ASSERT_NOT_EQUAL(idleCounter.counter, 0, "app idle callbacks MUST have been triggered");
    }

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
