// Copyright 2012-2023 David Robillard <d@drobilla.net>
// Copyright 2025 Dusk Audio Framework contributors
// SPDX-License-Identifier: ISC

/*
  Native Wayland backend for pugl.

  Scope, and why it is what it is:

  * Top level (floating) windows only.  Wayland has no cross-client window embedding, so
    puglSetParent() cannot be honoured; a "parent" that happens to be another view of this same
    process is turned into an xdg_toplevel transient parent instead, and anything else is ignored
    with a debug message.  See README.wayland.
  * All coordinates Pugl deals in are *buffer pixels*.  The compositor speaks *logical pixels*.
    The two differ by the view scale factor, which comes either from wp_fractional_scale_v1 (exact,
    in 120ths) or from the largest integer wl_output scale the surface overlaps.  Every conversion
    goes through puglWaylandLogicalToPixels()/puglWaylandPixelsToLogical() so the direction is never
    in doubt.
  * There is no such thing as a window position on Wayland, so configure events always report 0,0
    and puglSetWindowPosition() is a no-op that reports PUGL_UNSUPPORTED.

  This file is compiled as C++, as part of the dgl/src/pugl.cpp unity build, so it avoids C-only
  constructs and casts explicitly from void*.
*/

#include "wayland.h"

#include "../pugl-upstream/src/internal.h"
#include "../pugl-upstream/src/macros.h"
#include "../pugl-upstream/src/platform.h"
#include "../pugl-upstream/src/types.h"

#include "pugl/pugl.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#  include <sys/timerfd.h>
#  define PUGL_WAYLAND_HAVE_TIMERFD 1
#else
#  define PUGL_WAYLAND_HAVE_TIMERFD 0
#endif

#ifdef __cplusplus
#  define PUGL_INIT_STRUCT \
    {                      \
    }
#else
#  define PUGL_INIT_STRUCT {0}
#endif

/* Button codes from linux/input-event-codes.h, spelled out rather than included so that this file
   does not depend on Linux UAPI headers (the codes are part of the wl_pointer protocol contract and
   are the same on the BSDs). */
#define PUGL_WAYLAND_BTN_LEFT 0x110U
#define PUGL_WAYLAND_BTN_RIGHT 0x111U
#define PUGL_WAYLAND_BTN_MIDDLE 0x112U
#define PUGL_WAYLAND_BTN_SIDE 0x113U
#define PUGL_WAYLAND_BTN_EXTRA 0x114U

/// Returned for button codes that have no Pugl button number, such events are ignored
#define PUGL_WAYLAND_BTN_UNKNOWN UINT32_MAX

/// Highest interface versions this backend knows how to speak
#define PUGL_WAYLAND_COMPOSITOR_VERSION 4U
#define PUGL_WAYLAND_SEAT_VERSION 7U
#define PUGL_WAYLAND_OUTPUT_VERSION 2U
#define PUGL_WAYLAND_WM_BASE_VERSION 4U
#define PUGL_WAYLAND_DATA_DEVICE_MANAGER_VERSION 3U

/* Frame callbacks are the right way to pace redraws, but a compositor only sends them for surfaces
   it actually composites, and some never send them at all for a window that is hidden, occluded or
   on another workspace.  Waiting forever would freeze the UI, so drawing goes ahead anyway after a
   timeout.  Until the first callback has come back there is no evidence that pacing works, so the
   timeout is short enough to keep the UI fluid (a floor of roughly 30 Hz, which is what an
   unthrottled backend like x11 would do); once one has arrived, a longer timeout is enough to cover
   the occasional missing frame without wasting effort on an invisible window. */
#define PUGL_WAYLAND_FIRST_FRAME_TIMEOUT 0.034
#define PUGL_WAYLAND_FRAME_TIMEOUT 0.25

/// One wl_pointer axis unit is conventionally a tenth of a "line"
#define PUGL_WAYLAND_AXIS_PER_LINE 10.0

/* Clipboard transfers happen over a pipe shared with another (possibly hostile or simply wedged)
   process, and both ends are serviced from the GUI thread.  Every wait is therefore bounded: the
   receiving side gives up this long after the transfer started, and the sending side after this
   long without the requester draining the pipe. */
#define PUGL_WAYLAND_CLIPBOARD_RECV_TIMEOUT_MS 1000
#define PUGL_WAYLAND_CLIPBOARD_SEND_TIMEOUT_MS 1000

/* The selection owner decides how much it writes, so the accumulated buffer needs a ceiling as
   well: without one another process could grow this client's heap until the host is killed. */
#define PUGL_WAYLAND_CLIPBOARD_MAX_SIZE (64U * 1024U * 1024U)

// --------------------------------------------------------------------------------------------
// Small helpers

static PuglSpan
puglWaylandSpan(const double value)
{
  return (value < 1.0)       ? (PuglSpan)1
         : (value > 32767.0) ? (PuglSpan)32767
                             : (PuglSpan)(value + 0.5);
}

static int32_t
puglWaylandLogical(const double pixels, const double scale)
{
  const double logical = pixels / (scale > 0.0 ? scale : 1.0);
  return (int32_t)(logical < 1.0 ? 1.0 : logical + 0.5);
}

static PuglArea
puglWaylandPixelsToLogical(const PuglArea pixels, const double scale)
{
  PuglArea out;
  out.width  = puglWaylandSpan((double)puglWaylandLogical(pixels.width, scale));
  out.height = puglWaylandSpan((double)puglWaylandLogical(pixels.height, scale));
  return out;
}

static PuglArea
puglWaylandLogicalToPixels(const PuglArea logical, const double scale)
{
  PuglArea out;
  out.width  = puglWaylandSpan(logical.width * scale);
  out.height = puglWaylandSpan(logical.height * scale);
  return out;
}

/// The size a graphics backend must make its drawable, in buffer pixels
PuglArea
puglWaylandGetBufferSize(const PuglView* const view)
{
  const PuglInternals* const impl = view->impl;

  if (puglIsValidArea(impl->size)) {
    return impl->size;
  }

  return puglGetInitialSize(view);
}

static double
puglWaylandTime(const PuglWorld* const world)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return 0.0;
  }

  return ((double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0) -
         world->startTime;
}

/// Monotonic milliseconds, for bounding waits that have no PuglWorld at hand
static int64_t
puglWaylandMonotonicMs(void)
{
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return 0;
  }

  return (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
}

/// Return the view a wl_surface belongs to, or NULL if it is not ours (or gone)
static PuglView*
puglWaylandViewForSurface(struct wl_surface* const surface)
{
  return surface ? (PuglView*)wl_surface_get_user_data(surface) : NULL;
}

/// Damage a whole surface, using the buffer-relative request when it is available
static void
puglWaylandDamageSurface(struct wl_surface* const surface,
                         const int32_t            width,
                         const int32_t            height)
{
#if defined(WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION)
  if (wl_surface_get_version(surface) >=
      WL_SURFACE_DAMAGE_BUFFER_SINCE_VERSION) {
    wl_surface_damage_buffer(surface, 0, 0, width, height);
    return;
  }
#endif

  wl_surface_damage(surface, 0, 0, width, height);
}

// --------------------------------------------------------------------------------------------
// Pending event accumulation (mirrors the x11 backend)

static bool
puglWaylandClipExpose(PuglExposeEvent* const event,
                      const PuglArea         viewSize,
                      const int              x,
                      const int              y,
                      const unsigned         width,
                      const unsigned         height)
{
  const int32_t left   = MAX(0, x);
  const int32_t top    = MAX(0, y);
  const int32_t right  = MIN((int32_t)viewSize.width, x + (int32_t)width);
  const int32_t bottom = MIN((int32_t)viewSize.height, y + (int32_t)height);

  if (right <= left || bottom <= top) {
    return false;
  }

  const PuglExposeEvent clipped = {PUGL_EXPOSE,
                                   0U,
                                   (PuglCoord)left,
                                   (PuglCoord)top,
                                   (PuglSpan)(right - left),
                                   (PuglSpan)(bottom - top)};

  *event = clipped;
  return true;
}

static void
mergeExposeEvents(PuglExposeEvent* const dst, const PuglExposeEvent* const src)
{
  if (!dst->type) {
    if (src->width && src->height) {
      *dst = *src;
    }
  } else {
    const int dst_r = dst->x + dst->width;
    const int src_r = src->x + src->width;
    const int max_x = MAX(dst_r, src_r);
    const int dst_b = dst->y + dst->height;
    const int src_b = src->y + src->height;
    const int max_y = MAX(dst_b, src_b);

    dst->x      = (PuglCoord)MIN(dst->x, src->x);
    dst->y      = (PuglCoord)MIN(dst->y, src->y);
    dst->width  = (PuglSpan)(max_x - dst->x);
    dst->height = (PuglSpan)(max_y - dst->y);
  }
}

static PuglViewStyleFlags
puglWaylandViewStyle(const PuglView* const view)
{
  const PuglInternals* const impl  = view->impl;
  PuglViewStyleFlags         style = impl->styleFlags;

  if (impl->visible && impl->configured) {
    style |= PUGL_VIEW_STYLE_MAPPED;
  } else {
    style &= ~(PuglViewStyleFlags)PUGL_VIEW_STYLE_MAPPED;
  }

  return style;
}

/// Queue a configure event describing the current state, to be flushed by puglUpdate()
static void
puglWaylandQueueConfigure(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  // Wayland never tells a client where its window is, so the position is always reported as 0,0
  PuglEvent event        = {{PUGL_CONFIGURE, 0U}};
  event.configure.x      = 0;
  event.configure.y      = 0;
  event.configure.width  = impl->size.width;
  event.configure.height = impl->size.height;
  event.configure.style  = puglWaylandViewStyle(view);

  impl->pendingConfigure = event;
}

/// Queue an expose of the whole view
static void
puglWaylandQueueFullExpose(PuglView* const view)
{
  const PuglInternals* const impl = view->impl;

  if (!puglIsValidArea(impl->size)) {
    return;
  }

  const PuglExposeEvent expose = {
    PUGL_EXPOSE, 0U, 0, 0, impl->size.width, impl->size.height};

  mergeExposeEvents(&view->impl->pendingExpose.expose, &expose);
}

// --------------------------------------------------------------------------------------------
// Scale and size

/// Recompute the scale factor from the compositor's fractional preference or output scales
static bool
puglWaylandUpdateScale(PuglView* const view)
{
  PuglInternals* const impl      = view->impl;
  const double         oldScale  = impl->scale;
  const int32_t        oldBuffer = impl->bufferScale;

  /* A pre-1.9 wl_compositor cannot be told about buffer scales at all, in which case the only
     honest answer is that everything is unscaled. */
  if (impl->wlSurface && wl_surface_get_version(impl->wlSurface) <
                           WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION) {
    impl->scale       = 1.0;
    impl->bufferScale = 1;
    return impl->scale != oldScale || impl->bufferScale != oldBuffer;
  }

  if (impl->preferredFractionalScale && impl->viewport) {
    // Exact scale from the compositor, applied by scaling the buffer under a viewport
    impl->scale       = impl->preferredFractionalScale / PUGL_WAYLAND_SCALE_DENOM;
    impl->bufferScale = 1;
  } else {
    int32_t scale = 1;
    for (uint32_t i = 0; i < impl->numEnteredOutputs; ++i) {
      const PuglWaylandOutput* const out =
        (const PuglWaylandOutput*)wl_output_get_user_data(
          impl->enteredOutputs[i]);
      if (out && out->scale > scale) {
        scale = out->scale;
      }
    }

    impl->scale       = (double)scale;
    impl->bufferScale = scale;
  }

  if (impl->scale < 1.0) {
    impl->scale = 1.0;
  }

  return impl->scale != oldScale || impl->bufferScale != oldBuffer;
}

/**
   Push the current logical size and scale down to the compositor, if they changed.

   Called by the graphics backends right before the commit that attaches a buffer (cairo's attach
   and commit, the GL backend's eglSwapBuffers), and from nowhere else.  All of this is
   double-buffered surface state that takes effect on the next commit, and the first commit on a new
   xdg_surface is the bufferless one that asks for the initial configure: a window geometry applied
   there describes a rectangle on a 0x0 surface, which mutter rejects ("Client provided invalid
   window geometry ... Working around.").  The buffer scale and viewport destination likewise belong
   with the buffer they describe.  puglWaylandSetSize() therefore only marks the geometry dirty, and
   the stub backend, which never attaches a buffer, never sends it at all.
*/
void
puglWaylandApplyGeometry(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (!impl->wlSurface || !impl->geometryDirty) {
    return;
  }

  impl->geometryDirty = false;

  const bool canSetBufferScale =
    wl_surface_get_version(impl->wlSurface) >=
    WL_SURFACE_SET_BUFFER_SCALE_SINCE_VERSION;

  if (impl->viewport) {
    wp_viewport_set_destination(
      impl->viewport, impl->logicalSize.width, impl->logicalSize.height);
    if (canSetBufferScale) {
      wl_surface_set_buffer_scale(impl->wlSurface, 1);
    }
  } else if (canSetBufferScale) {
    wl_surface_set_buffer_scale(impl->wlSurface, impl->bufferScale);
  }

  if (impl->xdgSurface) {
    xdg_surface_set_window_geometry(
      impl->xdgSurface, 0, 0, impl->logicalSize.width, impl->logicalSize.height);
  }
}

/// Constrain a pixel size to the view's size hints
static PuglArea
puglWaylandConstrainSize(const PuglView* const view, PuglArea size)
{
  const PuglArea minSize = view->sizeHints[PUGL_MIN_SIZE];
  const PuglArea maxSize = view->sizeHints[PUGL_MAX_SIZE];

  if (!view->hints[PUGL_RESIZABLE]) {
    PuglArea fixed = view->sizeHints[PUGL_CURRENT_SIZE];
    if (!puglIsValidArea(fixed)) {
      fixed = view->sizeHints[PUGL_DEFAULT_SIZE];
    }
    if (puglIsValidArea(fixed)) {
      return fixed;
    }
  }

  if (puglIsValidArea(minSize)) {
    size.width  = MAX(size.width, minSize.width);
    size.height = MAX(size.height, minSize.height);
  }

  if (puglIsValidArea(maxSize)) {
    size.width  = MIN(size.width, maxSize.width);
    size.height = MIN(size.height, maxSize.height);
  }

  return size;
}

/**
   Settle on a new view size.

   `logicalRequest` is what the compositor asked for, zero when it does not care and the choice is
   ours.  Everything else is derived from it and the current scale.
*/
static bool
puglWaylandSetSize(PuglView* const view, const PuglArea logicalRequest)
{
  PuglInternals* const impl    = view->impl;
  const PuglArea       oldSize = impl->size;
  PuglArea             logical = PUGL_INIT_STRUCT;

  if (puglIsValidArea(logicalRequest)) {
    logical = logicalRequest;
  } else if (puglIsValidArea(impl->logicalSize)) {
    /* Free choice, and we already have one: keep it.  Working from the logical size rather than the
       pixel size is what makes a scale change keep the window the same size on screen, growing the
       buffer instead of shrinking the window. */
    logical = impl->logicalSize;
  } else {
    /* First time.  DAF's default size is unscaled here, because getDesktopScaleFactor() has no
       scale to report before a surface exists, so it is the logical size. */
    logical = puglGetInitialSize(view);
  }

  // The size hints are in pixels, so constrain there and convert back
  PuglArea pixels = puglWaylandConstrainSize(
    view, puglWaylandLogicalToPixels(logical, impl->scale));

  logical = puglWaylandPixelsToLogical(pixels, impl->scale);

  // Renormalise so the buffer is exactly the logical size times the scale
  pixels = puglWaylandLogicalToPixels(logical, impl->scale);

  impl->size          = pixels;
  impl->logicalSize   = logical;
  impl->geometryDirty = true;

  return oldSize.width != pixels.width || oldSize.height != pixels.height;
}

// --------------------------------------------------------------------------------------------
// Size hints

/* Pugl 703a018 dropped puglUpdateSizeHints from the platform interface, leaving puglApplySizeHint
   as the only entry point, so this is a file-local helper now. Defined ahead of its callers, the
   way x11.c does it, rather than forward declared. */
static PuglStatus
puglUpdateSizeHints(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (!impl->xdgToplevel) {
    return PUGL_SUCCESS;
  }

  if (!view->hints[PUGL_RESIZABLE]) {
    PuglArea size = puglGetSizeHint(view, PUGL_CURRENT_SIZE);
    if (!puglIsValidArea(size)) {
      size = puglGetSizeHint(view, PUGL_DEFAULT_SIZE);
    }

    const PuglArea logical = puglWaylandPixelsToLogical(size, impl->scale);
    xdg_toplevel_set_min_size(
      impl->xdgToplevel, logical.width, logical.height);
    xdg_toplevel_set_max_size(
      impl->xdgToplevel, logical.width, logical.height);
    return PUGL_SUCCESS;
  }

  const PuglArea minSize = view->sizeHints[PUGL_MIN_SIZE];
  if (puglIsValidArea(minSize)) {
    const PuglArea logical = puglWaylandPixelsToLogical(minSize, impl->scale);
    xdg_toplevel_set_min_size(
      impl->xdgToplevel, logical.width, logical.height);
  } else {
    xdg_toplevel_set_min_size(impl->xdgToplevel, 0, 0);
  }

  const PuglArea maxSize = view->sizeHints[PUGL_MAX_SIZE];
  if (puglIsValidArea(maxSize)) {
    const PuglArea logical = puglWaylandPixelsToLogical(maxSize, impl->scale);
    xdg_toplevel_set_max_size(
      impl->xdgToplevel, logical.width, logical.height);
  } else {
    xdg_toplevel_set_max_size(impl->xdgToplevel, 0, 0);
  }

  /* Aspect ratio hints (PUGL_MIN_ASPECT / PUGL_MAX_ASPECT / PUGL_FIXED_ASPECT) have no xdg-shell
     equivalent: the compositor owns interactive resizing and there is no way to constrain it.
     DAF keeps the aspect itself by resizing the view from its own configure handler. */

  return PUGL_SUCCESS;
}

