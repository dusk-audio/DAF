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

// pugl-extra/wayland_cairo.c, included at the end of this unity build, calls memfd_create() and
// mkostemp(), which glibc only declares under _GNU_SOURCE. g++ and clang++ pass -D_GNU_SOURCE for
// C++ on Linux themselves, but that is a driver default rather than a guarantee, and the define has
// to be in place before the first system header of the translation unit, hence here.
#if defined(__linux__) && !defined(_GNU_SOURCE)
# define _GNU_SOURCE
#endif

#include "pugl.hpp"

// --------------------------------------------------------------------------------------------------------------------
// include base headers

#ifdef DGL_CAIRO
# include <cairo.h>
#endif
#ifdef DGL_OPENGL
# include "../OpenGL-include.hpp"
#endif
#ifdef DGL_VULKAN
# include <vulkan/vulkan_core.h>
#endif

/* we will include all header files used in pugl in their C++ friendly form, then pugl stuff in custom namespace */
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>

#if defined(DAF_OS_HAIKU)
# include <Application.h>
# include <Window.h>
# ifdef DGL_OPENGL
#  include <GL/gl.h>
#  include <opengl/GLView.h>
# endif
#elif defined(DAF_OS_MAC)
# import <Cocoa/Cocoa.h>
# include <dlfcn.h>
# include <mach/mach_time.h>
# ifdef DGL_CAIRO
#  include <cairo-quartz.h>
# endif
# ifdef DGL_VULKAN
#  import <QuartzCore/CAMetalLayer.h>
#  include <vulkan/vulkan_macos.h>
# endif
#elif defined(DAF_OS_WASM)
# include <emscripten/emscripten.h>
# include <emscripten/html5.h>
# ifdef DGL_OPENGL
#  include <EGL/egl.h>
# endif
#elif defined(DAF_OS_WINDOWS)
# include <wctype.h>
# include <winsock2.h>
# include <windows.h>
# include <windowsx.h>
# ifdef DGL_CAIRO
#  include <cairo-win32.h>
# endif
# ifdef DGL_OPENGL
#  include <GL/gl.h>
# endif
# ifdef DGL_VULKAN
#  include <vulkan/vulkan.h>
#  include <vulkan/vulkan_win32.h>
# endif
#elif defined(HAVE_X11)
# include <dlfcn.h>
# include <limits.h>
# include <unistd.h>
# include <sys/select.h>
// # include <sys/time.h>
# include <X11/X.h>
# include <X11/Xatom.h>
# include <X11/Xlib.h>
# include <X11/Xresource.h>
# include <X11/Xutil.h>
# include <X11/keysym.h>
# ifdef HAVE_XCURSOR
#  include <X11/Xcursor/Xcursor.h>
// #  include <X11/cursorfont.h>
# endif
# ifdef HAVE_XRANDR
#  include <X11/extensions/Xrandr.h>
# endif
# ifdef HAVE_XSYNC
#  include <X11/extensions/sync.h>
#  include <X11/extensions/syncconst.h>
# endif
# ifdef DGL_CAIRO
#  include <cairo-xlib.h>
# endif
# ifdef DGL_OPENGL
#  include <GL/glx.h>
# endif
# ifdef DGL_VULKAN
#  include <vulkan/vulkan_xlib.h>
# endif
// NOTE: reachable only when HAVE_X11 is undefined -- see the backend policy comment in pugl.hpp.
// The arm ordering here must stay identical to the one there, or the headers pulled in would stop
// matching the backend that gets compiled below.
#elif defined(HAVE_WAYLAND)
# include <dlfcn.h>
# include <errno.h>
# include <fcntl.h>
# include <limits.h>
# include <poll.h>
# include <unistd.h>
# include <sys/mman.h>
# include <sys/select.h>
# ifdef __linux__
#  include <sys/timerfd.h>
# endif
# include <wayland-client.h>
# include <wayland-cursor.h>
# include <xkbcommon/xkbcommon.h>
# include <xkbcommon/xkbcommon-compose.h>
# include <xkbcommon/xkbcommon-keysyms.h>
/* Wayland protocol bindings, pre-generated and vendored so that neither wayland-scanner nor the
   wayland-protocols data package is needed to build. See pugl-extra/wayland-protocols/README.

   The marshalling code has to stay at global scope with C linkage: it declares the wl_*_interface
   symbols that libwayland-client exports, and defines the xdg, zxdg and wp ones it needs itself.
   Pulling it into the DGL namespace would declare a second, namespaced set of the former that could
   never resolve at link time.

   It is also included *before* the matching client headers on purpose: the generated code declares
   the interfaces it defines, and seeing a later `extern` declaration from the client header first
   would make those definitions extern too.

   What the generated code does *not* do is give those definitions hidden visibility.  wayland-
   scanner emits its own forward declarations without WL_PRIVATE, so by the time the attribute
   appears on the definition it is too late to apply and the compiler ignores it -- which is exactly
   the warning that -Wattributes is silenced for here.  The enclosing extern "C" block keeps the
   linkage external as well, so nothing in this file stops a symbol from being exported.

   Isolation from a host that links its own copy of the same protocol therefore comes entirely from
   the build's global -fvisibility=hidden (see Makefile.base.mk), which release builds set.  DEBUG
   builds do not, so a debug plugin loaded into a host that also uses xdg-shell can have its
   xdg_*_interface symbols interposed by the host's copies. The layouts are generated from the same
   protocol XML, so in practice this is benign, but it is worth knowing when debugging one. */
