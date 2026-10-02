// Copyright 2012-2023 David Robillard <d@drobilla.net>
// Copyright 2025 Dusk Audio Framework contributors
// SPDX-License-Identifier: ISC

#ifndef PUGL_SRC_WAYLAND_H
#define PUGL_SRC_WAYLAND_H

#include "../pugl-upstream/src/types.h"

#include "pugl/pugl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* NOTE: This header deliberately does not include wayland-client.h, wayland-cursor.h, the xkbcommon
   headers, or the generated protocol headers.

   The Wayland backend is compiled as part of the dgl/src/pugl.cpp unity build, which pulls those
   system headers in at global scope (with C linkage) *before* it enters the DGL namespace, and only
   then includes wayland.c.  Including them again from here would be a no-op at best (include
   guards) and, if this header were ever included from inside the namespace without the global
   includes having happened first, would declare a whole second set of namespaced Wayland types that
   could never link.  The elaborated type specifiers below therefore resolve, by ordinary unqualified
   lookup, to the global declarations.  See the HAVE_WAYLAND arm near the top of pugl.cpp. */

/// Maximum number of outputs (monitors) tracked per world / per view
#define PUGL_WAYLAND_MAX_OUTPUTS 16

/// Number of 120ths in a whole fractional scale step (wp_fractional_scale_v1 unit)
#define PUGL_WAYLAND_SCALE_DENOM 120.0

/// A monitor the compositor has advertised, with its integer scale
typedef struct {
  struct wl_output*   output;       ///< Proxy, NULL for an unused slot
  PuglWorldInternals* wimpl;        ///< Owning world, used to refresh entered views
  uint32_t            globalName;   ///< wl_registry name, used to handle global_remove
  int32_t             scale;        ///< Integer scale factor, at least 1
  bool                scaleChanged; ///< Whether the current wl_output batch changed scale
} PuglWaylandOutput;

/// An application timer started by puglStartTimer(), backed by a timerfd
typedef struct {
  PuglView* view; ///< View the PUGL_TIMER event is dispatched to
  uintptr_t id;   ///< Application timer ID
  int       fd;   ///< timerfd, polled along with the display socket
} PuglWaylandTimer;

/// A wl_data_offer we are tracking, with the MIME types it advertises
typedef struct {
  struct wl_data_offer* offer;        ///< Proxy, owned by this struct
  char**                mimeTypes;    ///< Advertised MIME type strings
  uint32_t              numMimeTypes; ///< Number of entries in mimeTypes
} PuglWaylandOffer;

/// The clipboard state of a single view, mirroring PuglX11Clipboard
typedef struct {
  char**   formatStrings;       ///< MIME types of the offer being handled
  uint32_t numFormats;          ///< Number of entries in formatStrings
  uint32_t acceptedFormatIndex; ///< Index passed to puglAcceptOffer(), or UINT32_MAX
  PuglBlob data;                ///< Data received from, or offered to, the clipboard
} PuglWaylandClipboard;

/**
   A clipboard receive that is currently in progress.

   Published on the world so that the wl_data_source.send handler can find it. When this client
   both owns the selection and is pasting from it, the two ends of the pipe are serviced by the
   same thread, and the writer has to drain the reader's end itself or the transfer stalls as soon
   as the payload outgrows the pipe buffer.
*/
typedef struct {
  int      fd;       ///< Read end of the transfer pipe
  uint8_t* buffer;   ///< Accumulated data, owned by puglWaylandReadPipe()
  size_t   len;      ///< Bytes accumulated so far
  size_t   capacity; ///< Bytes allocated in buffer
  bool     done;     ///< Whether the receive reached EOF or failed
  PuglStatus status; ///< Error encountered while reading, or PUGL_SUCCESS
} PuglWaylandPipeRecv;

/**
   Data this client has put on the clipboard, kept alive for wl_data_source.send.

   Owned by the wl_data_source it is attached to as listener data, and normally freed by the
   wl_data_source.cancelled handler.  The world keeps a pointer to the live one as well, because at
   teardown there is nobody left to deliver that cancelled event.
*/
typedef struct {
  PuglBlob            data;
  PuglWorldInternals* wimpl; ///< Owning world, for reaching a receive in progress
} PuglWaylandSourceData;

/// Everything needed to turn wl_keyboard events into Pugl key and text events
typedef struct {
  struct xkb_context*       context;
  struct xkb_keymap*        keymap;
  struct xkb_state*         state;
  struct xkb_compose_table* composeTable;
  struct xkb_compose_state* composeState;
} PuglWaylandXkb;

/// Key auto-repeat state, driven by a timerfd because Wayland does not repeat for us
typedef struct {
  int       fd;    ///< timerfd, -1 when unsupported
  int32_t   rate;  ///< Repeats per second, 0 disables repeating entirely
  int32_t   delay; ///< Milliseconds before the first repeat
  uint32_t  key;   ///< Raw (evdev) key code currently repeating, 0 for none
  PuglView* view;  ///< View the repeat is delivered to
  double    time;  ///< Event time of the originating press, in seconds
} PuglWaylandKeyRepeat;

/// Scroll deltas accumulated between wl_pointer.frame events
typedef struct {
  double   dx;         ///< Horizontal distance in lines
  double   dy;         ///< Vertical distance in lines
  bool     any;        ///< Whether any axis event was seen in this frame
  bool     discrete;   ///< Whether the source is a detented wheel
  uint32_t time;       ///< Timestamp of the most recent axis event
} PuglWaylandScroll;

struct PuglWorldInternalsImpl {
  /* The world these internals belong to.  puglInitWorldInternals() is not given it, so it is filled
     in as soon as anything that has it runs (see puglInitViewInternals and
     puglWaylandDispatchEvents).  Only needed by listeners that have to reach across every view, such
     as wl_registry.global_remove dropping a wl_output; those can only fire once views exist. */
  PuglWorld* world;