PuglStatus
puglApplySizeHint(PuglView* const view, const PuglSizeHint PUGL_UNUSED(hint))
{
  // No fine-grained updates, hints are always recalculated together
  return puglUpdateSizeHints(view);
}

// --------------------------------------------------------------------------------------------
// Frame callbacks

static void puglWaylandFrameDone(void*               data,
                                 struct wl_callback* callback,
                                 uint32_t            time);

static const struct wl_callback_listener puglWaylandFrameListener = {
  puglWaylandFrameDone};

static void
puglWaylandFrameDone(void* const               data,
                     struct wl_callback* const callback,
                     const uint32_t            PUGL_UNUSED(time))
{
  PuglView* const      view = (PuglView*)data;
  PuglInternals* const impl = view->impl;

  /* Compare before destroying: once the proxy is freed the pointer is dead, and reading it back
     (even just to compare) is undefined.  puglWaylandCanDraw() can have dropped a stale callback
     and started another one, so this really can be a different object than the one we track. */
  if (impl->frameCallback == callback) {
    impl->frameCallback = NULL;
  }

  wl_callback_destroy(callback);

  impl->frameCallbackWorks = true;

  if (impl->needsRedisplay) {
    impl->needsRedisplay = false;
    puglWaylandQueueFullExpose(view);
  }
}

/// Ask the compositor to tell us when the next frame can usefully be drawn
static void
puglWaylandRequestFrame(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (impl->frameCallback || !impl->wlSurface) {
    return;
  }

  impl->frameCallback     = wl_surface_frame(impl->wlSurface);
  impl->frameCallbackTime = puglWaylandTime(view->world);
  wl_callback_add_listener(
    impl->frameCallback, &puglWaylandFrameListener, view);
}

/**
   Return whether it is worth drawing this view right now.

   Normally the answer is "only once the compositor has asked for a frame", which paces redraws to
   the refresh rate and stops an invisible window burning cycles.  See the timeout constants above
   for what happens when the callback does not come.
*/
static bool
puglWaylandCanDraw(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (!impl->frameCallback) {
    return true;
  }

  const double timeout = impl->frameCallbackWorks
                           ? PUGL_WAYLAND_FRAME_TIMEOUT
                           : PUGL_WAYLAND_FIRST_FRAME_TIMEOUT;

  if (puglWaylandTime(view->world) - impl->frameCallbackTime > timeout) {
    // Stale: drop it, and ask again with the next frame that actually gets drawn
    wl_callback_destroy(impl->frameCallback);
    impl->frameCallback = NULL;
    return true;
  }

  return false;
}

// --------------------------------------------------------------------------------------------
// xdg-shell

static void
puglWaylandWmBasePing(void* const               PUGL_UNUSED(data),
                      struct xdg_wm_base* const wmBase,
                      const uint32_t            serial)
{
  xdg_wm_base_pong(wmBase, serial);
}

static const struct xdg_wm_base_listener puglWaylandWmBaseListener = {
  puglWaylandWmBasePing};

static void
puglWaylandSurfaceConfigure(void* const               data,
                            struct xdg_surface* const xdgSurface,
                            const uint32_t            serial)
{
  PuglView* const      view = (PuglView*)data;
  PuglInternals* const impl = view->impl;

  xdg_surface_ack_configure(xdgSurface, serial);

  puglWaylandUpdateScale(view);
  puglWaylandSetSize(view, impl->requestedLogicalSize);

  impl->configured = true;

  puglWaylandQueueConfigure(view);
  puglWaylandQueueFullExpose(view);
}

static const struct xdg_surface_listener puglWaylandSurfaceListener = {
  puglWaylandSurfaceConfigure};

static void
puglWaylandToplevelConfigure(void* const                data,
                             struct xdg_toplevel* const PUGL_UNUSED(toplevel),
                             const int32_t              width,
                             const int32_t              height,
                             struct wl_array* const     states)
{
  PuglView* const      view  = (PuglView*)data;
  PuglInternals* const impl  = view->impl;
  PuglViewStyleFlags   flags = 0U;

  const uint32_t* const first = (const uint32_t*)states->data;
  const size_t          count = states->size / sizeof(uint32_t);

  for (size_t i = 0; i < count; ++i) {
    switch (first[i]) {
    case XDG_TOPLEVEL_STATE_MAXIMIZED:
      flags |= PUGL_VIEW_STYLE_TALL | PUGL_VIEW_STYLE_WIDE;
      break;
    case XDG_TOPLEVEL_STATE_FULLSCREEN:
      flags |= PUGL_VIEW_STYLE_FULLSCREEN;
      break;
    case XDG_TOPLEVEL_STATE_RESIZING:
      flags |= PUGL_VIEW_STYLE_RESIZING;
      break;
    default:
      // Activated, tiled, suspended and constrained states have no Pugl equivalent
      break;
    }
  }

  impl->styleFlags = flags;

  impl->requestedLogicalSize.width =
    (width > 0) ? puglWaylandSpan((double)width) : (PuglSpan)0;
  impl->requestedLogicalSize.height =
    (height > 0) ? puglWaylandSpan((double)height) : (PuglSpan)0;
}

static void
puglWaylandToplevelClose(void* const                data,
                         struct xdg_toplevel* const PUGL_UNUSED(toplevel))
{
  puglDispatchSimpleEvent((PuglView*)data, PUGL_CLOSE);
}

static void
puglWaylandToplevelConfigureBounds(void* const PUGL_UNUSED(data),
                                   struct xdg_toplevel* const PUGL_UNUSED(t),
                                   const int32_t PUGL_UNUSED(width),
                                   const int32_t PUGL_UNUSED(height))
{
  // The largest size that would fit the output: purely advisory, ignored
}

static void
puglWaylandToplevelWmCapabilities(void* const PUGL_UNUSED(data),
                                  struct xdg_toplevel* const PUGL_UNUSED(t),
                                  struct wl_array* const PUGL_UNUSED(caps))
{
  // Which window-menu actions the compositor supports: not used
}

static const struct xdg_toplevel_listener puglWaylandToplevelListener = {
  puglWaylandToplevelConfigure,
  puglWaylandToplevelClose,
  puglWaylandToplevelConfigureBounds,
  puglWaylandToplevelWmCapabilities};

static void
puglWaylandDecorationConfigure(
  void* const                               PUGL_UNUSED(data),
  struct zxdg_toplevel_decoration_v1* const PUGL_UNUSED(decoration),
  const uint32_t                            PUGL_UNUSED(mode))
{
  /* We asked for server-side decorations; if the compositor answers CLIENT_SIDE there is nothing
     useful to do about it (drawing our own titlebar is well out of scope), so the window simply
     ends up undecorated on such compositors. */
}

static const struct zxdg_toplevel_decoration_v1_listener
  puglWaylandDecorationListener = {puglWaylandDecorationConfigure};

static void
puglWaylandExportedHandle(void* const                    data,
                          struct zxdg_exported_v2* const PUGL_UNUSED(exported),
                          const char* const              handle)
{
  PuglView* const      view = (PuglView*)data;
  PuglInternals* const impl = view->impl;

  free(impl->exportedHandle);
  impl->exportedHandle = handle ? strdup(handle) : NULL;
}

static const struct zxdg_exported_v2_listener puglWaylandExportedListener = {
  puglWaylandExportedHandle};

static void
puglWaylandPreferredScale(void* const                          data,
                          struct wp_fractional_scale_v1* const PUGL_UNUSED(fs),
                          const uint32_t                       scale)
{
  PuglView* const      view = (PuglView*)data;
  PuglInternals* const impl = view->impl;

  if (impl->preferredFractionalScale == scale) {
    return;
  }

  impl->preferredFractionalScale = scale;

  if (puglWaylandUpdateScale(view) && impl->configured) {
    puglWaylandSetSize(view, impl->requestedLogicalSize);
    puglWaylandQueueConfigure(view);
    puglWaylandQueueFullExpose(view);
  }
}

static const struct wp_fractional_scale_v1_listener
  puglWaylandFractionalScaleListener = {puglWaylandPreferredScale};

// --------------------------------------------------------------------------------------------
// Surface output tracking (integer scale)

static void
puglWaylandSurfaceEnter(void* const              data,
                        struct wl_surface* const PUGL_UNUSED(surface),
                        struct wl_output* const  output)
{
  PuglView* const      view = (PuglView*)data;
  PuglInternals* const impl = view->impl;

  /* libwayland passes NULL for an object that was destroyed before the event reached us, which is
     exactly what happens when an output is unplugged with an enter still in flight. */
  if (!output) {
    return;
  }

  for (uint32_t i = 0; i < impl->numEnteredOutputs; ++i) {
    if (impl->enteredOutputs[i] == output) {
      return;
    }
  }

  if (impl->numEnteredOutputs >= PUGL_WAYLAND_MAX_OUTPUTS) {
    return;
  }

  impl->enteredOutputs[impl->numEnteredOutputs++] = output;

  if (puglWaylandUpdateScale(view) && impl->configured) {
    puglWaylandSetSize(view, impl->requestedLogicalSize);
    puglWaylandQueueConfigure(view);
    puglWaylandQueueFullExpose(view);
  }
}

/**
   Stop tracking an output on a view, and react to the scale change that may follow.

   Shared by wl_surface.leave and by wl_registry.global_remove: a removed output has to be dropped
   from every view before its proxy is destroyed, or puglWaylandUpdateScale() will call
   wl_output_get_user_data() on freed memory.
*/
static void
puglWaylandForgetOutput(PuglView* const view, struct wl_output* const output)
{
  PuglInternals* const impl = view->impl;

  for (uint32_t i = 0; i < impl->numEnteredOutputs; ++i) {
    if (impl->enteredOutputs[i] == output) {
      impl->enteredOutputs[i] =
        impl->enteredOutputs[impl->numEnteredOutputs - 1];
      --impl->numEnteredOutputs;
      break;
    }
  }

  if (puglWaylandUpdateScale(view) && impl->configured) {
    puglWaylandSetSize(view, impl->requestedLogicalSize);
    puglWaylandQueueConfigure(view);
    puglWaylandQueueFullExpose(view);
  }
}

static void
puglWaylandSurfaceLeave(void* const              data,
                        struct wl_surface* const PUGL_UNUSED(surface),
                        struct wl_output* const  output)
{
  /* A leave queued behind the destruction of its output arrives with a NULL object, and the output
     has already been dropped from every view by puglWaylandRegistryGlobalRemove(). */
  if (output) {
    puglWaylandForgetOutput((PuglView*)data, output);
  }
}

#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
static void
puglWaylandSurfacePreferredScale(void* const              PUGL_UNUSED(data),
                                 struct wl_surface* const PUGL_UNUSED(surface),
                                 const int32_t PUGL_UNUSED(factor))
{
  // wl_compositor v6; this backend derives its integer scale from enter/leave instead
}

static void
puglWaylandSurfacePreferredTransform(void* const PUGL_UNUSED(data),
                                     struct wl_surface* const PUGL_UNUSED(s),
                                     const uint32_t PUGL_UNUSED(transform))
{
  // Output transforms (rotated screens) are handled by the compositor for us
}
#endif

static const struct wl_surface_listener puglWaylandSurfaceEventListener = {
  puglWaylandSurfaceEnter,
  puglWaylandSurfaceLeave,
#if defined(WL_SURFACE_PREFERRED_BUFFER_SCALE_SINCE_VERSION)
  puglWaylandSurfacePreferredScale,
  puglWaylandSurfacePreferredTransform,
#endif
};

// --------------------------------------------------------------------------------------------
// Outputs

static void
puglWaylandOutputGeometry(void* const             PUGL_UNUSED(data),
                          struct wl_output* const PUGL_UNUSED(output),
                          const int32_t           PUGL_UNUSED(x),
                          const int32_t           PUGL_UNUSED(y),
                          const int32_t           PUGL_UNUSED(physicalWidth),
                          const int32_t           PUGL_UNUSED(physicalHeight),
                          const int32_t           PUGL_UNUSED(subpixel),
                          const char* const       PUGL_UNUSED(make),
                          const char* const       PUGL_UNUSED(model),
                          const int32_t           PUGL_UNUSED(transform))
{
}

static void
puglWaylandOutputMode(void* const             PUGL_UNUSED(data),
                      struct wl_output* const PUGL_UNUSED(output),
                      const uint32_t          PUGL_UNUSED(flags),
                      const int32_t           PUGL_UNUSED(width),
                      const int32_t           PUGL_UNUSED(height),
                      const int32_t           PUGL_UNUSED(refresh))
{
}

static void
puglWaylandOutputDone(void* const             data,
                      struct wl_output* const PUGL_UNUSED(output))
{
  PuglWaylandOutput* const out = (PuglWaylandOutput*)data;

  if (!out->scaleChanged) {
    return;
  }

  out->scaleChanged = false;

  if (!out->wimpl || !out->wimpl->world) {
    return;
  }

  PuglWorld* const world = out->wimpl->world;
  for (size_t i = 0; i < world->numViews; ++i) {
    PuglView* const view = world->views[i];
    if (!view || !view->impl) {
      continue;
    }

    PuglInternals* const impl    = view->impl;
    bool                 entered = false;
    for (uint32_t j = 0; j < impl->numEnteredOutputs; ++j) {
      if (impl->enteredOutputs[j] == out->output) {
        entered = true;
        break;
      }
    }

    if (entered && puglWaylandUpdateScale(view) && impl->configured) {
      puglWaylandSetSize(view, impl->requestedLogicalSize);
      puglWaylandQueueConfigure(view);
      puglWaylandQueueFullExpose(view);
    }
  }
}

static void
puglWaylandOutputScale(void* const             data,
                       struct wl_output* const PUGL_UNUSED(output),
                       const int32_t           factor)
{
  PuglWaylandOutput* const out = (PuglWaylandOutput*)data;
  const int32_t             nextScale = factor > 0 ? factor : 1;

  if (out->scale != nextScale) {
    out->scale        = nextScale;
    out->scaleChanged = true;
  }
}

#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
static void
puglWaylandOutputName(void* const             PUGL_UNUSED(data),
                      struct wl_output* const PUGL_UNUSED(output),
                      const char* const       PUGL_UNUSED(name))
{
}

static void
puglWaylandOutputDescription(void* const             PUGL_UNUSED(data),
                             struct wl_output* const PUGL_UNUSED(output),
                             const char* const       PUGL_UNUSED(description))
{
}
#endif

static const struct wl_output_listener puglWaylandOutputListener = {
  puglWaylandOutputGeometry,
  puglWaylandOutputMode,
  puglWaylandOutputDone,
  puglWaylandOutputScale,
#if defined(WL_OUTPUT_NAME_SINCE_VERSION)
  puglWaylandOutputName,
  puglWaylandOutputDescription,
#endif
};

// --------------------------------------------------------------------------------------------
// Pointer

static PuglMods
puglWaylandMods(const PuglWorldInternals* const impl)
{
  return impl->mods;
}

static void
puglWaylandFlushScroll(PuglWorldInternals* const impl)
{
  PuglView* const view = impl->pointerFocus;

  if (!impl->scroll.any) {
    return;
  }

  const PuglWaylandScroll scroll = impl->scroll;

  impl->scroll.any      = false;
  impl->scroll.discrete = false;
  impl->scroll.dx       = 0.0;
  impl->scroll.dy       = 0.0;

  if (!view || (scroll.dx == 0.0 && scroll.dy == 0.0)) {
    return;
  }

  PuglEvent event        = {{PUGL_SCROLL, 0U}};
  event.scroll.time      = scroll.time / 1e3;
  event.scroll.x         = impl->pointerX;
  event.scroll.y         = impl->pointerY;
  event.scroll.xRoot     = impl->pointerX;
  event.scroll.yRoot     = impl->pointerY;
  event.scroll.state     = puglWaylandMods(impl);
  event.scroll.dx        = scroll.dx;
  event.scroll.dy        = scroll.dy;
  event.scroll.direction = PUGL_SCROLL_SMOOTH;

  if (scroll.discrete) {
    if (scroll.dy > 0.0) {
      event.scroll.direction = PUGL_SCROLL_UP;
    } else if (scroll.dy < 0.0) {
      event.scroll.direction = PUGL_SCROLL_DOWN;
    } else if (scroll.dx < 0.0) {
      event.scroll.direction = PUGL_SCROLL_LEFT;
    } else {
      event.scroll.direction = PUGL_SCROLL_RIGHT;
    }
  }

  puglDispatchEvent(view, &event);
}

static PuglStatus puglWaylandApplyCursor(PuglWorldInternals* impl,
                                         PuglCursor          cursor);