# if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wattributes"
# endif
extern "C" {
# include "pugl-extra/wayland-protocols/xdg-shell-protocol.c"
# include "pugl-extra/wayland-protocols/xdg-decoration-unstable-v1-protocol.c"
# include "pugl-extra/wayland-protocols/viewporter-protocol.c"
# include "pugl-extra/wayland-protocols/fractional-scale-v1-protocol.c"
# include "pugl-extra/wayland-protocols/xdg-foreign-unstable-v2-protocol.c"
}
# if defined(__GNUC__)
#  pragma GCC diagnostic pop
# endif
# include "pugl-extra/wayland-protocols/xdg-shell-client-protocol.h"
# include "pugl-extra/wayland-protocols/xdg-decoration-unstable-v1-client-protocol.h"
# include "pugl-extra/wayland-protocols/viewporter-client-protocol.h"
# include "pugl-extra/wayland-protocols/fractional-scale-v1-client-protocol.h"
# include "pugl-extra/wayland-protocols/xdg-foreign-unstable-v2-client-protocol.h"
# ifdef DGL_CAIRO
#  include <cairo.h>
# endif
# ifdef DGL_OPENGL
#  include <EGL/egl.h>
#  include <EGL/eglext.h>
#  include <wayland-egl.h>
# endif
// NOTE: no vulkan/vulkan_wayland.h here on purpose. There is no wayland_vulkan backend to include
// below, so the header would only be dead weight ahead of the diagnostic in the backend arm.
#endif

#ifdef DGL_USE_FILE_BROWSER
# define DGL_FILE_BROWSER_DIALOG_HPP_INCLUDED
# define FILE_BROWSER_DIALOG_DGL_NAMESPACE
# define FILE_BROWSER_DIALOG_NAMESPACE DGL_NAMESPACE
START_NAMESPACE_DGL
# include "../../daf/extra/FileBrowserDialogImpl.hpp"
END_NAMESPACE_DGL
# include "../../daf/extra/FileBrowserDialogImpl.cpp"
#endif

#ifdef DGL_USE_WEB_VIEW
# define DGL_WEB_VIEW_HPP_INCLUDED
# define WEB_VIEW_NAMESPACE DGL_NAMESPACE
# define WEB_VIEW_DGL_NAMESPACE
START_NAMESPACE_DGL
# include "../../daf/extra/WebViewImpl.hpp"
END_NAMESPACE_DGL
# include "../../daf/extra/WebViewImpl.cpp"
#endif

