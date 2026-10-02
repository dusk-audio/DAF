// Copyright 2012-2023 David Robillard <d@drobilla.net>
// Copyright 2025 Dusk Audio Framework contributors
// SPDX-License-Identifier: ISC

/*
  Cairo backend for the Wayland platform, over wl_shm.

  There is no "Cairo surface for a Wayland window" the way there is for X11, so this draws into a
  plain image surface backed by shared memory and hands the compositor the resulting wl_buffer.

  Two buffers are used and alternated, because a buffer that has been attached stays owned by the
  compositor until it sends wl_buffer.release.  Since the two buffers hold different frames, a
  partial expose paints the previous frame over the new buffer first, so that the parts the
  application does not redraw are still correct.
*/

#include "../pugl-upstream/src/macros.h"
#include "../pugl-upstream/src/types.h"
#include "wayland.h"

#include "pugl/cairo.h"
#include "pugl/pugl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PUGL_WAYLAND_CAIRO_NUM_BUFFERS 2

typedef struct {
  int                 fd;
  size_t              poolSize;
  uint8_t*            data;
  struct wl_shm_pool* pool;
  struct wl_buffer*   buffers[PUGL_WAYLAND_CAIRO_NUM_BUFFERS];
  cairo_surface_t*    images[PUGL_WAYLAND_CAIRO_NUM_BUFFERS];
  bool                busy[PUGL_WAYLAND_CAIRO_NUM_BUFFERS];
  cairo_t*            cr;
  PuglArea            size;
  int                 stride;
  int                 index;
  int                 lastIndex;
  bool                entered; ///< Whether the matching enter() got as far as succeeding
} PuglWaylandCairoSurface;

static void
puglWaylandCairoBufferRelease(void* const             data,
                              struct wl_buffer* const PUGL_UNUSED(buffer))
{
  *(bool*)data = false;
}

static const struct wl_buffer_listener puglWaylandCairoBufferListener = {
  puglWaylandCairoBufferRelease};

/// Create an anonymous file suitable for a wl_shm pool
static int
puglWaylandCairoCreateShmFile(const size_t size)
{
  int fd = -1;

#if defined(__linux__) && defined(MFD_CLOEXEC)
  fd = memfd_create("pugl-shm", MFD_CLOEXEC);
#endif

  if (fd < 0) {
    const char* const dir = getenv("XDG_RUNTIME_DIR");
    if (!dir) {
      return -1;
    }

    char      path[PATH_MAX];
    const int n = snprintf(path, sizeof(path), "%s/pugl-shm-XXXXXX", dir);

    if (n < 0 || (size_t)n >= sizeof(path)) {
      return -1; // XDG_RUNTIME_DIR is absurdly long, the template got truncated
    }

    fd = mkostemp(path, O_CLOEXEC);
    if (fd < 0) {
      return -1;
    }

    unlink(path);
  }

  if (ftruncate(fd, (off_t)size) != 0) {
    close(fd);
    return -1;
  }

  return fd;
}

static void
puglWaylandCairoClosePool(PuglWaylandCairoSurface* const surface)
{
  for (int i = 0; i < PUGL_WAYLAND_CAIRO_NUM_BUFFERS; ++i) {
    if (surface->images[i]) {
      cairo_surface_destroy(surface->images[i]);
      surface->images[i] = NULL;
    }
    if (surface->buffers[i]) {
      wl_buffer_destroy(surface->buffers[i]);
      surface->buffers[i] = NULL;
    }
    surface->busy[i] = false;
  }

  if (surface->pool) {
    wl_shm_pool_destroy(surface->pool);
    surface->pool = NULL;
  }

  if (surface->data) {
    munmap(surface->data, surface->poolSize);
    surface->data = NULL;
  }

  if (surface->fd >= 0) {
    close(surface->fd);
    surface->fd = -1;
  }

  surface->poolSize  = 0;
  surface->lastIndex = -1;
}