static void
puglWaylandPointerEnter(void* const              data,
                        struct wl_pointer* const PUGL_UNUSED(pointer),
                        const uint32_t           serial,
                        struct wl_surface* const surface,
                        const wl_fixed_t         surfaceX,
                        const wl_fixed_t         surfaceY)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglView* const           view = puglWaylandViewForSurface(surface);

  impl->lastSerial         = serial;
  impl->pointerEnterSerial = serial;
  impl->pointerFocus       = view;

  if (!view) {
    return;
  }

  const double scale = view->impl->scale;
  impl->pointerX     = wl_fixed_to_double(surfaceX) * scale;
  impl->pointerY     = wl_fixed_to_double(surfaceY) * scale;

  puglWaylandApplyCursor(impl, view->impl->cursor);

  PuglEvent event       = {{PUGL_POINTER_IN, 0U}};
  event.crossing.time   = puglWaylandTime(view->world);
  event.crossing.x      = impl->pointerX;
  event.crossing.y      = impl->pointerY;
  event.crossing.xRoot  = impl->pointerX;
  event.crossing.yRoot  = impl->pointerY;
  event.crossing.state  = puglWaylandMods(impl);
  event.crossing.mode   = PUGL_CROSSING_NORMAL;
  puglDispatchEvent(view, &event);
}

static void
puglWaylandPointerLeave(void* const              data,
                        struct wl_pointer* const PUGL_UNUSED(pointer),
                        const uint32_t           serial,
                        struct wl_surface* const surface)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglView* const           view = puglWaylandViewForSurface(surface);

  impl->lastSerial = serial;

  if (impl->pointerFocus == view) {
    impl->pointerFocus = NULL;
  }

  if (!view) {
    return;
  }

  PuglEvent event      = {{PUGL_POINTER_OUT, 0U}};
  event.crossing.time  = puglWaylandTime(view->world);
  event.crossing.x     = impl->pointerX;
  event.crossing.y     = impl->pointerY;
  event.crossing.xRoot = impl->pointerX;
  event.crossing.yRoot = impl->pointerY;
  event.crossing.state = puglWaylandMods(impl);
  event.crossing.mode  = PUGL_CROSSING_NORMAL;
  puglDispatchEvent(view, &event);
}

static void
puglWaylandPointerMotion(void* const              data,
                         struct wl_pointer* const PUGL_UNUSED(pointer),
                         const uint32_t           time,
                         const wl_fixed_t         surfaceX,
                         const wl_fixed_t         surfaceY)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglView* const           view = impl->pointerFocus;

  if (!view) {
    return;
  }

  const double scale = view->impl->scale;
  impl->pointerX     = wl_fixed_to_double(surfaceX) * scale;
  impl->pointerY     = wl_fixed_to_double(surfaceY) * scale;

  PuglEvent event    = {{PUGL_MOTION, 0U}};
  event.motion.time  = time / 1e3;
  event.motion.x     = impl->pointerX;
  event.motion.y     = impl->pointerY;
  event.motion.xRoot = impl->pointerX;
  event.motion.yRoot = impl->pointerY;
  event.motion.state = puglWaylandMods(impl);
  puglDispatchEvent(view, &event);
}

static uint32_t
puglWaylandButtonNumber(const uint32_t code)
{
  // Pugl orders buttons primary, secondary, middle (see PuglButtonEvent)
  switch (code) {
  case PUGL_WAYLAND_BTN_LEFT:
    return 0U;
  case PUGL_WAYLAND_BTN_RIGHT:
    return 1U;
  case PUGL_WAYLAND_BTN_MIDDLE:
    return 2U;
  case PUGL_WAYLAND_BTN_SIDE:
    return 3U;
  case PUGL_WAYLAND_BTN_EXTRA:
    return 4U;
  default:
    break;
  }

  // Anything above keeps the order the device reports it in, after the five named buttons.
  // Codes below BTN_LEFT are not mouse buttons at all (BTN_MISC and friends) and have no slot
  // in that order, so they are dropped rather than reported as the primary button.
  return (code > PUGL_WAYLAND_BTN_EXTRA)
           ? (5U + (code - PUGL_WAYLAND_BTN_EXTRA - 1U))
           : PUGL_WAYLAND_BTN_UNKNOWN;
}

static void
puglWaylandPointerButton(void* const              data,
                         struct wl_pointer* const PUGL_UNUSED(pointer),
                         const uint32_t           serial,
                         const uint32_t           time,
                         const uint32_t           button,
                         const uint32_t           state)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglView* const           view = impl->pointerFocus;

  impl->lastSerial = serial;

  if (!view) {
    return;
  }

  const uint32_t number = puglWaylandButtonNumber(button);
  if (number == PUGL_WAYLAND_BTN_UNKNOWN) {
    return;
  }

  PuglEvent event     = {{PUGL_NOTHING, 0U}};
  event.button.type   = (state == WL_POINTER_BUTTON_STATE_PRESSED)
                          ? PUGL_BUTTON_PRESS
                          : PUGL_BUTTON_RELEASE;
  event.button.time   = time / 1e3;
  event.button.x      = impl->pointerX;
  event.button.y      = impl->pointerY;
  event.button.xRoot  = impl->pointerX;
  event.button.yRoot  = impl->pointerY;
  event.button.state  = puglWaylandMods(impl);
  event.button.button = number;
  puglDispatchEvent(view, &event);
}

static void
puglWaylandPointerAxis(void* const              data,
                       struct wl_pointer* const pointer,
                       const uint32_t           time,
                       const uint32_t           axis,
                       const wl_fixed_t         value)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  const double lines = wl_fixed_to_double(value) / PUGL_WAYLAND_AXIS_PER_LINE;

  impl->scroll.any  = true;
  impl->scroll.time = time;

  if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) {
    impl->scroll.dy -= lines; // Wayland counts down as positive, Pugl counts up
  } else {
    impl->scroll.dx += lines;
  }

  if (wl_pointer_get_version(pointer) < WL_POINTER_FRAME_SINCE_VERSION) {
    puglWaylandFlushScroll(impl);
  }
}

static void
puglWaylandPointerFrame(void* const              data,
                        struct wl_pointer* const PUGL_UNUSED(pointer))
{
  puglWaylandFlushScroll((PuglWorldInternals*)data);
}

static void
puglWaylandPointerAxisSource(void* const              data,
                             struct wl_pointer* const PUGL_UNUSED(pointer),
                             const uint32_t           axisSource)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  impl->scroll.discrete = (axisSource == WL_POINTER_AXIS_SOURCE_WHEEL) ||
                          (axisSource == WL_POINTER_AXIS_SOURCE_WHEEL_TILT);
}

static void
puglWaylandPointerAxisStop(void* const              PUGL_UNUSED(data),
                           struct wl_pointer* const PUGL_UNUSED(pointer),
                           const uint32_t           PUGL_UNUSED(time),
                           const uint32_t           PUGL_UNUSED(axis))
{
}

static void
puglWaylandPointerAxisDiscrete(void* const              data,
                               struct wl_pointer* const PUGL_UNUSED(pointer),
                               const uint32_t           PUGL_UNUSED(axis),
                               const int32_t            PUGL_UNUSED(discrete))
{
  ((PuglWorldInternals*)data)->scroll.discrete = true;
}

#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
static void
puglWaylandPointerAxisValue120(void* const              data,
                               struct wl_pointer* const PUGL_UNUSED(pointer),
                               const uint32_t           PUGL_UNUSED(axis),
                               const int32_t            PUGL_UNUSED(value120))
{
  ((PuglWorldInternals*)data)->scroll.discrete = true;
}
#endif

#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
static void
puglWaylandPointerAxisRelativeDirection(void* const PUGL_UNUSED(data),
                                        struct wl_pointer* const PUGL_UNUSED(p),
                                        const uint32_t PUGL_UNUSED(axis),
                                        const uint32_t PUGL_UNUSED(direction))
{
}
#endif

static const struct wl_pointer_listener puglWaylandPointerListener = {
  puglWaylandPointerEnter,
  puglWaylandPointerLeave,
  puglWaylandPointerMotion,
  puglWaylandPointerButton,
  puglWaylandPointerAxis,
  puglWaylandPointerFrame,
  puglWaylandPointerAxisSource,
  puglWaylandPointerAxisStop,
  puglWaylandPointerAxisDiscrete,
#if defined(WL_POINTER_AXIS_VALUE120_SINCE_VERSION)
  puglWaylandPointerAxisValue120,
#endif
#if defined(WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION)
  puglWaylandPointerAxisRelativeDirection,
#endif
};

// --------------------------------------------------------------------------------------------
// Keyboard

static PuglKey
puglWaylandKeyInRange(const uint32_t sym,
                      const uint32_t min,
                      const uint32_t max,
                      const PuglKey  puglMin)
{
  return (sym >= min && sym <= max) ? (PuglKey)(puglMin + (sym - min))
                                    : PUGL_KEY_NONE;
}

static PuglKey
puglWaylandKeysymToSpecial(const uint32_t sym)
{
  PuglKey key = PUGL_KEY_NONE;
  if ((key = puglWaylandKeyInRange(sym, XKB_KEY_F1, XKB_KEY_F12, PUGL_KEY_F1)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_Page_Up, XKB_KEY_End, PUGL_KEY_PAGE_UP)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_Home, XKB_KEY_Down, PUGL_KEY_HOME)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_Shift_L, XKB_KEY_Control_R, PUGL_KEY_SHIFT_L)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_Alt_L, XKB_KEY_Super_R, PUGL_KEY_ALT_L)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_KP_Home, XKB_KEY_KP_Down, PUGL_KEY_PAD_HOME)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_KP_0, XKB_KEY_KP_9, PUGL_KEY_PAD_0)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_KP_Begin, XKB_KEY_KP_Delete, PUGL_KEY_PAD_CLEAR)) ||
      (key = puglWaylandKeyInRange(
         sym, XKB_KEY_KP_Multiply, XKB_KEY_KP_Divide, PUGL_KEY_PAD_MULTIPLY))) {
    return key;
  }

  // clang-format off
  switch (sym) {
  case XKB_KEY_ISO_Level3_Shift: return PUGL_KEY_ALT_R;
  case XKB_KEY_Pause:            return PUGL_KEY_PAUSE;
  case XKB_KEY_Scroll_Lock:      return PUGL_KEY_SCROLL_LOCK;
  case XKB_KEY_Print:            return PUGL_KEY_PRINT_SCREEN;
  case XKB_KEY_Insert:           return PUGL_KEY_INSERT;
  case XKB_KEY_Menu:             return PUGL_KEY_MENU;
  case XKB_KEY_Num_Lock:         return PUGL_KEY_NUM_LOCK;
  case XKB_KEY_KP_Enter:         return PUGL_KEY_PAD_ENTER;
  case XKB_KEY_KP_Page_Up:       return PUGL_KEY_PAD_PAGE_UP;
  case XKB_KEY_KP_Page_Down:     return PUGL_KEY_PAD_PAGE_DOWN;
  case XKB_KEY_KP_End:           return PUGL_KEY_PAD_END;
  case XKB_KEY_KP_Equal:         return PUGL_KEY_PAD_CLEAR;
  case XKB_KEY_Caps_Lock:        return PUGL_KEY_CAPS_LOCK;
  default: break;
  }
  // clang-format on

  return PUGL_KEY_NONE;
}

/// The code point the key would produce with no modifiers at all
static uint32_t
puglWaylandUnshiftedCodepoint(const PuglWaylandXkb* const xkb,
                              const uint32_t              keycode)
{
  const xkb_layout_index_t layout =
    xkb_state_key_get_layout(xkb->state, keycode);
  const xkb_keysym_t* syms = NULL;
  const int           n =
    xkb_keymap_key_get_syms_by_level(xkb->keymap, keycode, layout, 0, &syms);

  return (n > 0) ? xkb_keysym_to_utf32(syms[0]) : 0U;
}

static void
puglWaylandStopRepeat(PuglWorldInternals* const impl)
{
  impl->repeat.key  = 0U;
  impl->repeat.view = NULL;

#if PUGL_WAYLAND_HAVE_TIMERFD
  if (impl->repeat.fd >= 0) {
    const struct itimerspec off = {{0, 0}, {0, 0}};
    timerfd_settime(impl->repeat.fd, 0, &off, NULL);
  }
#endif
}

static void
puglWaylandStartRepeat(PuglWorldInternals* const impl,
                       PuglView* const           view,
                       const uint32_t            key,
                       const double              time)
{
#if PUGL_WAYLAND_HAVE_TIMERFD
  if (impl->repeat.fd < 0 || impl->repeat.rate <= 0) {
    return;
  }

  const long intervalNs = (long)(1000000000L / impl->repeat.rate);
  const long delayMs    = impl->repeat.delay > 0 ? impl->repeat.delay : 400;

  struct itimerspec spec = {{0, 0}, {0, 0}};
  spec.it_value.tv_sec      = delayMs / 1000;
  spec.it_value.tv_nsec     = (delayMs % 1000) * 1000000L;
  spec.it_interval.tv_sec   = 0;
  spec.it_interval.tv_nsec  = intervalNs;

  if (timerfd_settime(impl->repeat.fd, 0, &spec, NULL) == 0) {
    impl->repeat.key  = key;
    impl->repeat.view = view;
    impl->repeat.time = time;
  }
#else
  (void)impl;
  (void)view;
  (void)key;
  (void)time;
#endif
}

/// Turn one raw key event into a Pugl key event, plus a text event where appropriate
static void
puglWaylandDispatchKey(PuglWorldInternals* const impl,
                       PuglView* const           view,
                       const uint32_t            key,
                       const bool                pressed,
                       const double              time)
{
  PuglWaylandXkb* const xkb     = &impl->xkb;
  const uint32_t        keycode = key + 8U; // evdev to xkb/X11 keycode

  if (!xkb->state) {
    return;
  }

  const xkb_keysym_t sym = xkb_state_key_get_one_sym(xkb->state, keycode);

  PuglEvent event   = {{PUGL_NOTHING, 0U}};
  event.key.type    = pressed ? PUGL_KEY_PRESS : PUGL_KEY_RELEASE;
  event.key.time    = time;
  event.key.x       = impl->pointerX;
  event.key.y       = impl->pointerY;
  event.key.xRoot   = impl->pointerX;
  event.key.yRoot   = impl->pointerY;
  event.key.state   = puglWaylandMods(impl);
  event.key.keycode = key;

  const PuglKey special = puglWaylandKeysymToSpecial(sym);
  if (special) {
    event.key.state = puglFilterMods(event.key.state, special);
    event.key.key   = special;
  } else {
    event.key.key = puglWaylandUnshiftedCodepoint(xkb, keycode);
  }

  puglDispatchEvent(view, &event);

  if (!pressed) {
    return;
  }

  // Work out the text this press produces, running it through the compose state first
  char buf[8]   = PUGL_INIT_STRUCT;
  int  n        = 0;
  bool composed = false;

  if (xkb->composeState) {
    xkb_compose_state_feed(xkb->composeState, sym);

    switch (xkb_compose_state_get_status(xkb->composeState)) {
    case XKB_COMPOSE_COMPOSING:
      return; // Swallow the key, a sequence is in progress
    case XKB_COMPOSE_COMPOSED:
      n = xkb_compose_state_get_utf8(xkb->composeState, buf, sizeof(buf));
      xkb_compose_state_reset(xkb->composeState);
      composed = true;
      break;
    case XKB_COMPOSE_CANCELLED:
      xkb_compose_state_reset(xkb->composeState);
      return;
    case XKB_COMPOSE_NOTHING:
    default:
      break;
    }
  }

  if (!composed) {
    // A modified key is a shortcut, not text (matches the x11 backend)
    if (event.key.state &
        (PUGL_MOD_CTRL | PUGL_MOD_ALT | PUGL_MOD_SUPER)) {
      return;
    }

    n = xkb_state_key_get_utf8(xkb->state, keycode, buf, sizeof(buf));
  }

  if (n <= 0 || (size_t)n >= sizeof(buf)) {
    return;
  }

  PuglEvent textEvent      = {{PUGL_TEXT, 0U}};
  textEvent.text.time      = time;
  textEvent.text.x         = event.key.x;
  textEvent.text.y         = event.key.y;
  textEvent.text.xRoot     = event.key.xRoot;
  textEvent.text.yRoot     = event.key.yRoot;
  textEvent.text.state     = event.key.state;
  textEvent.text.keycode   = key;
  textEvent.text.character = puglDecodeUTF8((const uint8_t*)buf);
  memcpy(textEvent.text.string, buf, sizeof(buf));

  puglDispatchEvent(view, &textEvent);
}

static void
puglWaylandKeyboardKeymap(void* const               data,
                          struct wl_keyboard* const PUGL_UNUSED(keyboard),
                          const uint32_t            format,
                          const int32_t             fd,
                          const uint32_t            size)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglWaylandXkb* const     xkb  = &impl->xkb;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !xkb->context) {
    close(fd);
    return;
  }

  char* const map =
    (char*)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (map == MAP_FAILED) {
    close(fd);
    return;
  }

  struct xkb_keymap* const keymap =
    xkb_keymap_new_from_string(xkb->context,
                               map,
                               XKB_KEYMAP_FORMAT_TEXT_V1,
                               XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap(map, size);
  close(fd);

  if (!keymap) {
    return;
  }

  struct xkb_state* const state = xkb_state_new(keymap);
  if (!state) {
    xkb_keymap_unref(keymap);
    return;
  }

  if (xkb->state) {
    xkb_state_unref(xkb->state);
  }
  if (xkb->keymap) {
    xkb_keymap_unref(xkb->keymap);
  }

  xkb->keymap = keymap;
  xkb->state  = state;

  /* A keycode that was repeating under the old keymap may mean something else (or nothing) under the
     new one, and a half-finished compose sequence was started with symbols that no longer apply. */
  puglWaylandStopRepeat(impl);

  if (xkb->composeState) {
    xkb_compose_state_reset(xkb->composeState);
  }
}