#if defined(DGL_USING_X11) && defined(DGL_X11_WINDOW_ICON_NAME)
extern const ulong* DGL_X11_WINDOW_ICON_NAME;
#endif

#ifndef DAF_OS_MAC
START_NAMESPACE_DGL
#endif

// --------------------------------------------------------------------------------------------------------------------

#if defined(DAF_OS_HAIKU)
# include "pugl-extra/haiku.cpp"
# include "pugl-extra/haiku_stub.cpp"
# ifdef DGL_OPENGL
#  include "pugl-extra/haiku_gl.cpp"
# endif
#elif defined(DAF_OS_MAC)
# ifndef DAF_MACOS_NAMESPACE_MACRO
#  ifndef DAF_MACOS_NAMESPACE_TIME
#   define DAF_MACOS_NAMESPACE_TIME __apple_build_version__
#  endif
#  define DAF_MACOS_NAMESPACE_MACRO_HELPER(NS, SEP, TIME, INTERFACE) NS ## SEP ## TIME ## SEP ## INTERFACE
#  define DAF_MACOS_NAMESPACE_MACRO(NS, TIME, INTERFACE) DAF_MACOS_NAMESPACE_MACRO_HELPER(NS, _, TIME, INTERFACE)
#  define PuglCairoView      DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, PuglCairoView)
#  define PuglOpenGLView     DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, PuglOpenGLView)
#  define PuglStubView       DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, PuglStubView)
#  define PuglVulkanView     DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, PuglVulkanView)
#  define PuglWindow         DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, PuglWindow)
#  define PuglWindowDelegate DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, PuglWindowDelegate)
#  define PuglWrapperView    DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, PuglWrapperView)
# endif
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wdeprecated-declarations"
# import "pugl-upstream/src/mac.m"
# import "pugl-upstream/src/mac_stub.m"
# ifdef DGL_CAIRO
#  import "pugl-upstream/src/mac_cairo.m"
# endif
# ifdef DGL_OPENGL
#  import "pugl-upstream/src/mac_gl.m"
# endif
# ifdef DGL_VULKAN
#  import "pugl-upstream/src/mac_vulkan.m"
# endif
# pragma clang diagnostic pop
#elif defined(DAF_OS_WASM)
# include "pugl-extra/wasm.c"
# include "pugl-extra/wasm_stub.c"
# ifdef DGL_OPENGL
#  include "pugl-extra/wasm_gl.c"
# endif
#elif defined(DAF_OS_WINDOWS)
# include "pugl-upstream/src/win.c"
# include "pugl-upstream/src/win_stub.c"
# ifdef DGL_CAIRO
#  include "pugl-upstream/src/win_cairo.c"
# endif
# ifdef DGL_OPENGL
#  include "pugl-upstream/src/win_gl.c"
# endif
# ifdef DGL_VULKAN
#  include "pugl-upstream/src/win_vulkan.c"
# endif
#elif defined(HAVE_X11)
# if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wsign-conversion"
# endif
# include "pugl-upstream/src/x11.c"
# if defined(__GNUC__) && (__GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6))
#  pragma GCC diagnostic pop
# endif
# include "pugl-upstream/src/x11_stub.c"
# ifdef DGL_CAIRO
#  include "pugl-upstream/src/x11_cairo.c"
# endif
# ifdef DGL_OPENGL
#  include "pugl-upstream/src/x11_gl.c"
# endif
# ifdef DGL_VULKAN
#  include "pugl-upstream/src/x11_vulkan.c"
# endif
#elif defined(HAVE_WAYLAND)
// The build system will only ever define HAVE_WAYLAND without HAVE_X11 (see pugl.hpp), so reaching
// this point means a genuine Wayland-only build was requested.
# include "pugl-extra/wayland.c"
# include "pugl-extra/wayland_stub.c"
# ifdef DGL_CAIRO
#  include "pugl-extra/wayland_cairo.c"
# endif
# ifdef DGL_OPENGL
#  include "pugl-extra/wayland_gl.c"
# endif
# ifdef DGL_VULKAN
// There is no wayland_vulkan.c, but puglSetMatchingBackendForCurrentBuild below calls
// puglVulkanBackend() whenever DGL_VULKAN is set. Say so here rather than leaving the build to fail
// at link time with an undefined reference. Reachable via the CMake UI_TYPE vulkan option.
#  error "Vulkan is not implemented on the DGL Wayland backend"
# endif
#endif

