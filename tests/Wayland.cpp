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
#include <fcntl.h>
#include <unistd.h>

#include "dgl/src/pugl.cpp"

#ifndef HAVE_WAYLAND
# error This test requires the native Wayland backend
#endif

// --------------------------------------------------------------------------------------------------------------------

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

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
