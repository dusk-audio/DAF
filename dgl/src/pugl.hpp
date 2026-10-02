/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2025 Filipe Coelho <falktx@falktx.com>
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

#ifndef DGL_PUGL_HPP_INCLUDED
#define DGL_PUGL_HPP_INCLUDED

#include "../Base.hpp"

// we will include all header files used in pugl.h in their C++ friendly form, then pugl stuff in custom namespace
#include <cstddef>
#ifdef DAF_PROPER_CPP11_SUPPORT
# include <cstdbool>
# include <cstdint>
#else
# include <stdbool.h>
# include <stdint.h>
#endif

// custom attributes
#define PUGL_ATTRIBUTES_H
#define PUGL_BEGIN_DECLS
#define PUGL_END_DECLS
#define PUGL_API
#define PUGL_DISABLE_DEPRECATED

// GCC function attributes
#if defined(__GNUC__) && !defined(__clang__)
 #define PUGL_CONST_FUNC __attribute__((const))
 #define PUGL_MALLOC_FUNC __attribute__((malloc))
#else
 #define PUGL_CONST_FUNC
 #define PUGL_MALLOC_FUNC
#endif

#define PUGL_CONST_API PUGL_CONST_FUNC
#define PUGL_MALLOC_API PUGL_MALLOC_FUNC

// we do our own OpenGL inclusion
#define PUGL_NO_INCLUDE_GL_H
#define PUGL_NO_INCLUDE_GLU_H

#ifndef DAF_OS_MAC
START_NAMESPACE_DGL
#endif

#include "pugl/pugl.h"

// --------------------------------------------------------------------------------------------------------------------

// DGL specific, expose backend enter
bool puglBackendEnter(PuglView* view);

// DGL specific, expose backend leave
bool puglBackendLeave(PuglView* view);

// DGL specific, whether the view's OpenGL context is the one current on the calling thread.
// Backends without such a context, and platforms where it cannot be asked, answer with fallback.
bool puglBackendIsCurrent(PuglView* view, bool fallback);

// DGL specific, assigns backend that matches current DGL build
void puglSetMatchingBackendForCurrentBuild(PuglView* view);

// bring view window into the foreground, aka "raise" window
void puglRaiseWindow(PuglView* view);

// combined puglSetSizeHint using PUGL_MIN_SIZE, PUGL_MIN_ASPECT and PUGL_MAX_ASPECT
PuglStatus puglSetGeometryConstraints(PuglView* view, uint width, uint height, bool aspect);

// set view as resizable (or not) during runtime
void puglSetResizable(PuglView* view, bool resizable);

// set window size while also changing default
PuglStatus puglSetSizeAndDefault(PuglView* view, uint width, uint height);

// DGL specific, build-specific drawing prepare
void puglOnDisplayPrepare(PuglView* view);

// DGL specific, build-specific fallback resize
void puglFallbackOnResize(PuglView* view, uint width, uint height);

#if defined(DAF_OS_HAIKU)

// nothing here yet

#elif defined(DAF_OS_MAC)

// macOS specific, add another view's window as child
PuglStatus puglMacOSAddChildWindow(PuglView* view, PuglView* child);

// macOS specific, remove another view's window as child
PuglStatus puglMacOSRemoveChildWindow(PuglView* view, PuglView* child);

// macOS specific, center view based on parent coordinates (if there is one)
void puglMacOSShowCentered(PuglView* view);

#elif defined(DAF_OS_WASM)

// nothing here yet

#elif defined(DAF_OS_WINDOWS)

// win32 specific, call ShowWindow with SW_RESTORE
void puglWin32RestoreWindow(PuglView* view);

// win32 specific, center view based on parent coordinates (if there is one)
void puglWin32ShowCentered(PuglView* view);

// win32 specific, whether this thread is sending a key some view declined on to its host right now
// A host that hands such a key back through the plugin format (IPlugView::onKeyDown, effEditKeyDown)
// would otherwise offer the UI the key it just declined, and could send it on again.
bool puglWin32IsForwardingKey();

// On Unix, HAVE_X11 and HAVE_WAYLAND can both be defined: the two sets of development files
// coexist happily and most desktop distributions ship both. The order of the arms below is
// therefore a deliberate policy, not an accident:
//
//   X11 is the primary backend whenever it is present.
//
// The reason is that a plugin UI is not free to pick its own windowing system. Hosts embed plugin
// UIs into their own window, and the only embedding contract that every plugin format has is the
// X11 one; of the formats DAF targets, only CLAP has a Wayland window API at all. Standalone
// builds have no such constraint, but they run perfectly well on a Wayland session through
// XWayland, so there is nothing to gain from splitting the backend choice between plugin and
// standalone builds of the same source tree.
//
// Consequently the Wayland arm below is reachable only on a build where X11 is absent entirely
// (no libx11-dev), which is the case this backend exists to serve. The build system mirrors this
// exactly -- see DGL_BACKEND_WAYLAND in Makefile.base.mk and the X11_FOUND branch in
// cmake/DAF-plugin.cmake, both of which refuse to define HAVE_WAYLAND when X11 is in play.
//
// A possible future step is runtime dispatch: build both backends and choose per window, so that a
// CLAP plugin could hand the host a real Wayland surface while everything else keeps using X11.
// That requires the pugl view/world internals to stop being a compile-time singleton, so it is
// explicitly out of scope here; the compile-time choice below is the stepping stone to it.

#elif defined(HAVE_X11)

#define DGL_USING_X11

// X11 specific, update world without triggering exposure events
PuglStatus puglX11UpdateWithoutExposures(PuglWorld* world);

// X11 specific, set dialog window type
void puglX11SetWindowType(const PuglView* view, bool isStandalone);

// X11 specific, hand a key event no widget used to the window this view is embedded in
// Core X11 delivers a key to the child under the pointer, so without this an embedded UI
// swallows host shortcuts whenever the pointer rests on it.
void puglX11ForwardKeyToParent(const PuglView* view, bool press, uint keycode, uint mods, uint time);

#elif defined(HAVE_WAYLAND)

#define DGL_USING_WAYLAND

// Wayland specific, update world without triggering exposure events
PuglStatus puglWaylandUpdateWithoutExposures(PuglWorld* world);

// Wayland specific, set the xdg-shell application id
// This is the closest analogue to puglX11SetWindowType: Wayland has no window-type atoms, the
// compositor keys window rules and icon matching off the app id instead.
void puglWaylandSetAppId(PuglView* view, const char* appId);

// Wayland specific, get this view's xdg-foreign handle, or null if it has none
// The handle is asked for while realizing and arrives in a later event, so it is null until the
// compositor answers, and stays null on a compositor without xdg-foreign.
const char* puglWaylandGetExportedHandle(const PuglView* view);

#endif

// --------------------------------------------------------------------------------------------------------------------

#ifndef DAF_OS_MAC
END_NAMESPACE_DGL
#endif

#endif // DGL_PUGL_HPP_INCLUDED