#include "pugl-upstream/src/common.c"
#include "pugl-upstream/src/internal.c"

// --------------------------------------------------------------------------------------------------------------------
// DGL specific, expose backend enter

bool puglBackendEnter(PuglView* const view)
{
    return view->backend->enter(view, nullptr) == PUGL_SUCCESS;
}

// --------------------------------------------------------------------------------------------------------------------
// DGL specific, expose backend leave

bool puglBackendLeave(PuglView* const view)
{
    return view->backend->leave(view, nullptr) == PUGL_SUCCESS;
}

// --------------------------------------------------------------------------------------------------------------------
// DGL specific, assigns backend that matches current DGL build

void puglSetMatchingBackendForCurrentBuild(PuglView* const view)
{
   #ifdef DGL_CAIRO
    puglSetBackend(view, puglCairoBackend());
   #endif
   #ifdef DGL_OPENGL
    puglSetBackend(view, puglGlBackend());
   #endif
   #ifdef DGL_VULKAN
    puglSetBackend(view, puglVulkanBackend());
   #endif

    if (view->backend != nullptr)
    {
       #if defined(DGL_USE_GLES2)
        puglSetViewHint(view, PUGL_CONTEXT_API, PUGL_OPENGL_ES_API);
        puglSetViewHint(view, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_CORE_PROFILE);
        puglSetViewHint(view, PUGL_CONTEXT_VERSION_MAJOR, 2);
       #elif defined(DGL_USE_GLES3)
        puglSetViewHint(view, PUGL_CONTEXT_API, PUGL_OPENGL_ES_API);
        puglSetViewHint(view, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_CORE_PROFILE);
        puglSetViewHint(view, PUGL_CONTEXT_VERSION_MAJOR, 3);
       #elif defined(DGL_USE_OPENGL3)
        puglSetViewHint(view, PUGL_CONTEXT_API, PUGL_OPENGL_API);
        puglSetViewHint(view, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_CORE_PROFILE);
        puglSetViewHint(view, PUGL_CONTEXT_VERSION_MAJOR, 3);
       #elif defined(DGL_OPENGL)
        puglSetViewHint(view, PUGL_CONTEXT_API, PUGL_OPENGL_API);
        puglSetViewHint(view, PUGL_CONTEXT_PROFILE, PUGL_OPENGL_COMPATIBILITY_PROFILE);
        puglSetViewHint(view, PUGL_CONTEXT_VERSION_MAJOR, 2);
       #endif
    }
    else
    {
        puglSetBackend(view, puglStubBackend());
    }
}

// --------------------------------------------------------------------------------------------------------------------
// bring view window into the foreground, aka "raise" window

void puglRaiseWindow(PuglView* const view)
{
    // this does the same as puglShow(view, PUGL_SHOW_FORCE_RAISE) + puglShow(view, PUGL_SHOW_RAISE)
   #if defined(DAF_OS_HAIKU)
   #elif defined(DAF_OS_MAC)
    NSWindow* const window = [view->impl->wrapperView window];
    [window orderFrontRegardless];
    [window orderFront:view->impl->wrapperView];
   #elif defined(DAF_OS_WASM)
    // nothing
   #elif defined(DAF_OS_WINDOWS)
    SetForegroundWindow(view->impl->hwnd);
    SetActiveWindow(view->impl->hwnd);
   #elif defined(HAVE_X11)
    XRaiseWindow(view->world->impl->display, view->impl->win);
   #elif defined(HAVE_WAYLAND)
    // nothing: a Wayland client cannot raise itself, stacking is entirely the compositor's business
   #endif
}