static void
puglWaylandKeyboardEnter(void* const               data,
                         struct wl_keyboard* const PUGL_UNUSED(keyboard),
                         const uint32_t            serial,
                         struct wl_surface* const  surface,
                         struct wl_array* const    PUGL_UNUSED(keys))
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglView* const           view = puglWaylandViewForSurface(surface);

  impl->lastSerial    = serial;
  impl->keyboardFocus = view;

  if (!view) {
    return;
  }

  PuglEvent event  = {{PUGL_FOCUS_IN, 0U}};
  event.focus.mode = PUGL_CROSSING_NORMAL;
  puglDispatchEvent(view, &event);
}

static void
puglWaylandKeyboardLeave(void* const               data,
                         struct wl_keyboard* const PUGL_UNUSED(keyboard),
                         const uint32_t            serial,
                         struct wl_surface* const  surface)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglView* const           view = puglWaylandViewForSurface(surface);

  impl->lastSerial = serial;
  puglWaylandStopRepeat(impl);

  if (impl->keyboardFocus == view) {
    impl->keyboardFocus = NULL;
  }

  if (!view) {
    return;
  }

  PuglEvent event  = {{PUGL_FOCUS_OUT, 0U}};
  event.focus.mode = PUGL_CROSSING_NORMAL;
  puglDispatchEvent(view, &event);
}

static void
puglWaylandKeyboardKey(void* const               data,
                       struct wl_keyboard* const PUGL_UNUSED(keyboard),
                       const uint32_t            serial,
                       const uint32_t            time,
                       const uint32_t            key,
                       const uint32_t            state)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;
  PuglView* const           view = impl->keyboardFocus;

  impl->lastSerial = serial;

  if (!view) {
    return;
  }

  const bool pressed = (state == WL_KEYBOARD_KEY_STATE_PRESSED);

  if (!pressed && impl->repeat.key == key) {
    puglWaylandStopRepeat(impl);
  }

  puglWaylandDispatchKey(impl, view, key, pressed, time / 1e3);

  if (pressed && !view->hints[PUGL_IGNORE_KEY_REPEAT] && impl->xkb.keymap &&
      xkb_keymap_key_repeats(impl->xkb.keymap, key + 8U)) {
    puglWaylandStartRepeat(impl, view, key, time / 1e3);
  }
}

static void
puglWaylandKeyboardModifiers(void* const               data,
                             struct wl_keyboard* const PUGL_UNUSED(keyboard),
                             const uint32_t            serial,
                             const uint32_t            depressed,
                             const uint32_t            latched,
                             const uint32_t            locked,
                             const uint32_t            group)
{
  PuglWorldInternals* const impl  = (PuglWorldInternals*)data;
  struct xkb_state* const   state = impl->xkb.state;

  impl->lastSerial = serial;

  if (!state) {
    return;
  }

  xkb_state_update_mask(state, depressed, latched, locked, 0, 0, group);

  PuglMods mods = 0U;
  if (xkb_state_mod_name_is_active(
        state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) {
    mods |= PUGL_MOD_SHIFT;
  }
  if (xkb_state_mod_name_is_active(
        state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) {
    mods |= PUGL_MOD_CTRL;
  }
  if (xkb_state_mod_name_is_active(
        state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) {
    mods |= PUGL_MOD_ALT;
  }
  if (xkb_state_mod_name_is_active(
        state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) {
    mods |= PUGL_MOD_SUPER;
  }
  if (xkb_state_mod_name_is_active(
        state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0) {
    mods |= PUGL_MOD_CAPS_LOCK;
  }
  if (xkb_state_mod_name_is_active(
        state, XKB_MOD_NAME_NUM, XKB_STATE_MODS_LOCKED) > 0) {
    mods |= PUGL_MOD_NUM_LOCK;
  }

  impl->mods = mods;
}

#if defined(WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
static void
puglWaylandKeyboardRepeatInfo(void* const               data,
                              struct wl_keyboard* const PUGL_UNUSED(keyboard),
                              const int32_t             rate,
                              const int32_t             delay)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  impl->repeat.rate  = rate;
  impl->repeat.delay = delay;

  if (rate <= 0) {
    puglWaylandStopRepeat(impl);
  }
}
#endif

static const struct wl_keyboard_listener puglWaylandKeyboardListener = {
  puglWaylandKeyboardKeymap,
  puglWaylandKeyboardEnter,
  puglWaylandKeyboardLeave,
  puglWaylandKeyboardKey,
  puglWaylandKeyboardModifiers,
#if defined(WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
  puglWaylandKeyboardRepeatInfo,
#endif
};

static void
puglWaylandHandleRepeat(PuglWorldInternals* const impl)
{
#if PUGL_WAYLAND_HAVE_TIMERFD
  uint64_t expirations = 0;

  if (read(impl->repeat.fd, &expirations, sizeof(expirations)) !=
      (ssize_t)sizeof(expirations)) {
    return;
  }

  PuglView* const view = impl->repeat.view;
  const uint32_t  key  = impl->repeat.key;

  if (!view || !key || expirations == 0) {
    return;
  }

  // Deliver at most a handful of repeats even if the loop was blocked for a while
  if (expirations > 8) {
    expirations = 8;
  }

  for (uint64_t i = 0; i < expirations; ++i) {
    puglWaylandDispatchKey(impl, view, key, true, puglWaylandTime(view->world));
  }
#else
  (void)impl;
#endif
}

// --------------------------------------------------------------------------------------------
// Seat

static void
puglWaylandSeatCapabilities(void* const           data,
                            struct wl_seat* const seat,
                            const uint32_t        capabilities)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  const bool hasPointer  = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0U;
  const bool hasKeyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0U;

  if (hasPointer && !impl->pointer) {
    impl->pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(impl->pointer, &puglWaylandPointerListener, impl);
  } else if (!hasPointer && impl->pointer) {
    wl_pointer_release(impl->pointer);
    impl->pointer      = NULL;
    impl->pointerFocus = NULL;
  }

  if (hasKeyboard && !impl->keyboard) {
    impl->keyboard = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(impl->keyboard, &puglWaylandKeyboardListener, impl);
  } else if (!hasKeyboard && impl->keyboard) {
    puglWaylandStopRepeat(impl);
    wl_keyboard_release(impl->keyboard);
    impl->keyboard      = NULL;
    impl->keyboardFocus = NULL;
  }
}

static void
puglWaylandSeatName(void* const           PUGL_UNUSED(data),
                    struct wl_seat* const PUGL_UNUSED(seat),
                    const char* const     PUGL_UNUSED(name))
{
}

static const struct wl_seat_listener puglWaylandSeatListener = {
  puglWaylandSeatCapabilities, puglWaylandSeatName};

// --------------------------------------------------------------------------------------------
// Clipboard

static void
puglWaylandFreeOffer(PuglWaylandOffer* const offer)
{
  if (!offer) {
    return;
  }

  for (uint32_t i = 0; i < offer->numMimeTypes; ++i) {
    free(offer->mimeTypes[i]);
  }

  free(offer->mimeTypes);

  if (offer->offer) {
    wl_data_offer_destroy(offer->offer);
  }

  free(offer);
}

static void
puglWaylandDataOfferMime(void* const                 data,
                         struct wl_data_offer* const PUGL_UNUSED(offer),
                         const char* const           mimeType)
{
  PuglWaylandOffer* const po = (PuglWaylandOffer*)data;

  if (!po || !mimeType) {
    return;
  }

  char** const types = (char**)realloc(
    po->mimeTypes, (po->numMimeTypes + 1U) * sizeof(char*));

  if (!types) {
    return;
  }

  po->mimeTypes = types;

  const size_t len         = strlen(mimeType);
  char* const  copy        = (char*)calloc(len + 1U, 1U);
  if (!copy) {
    return;
  }

  memcpy(copy, mimeType, len);
  po->mimeTypes[po->numMimeTypes++] = copy;
}

static void
puglWaylandDataOfferSourceActions(void* const                 PUGL_UNUSED(data),
                                  struct wl_data_offer* const PUGL_UNUSED(o),
                                  const uint32_t PUGL_UNUSED(sourceActions))
{
}

static void
puglWaylandDataOfferAction(void* const                 PUGL_UNUSED(data),
                           struct wl_data_offer* const PUGL_UNUSED(offer),
                           const uint32_t              PUGL_UNUSED(dndAction))
{
}

static const struct wl_data_offer_listener puglWaylandDataOfferListener = {
  puglWaylandDataOfferMime,
  puglWaylandDataOfferSourceActions,
  puglWaylandDataOfferAction};

static void
puglWaylandDataDeviceOffer(void* const                  data,
                           struct wl_data_device* const PUGL_UNUSED(device),
                           struct wl_data_offer* const  offer)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  puglWaylandFreeOffer(impl->unclaimedOffer);
  impl->unclaimedOffer = NULL;

  PuglWaylandOffer* const po =
    (PuglWaylandOffer*)calloc(1, sizeof(PuglWaylandOffer));

  if (!po) {
    wl_data_offer_destroy(offer);
    return;
  }

  po->offer = offer;
  wl_data_offer_add_listener(offer, &puglWaylandDataOfferListener, po);
  impl->unclaimedOffer = po;
}

/**
   Take ownership of the offer the compositor introduced just before naming it here.

   Deliberately not wl_data_offer_get_user_data(): puglWaylandDataDeviceOffer() destroys the proxy
   outright when it cannot allocate the bookkeeping for it, and the enter/selection naming that same
   offer still arrives afterwards, so the proxy handed over here is not guaranteed to be alive.  The
   protocol always sends wl_data_device.data_offer immediately before the event that uses it, so the
   one still held as unclaimed is the one being named -- and if it is not, it never will be.
*/
static PuglWaylandOffer*
puglWaylandClaimOffer(PuglWorldInternals* const   impl,
                      struct wl_data_offer* const offer)
{
  PuglWaylandOffer* const po = impl->unclaimedOffer;

  impl->unclaimedOffer = NULL;

  if (po && po->offer == offer) {
    return po;
  }

  puglWaylandFreeOffer(po);
  return NULL;
}

static void
puglWaylandDataDeviceEnter(void* const                  data,
                           struct wl_data_device* const PUGL_UNUSED(device),
                           const uint32_t               serial,
                           struct wl_surface* const     PUGL_UNUSED(surface),
                           const wl_fixed_t             PUGL_UNUSED(x),
                           const wl_fixed_t             PUGL_UNUSED(y),
                           struct wl_data_offer* const  offer)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  /* Drag and drop is not supported (Pugl has no API for it): decline by accepting no MIME type at
     all. The offer still has to be kept until leave or drop, then destroyed, or it leaks. */
  puglWaylandFreeOffer(impl->dndOffer);
  impl->dndOffer = NULL;

  if (offer) {
    impl->dndOffer = puglWaylandClaimOffer(impl, offer);

    // only once the proxy is known to be one we are still holding, see puglWaylandClaimOffer
    if (impl->dndOffer) {
      wl_data_offer_accept(offer, serial, NULL);
    }
  }
}

static void
puglWaylandDataDeviceLeave(void* const                  data,
                           struct wl_data_device* const PUGL_UNUSED(device))
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  puglWaylandFreeOffer(impl->dndOffer);
  impl->dndOffer = NULL;
}

static void
puglWaylandDataDeviceMotion(void* const                  PUGL_UNUSED(data),
                            struct wl_data_device* const PUGL_UNUSED(device),
                            const uint32_t               PUGL_UNUSED(time),
                            const wl_fixed_t             PUGL_UNUSED(x),
                            const wl_fixed_t             PUGL_UNUSED(y))
{
}

static void
puglWaylandDataDeviceDrop(void* const                  data,
                          struct wl_data_device* const PUGL_UNUSED(device))
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  puglWaylandFreeOffer(impl->dndOffer);
  impl->dndOffer = NULL;
}

static void
puglWaylandDataDeviceSelection(void* const                  data,
                               struct wl_data_device* const PUGL_UNUSED(device),
                               struct wl_data_offer* const  offer)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  puglWaylandFreeOffer(impl->selectionOffer);
  impl->selectionOffer = NULL;

  if (offer) {
    impl->selectionOffer = puglWaylandClaimOffer(impl, offer);
  } else {
    // The selection was cleared, so whatever offer was introduced for it is never going to be used
    puglWaylandFreeOffer(impl->unclaimedOffer);
    impl->unclaimedOffer = NULL;
  }
}

static const struct wl_data_device_listener puglWaylandDataDeviceListener = {
  puglWaylandDataDeviceOffer,
  puglWaylandDataDeviceEnter,
  puglWaylandDataDeviceLeave,
  puglWaylandDataDeviceMotion,
  puglWaylandDataDeviceDrop,
  puglWaylandDataDeviceSelection};

/// Append whatever is readable to a receive in progress; sets done at end of stream or on failure
static void
puglWaylandRecvRead(PuglWaylandPipeRecv* const recv)
{
  if (recv->done || recv->status) {
    return;
  }

  if (recv->len == recv->capacity) {
    if (recv->capacity >= (size_t)PUGL_WAYLAND_CLIPBOARD_MAX_SIZE) {
      /* Probe once past the ceiling so an exactly-full transfer can still finish successfully.
         Any additional byte means the payload is too large and the transfer must be rejected. */
      uint8_t       excess = 0U;
      const ssize_t n      = read(recv->fd, &excess, 1U);

      if (n > 0) {
        recv->status = PUGL_FAILURE;
        recv->done   = true;
      } else if (n == 0) {
        recv->done = true;
      } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
        recv->status = PUGL_UNKNOWN_ERROR;
        recv->done   = true;
      }

      return;
    }

    const size_t capacity = MIN(recv->capacity * 2U,
                                (size_t)PUGL_WAYLAND_CLIPBOARD_MAX_SIZE);
    uint8_t* const grown = (uint8_t*)realloc(recv->buffer, capacity);

    if (!grown) {
      recv->status = PUGL_NO_MEMORY;
      recv->done   = true;
      return;
    }

    recv->buffer   = grown;
    recv->capacity = capacity;
  }

  const ssize_t n =
    read(recv->fd, recv->buffer + recv->len, recv->capacity - recv->len);

  if (n > 0) {
    recv->len += (size_t)n;
  } else if (n == 0) {
    recv->done = true; // Writer closed the pipe, transfer complete
  } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
    recv->status = PUGL_UNKNOWN_ERROR;
    recv->done   = true;
  }
}

static PuglStatus
puglWaylandReceiveStatus(const PuglWaylandPipeRecv* const recv,
                         const PuglStatus                  loopStatus)
{
  if (recv->status) {
    return recv->status;
  }

  if (recv->done) {
    return PUGL_SUCCESS;
  }

  if (loopStatus) {
    return loopStatus;
  }

  return PUGL_FAILURE;
}

static void
puglWaylandDataSourceTarget(void* const                  PUGL_UNUSED(data),
                            struct wl_data_source* const PUGL_UNUSED(source),
                            const char* const            PUGL_UNUSED(mimeType))
{
}

/**
   Scoped SIGPIPE suppression for a clipboard write.

   Writing into a pipe whose reader has gone away raises SIGPIPE, which by default kills the process
   -- and this code runs inside somebody else's host, so taking the process down is not an option.
   A library must not install a global SIGPIPE handler either, since that is the application's to
   own.  The portable middle ground (what glib and other libraries do) is to block the signal on
   this thread only for the duration of the writes, consume any instance that went pending, then put
   the old mask back.  EPIPE is then reported through errno as an ordinary write error.
*/
typedef struct {
  sigset_t oldMask;
  bool     blocked;
} PuglWaylandSigpipeGuard;

static void
puglWaylandBlockSigpipe(PuglWaylandSigpipeGuard* const guard)
{
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGPIPE);

  guard->blocked = pthread_sigmask(SIG_BLOCK, &mask, &guard->oldMask) == 0;
}

static void
puglWaylandUnblockSigpipe(PuglWaylandSigpipeGuard* const guard,
                          const bool                     pending)
{
  if (!guard->blocked) {
    return;
  }

  /* Only drain when a write actually failed with EPIPE, and only when the caller was not already
     blocking SIGPIPE -- in that case the signal belongs to them and must be left alone. */
  if (pending && !sigismember(&guard->oldMask, SIGPIPE)) {
    const struct timespec zero = {0, 0};
    sigset_t              pipeOnly;
    sigemptyset(&pipeOnly);
    sigaddset(&pipeOnly, SIGPIPE);

    while (sigtimedwait(&pipeOnly, NULL, &zero) < 0 && errno == EINTR) {
    }
  }

  pthread_sigmask(SIG_SETMASK, &guard->oldMask, NULL);
}

