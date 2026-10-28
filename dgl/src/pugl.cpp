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
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
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
# include <ole2.h>
# include <shellapi.h>
# include <shlobj.h>
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
#endif

// DGL_USING_X11 or DGL_USING_WAYLAND comes from pugl.hpp, which picks the backend. Each block below
// holds the system headers of one backend.
#if defined(DGL_USING_X11)
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
# include <X11/cursorfont.h>
# include <sys/stat.h>
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
#endif

#if defined(DGL_USING_WAYLAND)
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
// pugl and the DGL extensions that reach into it

#if defined(DGL_USING_X11)
# define DGL_PUGL_X11
#elif defined(DGL_USING_WAYLAND)
# define DGL_PUGL_WAYLAND
#endif
#include "pugl-platform.hpp"

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

#ifndef DAF_OS_MAC
END_NAMESPACE_DGL
#endif