// --------------------------------------------------------------------------------------------------------------------
// Combined puglSetSizeHint using PUGL_MIN_SIZE and PUGL_FIXED_ASPECT

PuglStatus puglSetGeometryConstraints(PuglView* const view, const uint width, const uint height, const bool aspect)
{
    view->sizeHints[PUGL_MIN_SIZE].width = static_cast<PuglSpan>(width);
    view->sizeHints[PUGL_MIN_SIZE].height = static_cast<PuglSpan>(height);

    if (aspect)
    {
        view->sizeHints[PUGL_FIXED_ASPECT].width = static_cast<PuglSpan>(width);
        view->sizeHints[PUGL_FIXED_ASPECT].height = static_cast<PuglSpan>(height);
    }

   #if defined(DAF_OS_HAIKU)
   #elif defined(DAF_OS_MAC)
    if (view->impl->window)
    {
        if (const PuglStatus status = puglUpdateSizeHints(view))
            return status;
    }
   #elif defined(DAF_OS_WASM)
    const char* const className = view->world->strings[PUGL_CLASS_NAME];
    EM_ASM({
      var canvasWrapper = document.getElementById(UTF8ToString($0)).parentElement;
      canvasWrapper.style.setProperty("min-width", parseInt($1 / window.devicePixelRatio) + 'px');
      canvasWrapper.style.setProperty("min-height", parseInt($2 / window.devicePixelRatio) + 'px');
    }, className, width, height);
   #elif defined(DAF_OS_WINDOWS)
    // nothing
   #elif defined(HAVE_X11)
    if (view->impl->win)
    {
        if (const PuglStatus status = puglUpdateSizeHints(view))
            return status;

        XFlush(view->world->impl->display);
    }
   #elif defined(HAVE_WAYLAND)
    // xdg_toplevel has min/max size but no aspect ratio, see puglUpdateSizeHints in wayland.c
    if (view->impl->xdgToplevel)
    {
        if (const PuglStatus status = puglUpdateSizeHints(view))
            return status;

        wl_display_flush(view->world->impl->display);
    }
   #endif

    return PUGL_SUCCESS;
}

// --------------------------------------------------------------------------------------------------------------------
// set view as resizable (or not) during runtime

void puglSetResizable(PuglView* const view, const bool resizable)
{
    puglSetViewHint(view, PUGL_RESIZABLE, resizable ? PUGL_TRUE : PUGL_FALSE);

   #if defined(DAF_OS_HAIKU)
   #elif defined(DAF_OS_MAC)
    if (PuglWindow* const window = view->impl->window)
    {
        const uint style = (NSClosableWindowMask | NSTitledWindowMask | NSMiniaturizableWindowMask)
                         | (resizable ? NSResizableWindowMask : 0);
        [window setStyleMask:style];
    }
    // FIXME use [view setAutoresizingMask:NSViewNotSizable] ?
   #elif defined(DAF_OS_WASM)
    puglUpdateSizeHints(view);
   #elif defined(DAF_OS_WINDOWS)
    if (const HWND hwnd = view->impl->hwnd)
    {
        const uint winFlags = resizable ? GetWindowLong(hwnd, GWL_STYLE) |  (WS_SIZEBOX | WS_MAXIMIZEBOX)
                                        : GetWindowLong(hwnd, GWL_STYLE) & ~(WS_SIZEBOX | WS_MAXIMIZEBOX);
        SetWindowLong(hwnd, GWL_STYLE, winFlags);
    }
   #elif defined(HAVE_X11)
    puglUpdateSizeHints(view);
   #elif defined(HAVE_WAYLAND)
    puglUpdateSizeHints(view);
   #endif
}

// --------------------------------------------------------------------------------------------------------------------
// set window size while also changing default