static void
puglWaylandDataSourceSend(void* const                  data,
                          struct wl_data_source* const PUGL_UNUSED(source),
                          const char* const            PUGL_UNUSED(mimeType),
                          const int32_t                fd)
{
  const PuglWaylandSourceData* const sd = (const PuglWaylandSourceData*)data;

  if (!sd || !sd->data.data || !sd->data.len) {
    close(fd);
    return;
  }

  /* The requester might never read, might read slowly, or might be this very client pasting from
     itself.  A blocking write would wedge the GUI thread as soon as the payload outgrows the pipe
     buffer, so the fd goes non-blocking and every wait is bounded. */
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  PuglWaylandSigpipeGuard guard;
  puglWaylandBlockSigpipe(&guard);

  const int64_t deadline =
    puglWaylandMonotonicMs() + PUGL_WAYLAND_CLIPBOARD_SEND_TIMEOUT_MS;

  const char* pos        = (const char*)sd->data.data;
  size_t      remaining  = sd->data.len;
  bool        gotSigpipe = false;

  while (remaining > 0) {
    const ssize_t written = write(fd, pos, remaining);

    if (written > 0) {
      pos += (size_t)written;
      remaining -= (size_t)written;
      continue;
    }

    if (written < 0 && errno == EINTR) {
      continue;
    }

    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      const int64_t now = puglWaylandMonotonicMs();
      if (now >= deadline) {
        break; // Requester is not draining the pipe, abort rather than hang
      }

      /* If a paste of our own is in flight, this handler was reached from inside its read loop and
         that loop is not going to run again until we return.  Drain its end of the pipe here, or a
         payload larger than the pipe buffer could never get through. */
      PuglWaylandPipeRecv* const recv =
        sd->wimpl ? sd->wimpl->activeRecv : NULL;
      const bool alsoRecv = recv && !recv->done;

      struct pollfd pfds[2];
      pfds[0].fd      = fd;
      pfds[0].events  = POLLOUT;
      pfds[0].revents = 0;
      if (alsoRecv) {
        pfds[1].fd      = recv->fd;
        pfds[1].events  = POLLIN;
        pfds[1].revents = 0;
      }

      const int ret = poll(pfds, alsoRecv ? 2 : 1, (int)(deadline - now));

      if (ret < 0) {
        if (errno == EINTR) {
          continue;
        }
        break;
      }

      if (alsoRecv && (pfds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
        puglWaylandRecvRead(recv);
        if (recv->status) {
          break;
        }
      }

      if (ret > 0 && !(pfds[0].revents & POLLOUT) &&
          (pfds[0].revents & (POLLERR | POLLHUP))) {
        break; // The reader is gone
      }

      continue;
    }

    // Any other error (EPIPE in particular) means the transfer is over
    gotSigpipe = written < 0 && errno == EPIPE;
    break;
  }

  puglWaylandUnblockSigpipe(&guard, gotSigpipe);

  close(fd);
}

static void
puglWaylandDataSourceCancelled(void* const                  data,
                               struct wl_data_source* const source)
{
  PuglWaylandSourceData* const sd    = (PuglWaylandSourceData*)data;
  PuglWorldInternals* const    wimpl = sd ? sd->wimpl : NULL;

  /* Only stop tracking if the world still points at *this* source: a cancelled event usually means
     puglSetClipboard() has already put a newer one in its place, which must not be forgotten. */
  if (wimpl && wimpl->dataSource == source) {
    wimpl->dataSource     = NULL;
    wimpl->dataSourceData = NULL;
  }

  wl_data_source_destroy(source);

  if (sd) {
    free(sd->data.data);
    free(sd);
  }
}

static void
puglWaylandDataSourceDndDropPerformed(
  void* const                  PUGL_UNUSED(data),
  struct wl_data_source* const PUGL_UNUSED(source))
{
}

static void
puglWaylandDataSourceDndFinished(void* const                  PUGL_UNUSED(data),
                                 struct wl_data_source* const PUGL_UNUSED(s))
{
}

static void
puglWaylandDataSourceAction(void* const                  PUGL_UNUSED(data),
                            struct wl_data_source* const PUGL_UNUSED(source),
                            const uint32_t               PUGL_UNUSED(dndAction))
{
}

static const struct wl_data_source_listener puglWaylandDataSourceListener = {
  puglWaylandDataSourceTarget,
  puglWaylandDataSourceSend,
  puglWaylandDataSourceCancelled,
  puglWaylandDataSourceDndDropPerformed,
  puglWaylandDataSourceDndFinished,
  puglWaylandDataSourceAction};

static void
puglWaylandClearClipboard(PuglWaylandClipboard* const board)
{
  for (uint32_t i = 0; i < board->numFormats; ++i) {
    free(board->formatStrings[i]);
    board->formatStrings[i] = NULL;
  }

  board->numFormats          = 0U;
  board->acceptedFormatIndex = UINT32_MAX;
}

/**
   Translate a Wayland MIME type into the name Pugl reports to the application.

   Returns NULL for types that are not worth reporting.  The parameter suffix of a MIME type is
   dropped ("text/plain;charset=utf-8" becomes "text/plain") because portable Pugl applications --
   DGL's Window::onClipboardDataOffer among them -- compare against bare type names.
*/
static char*
puglWaylandReportedType(const char* const mimeType)
{
  const char* const slash = strchr(mimeType, '/');

  if (!slash) {
    if (!strcmp(mimeType, "UTF8_STRING") || !strcmp(mimeType, "STRING") ||
        !strcmp(mimeType, "TEXT")) {
      char* const out = (char*)calloc(11U, 1U);
      if (out) {
        memcpy(out, "text/plain", 10U);
      }
      return out;
    }

    return NULL;
  }

  const char* const semi = strchr(mimeType, ';');
  const size_t      len  = semi ? (size_t)(semi - mimeType) : strlen(mimeType);
  char* const       out  = (char*)calloc(len + 1U, 1U);

  if (out) {
    memcpy(out, mimeType, len);
  }

  return out;
}

static bool
puglWaylandHasFormat(const PuglWaylandClipboard* const board,
                     const char* const                 name)
{
  for (uint32_t i = 0; i < board->numFormats; ++i) {
    if (!strcmp(board->formatStrings[i], name)) {
      return true;
    }
  }

  return false;
}

/**
   Fill a view's clipboard board from an offer's MIME list.

   Two passes so that a UTF-8 flavour of a type always wins over a plain one: DAF only ever asks for
   "text/plain", and receiving that as UTF-8 is what it expects.
*/
static PuglStatus
puglWaylandSetClipboardFormats(PuglWaylandClipboard* const board,
                               const PuglWaylandOffer* const offer)
{
  puglWaylandClearClipboard(board);

  char** const strings =
    (char**)realloc(board->formatStrings, offer->numMimeTypes * sizeof(char*));

  if (!strings && offer->numMimeTypes) {
    return PUGL_NO_MEMORY;
  }

  board->formatStrings = strings;

  for (int pass = 0; pass < 2; ++pass) {
    for (uint32_t i = 0; i < offer->numMimeTypes; ++i) {
      const char* const mime  = offer->mimeTypes[i];
      const bool        isUtf = strstr(mime, "utf") != NULL ||
                         strstr(mime, "UTF") != NULL;

      if ((pass == 0) != isUtf) {
        continue;
      }

      char* const name = puglWaylandReportedType(mime);
      if (!name) {
        continue;
      }

      if (puglWaylandHasFormat(board, name)) {
        free(name);
        continue;
      }

      board->formatStrings[board->numFormats++] = name;
    }
  }

  return PUGL_SUCCESS;
}

/**
   Read everything from a pipe the clipboard owner is writing into.

   The display connection is pumped alongside the pipe, which is not an optimisation but a
   correctness requirement: when this client owns the selection (pasting from itself, the common
   case for a plugin UI's own copy/paste) the compositor routes the wl_data_source.send request
   straight back to us.  Nobody else is going to dispatch it -- the GUI thread is right here -- so
   blocking on the pipe alone would deadlock until the timeout, hand back nothing, and leave a
   doomed write to a closed pipe queued up for the next dispatch.
*/
static PuglStatus
puglWaylandReadPipe(PuglWorldInternals* const impl,
                    const int                 fd,
                    PuglBlob* const           out)
{
  struct wl_display* const display = impl->display;

  PuglWaylandPipeRecv recv;
  recv.fd       = fd;
  recv.capacity = 4096U;
  recv.len      = 0U;
  recv.done     = false;
  recv.status   = PUGL_SUCCESS;
  recv.buffer   = (uint8_t*)malloc(recv.capacity);

  if (!recv.buffer) {
    return PUGL_NO_MEMORY;
  }

  /* Two places drain this fd now -- the loop below and, for a self-paste, the send handler it
     dispatches -- so a read that races the other one must not block. */
  const int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) {
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  // Publish it so our own wl_data_source.send handler can drain this end while it writes
  PuglWaylandPipeRecv* const previousRecv = impl->activeRecv;
  impl->activeRecv                        = &recv;

  const int displayFd = wl_display_get_fd(display);

  /* One deadline for the whole transfer, never extended by progress: a peer that drips a byte at a
     time is just as able to wedge the GUI thread as one that stops writing entirely. */
  const int64_t deadline =
    puglWaylandMonotonicMs() + PUGL_WAYLAND_CLIPBOARD_RECV_TIMEOUT_MS;

  PuglStatus status = PUGL_SUCCESS;

  while (!recv.done) {
    // Same prepare_read/poll/read_events dance as puglWaylandDispatchEvents, plus the pipe
    if (wl_display_dispatch_pending(display) < 0) {
      status = PUGL_UNKNOWN_ERROR;
      break;
    }

    if (recv.done || recv.status) {
      break;
    }

    bool readPrepared = true;
    while (wl_display_prepare_read(display) != 0) {
      if (wl_display_dispatch_pending(display) < 0) {
        readPrepared = false;
        status       = PUGL_UNKNOWN_ERROR;
        break;
      }

      if (recv.done || recv.status) {
        break;
      }
    }

    if (!readPrepared || recv.done || recv.status) {
      break;
    }

    if (wl_display_flush(display) < 0 && errno != EAGAIN) {
      wl_display_cancel_read(display);
      status = PUGL_UNKNOWN_ERROR;
      break;
    }

    const int64_t now = puglWaylandMonotonicMs();
    if (now >= deadline) {
      wl_display_cancel_read(display);
      status = PUGL_FAILURE;
      break;
    }

    struct pollfd pfds[2];
    pfds[0].fd      = fd;
    pfds[0].events  = POLLIN;
    pfds[0].revents = 0;
    pfds[1].fd      = displayFd;
    pfds[1].events  = POLLIN;
    pfds[1].revents = 0;

    const int ret = poll(pfds, 2, (int)(deadline - now));

    if (ret < 0) {
      wl_display_cancel_read(display);
      if (errno == EINTR) {
        continue;
      }
      status = PUGL_UNKNOWN_ERROR;
      break;
    }

    if (ret == 0) {
      wl_display_cancel_read(display);
      status = PUGL_FAILURE;
      break;
    }

    if (pfds[1].revents & (POLLIN | POLLERR | POLLHUP)) {
      // On failure read_events releases the read intent itself, so no cancel here
      if (wl_display_read_events(display) < 0) {
        status = PUGL_UNKNOWN_ERROR;
        break;
      }
    } else {
      wl_display_cancel_read(display);
    }

    /* This is what lets a self-paste complete: our own wl_data_source.send handler runs from here,
       and for anything bigger than the pipe buffer it drains recv itself as it goes. */
    if (wl_display_dispatch_pending(display) < 0) {
      status = PUGL_UNKNOWN_ERROR;
      break;
    }

    if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
      puglWaylandRecvRead(&recv);
    }

    if (recv.status) {
      status = recv.status;
      break;
    }
  }

  impl->activeRecv = previousRecv;

  status = puglWaylandReceiveStatus(&recv, status);

  if (status) {
    free(recv.buffer);
    return status;
  }

  const PuglStatus st = puglSetBlob(out, recv.buffer, recv.len);
  free(recv.buffer);
  return st;
}

// --------------------------------------------------------------------------------------------
// Cursors

static const char* const puglWaylandCursorNames[PUGL_NUM_CURSORS] = {
  "default",           // ARROW
  "text",              // CARET
  "crosshair",         // CROSSHAIR
  "pointer",           // HAND
  "not-allowed",       // NO
  "sb_h_double_arrow", // LEFT_RIGHT
  "sb_v_double_arrow", // UP_DOWN
  "size_fdiag",        // UP_LEFT_DOWN_RIGHT
  "size_bdiag",        // UP_RIGHT_DOWN_LEFT
  "all-scroll",        // ALL_SCROLL
  NULL,                // NONE, hidden with a null surface instead of a theme image
};

/// Legacy X11 cursor names, for themes that predate the XDG cursor naming spec
static const char* const puglWaylandLegacyCursorNames[PUGL_NUM_CURSORS] = {
  "left_ptr",
  "xterm",
  "crosshair",
  "hand2",
  "crossed_circle",
  "sb_h_double_arrow",
  "sb_v_double_arrow",
  "bottom_right_corner",
  "bottom_left_corner",
  "fleur",
  NULL,
};

static bool
puglWaylandLoadCursorTheme(PuglWorldInternals* const impl)
{
  /* Both halves are needed: the theme supplies the wl_buffer, the surface is what it is attached to.
     Checking only the theme would let a previous run that loaded the theme but failed to create the
     surface report success, and puglWaylandApplyCursor() would then attach to a NULL surface. */
  if (impl->cursorTheme && impl->cursorSurface) {
    return true;
  }

  if (impl->cursorLoadFailed || !impl->shm || !impl->compositor) {
    return false;
  }

  if (!impl->cursorTheme) {
    const char* const sizeEnv = getenv("XCURSOR_SIZE");
    if (sizeEnv) {
      const int size = atoi(sizeEnv);
      if (size > 0) {
        impl->cursorSize = size;
      }
    }

    impl->cursorTheme = wl_cursor_theme_load(
      getenv("XCURSOR_THEME"), impl->cursorSize, impl->shm);

    if (!impl->cursorTheme) {
      impl->cursorLoadFailed = true;
      return false;
    }
  }

  if (!impl->cursorSurface) {
    impl->cursorSurface = wl_compositor_create_surface(impl->compositor);
  }

  return impl->cursorSurface != NULL;
}

static PuglStatus
puglWaylandApplyCursor(PuglWorldInternals* const impl, const PuglCursor cursor)
{
  if (!impl->pointer) {
    return PUGL_FAILURE;
  }

  const unsigned index = (unsigned)cursor;
  if (index >= PUGL_NUM_CURSORS) {
    return PUGL_BAD_PARAMETER;
  }

  /* Hiding needs no theme and no surface: a null surface *is* the hidden pointer, so this is
     handled before the theme is loaded, and works on a compositor with no cursor theme at all. */
  if (cursor == PUGL_CURSOR_NONE) {
    wl_pointer_set_cursor(impl->pointer, impl->pointerEnterSerial, NULL, 0, 0);
    return PUGL_SUCCESS;
  }

  if (!puglWaylandLoadCursorTheme(impl)) {
    return PUGL_UNSUPPORTED;
  }

  struct wl_cursor* wlCursor = wl_cursor_theme_get_cursor(
    impl->cursorTheme, puglWaylandCursorNames[index]);

  if (!wlCursor) {
    wlCursor = wl_cursor_theme_get_cursor(
      impl->cursorTheme, puglWaylandLegacyCursorNames[index]);
  }

  if (!wlCursor || wlCursor->image_count == 0) {
    return PUGL_FAILURE;
  }

  struct wl_cursor_image* const image  = wlCursor->images[0];
  struct wl_buffer* const       buffer = wl_cursor_image_get_buffer(image);

  if (!buffer) {
    return PUGL_FAILURE;
  }

  wl_surface_attach(impl->cursorSurface, buffer, 0, 0);
  puglWaylandDamageSurface(
    impl->cursorSurface, (int32_t)image->width, (int32_t)image->height);
  wl_surface_commit(impl->cursorSurface);

  wl_pointer_set_cursor(impl->pointer,
                        impl->pointerEnterSerial,
                        impl->cursorSurface,
                        (int32_t)image->hotspot_x,
                        (int32_t)image->hotspot_y);

  return PUGL_SUCCESS;
}

// --------------------------------------------------------------------------------------------
// Registry

static void
puglWaylandShmFormat(void* const          PUGL_UNUSED(data),
                     struct wl_shm* const PUGL_UNUSED(shm),
                     const uint32_t       PUGL_UNUSED(format))
{
  /* Only ARGB8888 is used (by wayland_cairo.c), and wl_shm is required to support it, so the
     advertised format list is of no interest.  The listener exists purely so that libwayland does
     not have to discard the events. */
}

static const struct wl_shm_listener puglWaylandShmListener = {
  puglWaylandShmFormat};

