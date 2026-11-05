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
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dgl/src/pugl.cpp"

#ifndef HAVE_WAYLAND
# error This test requires the native Wayland backend
#endif

// --------------------------------------------------------------------------------------------------------------------

namespace {

using namespace DGL_NAMESPACE;
#ifdef DGL_RUNTIME_BACKEND
namespace wl = DGL_NAMESPACE::pugl_wl;
#else
namespace wl = DGL_NAMESPACE;
#endif

// Only registry discovery and sync replies are needed; no compositor or display server is used.
class ConnectionServer
{
public:
    explicit ConnectionServer(const bool advertiseCompositor = true)
        : advertiseCompositor(advertiseCompositor)
    {
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
        serverFd = fds[0];
        clientFd = fds[1];
        objects[1] = "wl_display";
        thread = std::thread(&ConnectionServer::serve, this);
    }

    ~ConnectionServer()
    {
        shutdown(serverFd, SHUT_RDWR);
        thread.join();
        close(serverFd);
        if (clientFd >= 0)
            close(clientFd);
    }

    wl_display* connect()
    {
        const char* const previous = std::getenv("WAYLAND_SOCKET");
        const bool hadPrevious = previous != nullptr;
        const std::string saved = previous ? previous : "";
        assert(setenv("WAYLAND_SOCKET", std::to_string(clientFd).c_str(), 1) == 0);
        wl_display* const display = wl_display_connect(nullptr);
        clientFd = -1; // wl_display_connect consumes the socket, even on failure
        if (hadPrevious)
            assert(setenv("WAYLAND_SOCKET", saved.c_str(), 1) == 0);
        else
            assert(unsetenv("WAYLAND_SOCKET") == 0);
        assert(display);
        return display;
    }

private:
    void post(const uint32_t object, const uint16_t opcode, std::vector<uint32_t> payload)
    {
        payload.insert(payload.begin(), (uint32_t(payload.size() + 2U) * 4U << 16U) | opcode);
        payload.insert(payload.begin(), object);
        const char* data = reinterpret_cast<const char*>(payload.data());
        size_t remaining = payload.size() * sizeof(uint32_t);
        while (remaining)
        {
            const ssize_t sent = send(serverFd, data, remaining, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent <= 0)
                return;
            remaining -= size_t(sent);
            data += sent;
        }
    }

    void global(const uint32_t registry, const uint32_t name, const char* const interface,
                const uint32_t version)
    {
        const size_t length = std::strlen(interface) + 1U;
        std::vector<uint32_t> words(2U + (length + 3U) / 4U, 0U);
        words[0] = name;
        words[1] = uint32_t(length);
        std::memcpy(words.data() + 2, interface, length);
        words.push_back(version);
        post(registry, 0, words);
    }

    bool read(void* const buffer, const size_t size)
    {
        char* data = static_cast<char*>(buffer);
        size_t remaining = size;
        while (remaining)
        {
            const ssize_t received = recv(serverFd, data, remaining, 0);
            if (received < 0 && errno == EINTR)
                continue;
            if (received <= 0)
                return false;
            remaining -= size_t(received);
            data += received;
        }
        return true;
    }

    void serve()
    {
        uint32_t header[2];
        while (read(header, sizeof(header)))
        {
            const uint32_t size = header[1] >> 16U;
            assert(size >= 8U && size % 4U == 0U);
            std::vector<uint32_t> words((size - 8U) / 4U);
            if (! read(words.data(), size - 8U))
                return;
            const uint16_t opcode = uint16_t(header[1]);
            const std::string type = objects[header[0]];
            if (type == "wl_display" && opcode == 0)
            {
                post(words[0], 0, {1U});
                post(1, 1, {words[0]});
            }
            else if (type == "wl_display" && opcode == 1)
            {
                objects[words[0]] = "wl_registry";
                if (advertiseCompositor)
                    global(words[0], 1, "wl_compositor", 4);
                global(words[0], 2, "xdg_wm_base", 1);
                global(words[0], 3, "wl_output", 2);
            }
            else if (type == "wl_registry" && opcode == 0)
            {
                objects[words.back()] = reinterpret_cast<const char*>(words.data() + 2);
                if (objects[words.back()] == "wl_output")
                {
                    post(words.back(), 3, {2U});
                    post(words.back(), 2, {});
                }
            }
        }
    }