  // Core globals
  struct wl_display*    display;
  struct wl_registry*   registry;
  struct wl_compositor* compositor;
  struct wl_shm*        shm;
  struct wl_seat*       seat;
  struct wl_pointer*    pointer;
  struct wl_keyboard*   keyboard;

  // Extension globals, any of which may be NULL if the compositor lacks them
  struct xdg_wm_base*                    wmBase;
  struct wl_data_device_manager*         dataDeviceManager;
  struct wl_data_device*                 dataDevice;
  struct wp_viewporter*                  viewporter;
  struct wp_fractional_scale_manager_v1* fractionalScaleManager;
  struct zxdg_decoration_manager_v1*     decorationManager;
  struct zxdg_exporter_v2*               exporter;

  // Cursor theme, loaded lazily on the first puglSetCursor() that needs it
  struct wl_cursor_theme* cursorTheme;
  struct wl_surface*      cursorSurface;
  int                     cursorSize;
  bool                    cursorLoadFailed;

  /* EGL display shared by every view, with a reference count.  An EGLDisplay is per wl_display, not
     per window, so eglTerminate() from one view would tear down every other view's context.  Held
     as void* so that this header stays usable in builds with no OpenGL. */
  void*    eglDisplay;
  unsigned eglRefCount;

  // Keyboard
  PuglWaylandXkb       xkb;
  PuglWaylandKeyRepeat repeat;
  PuglMods             mods;

  // Pointer
  PuglView*         pointerFocus;
  double            pointerX;
  double            pointerY;
  uint32_t          pointerEnterSerial;
  PuglWaylandScroll scroll;

  // Keyboard focus and the most recent input serial, needed by set_selection etc.
  PuglView* keyboardFocus;
  uint32_t  lastSerial;

  // Clipboard, plus unclaimed and drag-and-drop offers that still need to be cleaned up
  PuglWaylandOffer* selectionOffer;
  PuglWaylandOffer* unclaimedOffer;
  PuglWaylandOffer* dndOffer;

  /* The selection this client currently owns, if any.  Ordinarily the wl_data_source.cancelled
     handler destroys these, but that event never arrives if the world is torn down while we still
     own the selection, so they are tracked here and freed by puglWaylandDestroyWorldInternals(). */
  struct wl_data_source* dataSource;
  PuglWaylandSourceData* dataSourceData;

  /// Set only for the duration of puglAcceptOffer(), see PuglWaylandPipeRecv
  PuglWaylandPipeRecv* activeRecv;

  // Outputs
  PuglWaylandOutput outputs[PUGL_WAYLAND_MAX_OUTPUTS];
  uint32_t          numOutputs;

  // Application timers
  PuglWaylandTimer* timers;
  size_t            numTimers;

  // poll() scratch buffer, grown as timers come and go
  struct pollfd* pollFds;
  size_t         numPollFds;

  /// Fallback scale factor for views that are not on any known output yet
  double scaleFactor;
};

struct PuglInternalsImpl {
  // Surface and its xdg-shell role
  struct wl_surface*                  wlSurface;
  struct xdg_surface*                 xdgSurface;
  struct xdg_toplevel*                xdgToplevel;
  struct zxdg_toplevel_decoration_v1* decoration;
  struct wp_viewport*                 viewport;
  struct wp_fractional_scale_v1*      fractionalScale;
  struct wl_callback*                 frameCallback;

  /* The surface exported through xdg-foreign, and the handle the compositor answered with.  Other
     processes, xdg-desktop-portal above all, take a dialog's parent as such a handle since Wayland
     has no window ids. */
  struct zxdg_exported_v2* exported;
  char*                    exportedHandle;

  /// Private data of the graphics backend (see wayland_gl.c / wayland_cairo.c)
  PuglSurface* surface;

  // Events accumulated during a dispatch pass, flushed at the end of puglUpdate()
  PuglEvent pendingConfigure;
  PuglEvent pendingExpose;

  PuglArea size;                 ///< Current size in buffer pixels (what Pugl reports)
  PuglArea logicalSize;          ///< Current size in logical pixels (what the compositor uses)
  PuglArea requestedLogicalSize; ///< Size the compositor last asked for, zero if it does not care

  double   scale;                    ///< Scale factor in effect, at least 1.0
  int32_t  bufferScale;              ///< wl_surface buffer scale, 1 when a viewport is used
  uint32_t preferredFractionalScale; ///< Compositor preference in 120ths, 0 if unknown

  double frameCallbackTime; ///< Time the outstanding frame callback was requested

  PuglViewStyleFlags styleFlags;
  PuglCursor         cursor;

  PuglWaylandClipboard clipboard;

  /// Outputs this surface currently overlaps, tracked via wl_surface.enter/leave
  struct wl_output* enteredOutputs[PUGL_WAYLAND_MAX_OUTPUTS];
  uint32_t          numEnteredOutputs;

  bool configured;         ///< At least one xdg_surface.configure has been acked
  bool visible;            ///< puglShow() called, puglHide() not called since
  bool needsRedisplay;     ///< A redraw was requested while a frame callback was pending
  bool geometryDirty;      ///< Size or scale changed and has not gone out with a buffer yet
  bool frameCallbackWorks; ///< A frame callback has come back at least once
};

/// Semi-public entry point used by the graphics backends to size their drawables
PUGL_API PuglArea
puglWaylandGetBufferSize(const PuglView* view);

/// Semi-public entry point the graphics backends call right before a commit that attaches a buffer
PUGL_API void
puglWaylandApplyGeometry(PuglView* view);

#endif // PUGL_SRC_WAYLAND_H