static void
puglWaylandRegistryGlobal(void* const               data,
                          struct wl_registry* const registry,
                          const uint32_t            name,
                          const char* const         interface,
                          const uint32_t            version)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  if (!strcmp(interface, wl_compositor_interface.name)) {
    impl->compositor = (struct wl_compositor*)wl_registry_bind(
      registry,
      name,
      &wl_compositor_interface,
      MIN(version, PUGL_WAYLAND_COMPOSITOR_VERSION));

  } else if (!strcmp(interface, wl_shm_interface.name)) {
    impl->shm =
      (struct wl_shm*)wl_registry_bind(registry, name, &wl_shm_interface, 1U);
    wl_shm_add_listener(impl->shm, &puglWaylandShmListener, impl);

  } else if (!strcmp(interface, xdg_wm_base_interface.name)) {
    impl->wmBase = (struct xdg_wm_base*)wl_registry_bind(
      registry,
      name,
      &xdg_wm_base_interface,
      MIN(version, PUGL_WAYLAND_WM_BASE_VERSION));
    xdg_wm_base_add_listener(
      impl->wmBase, &puglWaylandWmBaseListener, impl);

  } else if (!strcmp(interface, wl_seat_interface.name)) {
    if (!impl->seat) {
      impl->seat = (struct wl_seat*)wl_registry_bind(
        registry,
        name,
        &wl_seat_interface,
        MIN(version, PUGL_WAYLAND_SEAT_VERSION));
      wl_seat_add_listener(impl->seat, &puglWaylandSeatListener, impl);
    }

  } else if (!strcmp(interface, wl_output_interface.name)) {
    if (impl->numOutputs < PUGL_WAYLAND_MAX_OUTPUTS) {
      PuglWaylandOutput* const out = &impl->outputs[impl->numOutputs];

      out->output = (struct wl_output*)wl_registry_bind(
        registry,
        name,
        &wl_output_interface,
        MIN(version, PUGL_WAYLAND_OUTPUT_VERSION));
      out->wimpl        = impl;
      out->globalName   = name;
      out->scale        = 1;
      out->scaleChanged = false;

      wl_output_add_listener(out->output, &puglWaylandOutputListener, out);
      ++impl->numOutputs;
    }

  } else if (!strcmp(interface, wl_data_device_manager_interface.name)) {
    impl->dataDeviceManager =
      (struct wl_data_device_manager*)wl_registry_bind(
        registry,
        name,
        &wl_data_device_manager_interface,
        MIN(version, PUGL_WAYLAND_DATA_DEVICE_MANAGER_VERSION));

  } else if (!strcmp(interface, wp_viewporter_interface.name)) {
    impl->viewporter = (struct wp_viewporter*)wl_registry_bind(
      registry, name, &wp_viewporter_interface, 1U);

  } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
    impl->fractionalScaleManager =
      (struct wp_fractional_scale_manager_v1*)wl_registry_bind(
        registry, name, &wp_fractional_scale_manager_v1_interface, 1U);

  } else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name)) {
    impl->decorationManager =
      (struct zxdg_decoration_manager_v1*)wl_registry_bind(
        registry, name, &zxdg_decoration_manager_v1_interface, 1U);

  } else if (!strcmp(interface, zxdg_exporter_v2_interface.name)) {
    impl->exporter = (struct zxdg_exporter_v2*)wl_registry_bind(
      registry, name, &zxdg_exporter_v2_interface, 1U);
  }
}

static void
puglWaylandRegistryGlobalRemove(void* const               data,
                                struct wl_registry* const PUGL_UNUSED(registry),
                                const uint32_t            name)
{
  PuglWorldInternals* const impl = (PuglWorldInternals*)data;

  for (uint32_t i = 0; i < impl->numOutputs; ++i) {
    if (impl->outputs[i].globalName != name) {
      continue;
    }

    struct wl_output* const output = impl->outputs[i].output;

    /* Every view that has entered this output holds a bare proxy pointer, so they all have to let
       go of it (and settle on a new scale) while it is still valid. */
    if (impl->world) {
      for (size_t v = 0; v < impl->world->numViews; ++v) {
        PuglView* const view = impl->world->views[v];
        if (view->impl) {
          puglWaylandForgetOutput(view, output);
        }
      }
    }

    wl_output_destroy(output);

    /* Compacting the array moves the last entry into this slot, which invalidates the listener data
       pointer wl_output_add_listener() was given for it: repoint it, or wl_output.scale would write
       to a stale (and possibly later reused) slot. */
    impl->outputs[i] = impl->outputs[impl->numOutputs - 1U];
    memset(&impl->outputs[impl->numOutputs - 1U], 0, sizeof(PuglWaylandOutput));
    --impl->numOutputs;

    if (impl->outputs[i].output) {
      wl_output_set_user_data(impl->outputs[i].output, &impl->outputs[i]);
    }

    return;
  }
}

static const struct wl_registry_listener puglWaylandRegistryListener = {
  puglWaylandRegistryGlobal, puglWaylandRegistryGlobalRemove};

// --------------------------------------------------------------------------------------------
// World

static void
puglWaylandDestroyWorldInternals(PuglWorldInternals* const impl)
{
  if (!impl) {
    return;
  }

  puglWaylandFreeOffer(impl->selectionOffer);
  puglWaylandFreeOffer(impl->unclaimedOffer);
  puglWaylandFreeOffer(impl->dndOffer);

  /* If this client still owns the selection, nobody is going to deliver the cancelled event that
     would normally clean the source up. */
  if (impl->dataSource) {
    wl_data_source_destroy(impl->dataSource);
    impl->dataSource = NULL;
  }
  if (impl->dataSourceData) {
    free(impl->dataSourceData->data.data);
    free(impl->dataSourceData);
    impl->dataSourceData = NULL;
  }

#if PUGL_WAYLAND_HAVE_TIMERFD
  if (impl->repeat.fd >= 0) {
    close(impl->repeat.fd);
  }
#endif

  for (size_t i = 0; i < impl->numTimers; ++i) {
    if (impl->timers[i].fd >= 0) {
      close(impl->timers[i].fd);
    }
  }
  free(impl->timers);
  free(impl->pollFds);

  if (impl->xkb.composeState) {
    xkb_compose_state_unref(impl->xkb.composeState);
  }
  if (impl->xkb.composeTable) {
    xkb_compose_table_unref(impl->xkb.composeTable);
  }
  if (impl->xkb.state) {
    xkb_state_unref(impl->xkb.state);
  }
  if (impl->xkb.keymap) {
    xkb_keymap_unref(impl->xkb.keymap);
  }
  if (impl->xkb.context) {
    xkb_context_unref(impl->xkb.context);
  }

  if (impl->cursorSurface) {
    wl_surface_destroy(impl->cursorSurface);
  }
  if (impl->cursorTheme) {
    wl_cursor_theme_destroy(impl->cursorTheme);
  }

  if (impl->keyboard) {
    if (wl_keyboard_get_version(impl->keyboard) >=
        WL_KEYBOARD_RELEASE_SINCE_VERSION) {
      wl_keyboard_release(impl->keyboard);
    } else {
      wl_keyboard_destroy(impl->keyboard);
    }
  }
  if (impl->pointer) {
    if (wl_pointer_get_version(impl->pointer) >=
        WL_POINTER_RELEASE_SINCE_VERSION) {
      wl_pointer_release(impl->pointer);
    } else {
      wl_pointer_destroy(impl->pointer);
    }
  }
  if (impl->dataDevice) {
    if (wl_data_device_get_version(impl->dataDevice) >=
        WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
      wl_data_device_release(impl->dataDevice);
    } else {
      wl_data_device_destroy(impl->dataDevice);
    }
  }
  if (impl->seat) {
    if (wl_seat_get_version(impl->seat) >= WL_SEAT_RELEASE_SINCE_VERSION) {
      wl_seat_release(impl->seat);
    } else {
      wl_seat_destroy(impl->seat);
    }
  }

  for (uint32_t i = 0; i < impl->numOutputs; ++i) {
    wl_output_destroy(impl->outputs[i].output);
  }

  if (impl->exporter) {
    zxdg_exporter_v2_destroy(impl->exporter);
  }
  if (impl->decorationManager) {
    zxdg_decoration_manager_v1_destroy(impl->decorationManager);
  }
  if (impl->fractionalScaleManager) {
    wp_fractional_scale_manager_v1_destroy(impl->fractionalScaleManager);
  }
  if (impl->viewporter) {
    wp_viewporter_destroy(impl->viewporter);
  }
  if (impl->dataDeviceManager) {
    wl_data_device_manager_destroy(impl->dataDeviceManager);
  }
  if (impl->wmBase) {
    xdg_wm_base_destroy(impl->wmBase);
  }
  if (impl->shm) {
    wl_shm_destroy(impl->shm);
  }
  if (impl->compositor) {
    wl_compositor_destroy(impl->compositor);
  }
  if (impl->registry) {
    wl_registry_destroy(impl->registry);
  }

  if (impl->display) {
    wl_display_disconnect(impl->display);
  }

  free(impl);
}

PuglWorldInternals*
puglInitWorldInternals(const PuglWorldType PUGL_UNUSED(type),
                       const PuglWorldFlags PUGL_UNUSED(flags))
{
  struct wl_display* const display = wl_display_connect(NULL);

  if (!display) {
    d_stderr("pugl: failed to connect to a Wayland display (WAYLAND_DISPLAY=%s)",
             getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "<unset>");
    return NULL;
  }

  PuglWorldInternals* const impl =
    (PuglWorldInternals*)calloc(1, sizeof(PuglWorldInternals));

  if (!impl) {
    wl_display_disconnect(display);
    return NULL;
  }

  impl->display     = display;
  impl->scaleFactor = 1.0;
  impl->cursorSize  = 24;
  impl->repeat.fd   = -1;
  impl->repeat.rate = 25;
  impl->repeat.delay = 400;

  impl->registry = wl_display_get_registry(display);
  wl_registry_add_listener(
    impl->registry, &puglWaylandRegistryListener, impl);

  // First roundtrip collects the globals, second one their initial events
  wl_display_roundtrip(display);
  wl_display_roundtrip(display);

  if (!impl->compositor || !impl->wmBase) {
    d_stderr("pugl: Wayland compositor does not provide %s, cannot create windows",
             impl->compositor ? "xdg_wm_base" : "wl_compositor");
    puglWaylandDestroyWorldInternals(impl);
    return NULL;
  }

  if (impl->dataDeviceManager && impl->seat) {
    impl->dataDevice = wl_data_device_manager_get_data_device(
      impl->dataDeviceManager, impl->seat);
    wl_data_device_add_listener(
      impl->dataDevice, &puglWaylandDataDeviceListener, impl);
  }

  impl->xkb.context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  if (impl->xkb.context) {
    const char* locale = getenv("LC_ALL");
    if (!locale || !*locale) {
      locale = getenv("LC_CTYPE");
    }
    if (!locale || !*locale) {
      locale = getenv("LANG");
    }
    if (!locale || !*locale) {
      locale = "C";
    }

    impl->xkb.composeTable = xkb_compose_table_new_from_locale(
      impl->xkb.context, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);

    if (impl->xkb.composeTable) {
      impl->xkb.composeState = xkb_compose_state_new(
        impl->xkb.composeTable, XKB_COMPOSE_STATE_NO_FLAGS);
    }
  }

#if PUGL_WAYLAND_HAVE_TIMERFD
  impl->repeat.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
#endif

  return impl;
}

void*
puglGetNativeWorld(PuglWorld* const world)
{
  return world->impl->display;
}

void
puglFreeWorldInternals(PuglWorld* const world)
{
  if (!world || !world->impl) {
    return;
  }

  puglWaylandDestroyWorldInternals(world->impl);
  world->impl = NULL;
}

// --------------------------------------------------------------------------------------------
// View lifecycle

PuglInternals*
puglInitViewInternals(PuglWorld* const world)
{
  PuglInternals* const impl = (PuglInternals*)calloc(1, sizeof(PuglInternals));

  if (!impl) {
    return NULL;
  }

  /* puglInitWorldInternals() is not handed the world, so this is the first opportunity to record
     it.  Doing it here is enough for everything that needs it, which is the listeners that walk
     world->views[]: those can do nothing useful before a view exists anyway. */
  world->impl->world = world;

  impl->scale                     = world->impl->scaleFactor;
  impl->bufferScale               = 1;
  impl->cursor                    = PUGL_CURSOR_ARROW;
  impl->clipboard.acceptedFormatIndex = UINT32_MAX;

  return impl;
}

PuglPoint
puglGetAncestorCenter(const PuglView* const PUGL_UNUSED(view))
{
  /* Wayland clients are not told where they, their parents, or the outputs are, so there is no
     meaningful centre to return.  Positions are ignored entirely (see puglSetWindowPosition). */
  const PuglPoint center = {0, 0};
  return center;
}

/// Find the view whose surface matches a native handle, used to resolve transient parents
static PuglView*
puglWaylandFindViewByNative(const PuglWorld* const world,
                            const PuglNativeView   native)
{
  if (!native) {
    return NULL;
  }

  for (size_t i = 0; i < world->numViews; ++i) {
    PuglView* const other = world->views[i];
    if (other->impl && (PuglNativeView)(uintptr_t)other->impl->wlSurface ==
                         native) {
      return other;
    }
  }

  return NULL;
}

static void
puglWaylandApplyTransientParent(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (!impl->xdgToplevel) {
    return;
  }

  PuglView* const parent =
    puglWaylandFindViewByNative(view->world, view->transientParent);

  if (parent && parent->impl->xdgToplevel) {
    xdg_toplevel_set_parent(impl->xdgToplevel, parent->impl->xdgToplevel);
  } else if (view->transientParent) {
    d_debug("pugl: ignoring transient parent %p, not a view of this process",
            (void*)(uintptr_t)view->transientParent);
  }
}

/* Title, app id and transient parent, as the application last set them.  Realize applies them to
   a new toplevel, and puglShow() again after puglHide(): xdg-shell discards a toplevel's
   attributes when it is unmapped, so a window shown again would otherwise come back untitled,
   unparented and without its size limits (puglUpdateSizeHints() re-sends those). */
static void
puglWaylandApplyToplevelAttributes(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (!impl->xdgToplevel) {
    return;
  }

  if (view->strings[PUGL_WINDOW_TITLE]) {
    xdg_toplevel_set_title(impl->xdgToplevel, view->strings[PUGL_WINDOW_TITLE]);
  }

  const char* const appId = impl->appId                          ? impl->appId
                            : view->strings[PUGL_CLASS_NAME]       ? view->strings[PUGL_CLASS_NAME]
                                                                   : view->world->strings[PUGL_CLASS_NAME];
  if (appId) {
    xdg_toplevel_set_app_id(impl->xdgToplevel, appId);
  }

  puglWaylandApplyTransientParent(view);
}

/**
   Destroy every protocol object hanging off a view's wl_surface, in reverse creation order.

   Shared by puglUnrealize() and puglRealize()'s failure path.  The latter used to leave the
   viewport, fractional scale and decoration objects behind, which made a retry fatal: their
   surfaces were gone, so wp_viewport.set_destination raised the no_surface protocol error.
*/
static void
puglWaylandDestroyViewSurface(PuglInternals* const impl)
{
  if (impl->frameCallback) {
    wl_callback_destroy(impl->frameCallback);
    impl->frameCallback = NULL;
  }
  if (impl->fractionalScale) {
    wp_fractional_scale_v1_destroy(impl->fractionalScale);
    impl->fractionalScale = NULL;
  }
  if (impl->viewport) {
    wp_viewport_destroy(impl->viewport);
    impl->viewport = NULL;
  }
  if (impl->decoration) {
    zxdg_toplevel_decoration_v1_destroy(impl->decoration);
    impl->decoration = NULL;
  }
  if (impl->exported) {
    zxdg_exported_v2_destroy(impl->exported);
    impl->exported = NULL;
  }
  free(impl->exportedHandle);
  impl->exportedHandle = NULL;
  if (impl->xdgToplevel) {
    xdg_toplevel_destroy(impl->xdgToplevel);
    impl->xdgToplevel = NULL;
  }
  if (impl->xdgSurface) {
    xdg_surface_destroy(impl->xdgSurface);
    impl->xdgSurface = NULL;
  }
  if (impl->wlSurface) {
    wl_surface_destroy(impl->wlSurface);
    impl->wlSurface = NULL;
  }

  impl->configured               = false;
  impl->geometryDirty            = false;
  impl->unmapped                 = false;
  impl->frameCallbackWorks       = false;
  impl->needsRedisplay           = false;
  impl->numEnteredOutputs        = 0U;
  impl->preferredFractionalScale = 0U;
}