PuglStatus puglSetSizeAndDefault(PuglView* const view, const uint width, const uint height)
{
    // set default size first
    view->sizeHints[PUGL_DEFAULT_SIZE].width = view->sizeHints[PUGL_CURRENT_SIZE].width = width;
    view->sizeHints[PUGL_DEFAULT_SIZE].height = view->sizeHints[PUGL_CURRENT_SIZE].height = height;

   #if defined(DAF_OS_HAIKU)
   #elif defined(DAF_OS_MAC)
    // matches upstream pugl
    if (view->impl->wrapperView)
    {
        // nothing to do for PUGL_DEFAULT_SIZE hint

        if (const PuglStatus status = puglSetWindowSize(view, width, height))
            return status;
    }
   #elif defined(DAF_OS_WASM)
    if (const PuglStatus status = puglUpdateSizeHints(view))
        return status;

    emscripten_set_canvas_element_size(view->world->strings[PUGL_CLASS_NAME], width, height);
   #elif defined(DAF_OS_WINDOWS)
    // matches upstream pugl, except we re-enter context after resize
    if (view->impl->hwnd)
    {
        // nothing to do for PUGL_DEFAULT_SIZE hint

        if (const PuglStatus status = puglSetWindowSize(view, width, height))
            return status;

        // make sure to return context back to ourselves
        puglBackendEnter(view);
    }
   #elif defined(HAVE_X11)
    // matches upstream pugl, adds flush at the end
    if (view->impl->win)
    {
        if (const PuglStatus status = puglUpdateSizeHints(view))
            return status;

        if (const PuglStatus status = puglSetWindowSize(view, width, height))
            return status;

        // flush size changes
        XFlush(view->world->impl->display);
    }
   #elif defined(HAVE_WAYLAND)
    if (view->impl->wlSurface)
    {
        if (const PuglStatus status = puglUpdateSizeHints(view))
            return status;

        if (const PuglStatus status = puglSetWindowSize(view, width, height))
            return status;

        wl_display_flush(view->world->impl->display);
    }
   #endif

    return PUGL_SUCCESS;
}

// --------------------------------------------------------------------------------------------------------------------
// DGL specific, build-specific drawing prepare

void puglOnDisplayPrepare(PuglView*)
{
  #ifdef DGL_OPENGL
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
   #ifndef DGL_USE_OPENGL3
    glLoadIdentity();
   #endif
  #endif
}

// --------------------------------------------------------------------------------------------------------------------
// DGL specific, build-specific fallback resize

void puglFallbackOnResize(PuglView* const view, const uint width, const uint height)
{
  #ifdef DGL_OPENGL
   #if defined(DGL_USE_OPENGL3)
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
   #else
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0.0, static_cast<GLdouble>(width), static_cast<GLdouble>(height), 0.0, 0.0, 1.0);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
   #endif
  #else
    // unused
    (void)view;
    (void)width;
    (void)height;
  #endif
}

// --------------------------------------------------------------------------------------------------------------------

#if defined(DAF_OS_HAIKU)

// --------------------------------------------------------------------------------------------------------------------

#elif defined(DAF_OS_MAC)

// --------------------------------------------------------------------------------------------------------------------
// macOS specific, add another view's window as child

PuglStatus
puglMacOSAddChildWindow(PuglView* const view, PuglView* const child)
{
    if (NSWindow* const viewWindow = view->impl->window ? view->impl->window
                                                        : [view->impl->wrapperView window])
    {
        if (NSWindow* const childWindow = child->impl->window ? child->impl->window
                                                              : [child->impl->wrapperView window])
        {
            [viewWindow addChildWindow:childWindow ordered:NSWindowAbove];
            return PUGL_SUCCESS;
        }
    }

    return PUGL_FAILURE;
}

// --------------------------------------------------------------------------------------------------------------------
// macOS specific, remove another view's window as child