    const bool advertiseCompositor;
    int serverFd;
    int clientFd;
    std::thread thread;
    std::map<uint32_t, std::string> objects;
};

static void hostGlobal(void* data, wl_registry* registry, uint32_t name,
                       const char* interface, uint32_t)
{
    if (std::strcmp(interface, "wl_compositor") == 0)
        *static_cast<wl_compositor**>(data) = static_cast<wl_compositor*>(
            wl_registry_bind(registry, name, &wl_compositor_interface, 4));
}

static void hostGlobalRemove(void*, wl_registry*, uint32_t) {}

static void syncDone(void* const data, wl_callback* const callback, uint32_t)
{
    ++*static_cast<unsigned*>(data);
    wl_callback_destroy(callback);
}

static void queueSync(wl_display* const display, wl_event_queue* const queue,
                      const wl_callback_listener* const listener, void* const data)
{
    wl_callback* callback;
    if (queue)
    {
        wl_display* const wrapper = static_cast<wl_display*>(wl_proxy_create_wrapper(display));
        assert(wrapper);
        wl_proxy_set_queue(reinterpret_cast<wl_proxy*>(wrapper), queue);
        callback = wl_display_sync(wrapper);
        wl_proxy_wrapper_destroy(wrapper);
    }
    else
        callback = wl_display_sync(display);
    assert(callback);
    assert(wl_callback_add_listener(callback, listener, data) == 0);
}

// Read all preceding replies without dispatching either the host queue or DAF's queue.
static void readReplies(wl_display* const display)
{
    wl_event_queue* const fence = wl_display_create_queue(display);
    assert(fence);
    assert(wl_display_roundtrip_queue(display, fence) >= 0);
    wl_event_queue_destroy(fence);
}

static void sendPaste(void* const data, wl_callback* const callback, uint32_t)
{
    const int fd = *static_cast<int*>(data);
    assert(write(fd, "paste", 5) == 5);
    close(fd);
    wl_callback_destroy(callback);
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

    for (unsigned cycle = 0; cycle < 3; ++cycle)
    {
        unsigned hostCalls = 0;
        queueSync(display, nullptr, &syncListener, &hostCalls);
        wl::PuglWorld world = {};
        world.impl = wl::puglWaylandInitWorldInternals(display, false, PUGL_PROGRAM);
        assert(world.impl && !world.impl->ownsDisplay);
        assert(world.impl->launcherToken == nullptr);
        assert(std::getenv("XDG_ACTIVATION_TOKEN"));
        assert(std::strcmp(std::getenv("XDG_ACTIVATION_TOKEN"), "host-launch-token") == 0);
        assert(hostCalls == 0);
        assert(world.impl->numOutputs == 1U && world.impl->outputs[0].scale == 2);

        unsigned privateCalls = 0;
        queueSync(display, nullptr, &syncListener, &hostCalls);
        queueSync(display, world.impl->queue, &syncListener, &privateCalls);
        readReplies(display);
        assert(privateCalls == 0 && hostCalls == 0);
        assert(wl::puglUpdate(&world, 60.0) == PUGL_SUCCESS);
        assert(privateCalls == 1 && hostCalls == 0);
        assert(wl::puglUpdate(&world, -1.0) == PUGL_SUCCESS);
        assert(wl_display_dispatch_pending(display) >= 0);
        assert(hostCalls == 2);

        // A synchronous receive must service its own sender without dispatching the host callback.
        int pipeFds[2];
        assert(pipe(pipeFds) == 0);
        const wl_callback_listener pasteListener = {sendPaste};
        queueSync(display, nullptr, &syncListener, &hostCalls);
        queueSync(display, world.impl->queue, &pasteListener, &pipeFds[1]);
        wl::PuglBlob received = {};
        assert(wl::puglWaylandReadPipe(world.impl, pipeFds[0], &received) == PUGL_SUCCESS);
        assert(received.len == 5U && std::memcmp(received.data, "paste", 5U) == 0);
        std::free(received.data);
        close(pipeFds[0]);
        assert(hostCalls == 2);

        wl::puglFreeWorldInternals(&world);
        assert(world.impl == nullptr);
        assert(fcntl(fd, F_GETFD) >= 0);
        assert(hostCalls == 2);
        assert(wl_display_roundtrip(display) >= 0);
        assert(hostCalls == 3);
    }

    if (hadPrevious)
        assert(setenv("XDG_ACTIVATION_TOKEN", saved.c_str(), 1) == 0);
    else
        assert(unsetenv("XDG_ACTIVATION_TOKEN") == 0);
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
                wl::puglWaylandInitWorldInternals(display, owned != 0, PUGL_MODULE);
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
                wl_display_disconnect(display);
            }
        }
    }
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

    wl::PuglWorld world = {};
#ifdef DGL_RUNTIME_BACKEND
    world.system = kPuglWindowSystemWayland;
#endif
    world.impl = wl::puglWaylandInitWorldInternals(display, false, PUGL_MODULE);
    assert(world.impl);
    wl::PuglView* const view = wl::puglNewView(&world);
    assert(view);
#ifdef DGL_RUNTIME_BACKEND
    view->system = kPuglWindowSystemWayland;
#endif
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
        puglWaylandPointerEnter(world.impl, nullptr, 1U, surface, 0, 0);
        puglWaylandKeyboardEnter(world.impl, nullptr, 2U, surface, nullptr);
        assert(world.impl->pointerFocus == nullptr && world.impl->keyboardFocus == nullptr);
        puglWaylandPointerLeave(world.impl, nullptr, 3U, surface);
        puglWaylandKeyboardLeave(world.impl, nullptr, 4U, surface);
        assert(wl_surface_get_user_data(surface) == foreign);
    }

    wl_output* const output = static_cast<wl_output*>(
        wl_registry_bind(registry, 3U, &wl_output_interface, 2));
    assert(output);
    wl_output_set_user_data(output, &foreignData);
    puglWaylandSurfaceEnter(view, view->impl->wlSurface, output);
    assert(view->impl->bufferScale == 1);
    puglWaylandSurfaceEnter(view, view->impl->wlSurface, world.impl->outputs[0].output);
    assert(view->impl->bufferScale == 2);
    assert(wl_output_get_user_data(output) == &foreignData);
    wl_output_destroy(output);
    wl::puglFreeView(view);
    std::free(world.views);
    wl::puglFreeWorldInternals(&world);

    wl_surface_destroy(surface);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_disconnect(display);
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
    alarm(30); // Bound a broken protocol or an accidental blocking borrowed-world update.
    testBorrowedConnection();
    testConnectionOwnership();
    testForeignSurface();
    alarm(0);

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