static PuglStatus
puglWaylandCairoOpenPool(PuglView* const view, const PuglArea size)
{
  PuglWaylandCairoSurface* const surface =
    (PuglWaylandCairoSurface*)view->impl->surface;

  struct wl_shm* const shm = view->world->impl->shm;

  puglWaylandCairoClosePool(surface);

  const int stride =
    cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, size.width);

  if (stride <= 0) {
    return PUGL_BAD_CONFIGURATION;
  }

  /* Computed in 64 bits: a maximum-sized view is 65535x65535 buffer pixels, whose pool is well
     past what a 32-bit size_t can hold, and the product has to survive long enough to be compared
     against the limit rather than wrapping on the way there. */
  const uint64_t wantedBufferSize = (uint64_t)stride * (uint64_t)size.height;
  const uint64_t wantedPoolSize =
    wantedBufferSize * (uint64_t)PUGL_WAYLAND_CAIRO_NUM_BUFFERS;

  /* wl_shm_create_pool and wl_shm_pool_create_buffer take int32_t, so an extreme size times an
     extreme scale could silently wrap into a negative size or offset. */
  if (wantedPoolSize > (uint64_t)INT32_MAX) {
    return PUGL_BAD_CONFIGURATION;
  }

  // Both fit in a size_t on every target now that they are known to be at most INT32_MAX
  const size_t bufferSize = (size_t)wantedBufferSize;
  const size_t poolSize   = (size_t)wantedPoolSize;

  const int fd = puglWaylandCairoCreateShmFile(poolSize);
  if (fd < 0) {
    return PUGL_CREATE_CONTEXT_FAILED;
  }

  uint8_t* const data = (uint8_t*)mmap(
    NULL, poolSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (data == MAP_FAILED) {
    close(fd);
    return PUGL_CREATE_CONTEXT_FAILED;
  }

  surface->fd       = fd;
  surface->data     = data;
  surface->poolSize = poolSize;
  surface->stride   = stride;
  surface->size     = size;
  surface->pool     = wl_shm_create_pool(shm, fd, (int32_t)poolSize);

  if (!surface->pool) {
    puglWaylandCairoClosePool(surface);
    return PUGL_CREATE_CONTEXT_FAILED;
  }

  for (int i = 0; i < PUGL_WAYLAND_CAIRO_NUM_BUFFERS; ++i) {
    surface->buffers[i] = wl_shm_pool_create_buffer(surface->pool,
                                                    (int32_t)(bufferSize * i),
                                                    size.width,
                                                    size.height,
                                                    stride,
                                                    WL_SHM_FORMAT_ARGB8888);

    if (!surface->buffers[i]) {
      puglWaylandCairoClosePool(surface);
      return PUGL_CREATE_CONTEXT_FAILED;
    }

    wl_buffer_add_listener(surface->buffers[i],
                           &puglWaylandCairoBufferListener,
                           &surface->busy[i]);

    surface->images[i] = cairo_image_surface_create_for_data(
      data + (bufferSize * i),
      CAIRO_FORMAT_ARGB32,
      size.width,
      size.height,
      stride);

    if (cairo_surface_status(surface->images[i])) {
      puglWaylandCairoClosePool(surface);
      return PUGL_CREATE_CONTEXT_FAILED;
    }
  }

  return PUGL_SUCCESS;
}

static PuglStatus
puglWaylandCairoConfigure(PuglView* const view)
{
  return view->world->impl->shm ? PUGL_SUCCESS : PUGL_BACKEND_FAILED;
}

static PuglStatus
puglWaylandCairoCreate(PuglView* const view)
{
  PuglWaylandCairoSurface* const surface =
    (PuglWaylandCairoSurface*)calloc(1, sizeof(PuglWaylandCairoSurface));

  if (!surface) {
    return PUGL_NO_MEMORY;
  }

  surface->fd        = -1;
  surface->lastIndex = -1;
  view->impl->surface = surface;

  return PUGL_SUCCESS;
}

static void
puglWaylandCairoDestroy(PuglView* const view)
{
  PuglWaylandCairoSurface* const surface =
    (PuglWaylandCairoSurface*)view->impl->surface;

  if (!surface) {
    return;
  }

  if (surface->cr) {
    cairo_destroy(surface->cr);
    surface->cr = NULL;
  }

  puglWaylandCairoClosePool(surface);
  free(surface);
  view->impl->surface = NULL;
}