PuglStatus
puglMacOSRemoveChildWindow(PuglView* const view, PuglView* const child)
{
    if (NSWindow* const viewWindow = view->impl->window ? view->impl->window
                                                        : [view->impl->wrapperView window])
    {
        if (NSWindow* const childWindow = child->impl->window ? child->impl->window
                                                              : [child->impl->wrapperView window])
        {
            [viewWindow removeChildWindow:childWindow];
            return PUGL_SUCCESS;
        }
    }

    return PUGL_FAILURE;
}

// --------------------------------------------------------------------------------------------------------------------
// macOS specific, center view based on parent coordinates (if there is one)

void puglMacOSShowCentered(PuglView* const view)
{
    if (puglShow(view, PUGL_SHOW_RAISE) != PUGL_SUCCESS)
        return;

    if (view->transientParent != 0)
    {
        NSWindow* const transientWindow = [(NSView*)view->transientParent window];
        DAF_SAFE_ASSERT_RETURN(transientWindow != nullptr,);

        const NSRect ourFrame       = [view->impl->window frame];
        const NSRect transientFrame = [transientWindow frame];

        const int x = transientFrame.origin.x + (transientFrame.size.width - ourFrame.size.width) / 2;
        const int y = transientFrame.origin.y + (transientFrame.size.height - ourFrame.size.height) / 2;

        [view->impl->window setFrameTopLeftPoint:NSMakePoint(x, y)];
    }
    else
    {
        [view->impl->window center];
    }
}

// --------------------------------------------------------------------------------------------------------------------

#elif defined(DAF_OS_WINDOWS)

// --------------------------------------------------------------------------------------------------------------------
// win32 specific, call ShowWindow with SW_RESTORE

void puglWin32RestoreWindow(PuglView* const view)
{
    PuglInternals* impl = view->impl;
    DAF_SAFE_ASSERT_RETURN(impl->hwnd != nullptr,);

    ShowWindow(impl->hwnd, SW_RESTORE);
    SetFocus(impl->hwnd);
}

// --------------------------------------------------------------------------------------------------------------------
// win32 specific, center view based on parent coordinates (if there is one)

void puglWin32ShowCentered(PuglView* const view)
{
    PuglInternals* impl = view->impl;
    DAF_SAFE_ASSERT_RETURN(impl->hwnd != nullptr,);

    RECT rectChild, rectParent;

    if (view->transientParent != 0 &&
        GetWindowRect(impl->hwnd, &rectChild) &&
        GetWindowRect((HWND)view->transientParent, &rectParent))
    {
        SetWindowPos(impl->hwnd, HWND_TOP,
                     rectParent.left + (rectParent.right-rectParent.left)/2 - (rectChild.right-rectChild.left)/2,
                     rectParent.top + (rectParent.bottom-rectParent.top)/2 - (rectChild.bottom-rectChild.top)/2,
                     0, 0, SWP_SHOWWINDOW|SWP_NOSIZE);
    }
    else
    {
        MONITORINFO mInfo;
        std::memset(&mInfo, 0, sizeof(mInfo));
        mInfo.cbSize = sizeof(mInfo);

        if (GetMonitorInfo(MonitorFromWindow(impl->hwnd, MONITOR_DEFAULTTOPRIMARY), &mInfo))
            SetWindowPos(impl->hwnd, HWND_TOP,
                         mInfo.rcWork.left + (mInfo.rcWork.right - mInfo.rcWork.left - view->lastConfigure.width) / 2,
                         mInfo.rcWork.top + (mInfo.rcWork.bottom - mInfo.rcWork.top - view->lastConfigure.height) / 2,
                         0, 0, SWP_SHOWWINDOW|SWP_NOSIZE);
        else
            ShowWindow(impl->hwnd, SW_NORMAL);
    }

    SetFocus(impl->hwnd);
}

// --------------------------------------------------------------------------------------------------------------------

#elif defined(DAF_OS_WASM)

// nothing here yet

// --------------------------------------------------------------------------------------------------------------------

#elif defined(HAVE_X11)

// --------------------------------------------------------------------------------------------------------------------
// X11 specific, update world without triggering exposure events