PuglStatus
puglRealize(PuglView* const view)
{
  PuglInternals* const      impl  = view->impl;
  PuglWorld* const          world = view->world;
  PuglWorldInternals* const wimpl = world->impl;
  PuglStatus                st    = PUGL_SUCCESS;

  if (impl->wlSurface) {
    return PUGL_FAILURE;
  }

  if ((st = puglPreRealize(view))) {
    return st;
  }

  if (view->parent) {
    /* Wayland has no cross-client embedding: there is no protocol by which a host can hand out a
       surface for a plugin to draw into.  Rather than fail (which would leave the UI with no window
       at all), fall back to a top level window and say so once. */
    d_debug("pugl: Wayland has no window embedding, ignoring parent %p and "
            "creating a top level window instead",
            (void*)(uintptr_t)view->parent);
  }

  puglEnsureHint(view, PUGL_IGNORE_KEY_REPEAT, PUGL_FALSE);
  puglEnsureHint(view, PUGL_RESIZABLE, PUGL_TRUE);
  puglEnsureHint(view, PUGL_VIEW_TYPE, PUGL_VIEW_TYPE_NORMAL);

  impl->wlSurface = wl_compositor_create_surface(wimpl->compositor);
  if (!impl->wlSurface) {
    return PUGL_REALIZE_FAILED;
  }

  wl_surface_set_user_data(impl->wlSurface, view);
  wl_surface_add_listener(
    impl->wlSurface, &puglWaylandSurfaceEventListener, view);

  impl->xdgSurface =
    xdg_wm_base_get_xdg_surface(wimpl->wmBase, impl->wlSurface);
  if (!impl->xdgSurface) {
    puglWaylandDestroyViewSurface(impl);
    return PUGL_REALIZE_FAILED;
  }

  xdg_surface_add_listener(
    impl->xdgSurface, &puglWaylandSurfaceListener, view);

  impl->xdgToplevel = xdg_surface_get_toplevel(impl->xdgSurface);
  xdg_toplevel_add_listener(
    impl->xdgToplevel, &puglWaylandToplevelListener, view);

  // Fractional scaling needs a viewport to scale the buffer down into the logical size
  if (wimpl->viewporter) {
    impl->viewport =
      wp_viewporter_get_viewport(wimpl->viewporter, impl->wlSurface);
  }

  if (wimpl->fractionalScaleManager && impl->viewport) {
    impl->fractionalScale = wp_fractional_scale_manager_v1_get_fractional_scale(
      wimpl->fractionalScaleManager, impl->wlSurface);
    wp_fractional_scale_v1_add_listener(
      impl->fractionalScale, &puglWaylandFractionalScaleListener, view);
  }

  if (wimpl->decorationManager) {
    impl->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
      wimpl->decorationManager, impl->xdgToplevel);
    zxdg_toplevel_decoration_v1_add_listener(
      impl->decoration, &puglWaylandDecorationListener, view);
    zxdg_toplevel_decoration_v1_set_mode(
      impl->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }

  /* Exported eagerly rather than on demand: the handle comes back in a later event, and the point
     where an application wants it (opening a portal dialog) is too late to start waiting. */
  if (wimpl->exporter) {
    impl->exported =
      zxdg_exporter_v2_export_toplevel(wimpl->exporter, impl->wlSurface);

    if (impl->exported) {
      zxdg_exported_v2_add_listener(
        impl->exported, &puglWaylandExportedListener, view);
    }
  }

  puglWaylandApplyToplevelAttributes(view);

  // Settle on an initial size before the compositor gets a chance to ask for one
  puglWaylandUpdateScale(view);
  puglWaylandSetSize(view, impl->requestedLogicalSize);
  puglUpdateSizeHints(view);

  // Configure and create the drawing surface
  if ((st = view->backend->configure(view)) || (st = view->backend->create(view))) {
    view->backend->destroy(view);
    puglWaylandDestroyViewSurface(impl);
    return st;
  }

  /* An initial commit with no buffer is how a client asks xdg-shell for its first configure.  The
     window does not appear until a buffer is attached, which happens on the first expose. */
  wl_surface_commit(impl->wlSurface);
  wl_display_flush(wimpl->display);

  return puglDispatchSimpleEvent(view, PUGL_REALIZE);
}

PuglStatus
puglUnrealize(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (!impl || !impl->wlSurface) {
    return PUGL_FAILURE;
  }

  puglDispatchSimpleEvent(view, PUGL_UNREALIZE);

  PuglWorldInternals* const wimpl = view->world->impl;
  if (wimpl->pointerFocus == view) {
    wimpl->pointerFocus = NULL;
  }
  if (wimpl->keyboardFocus == view) {
    wimpl->keyboardFocus = NULL;
  }
  if (wimpl->repeat.view == view) {
    puglWaylandStopRepeat(wimpl);
  }

  puglWaylandClearClipboard(&impl->clipboard);

  if (view->backend) {
    view->backend->destroy(view);
  }

  puglWaylandDestroyViewSurface(impl);

  memset(&view->lastConfigure, 0, sizeof(PuglConfigureEvent));
  memset(&impl->pendingConfigure, 0, sizeof(PuglEvent));
  memset(&impl->pendingExpose, 0, sizeof(PuglEvent));

  return PUGL_SUCCESS;
}

PuglStatus
puglShow(PuglView* const view, const PuglShowCommand PUGL_UNUSED(command))
{
  PuglInternals* const impl = view->impl;
  PuglStatus st = impl->wlSurface ? PUGL_SUCCESS : puglRealize(view);

  if (st) {
    return st;
  }

  /* There is no "raise" on Wayland: window stacking and focus are entirely the compositor's
     business, and a client cannot put itself in front.  All three show commands therefore do the
     same thing, which is to make sure a buffer gets attached. */
  impl->visible = true;

  /* puglHide() unmapped the surface, and xdg-shell treats an unmapped surface like a new one: it
     needs another bufferless commit and a fresh configure before a buffer may be attached.  Without
     this the next expose attaches straight away, which mutter calls a buggy client ("committed
     initial non-empty content without acknowledging configuration") and stricter compositors may
     refuse.  The expose queued below waits for the configure, as it does after realize. */
  if (impl->unmapped) {
    impl->unmapped = false;
    puglWaylandApplyToplevelAttributes(view);
    puglUpdateSizeHints(view);
    wl_surface_commit(impl->wlSurface);
    wl_display_flush(view->world->impl->display);
  }

  puglWaylandQueueConfigure(view);
  puglWaylandQueueFullExpose(view);

  return PUGL_SUCCESS;
}

PuglStatus
puglHide(PuglView* const view)
{
  PuglInternals* const impl = view->impl;

  if (view->world->state == PUGL_WORLD_EXPOSING) {
    return PUGL_BAD_CALL;
  }

  if (!impl->wlSurface) {
    return PUGL_FAILURE;
  }

  // Attaching a null buffer unmaps the surface
  wl_surface_attach(impl->wlSurface, NULL, 0, 0);
  wl_surface_commit(impl->wlSurface);
  wl_display_flush(view->world->impl->display);

  /* The configure acked so far belonged to the mapping that just ended, so nothing may be drawn
     until puglShow() has asked for a new one.  A frame callback still pending was for a buffer that
     is gone and may never come back; drop it so it does not hold up the first frame after showing
     again. */
  impl->visible    = false;
  impl->configured = false;
  impl->unmapped   = true;

  if (impl->frameCallback) {
    wl_callback_destroy(impl->frameCallback);
    impl->frameCallback = NULL;
  }

  puglWaylandQueueConfigure(view);

  return PUGL_SUCCESS;
}

#if PUGL_WAYLAND_HAVE_TIMERFD
static void
puglWaylandRemoveTimer(PuglWorldInternals* const impl, const size_t index)
{
  close(impl->timers[index].fd);

  if (index + 1U < impl->numTimers) {
    memmove(impl->timers + index,
            impl->timers + index + 1U,
            sizeof(PuglWaylandTimer) * (impl->numTimers - index - 1U));
  }

  --impl->numTimers;
}

static void
puglWaylandRemoveViewTimers(PuglWorldInternals* const impl,
                            const PuglView* const      view)
{
  for (size_t i = impl->numTimers; i > 0; --i) {
    if (impl->timers[i - 1U].view == view) {
      puglWaylandRemoveTimer(impl, i - 1U);
    }
  }
}
#endif

void
puglFreeViewInternals(PuglView* const view)
{
  if (view && view->impl) {
    puglUnrealize(view);

#if PUGL_WAYLAND_HAVE_TIMERFD
    /* Timers live in the world and are keyed by view, so one left running here would keep a
       pointer to memory that is about to be freed and puglWaylandHandleTimers() would dispatch
       to it.  Applications are not required to stop their timers before destroying a view. */
    if (view->world && view->world->impl) {
      puglWaylandRemoveViewTimers(view->world->impl, view);
    }
#endif

    for (uint32_t i = 0; i < view->impl->clipboard.numFormats; ++i) {
      free(view->impl->clipboard.formatStrings[i]);
    }

    free(view->impl->clipboard.formatStrings);
    free(view->impl->clipboard.data.data);
    free(view->impl->appId);
    free(view->impl);
  }
}

// --------------------------------------------------------------------------------------------
// Event loop

/// Rebuild the poll set: the display socket, the key repeat timer, and any application timers
static nfds_t
puglWaylandBuildPollFds(PuglWorldInternals* const impl)
{
  const size_t needed = 2U + impl->numTimers;

  if (impl->numPollFds < needed) {
    struct pollfd* const grown = (struct pollfd*)realloc(
      impl->pollFds, needed * sizeof(struct pollfd));

    if (!grown) {
      return 0;
    }

    impl->pollFds    = grown;
    impl->numPollFds = needed;
  }

  nfds_t n = 0;

  impl->pollFds[n].fd      = wl_display_get_fd(impl->display);
  impl->pollFds[n].events  = POLLIN;
  impl->pollFds[n].revents = 0;
  ++n;

  if (impl->repeat.fd >= 0) {
    impl->pollFds[n].fd      = impl->repeat.fd;
    impl->pollFds[n].events  = POLLIN;
    impl->pollFds[n].revents = 0;
    ++n;
  }

  for (size_t i = 0; i < impl->numTimers; ++i) {
    if (impl->timers[i].fd >= 0) {
      impl->pollFds[n].fd      = impl->timers[i].fd;
      impl->pollFds[n].events  = POLLIN;
      impl->pollFds[n].revents = 0;
      ++n;
    }
  }

  return n;
}

static void
puglWaylandHandleTimers(PuglWorldInternals* const impl)
{
#if PUGL_WAYLAND_HAVE_TIMERFD
  for (size_t i = 0; i < impl->numTimers; ++i) {
    const PuglWaylandTimer timer = impl->timers[i];

    if (timer.fd < 0) {
      continue;
    }

    uint64_t expirations = 0;
    if (read(timer.fd, &expirations, sizeof(expirations)) !=
        (ssize_t)sizeof(expirations)) {
      continue;
    }

    if (!expirations) {
      continue;
    }

    PuglEvent event = {{PUGL_TIMER, 0U}};
    event.timer.id  = timer.id;
    puglDispatchEvent(timer.view, &event);

    /* The dispatch above can start or stop timers, which would move the array out from under this
       loop.  Bail out and pick up the rest on the next pass rather than risk a stale index. */
    if (i >= impl->numTimers || impl->timers[i].view != timer.view ||
        impl->timers[i].id != timer.id || impl->timers[i].fd != timer.fd) {
      break;
    }
  }
#else
  (void)impl;
#endif
}

/**
   Read and dispatch whatever the compositor has for us, waiting at most `timeout` seconds.

   This is the canonical libwayland prepare_read/poll/read_events dance rather than a plain
   wl_display_dispatch(), because the latter blocks with no timeout.
*/
static PuglStatus
puglWaylandDispatchEvents(PuglWorld* const world, const double timeout)
{
  PuglWorldInternals* const impl    = world->impl;
  struct wl_display* const  display = impl->display;

  impl->world = world;

  if (wl_display_dispatch_pending(display) < 0) {
    return PUGL_UNKNOWN_ERROR;
  }

  while (wl_display_prepare_read(display) != 0) {
    if (wl_display_dispatch_pending(display) < 0) {
      return PUGL_UNKNOWN_ERROR;
    }
  }

  if (wl_display_flush(display) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(display);
    return PUGL_UNKNOWN_ERROR;
  }

  const nfds_t nfds = puglWaylandBuildPollFds(impl);
  if (!nfds) {
    wl_display_cancel_read(display);
    return PUGL_NO_MEMORY;
  }

  const int timeoutMs =
    (timeout < 0.0) ? -1 : (int)(timeout * 1000.0 + 0.5);

  const int ret = poll(impl->pollFds, nfds, timeoutMs);

  if (ret < 0) {
    wl_display_cancel_read(display);
    return (errno == EINTR) ? PUGL_SUCCESS : PUGL_UNKNOWN_ERROR;
  }

  if (impl->pollFds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
    if (wl_display_read_events(display) < 0) {
      return PUGL_UNKNOWN_ERROR;
    }
  } else {
    wl_display_cancel_read(display);
  }

  if (wl_display_dispatch_pending(display) < 0) {
    return PUGL_UNKNOWN_ERROR;
  }

  /* One pass over the timer list already services every timer that expired, so several ready timer
     fds in the same cycle must not each trigger a full scan of their own. */
  bool timersReady = false;
  for (nfds_t i = 1; i < nfds; ++i) {
    if (!(impl->pollFds[i].revents & POLLIN)) {
      continue;
    }

    if (impl->pollFds[i].fd == impl->repeat.fd) {
      puglWaylandHandleRepeat(impl);
    } else {
      timersReady = true;
    }
  }

  if (timersReady) {
    puglWaylandHandleTimers(impl);
  }

  return PUGL_SUCCESS;
}

/// Flush pending configure and expose events for all views (mirrors the x11 backend)
PUGL_WARN_UNUSED_RESULT static PuglStatus
puglWaylandFlushExposures(PuglWorld* const world)
{
  PuglStatus st0 = PUGL_SUCCESS;
  PuglStatus st1 = PUGL_SUCCESS;
  PuglStatus st2 = PUGL_SUCCESS;

  for (size_t i = 0; i < world->numViews; ++i) {
    if (puglGetVisible(world->views[i])) {
      puglDispatchSimpleEvent(world->views[i], PUGL_UPDATE);
    }
  }

  world->state = PUGL_WORLD_EXPOSING;

  for (size_t i = 0; i < world->numViews; ++i) {
    PuglView* const      view = world->views[i];
    PuglInternals* const impl = view->impl;

    /* Retry a repaint that an earlier pass had to put off.  The frame callback re-queues one too,
       but a compositor that never sends frame callbacks -- an occluded or unmapped window, exactly
       what the timeouts in puglWaylandCanDraw() exist for -- would otherwise drop the repaint on
       the floor forever, because nothing else consumes needsRedisplay.

       This is reached often enough to matter because DGL always calls puglUpdate() with a finite
       timeout (Application::PrivateData::idle(), dgl/src/ApplicationPrivateData.cpp), which bounds
       the poll() deadline in puglWaylandDispatchEvents() even when the compositor sends nothing. */
    if (impl->needsRedisplay && impl->visible && impl->configured &&
        puglWaylandCanDraw(view)) {
      impl->needsRedisplay = false;
      puglWaylandQueueFullExpose(view);
    }

    PuglEvent configure = impl->pendingConfigure;
    PuglEvent expose    = impl->pendingExpose;

    impl->pendingConfigure.type = PUGL_NOTHING;
    impl->pendingExpose.type    = PUGL_NOTHING;

    if (expose.type && (!impl->visible || !impl->configured)) {
      // Nothing to draw into yet, keep the expose for when the surface is up
      impl->needsRedisplay = true;
      expose.type          = PUGL_NOTHING;
    } else if (expose.type && !puglWaylandCanDraw(view)) {
      // The compositor has not asked for a new frame yet, so wait for it
      impl->needsRedisplay = true;
      expose.type          = PUGL_NOTHING;
    }

    /* Configure first, and outside the graphics context.  Pugl c125c44 stopped entering the
       context for configure events and deleted puglConfigure along with the idea; puglDispatchEvent
       is what records lastConfigure and advances the view stage now.  Wayland still coalesces its
       own configures into pendingConfigure, unlike x11.c which dispatches them as they arrive: an
       xdg_surface configure is only meaningful once per ack_configure and commit, so the batching
       stays. */
    if (configure.type) {
      st0 = puglDispatchEvent(view, &configure);
    }

    if (!expose.type) {
      continue;
    }

    if (!(st1 = view->backend->enter(view, &expose.expose))) {
      st1 = view->eventFunc(view, &expose);

      /* Ask for the next frame before leaving the context: the backend's leave() is what commits
         the surface, and the frame request has to ride along with that same commit. */
      puglWaylandRequestFrame(view);

      st2 = view->backend->leave(view, &expose.expose);
    } else {
      /* The backend could not give us anything to draw into (both shm buffers still held by the
         compositor, a context that would not go current, ...).  The expose has already been taken
         off pendingExpose, so hand it to needsRedisplay or it is lost; the retry at the top of this
         loop picks it up again next pass.  No leave() to match a failed enter(). */
      impl->needsRedisplay = true;
    }
  }

  wl_display_flush(world->impl->display);

  return st0 ? st0 : st1 ? st1 : st2;
}

PuglStatus
puglUpdate(PuglWorld* const world, const double timeout)
{
  const double         startTime  = puglWaylandTime(world);
  const PuglWorldState startState = world->state;
  PuglStatus           st0        = PUGL_SUCCESS;
  PuglStatus           st1        = PUGL_SUCCESS;

  if (startState == PUGL_WORLD_IDLE) {
    world->state = PUGL_WORLD_UPDATING;
  } else if (startState != PUGL_WORLD_RECURSING) {
    return PUGL_BAD_CALL;
  }

  if (timeout < 0.0) {
    st0 = puglWaylandDispatchEvents(world, timeout);
  } else if (timeout <= 0.001) {
    st0 = puglWaylandDispatchEvents(world, 0.0);
  } else {
    const double endTime = startTime + timeout - 0.001;
    double       t       = startTime;

    while (!st0 && t < endTime) {
      st0 = puglWaylandDispatchEvents(world, endTime - t);
      t   = puglWaylandTime(world);
    }
  }

  st1          = puglWaylandFlushExposures(world);
  world->state = startState;
  return st0 ? st0 : st1;
}

double
puglGetTime(const PuglWorld* const world)
{
  return puglWaylandTime(world);
}

// --------------------------------------------------------------------------------------------
// Redisplay

PuglStatus
puglObscureView(PuglView* const view)
{
  const PuglArea size = view->impl->size;

  return puglIsValidArea(size)
           ? puglObscureRegion(view, 0, 0, size.width, size.height)
           : PUGL_FAILURE;
}