static PuglStatus
puglWaylandCairoEnter(PuglView* const view, const PuglExposeEvent* const expose)
{
  PuglWaylandCairoSurface* const surface =
    (PuglWaylandCairoSurface*)view->impl->surface;

  if (!surface || !expose) {
    return PUGL_SUCCESS;
  }

  /* Every failure below leaves this false, which is what stops leave() from committing a buffer
     that was never drawn into.  wayland.c re-arms needsRedisplay for a failed enter(), so the frame
     is retried rather than lost. */
  surface->entered = false;

  const PuglArea size = puglWaylandGetBufferSize(view);

  if (!puglIsValidArea(size)) {
    return PUGL_FAILURE;
  }

  if (!surface->pool || size.width != surface->size.width ||
      size.height != surface->size.height) {
    const PuglStatus st = puglWaylandCairoOpenPool(view, size);
    if (st) {
      return st;
    }
  }

  // Pick a buffer the compositor is not holding
  int index = -1;
  for (int i = 0; i < PUGL_WAYLAND_CAIRO_NUM_BUFFERS; ++i) {
    if (!surface->busy[i]) {
      index = i;
      break;
    }
  }

  if (index < 0) {
    /* Both buffers are still owned by the compositor.  Drawing into one anyway would scribble over
       a frame that is on screen, and attaching it a second time would leave the busy flags
       permanently out of step with reality (two attaches, one release).  Skip this frame instead
       and let wayland.c retry it once a release arrives. */
    return PUGL_FAILURE;
  }

  surface->index = index;
  surface->cr    = cairo_create(surface->images[index]);

  if (cairo_status(surface->cr)) {
    cairo_destroy(surface->cr);
    surface->cr = NULL;
    return PUGL_CREATE_CONTEXT_FAILED;
  }

  /* Carry over whatever the last frame drew: this buffer holds an older frame, and the application
     is only required to redraw the exposed region. */
  if (surface->lastIndex >= 0 && surface->lastIndex != index) {
    cairo_save(surface->cr);
    cairo_set_operator(surface->cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_surface(surface->cr, surface->images[surface->lastIndex], 0, 0);
    cairo_paint(surface->cr);
    cairo_restore(surface->cr);
  }

  cairo_rectangle(
    surface->cr, expose->x, expose->y, expose->width, expose->height);
  cairo_clip(surface->cr);

  surface->entered = true;
  return PUGL_SUCCESS;
}

static PuglStatus
puglWaylandCairoLeave(PuglView* const view, const PuglExposeEvent* const expose)
{
  PuglWaylandCairoSurface* const surface =
    (PuglWaylandCairoSurface*)view->impl->surface;
  struct wl_surface* const wlSurface = view->impl->wlSurface;

  if (!surface || !expose) {
    return PUGL_SUCCESS;
  }

  if (surface->cr) {
    cairo_destroy(surface->cr);
    surface->cr = NULL;
  }

  if (!surface->entered) {
    /* The matching enter() failed, so the buffer holds either nothing or a frame the compositor is
       still showing.  Committing it would put garbage (or a duplicate attach) on screen. */
    return PUGL_FAILURE;
  }

  surface->entered = false;

  const int index = surface->index;

  if (!surface->images[index] || !wlSurface) {
    return PUGL_FAILURE;
  }

  cairo_surface_flush(surface->images[index]);

  // Any new size and scale go out in the same commit as the buffer drawn for them
  puglWaylandApplyGeometry(view);

  wl_surface_attach(wlSurface, surface->buffers[index], 0, 0);

#if defined(WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
  if (wl_surface_get_version(wlSurface) >=
      WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
    wl_surface_damage_buffer(
      wlSurface, expose->x, expose->y, expose->width, expose->height);
  } else
#endif
  {
    /* wl_surface_damage takes surface-local coordinates while expose is in buffer pixels, so the
       rectangle has to be divided by the buffer scale.  That scale is 1 from puglInitViewInternals()
       onwards and only ever set to a positive output scale, but division is unforgiving: check
       rather than risk a SIGFPE if it is ever left at zero. */
    const int32_t scale = view->impl->bufferScale;

    if (scale > 0) {
      const int32_t x      = expose->x / scale;
      const int32_t y      = expose->y / scale;
      const int32_t right  = (expose->x + expose->width + scale - 1) / scale;
      const int32_t bottom = (expose->y + expose->height + scale - 1) / scale;

      wl_surface_damage(wlSurface, x, y, right - x, bottom - y);
    }
  }

  wl_surface_commit(wlSurface);

  surface->busy[index] = true;
  surface->lastIndex   = index;

  return PUGL_SUCCESS;
}

static void*
puglWaylandCairoGetContext(PuglView* const view)
{
  PuglWaylandCairoSurface* const surface =
    (PuglWaylandCairoSurface*)view->impl->surface;

  return surface ? surface->cr : NULL;
}

const PuglBackend*
puglCairoBackend(void)
{
  static const PuglBackend backend = {puglWaylandCairoConfigure,
                                      puglWaylandCairoCreate,
                                      puglWaylandCairoDestroy,
                                      puglWaylandCairoEnter,
                                      puglWaylandCairoLeave,
                                      puglWaylandCairoGetContext};

  return &backend;
}