PuglStatus puglX11UpdateWithoutExposures(PuglWorld* const world)
{
    const PuglWorldState startState = world->state;
    world->state = PUGL_WORLD_UPDATING;
    PuglStatus st = PUGL_SUCCESS;

    const double startTime = puglGetTime(world);
    const double endTime   = startTime + 0.03;

    for (double t = startTime; !st && t < endTime; t = puglGetTime(world))
    {
        if (!(st = pollX11Socket(world, endTime - t)))
            st = dispatchX11Events(world);
    }

    world->state = startState;
    return st;
}

// --------------------------------------------------------------------------------------------------------------------
// X11 specific, hand a key event no widget used to the window this view is embedded in

void puglX11ForwardKeyToParent(const PuglView* const view,
                               const bool press, const uint keycode, const uint mods, const uint time)
{
    if (view->parent == 0 || keycode == 0)
        return;

    Display* const display = view->world->impl->display;

    XEvent xevent = {};
    XKeyEvent& xkey(xevent.xkey);
    xkey.type        = press ? KeyPress : KeyRelease;
    xkey.display     = display;
    xkey.window      = static_cast<::Window>(view->parent);
    xkey.root        = RootWindow(display, view->impl->screen);
    xkey.subwindow   = None;
    xkey.time        = time;
    xkey.same_screen = True;
    xkey.keycode     = keycode;
    xkey.state       = ((mods & kModifierShift)   ? ShiftMask   : 0U)
                     | ((mods & kModifierControl) ? ControlMask : 0U)
                     | ((mods & kModifierAlt)     ? Mod1Mask    : 0U)
                     | ((mods & kModifierSuper)   ? Mod4Mask    : 0U);

    XSendEvent(display, xkey.window, True, press ? KeyPressMask : KeyReleaseMask, &xevent);
    XFlush(display);
}

// --------------------------------------------------------------------------------------------------------------------
// X11 specific, set dialog window type

void puglX11SetWindowType(const PuglView* const view, const bool isStandalone)
{
    const PuglInternals* const impl    = view->impl;
    Display*             const display = view->world->impl->display;

   #if defined(DGL_X11_WINDOW_ICON_NAME) && defined(DGL_X11_WINDOW_ICON_SIZE)
    if (isStandalone)
    {
        const Atom NET_WM_ICON = XInternAtom(display, "_NET_WM_ICON", False);
        XChangeProperty(display,
                        impl->win,
                        NET_WM_ICON,
                        XA_CARDINAL,
                        32,
                        PropModeReplace,
                        reinterpret_cast<const uchar*>(DGL_X11_WINDOW_ICON_NAME),
                        DGL_X11_WINDOW_ICON_SIZE);
    }
   #endif

    const Atom NET_WM_WINDOW_TYPE = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);

    Atom windowTypes[2];
    int numWindowTypes = 0;

    if (! isStandalone)
        windowTypes[numWindowTypes++] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False);

    windowTypes[numWindowTypes++] = XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False);

    XChangeProperty(display,
                    impl->win,
                    NET_WM_WINDOW_TYPE,
                    XA_ATOM,
                    32,
                    PropModeReplace,
                    reinterpret_cast<const uchar*>(&windowTypes),
                    numWindowTypes);
}

// --------------------------------------------------------------------------------------------------------------------

#elif defined(HAVE_WAYLAND)

// --------------------------------------------------------------------------------------------------------------------
// Wayland specific
//
// puglWaylandUpdateWithoutExposures() and puglWaylandSetAppId() are declared in pugl.hpp so that the
// Wayland arm there has the same shape as the X11 one. Their bodies live at the bottom of
// pugl-extra/wayland.c, next to the internals they need, rather than being duplicated here.

// --------------------------------------------------------------------------------------------------------------------

#endif // HAVE_X11 / HAVE_WAYLAND

#ifndef DAF_OS_MAC
END_NAMESPACE_DGL
#endif
