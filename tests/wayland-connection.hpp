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

#pragma once

#include <cassert>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace dafTestWayland {

// Socket-pair protocol fixture; the caller must load the Wayland runtime before connecting.
class ConnectionServer
{
public:
    struct Request {
        uint32_t object;
        std::string type;
        uint16_t opcode;
        std::vector<uint32_t> words;
    };

    explicit ConnectionServer(const bool advertiseCompositor = true, const bool advertiseSubcompositor = true,
                              const bool advertiseSeat = false)
        : advertiseCompositor(advertiseCompositor), advertiseSubcompositor(advertiseSubcompositor),
          advertiseSeat(advertiseSeat)
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

    std::vector<Request> requests() const
    {
        const std::lock_guard<std::mutex> lock(requestMutex);
        return recordedRequests;
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
            {
                const std::lock_guard<std::mutex> lock(requestMutex);
                recordedRequests.push_back({header[0], type, opcode, words});
            }
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
                if (advertiseSubcompositor)
                    global(words[0], 4, "wl_subcompositor", 1);
                if (advertiseSeat)
                    global(words[0], 5, "wl_seat", 5);
            }
            else if (type == "wl_registry" && opcode == 0)
            {
                objects[words.back()] = reinterpret_cast<const char*>(words.data() + 2);
                if (objects[words.back()] == "wl_output")
                {
                    post(words.back(), 3, {2U});
                    post(words.back(), 2, {});
                }
                else if (objects[words.back()] == "wl_seat")
                    post(words.back(), 0, {WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD});
            }
            else if (type == "wl_seat" && opcode == 0)
                objects[words[0]] = "wl_pointer";
            else if (type == "wl_seat" && opcode == 1)
                objects[words[0]] = "wl_keyboard";
            else if (type == "wl_compositor" && opcode == 0)
                objects[words[0]] = "wl_surface";
            else if (type == "wl_subcompositor" && opcode == 1)
                objects[words[0]] = "wl_subsurface";
            else if (type == "xdg_wm_base" && opcode == 2)
                objects[words[0]] = "xdg_surface";
            else if (type == "xdg_surface" && opcode == 1)
                objects[words[0]] = "xdg_toplevel";
        }
    }

    const bool advertiseCompositor;
    const bool advertiseSubcompositor;
    const bool advertiseSeat;
    int serverFd;
    int clientFd;
    std::thread thread;
    std::map<uint32_t, std::string> objects;
    mutable std::mutex requestMutex;
    std::vector<Request> recordedRequests;
};

inline void syncDone(void* const data, wl_callback* const callback, uint32_t)
{
    ++*static_cast<unsigned*>(data);
    wl_callback_destroy(callback);
}

inline void queueSync(wl_display* const display, wl_event_queue* const queue,
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
inline void readReplies(wl_display* const display)
{
    wl_event_queue* const fence = wl_display_create_queue(display);
    assert(fence);
    assert(wl_display_roundtrip_queue(display, fence) >= 0);
    wl_event_queue_destroy(fence);
}

} // namespace dafTestWayland