PuglStatus
puglObscureRegion(PuglView* const view,
                  const int       x,
                  const int       y,
                  const unsigned  width,
                  const unsigned  height)
{
  if (!puglIsValidPosition(x, y) || !puglIsValidSize(width, height)) {
    return PUGL_BAD_PARAMETER;
  }

  if (view->world->state == PUGL_WORLD_EXPOSING) {
    return PUGL_BAD_CALL;
  }

  PuglExposeEvent event;
  if (!puglWaylandClipExpose(
        &event, view->impl->size, x, y, width, height)) {
    return PUGL_SUCCESS;
  }

  /* Unlike X11 there is no server side event queue to bounce an expose off, so a redraw request is
     always just recorded here and picked up by the next puglUpdate(). */
  mergeExposeEvents(&view->impl->pendingExpose.expose, &event);

  return PUGL_SUCCESS;
}

PuglStatus
puglSendEvent(PuglView* const view, const PuglEvent* const event)
{
  if (view->world->state == PUGL_WORLD_EXPOSING) {
    return PUGL_FAILURE;
  }

  switch (event->type) {
  case PUGL_CLOSE:
    return puglDispatchSimpleEvent(view, PUGL_CLOSE);

  case PUGL_CLIENT:
    return puglDispatchEvent(view, event);

  case PUGL_EXPOSE:
    mergeExposeEvents(&view->impl->pendingExpose.expose, &event->expose);
    return PUGL_SUCCESS;

  default:
    break;
  }

  return PUGL_UNSUPPORTED;
}

// --------------------------------------------------------------------------------------------
// View properties

/* Const on the view since pugl acafe87, along with the removal of the PuglHandle typedefs.  In C++,
   where DGL compiles this file, the old spelling is a different overload and goes silently
   undefined at link time rather than failing to compile. */
PuglNativeView
puglGetNativeView(const PuglView* const view)
{
  return (PuglNativeView)(uintptr_t)view->impl->wlSurface;
}

/* Named puglViewStringChanged until pugl 3ecf2df renamed the platform hook. */
PuglStatus
puglApplyViewString(PuglView* const      view,
                    const PuglStringHint key,
                    const char* const    value)
{
  PuglInternals* const impl = view->impl;

  if (!impl->xdgToplevel || !value) {
    return PUGL_SUCCESS;
  }

  switch (key) {
  case PUGL_APPLICATION_NAME:
    /* Added by pugl 3ecf2df, and nothing to do with it here.  Xdg-shell has exactly one identifier
       for a toplevel, the app id, and PUGL_CLASS_NAME below is already mapped to it; that is the
       one DGL sets (Application::setClassName).  x11.c ignores this hint in the same place. */
    break;

  case PUGL_CLASS_NAME:
    xdg_toplevel_set_app_id(impl->xdgToplevel, value);
    break;

  case PUGL_WINDOW_TITLE:
    xdg_toplevel_set_title(impl->xdgToplevel, value);
    break;
  }

  return PUGL_SUCCESS;
}

double
puglGetScaleFactor(const PuglView* const view)
{
  return view->impl->scale > 0.0 ? view->impl->scale
                                 : view->world->impl->scaleFactor;
}

PuglStatus
puglSetWindowPosition(PuglView* const PUGL_UNUSED(view),
                      const int       PUGL_UNUSED(x),
                      const int       PUGL_UNUSED(y))
{
  /* Wayland deliberately does not let a client place its own windows, and does not tell it where
     they ended up.  Reporting this rather than silently succeeding keeps callers honest. */
  return PUGL_UNSUPPORTED;
}

PuglStatus
puglSetWindowSize(PuglView* const view,
                  const unsigned  width,
                  const unsigned  height)
{
  PuglInternals* const impl = view->impl;

  if (!puglIsValidSize(width, height)) {
    return PUGL_BAD_PARAMETER;
  }

  if (!impl->wlSurface) {
    impl->size.width  = (PuglSpan)width;
    impl->size.height = (PuglSpan)height;
    return PUGL_SUCCESS;
  }

  PuglArea request;
  request.width  = (PuglSpan)width;
  request.height = (PuglSpan)height;

  /* A client-initiated resize is not negotiated: we simply pick a new size and commit it.  Whatever
     the compositor last asked for is forgotten at the same time -- it is not what is on screen any
     more, and leaving it behind would make the next configure, scale change or output enter re-apply
     it and snap the window straight back. */
  impl->requestedLogicalSize.width  = 0;
  impl->requestedLogicalSize.height = 0;

  puglWaylandSetSize(view, puglWaylandPixelsToLogical(request, impl->scale));

  puglWaylandQueueConfigure(view);
  puglWaylandQueueFullExpose(view);

  return PUGL_SUCCESS;
}

PuglStatus
puglSetTransientParent(PuglView* const view, const PuglNativeView parent)
{
  view->transientParent = parent;

  puglWaylandApplyTransientParent(view);

  return PUGL_SUCCESS;
}

PuglStatus
puglSetViewStyle(PuglView* const view, const PuglViewStyleFlags flags)
{
  PuglInternals* const     impl     = view->impl;
  const PuglViewStyleFlags oldFlags = puglGetViewStyle(view);

  if (!impl->xdgToplevel) {
    return PUGL_FAILURE;
  }

  const PuglViewStyleFlags changed = oldFlags ^ flags;

  if (changed & PUGL_VIEW_STYLE_FULLSCREEN) {
    if (flags & PUGL_VIEW_STYLE_FULLSCREEN) {
      xdg_toplevel_set_fullscreen(impl->xdgToplevel, NULL);
    } else {
      xdg_toplevel_unset_fullscreen(impl->xdgToplevel);
    }
  }

  if (changed & (PUGL_VIEW_STYLE_TALL | PUGL_VIEW_STYLE_WIDE)) {
    if (flags & (PUGL_VIEW_STYLE_TALL | PUGL_VIEW_STYLE_WIDE)) {
      xdg_toplevel_set_maximized(impl->xdgToplevel);
    } else {
      xdg_toplevel_unset_maximized(impl->xdgToplevel);
    }
  }

  if ((changed & PUGL_VIEW_STYLE_HIDDEN) && (flags & PUGL_VIEW_STYLE_HIDDEN)) {
    xdg_toplevel_set_minimized(impl->xdgToplevel);
  }

  /* MODAL, ABOVE, BELOW and DEMANDING have no xdg-shell equivalent: stacking and attention are
     compositor policy on Wayland, not something a client can ask for. */

  return PUGL_SUCCESS;
}

PuglStatus
puglGrabFocus(PuglView* const PUGL_UNUSED(view))
{
  // A Wayland client cannot take the keyboard focus, only the compositor can give it
  return PUGL_UNSUPPORTED;
}

bool
puglHasFocus(const PuglView* const view)
{
  return view->world->impl->keyboardFocus == view;
}

PuglStatus
puglSetCursor(PuglView* const view, const PuglCursor cursor)
{
  PuglInternals* const      impl  = view->impl;
  PuglWorldInternals* const wimpl = view->world->impl;

  if ((unsigned)cursor >= PUGL_NUM_CURSORS) {
    return PUGL_BAD_PARAMETER;
  }

  if (impl->cursor == cursor) {
    return PUGL_SUCCESS;
  }

  impl->cursor = cursor;

  if (wimpl->pointerFocus != view) {
    return PUGL_SUCCESS; // Will be applied on the next pointer enter
  }

  return puglWaylandApplyCursor(wimpl, cursor);
}

// --------------------------------------------------------------------------------------------
// Timers

PuglStatus
puglStartTimer(PuglView* const view, const uintptr_t id, const double timeout)
{
#if PUGL_WAYLAND_HAVE_TIMERFD
  PuglWorldInternals* const impl = view->world->impl;

  if (timeout <= 0.0) {
    return PUGL_BAD_PARAMETER;
  }

  const long        seconds = (long)timeout;
  struct itimerspec spec    = {{0, 0}, {0, 0}};

  spec.it_value.tv_sec     = seconds;
  spec.it_value.tv_nsec    = (long)((timeout - (double)seconds) * 1e9);
  spec.it_interval.tv_sec  = spec.it_value.tv_sec;
  spec.it_interval.tv_nsec = spec.it_value.tv_nsec;

  // Replace an existing timer with the same ID
  for (size_t i = 0; i < impl->numTimers; ++i) {
    if (impl->timers[i].view == view && impl->timers[i].id == id) {
      return timerfd_settime(impl->timers[i].fd, 0, &spec, NULL) == 0
               ? PUGL_SUCCESS
               : PUGL_UNKNOWN_ERROR;
    }
  }

  const int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
  if (fd < 0) {
    return PUGL_UNKNOWN_ERROR;
  }

  if (timerfd_settime(fd, 0, &spec, NULL) != 0) {
    close(fd);
    return PUGL_UNKNOWN_ERROR;
  }

  PuglWaylandTimer* const timers = (PuglWaylandTimer*)realloc(
    impl->timers, (impl->numTimers + 1U) * sizeof(PuglWaylandTimer));

  if (!timers) {
    close(fd);
    return PUGL_NO_MEMORY;
  }

  impl->timers                   = timers;
  impl->timers[impl->numTimers].view = view;
  impl->timers[impl->numTimers].id   = id;
  impl->timers[impl->numTimers].fd   = fd;
  ++impl->numTimers;

  return PUGL_SUCCESS;
#else
  (void)view;
  (void)id;
  (void)timeout;
  return PUGL_UNSUPPORTED;
#endif
}

PuglStatus
puglStopTimer(PuglView* const view, const uintptr_t id)
{
#if PUGL_WAYLAND_HAVE_TIMERFD
  PuglWorldInternals* const impl = view->world->impl;

  for (size_t i = 0; i < impl->numTimers; ++i) {
    if (impl->timers[i].view == view && impl->timers[i].id == id) {
      puglWaylandRemoveTimer(impl, i);
      return PUGL_SUCCESS;
    }
  }
#else
  (void)view;
  (void)id;
#endif

  return PUGL_FAILURE;
}

// --------------------------------------------------------------------------------------------
// Clipboard API

PuglStatus
puglPaste(PuglView* const view)
{
  PuglWorldInternals* const    wimpl = view->world->impl;
  PuglWaylandClipboard* const  board = &view->impl->clipboard;
  const PuglWaylandOffer* const offer = wimpl->selectionOffer;

  if (!offer || !offer->numMimeTypes) {
    return PUGL_FAILURE;
  }

  const PuglStatus st = puglWaylandSetClipboardFormats(board, offer);
  if (st) {
    return st;
  }

  if (!board->numFormats) {
    return PUGL_FAILURE;
  }

  board->acceptedFormatIndex = UINT32_MAX;

  PuglEvent event  = {{PUGL_DATA_OFFER, 0U}};
  event.offer.time = puglWaylandTime(view->world);

  return puglDispatchEvent(view, &event);
}

PuglStatus
puglAcceptOffer(PuglView* const                 view,
                const PuglDataOfferEvent* const PUGL_UNUSED(offer),
                const uint32_t                  typeIndex)
{
  PuglWorldInternals* const   wimpl = view->world->impl;
  PuglWaylandClipboard* const board = &view->impl->clipboard;
  PuglWaylandOffer* const     po    = wimpl->selectionOffer;

  if (!po || typeIndex >= board->numFormats) {
    return PUGL_BAD_PARAMETER;
  }

  /* board->formatStrings holds the *reported* names ("text/plain"); the transfer has to name a MIME
     type the owner actually offered, so find the first raw type that maps to the accepted one. */
  const char* mimeType = NULL;
  for (int pass = 0; pass < 2 && !mimeType; ++pass) {
    for (uint32_t i = 0; i < po->numMimeTypes; ++i) {
      const char* const raw = po->mimeTypes[i];
      const bool        isUtf =
        strstr(raw, "utf") != NULL || strstr(raw, "UTF") != NULL;

      if ((pass == 0) != isUtf) {
        continue;
      }

      char* const name = puglWaylandReportedType(raw);
      if (!name) {
        continue;
      }

      const bool match = !strcmp(name, board->formatStrings[typeIndex]);
      free(name);

      if (match) {
        mimeType = raw;
        break;
      }
    }
  }

  if (!mimeType) {
    return PUGL_FAILURE;
  }

  // Do not leave data from a previous acceptance visible if this transfer fails
  board->acceptedFormatIndex = UINT32_MAX;

  /* Close-on-exec matters here: this runs inside somebody else's host, which may fork and exec at
     any moment, and the write end staying open in a child would keep the transfer from ever ending.
     The fd handed to the compositor is a separate copy made by the socket, so it is unaffected. */
  int fds[2] = {-1, -1};
  if (pipe2(fds, O_CLOEXEC) != 0) {
    return PUGL_UNKNOWN_ERROR;
  }

  wl_data_offer_receive(po->offer, mimeType, fds[1]);
  close(fds[1]);
  wl_display_flush(wimpl->display);

  const PuglStatus st = puglWaylandReadPipe(wimpl, fds[0], &board->data);
  close(fds[0]);

  if (st) {
    return st;
  }

  board->acceptedFormatIndex = typeIndex;

  /* Unlike X11 there is no asynchronous round trip here: the data is in hand, so the data event can
     be dispatched immediately. */
  PuglEvent event       = {{PUGL_DATA, 0U}};
  event.data.time       = puglWaylandTime(view->world);
  event.data.typeIndex  = typeIndex;

  return puglDispatchEvent(view, &event);
}

uint32_t
puglGetNumClipboardTypes(const PuglView* const view)
{
  return view->impl->clipboard.numFormats;
}

const char*
puglGetClipboardType(const PuglView* const view, const uint32_t typeIndex)
{
  const PuglWaylandClipboard* const board = &view->impl->clipboard;

  return typeIndex < board->numFormats ? board->formatStrings[typeIndex] : NULL;
}

const void*
puglGetClipboard(PuglView* const view,
                 const uint32_t  typeIndex,
                 size_t* const   len)
{
  PuglWaylandClipboard* const board = &view->impl->clipboard;

  if (typeIndex != board->acceptedFormatIndex) {
    *len = 0;
    return NULL;
  }

  *len = board->data.len;
  return board->data.data;
}

PuglStatus
puglSetClipboard(PuglView* const   view,
                 const char* const type,
                 const void* const data,
                 const size_t      len)
{
  PuglWorldInternals* const wimpl = view->world->impl;

  if (!wimpl->dataDeviceManager || !wimpl->dataDevice) {
    return PUGL_UNSUPPORTED;
  }

  PuglWaylandSourceData* const sd =
    (PuglWaylandSourceData*)calloc(1, sizeof(PuglWaylandSourceData));

  if (!sd) {
    return PUGL_NO_MEMORY;
  }

  sd->wimpl = wimpl;

  const PuglStatus st = puglSetBlob(&sd->data, data, len);
  if (st) {
    free(sd);
    return st;
  }

  struct wl_data_source* const source =
    wl_data_device_manager_create_data_source(wimpl->dataDeviceManager);

  if (!source) {
    free(sd->data.data);
    free(sd);
    return PUGL_UNKNOWN_ERROR;
  }

  wl_data_source_add_listener(source, &puglWaylandDataSourceListener, sd);

  const char* const mimeType = type ? type : "text/plain";
  wl_data_source_offer(source, mimeType);

  /* Advertise the usual aliases as well, so that clipboard managers and X11 clients bridged through
     XWayland can paste what we put up. */
  if (!strcmp(mimeType, "text/plain")) {
    wl_data_source_offer(source, "text/plain;charset=utf-8");
    wl_data_source_offer(source, "UTF8_STRING");
    wl_data_source_offer(source, "STRING");
    wl_data_source_offer(source, "TEXT");
  }

  /* NOTE: this reuses whatever the most recent input serial happened to be.  Compositors are
     entitled to ignore a set_selection carrying a serial they have already acted on, and some
     (mutter) do.  In practice a real copy always follows fresh keyboard or pointer input, so the
     serial has moved on by the time this runs; a copy triggered with no input at all (from a timer,
     say) is the case that may silently do nothing. */
  wl_data_device_set_selection(
    wimpl->dataDevice, source, wimpl->lastSerial);
  wl_display_flush(wimpl->display);

  /* Take over the tracking: any previous source is left for its cancelled event to destroy, which
     the compositor sends precisely because this call replaced it. */
  wimpl->dataSource     = source;
  wimpl->dataSourceData = sd;

  return PUGL_SUCCESS;
}

// --------------------------------------------------------------------------------------------
// DGL extensions declared in pugl.hpp

PuglStatus
puglWaylandUpdateWithoutExposures(PuglWorld* const world)
{
  const PuglWorldState startState = world->state;

  world->state = PUGL_WORLD_UPDATING;

  const double startTime = puglWaylandTime(world);
  const double endTime   = startTime + 0.03;
  PuglStatus   st        = PUGL_SUCCESS;

  for (double t = startTime; !st && t < endTime; t = puglWaylandTime(world)) {
    st = puglWaylandDispatchEvents(world, endTime - t);
  }

  world->state = startState;
  return st;
}

void
puglWaylandSetAppId(PuglView* const view, const char* const appId)
{
  if (!appId || !*appId) {
    return;
  }

  // kept for puglShow() to re-apply after an unmap, which discards it
  char* const copy = strdup(appId);
  if (copy) {
    free(view->impl->appId);
    view->impl->appId = copy;
  }

  if (view->impl->xdgToplevel) {
    xdg_toplevel_set_app_id(view->impl->xdgToplevel, appId);
  }
}

const char*
puglWaylandGetExportedHandle(const PuglView* const view)
{
  return view && view->impl ? view->impl->exportedHandle : NULL;
}
