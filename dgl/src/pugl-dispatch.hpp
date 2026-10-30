// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// The runtime-backend half of the pugl unity build (DGL_BACKEND=both), included by pugl.cpp inside
// the DGL namespace in place of a single pugl-platform.hpp. See dgl/src/runtime-backend.md.
//
// pugl-platform.hpp is compiled twice, once for X11 inside namespace pugl_x11 and once for Wayland
// inside namespace pugl_wl. Each pass is a complete pugl of its own: its view and world structs,
// platform functions, common.c and internal.c, graphics backends, and DGL's extensions. They only
// share what lives at DGL scope, which is pugl.h's public types (events, enums, geometry) and the
// two handle types PuglView and PuglWorld.
//
// The handles tie the passes together. Each pass derives its PuglViewImpl and PuglWorldImpl from the
// DGL-scope ones through the PUGL_*_IMPL_BASE hooks in types.h, so a pass object converts to the
// public handle implicitly and back with a static_cast, and the handle records which pass made it.
// The public pugl API and DGL's extensions are then defined below at DGL scope, each one forwarding
// to the pass that owns its argument.
//
// Inside a pass, PuglView and PuglWorld are redeclared to name the pass's own types, so the backend
// sources compile unchanged. A call there whose callee is only defined further down in the same pass
// binds to the forwarder here instead, by argument-dependent lookup through the base class, and
// reaches the same function one step later; dgl/src/runtime-backend.md lists the eleven calls that do.
// That is sound because each of them is made on an object that is complete and tagged by then, and
// the forwarders treat an untagged one as the corruption it would be.

#if ! defined(DGL_USING_X11) || ! defined(DGL_USING_WAYLAND)
# error "pugl-dispatch.hpp is the DGL_RUNTIME_BACKEND build, which needs both X11 and Wayland"
#endif

// --------------------------------------------------------------------------------------------------------------------
// public handles

enum PuglWindowSystem {
    kPuglWindowSystemNone,
    kPuglWindowSystemX11,
    kPuglWindowSystemWayland
};

struct PuglWorldImpl {
    PuglWindowSystem system;
};

struct PuglViewImpl {
    PuglWindowSystem system;
};

#define PUGL_WORLD_IMPL_BASE : public ::DGL_NAMESPACE::PuglWorldImpl
#define PUGL_VIEW_IMPL_BASE  : public ::DGL_NAMESPACE::PuglViewImpl

// --------------------------------------------------------------------------------------------------------------------
// X11 pass

namespace pugl_x11 {

struct PuglWorldImpl;
struct PuglViewImpl;
struct PuglBackendImpl;
typedef PuglWorldImpl   PuglWorld;
typedef PuglViewImpl    PuglView;
typedef PuglBackendImpl PuglBackend;

#define DGL_PUGL_X11
#include "pugl-platform.hpp"
#undef DGL_PUGL_X11

}

// The guarded pugl headers that declare things rather than only define macros have to be seen again
// by the second pass, which declares them in its own namespace.
#undef PUGL_SRC_TYPES_H
#undef PUGL_PLATFORM_H
#undef PUGL_INTERNAL_H
#undef PUGL_SRC_STUB_H
#undef PUGL_STUB_H
#undef PUGL_GL_H
#undef PUGL_CAIRO_H

// --------------------------------------------------------------------------------------------------------------------
// Wayland pass

namespace pugl_wl {

struct PuglWorldImpl;
struct PuglViewImpl;
struct PuglBackendImpl;
typedef PuglWorldImpl   PuglWorld;
typedef PuglViewImpl    PuglView;
typedef PuglBackendImpl PuglBackend;

#define DGL_PUGL_WAYLAND
#include "pugl-platform.hpp"
#undef DGL_PUGL_WAYLAND

}

#undef PUGL_WORLD_IMPL_BASE
#undef PUGL_VIEW_IMPL_BASE

// --------------------------------------------------------------------------------------------------------------------
// handle helpers

// Every world and view is tagged as soon as its pass has made it, see puglNewWorldOn and puglNewView.
static bool puglIsWayland(const PuglWindowSystem system)
{
    if (system == kPuglWindowSystemNone)
    {
        d_stderr2("DGL: a pugl world or view of no backend, not one puglNewWorld or puglNewView made");
        std::abort();
    }

    return system == kPuglWindowSystemWayland;
}

static inline bool puglIsWayland(const PuglWorld* const world)
{
    return puglIsWayland(world->system);
}

static inline bool puglIsWayland(const PuglView* const view)
{
    return puglIsWayland(view->system);
}

static inline pugl_x11::PuglWorld* asX11(PuglWorld* const world) { return static_cast<pugl_x11::PuglWorld*>(world); }
static inline const pugl_x11::PuglWorld* asX11(const PuglWorld* const world) { return static_cast<const pugl_x11::PuglWorld*>(world); }
static inline pugl_x11::PuglView* asX11(PuglView* const view) { return static_cast<pugl_x11::PuglView*>(view); }
static inline const pugl_x11::PuglView* asX11(const PuglView* const view) { return static_cast<const pugl_x11::PuglView*>(view); }

static inline pugl_wl::PuglWorld* asWayland(PuglWorld* const world) { return static_cast<pugl_wl::PuglWorld*>(world); }
static inline const pugl_wl::PuglWorld* asWayland(const PuglWorld* const world) { return static_cast<const pugl_wl::PuglWorld*>(world); }
static inline pugl_wl::PuglView* asWayland(PuglView* const view) { return static_cast<pugl_wl::PuglView*>(view); }
static inline const pugl_wl::PuglView* asWayland(const PuglView* const view) { return static_cast<const pugl_wl::PuglView*>(view); }

// Forward a call taking a world or view first, to the pass that owns it.
#define DGL_PUGL_FORWARD(object, function, ...) \
    (puglIsWayland(object) ? pugl_wl::function(asWayland(object), __VA_ARGS__) \
                           : pugl_x11::function(asX11(object), __VA_ARGS__))
#define DGL_PUGL_FORWARD0(object, function) \
    (puglIsWayland(object) ? pugl_wl::function(asWayland(object)) \
                           : pugl_x11::function(asX11(object)))

// --------------------------------------------------------------------------------------------------------------------
// backend selection

#ifdef DGL_OPENGL
// Whether EGL can give a window of this world the kind of context the build renders with. A compositor can
// be reachable while EGL is of no use on it, a driver without Wayland support say, which would otherwise
// only show when the first window failed to get its context.
static bool puglWaylandCanMakeGlContext(pugl_wl::PuglWorld* const world)
{
    const EGLDisplay display = pugl_wl::puglWaylandGlAcquireDisplay(world->impl);
    if (display == EGL_NO_DISPLAY)
        return false;

   #if defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3)
    const EGLenum api = EGL_OPENGL_ES_API;
    const EGLint renderableType = EGL_OPENGL_ES2_BIT;
   #else
    const EGLenum api = EGL_OPENGL_API;
    const EGLint renderableType = EGL_OPENGL_BIT;
   #endif
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, renderableType,
        EGL_NONE
    };

    EGLConfig config = nullptr;
    EGLint numConfigs = 0;
    const bool usable = eglBindAPI(api) == EGL_TRUE
                     && eglChooseConfig(display, attributes, &config, 1, &numConfigs) == EGL_TRUE
                     && numConfigs > 0;

    pugl_wl::puglWaylandGlReleaseDisplay(world->impl);
    return usable;
}
#endif

static PuglWorld* puglNewWorldOn(const PuglWindowSystem system, const PuglWorldType type, const PuglWorldFlags flags)
{
    if (system == kPuglWindowSystemWayland)
    {
        // nothing of Wayland is linked, see pugl-extra/wayland-loader-names.h
        if (const char* const failure = ::dglWaylandLoad())
        {
            d_stderr("DGL: cannot use Wayland, %s", failure);
            return nullptr;
        }

        pugl_wl::PuglWorld* const world = pugl_wl::puglNewWorld(type, flags);
        if (world == nullptr)
            return nullptr;

        world->system = kPuglWindowSystemWayland;

       #ifdef DGL_OPENGL
        if (! puglWaylandCanMakeGlContext(world))
        {
            d_stderr("DGL: cannot use Wayland, EGL cannot make an OpenGL context on this compositor");
            pugl_wl::puglFreeWorld(world);
            return nullptr;
        }
       #endif

        return world;
    }

    pugl_x11::PuglWorld* const world = pugl_x11::puglNewWorld(type, flags);
    if (world != nullptr)
        world->system = kPuglWindowSystemX11;
    return world;
}

static bool puglEnvIsSet(const char* const name)
{
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] != '\0';
}

static const char* puglEnvOrUnset(const char* const name)
{
    const char* const value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? value : "<unset>";
}

// A world on the one system that was asked for, or none: a failure to open it is final
static PuglWorld* puglNewForcedWorld(const PuglWindowSystem system,
                                     const PuglWorldType type,
                                     const PuglWorldFlags flags,
                                     const char* const forcedBy)
{
    // libwayland unsets WAYLAND_SOCKET when it takes the socket over, so the message below reports it as it was
    char waylandSocket[32];
    std::snprintf(waylandSocket, sizeof(waylandSocket), "%s", puglEnvOrUnset("WAYLAND_SOCKET"));

    if (PuglWorld* const world = puglNewWorldOn(system, type, flags))
        return world;

    d_stderr2("DGL: %s, but %s cannot be used here (WAYLAND_DISPLAY=%s, WAYLAND_SOCKET=%s, DISPLAY=%s)",
              forcedBy, system == kPuglWindowSystemX11 ? "X11" : "Wayland",
              puglEnvOrUnset("WAYLAND_DISPLAY"), waylandSocket, puglEnvOrUnset("DISPLAY"));
    return nullptr;
}

// The backend is chosen once, here, for the whole world:
//  - For a program, DAF_WINDOW_BACKEND=x11 or =wayland forces that backend, and a failure to open it
//    is final.
//  - Otherwise a program tries Wayland first, but only when WAYLAND_DISPLAY or WAYLAND_SOCKET names a
//    compositor (libwayland would fall back to wayland-0 on its own, which on an X11 session can be
//    anybody's), then X11.
//  - A module tries X11 first, whatever DAF_WINDOW_BACKEND says. Hosts embed plugin UIs through an X11
//    window id in every format DAF supports, so an X11 server, XWayland included, is what a module
//    needs whenever there is one.
// An application that asks for a backend of its own goes through puglNewWorldOnSystem instead.
PuglWorld* puglNewWorld(const PuglWorldType type, const PuglWorldFlags flags)
{
    if (const char* const requested = type == PUGL_PROGRAM ? std::getenv("DAF_WINDOW_BACKEND") : nullptr)
    {
        if (std::strcmp(requested, "x11") == 0)
            return puglNewForcedWorld(kPuglWindowSystemX11, type, flags, "DAF_WINDOW_BACKEND=x11");
        if (std::strcmp(requested, "wayland") == 0)
            return puglNewForcedWorld(kPuglWindowSystemWayland, type, flags, "DAF_WINDOW_BACKEND=wayland");
        if (requested[0] != '\0')
            d_stderr2("DGL: ignoring DAF_WINDOW_BACKEND=%s, expected x11 or wayland", requested);
    }

    const bool haveWayland = puglEnvIsSet("WAYLAND_DISPLAY") || puglEnvIsSet("WAYLAND_SOCKET");
    PuglWindowSystem order[2];

    if (type == PUGL_MODULE)
    {
        order[0] = kPuglWindowSystemX11;
        order[1] = haveWayland ? kPuglWindowSystemWayland : kPuglWindowSystemNone;
    }
    else
    {
        order[0] = haveWayland ? kPuglWindowSystemWayland : kPuglWindowSystemX11;
        order[1] = haveWayland ? kPuglWindowSystemX11 : kPuglWindowSystemNone;
    }

    // libwayland unsets WAYLAND_SOCKET when it takes the socket over, so the message below reports it as it was
    char waylandSocket[32];
    std::snprintf(waylandSocket, sizeof(waylandSocket), "%s", puglEnvOrUnset("WAYLAND_SOCKET"));

    for (const PuglWindowSystem system : order)
    {
        if (system == kPuglWindowSystemNone)
            break;
        if (PuglWorld* const world = puglNewWorldOn(system, type, flags))
            return world;
    }

    d_stderr2("DGL: no windowing system to open a window on, neither a Wayland compositor nor an X server "
              "could be reached (WAYLAND_DISPLAY=%s, WAYLAND_SOCKET=%s, DISPLAY=%s)",
              puglEnvOrUnset("WAYLAND_DISPLAY"), waylandSocket, puglEnvOrUnset("DISPLAY"));

    return nullptr;
}

PuglWorld* puglNewWorldOnSystem(const PuglWorldType type, const PuglWorldFlags flags, const bool x11)
{
    const char* const environment = type == PUGL_PROGRAM ? std::getenv("DAF_WINDOW_BACKEND") : nullptr;
    if (environment != nullptr && environment[0] != '\0' && std::strcmp(environment, x11 ? "x11" : "wayland") != 0)
        d_stderr2("DGL: ignoring DAF_WINDOW_BACKEND=%s, the application asked for %s",
                  environment, x11 ? "X11" : "Wayland");

    return puglNewForcedWorld(x11 ? kPuglWindowSystemX11 : kPuglWindowSystemWayland, type, flags,
                              x11 ? "the application asked for X11" : "the application asked for Wayland");
}

bool puglWorldIsX11(const PuglWorld* const world)
{
    return ! puglIsWayland(world);
}

bool puglViewIsX11(const PuglView* const view)
{
    return ! puglIsWayland(view);
}

// --------------------------------------------------------------------------------------------------------------------
// public pugl API, world

const char* puglStrerror(const PuglStatus status)
{
    return pugl_x11::puglStrerror(status);
}

void puglFreeWorld(PuglWorld* const world)
{
    DGL_PUGL_FORWARD0(world, puglFreeWorld);
}

void puglSetWorldHandle(PuglWorld* const world, void* const handle)
{
    DGL_PUGL_FORWARD(world, puglSetWorldHandle, handle);
}

void* puglGetWorldHandle(PuglWorld* const world)
{
    return DGL_PUGL_FORWARD0(world, puglGetWorldHandle);
}

void* puglGetNativeWorld(PuglWorld* const world)
{
    return DGL_PUGL_FORWARD0(world, puglGetNativeWorld);
}

PuglStatus puglSetWorldString(PuglWorld* const world, const PuglStringHint key, const char* const value)
{
    return DGL_PUGL_FORWARD(world, puglSetWorldString, key, value);
}

const char* puglGetWorldString(const PuglWorld* const world, const PuglStringHint key)
{
    return DGL_PUGL_FORWARD(world, puglGetWorldString, key);
}

double puglGetTime(const PuglWorld* const world)
{
    return DGL_PUGL_FORWARD0(world, puglGetTime);
}

PuglStatus puglUpdate(PuglWorld* const world, const double timeout)
{
    return DGL_PUGL_FORWARD(world, puglUpdate, timeout);
}

// --------------------------------------------------------------------------------------------------------------------
// public pugl API, view

PuglView* puglNewView(PuglWorld* const world)
{
    if (puglIsWayland(world))
    {
        pugl_wl::PuglView* const view = pugl_wl::puglNewView(asWayland(world));
        if (view != nullptr)
            view->system = kPuglWindowSystemWayland;
        return view;
    }

    pugl_x11::PuglView* const view = pugl_x11::puglNewView(asX11(world));
    if (view != nullptr)
        view->system = kPuglWindowSystemX11;
    return view;
}

void puglFreeView(PuglView* const view)
{
    DGL_PUGL_FORWARD0(view, puglFreeView);
}

PuglWorld* puglGetWorld(PuglView* const view)
{
    if (puglIsWayland(view))
        return pugl_wl::puglGetWorld(asWayland(view));
    return pugl_x11::puglGetWorld(asX11(view));
}

void puglSetHandle(PuglView* const view, void* const handle)
{
    DGL_PUGL_FORWARD(view, puglSetHandle, handle);
}

void* puglGetHandle(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetHandle);
}

PuglStatus puglSetEventFunc(PuglView* const view, const PuglEventFunc eventFunc)
{
    return DGL_PUGL_FORWARD(view, puglSetEventFunc, eventFunc);
}

PuglStatus puglSetViewHint(PuglView* const view, const PuglViewHint hint, const int value)
{
    return DGL_PUGL_FORWARD(view, puglSetViewHint, hint, value);
}

int puglGetViewHint(const PuglView* const view, const PuglViewHint hint)
{
    return DGL_PUGL_FORWARD(view, puglGetViewHint, hint);
}

PuglStatus puglSetViewString(PuglView* const view, const PuglStringHint key, const char* const value)
{
    return DGL_PUGL_FORWARD(view, puglSetViewString, key, value);
}

const char* puglGetViewString(const PuglView* const view, const PuglStringHint key)
{
    return DGL_PUGL_FORWARD(view, puglGetViewString, key);
}

double puglGetScaleFactor(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetScaleFactor);
}

PuglPoint puglGetPositionHint(const PuglView* const view, const PuglPositionHint hint)
{
    return DGL_PUGL_FORWARD(view, puglGetPositionHint, hint);
}

PuglStatus puglSetPositionHint(PuglView* const view, const PuglPositionHint hint, const int x, const int y)
{
    return DGL_PUGL_FORWARD(view, puglSetPositionHint, hint, x, y);
}

PuglArea puglGetSizeHint(const PuglView* const view, const PuglSizeHint hint)
{
    return DGL_PUGL_FORWARD(view, puglGetSizeHint, hint);
}

PuglStatus puglSetSizeHint(PuglView* const view, const PuglSizeHint hint, const unsigned width, const unsigned height)
{
    return DGL_PUGL_FORWARD(view, puglSetSizeHint, hint, width, height);
}

PuglStatus puglSetParent(PuglView* const view, const PuglNativeView parent)
{
    return DGL_PUGL_FORWARD(view, puglSetParent, parent);
}

PuglNativeView puglGetParent(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetParent);
}

PuglStatus puglSetTransientParent(PuglView* const view, const PuglNativeView parent)
{
    return DGL_PUGL_FORWARD(view, puglSetTransientParent, parent);
}

PuglNativeView puglGetTransientParent(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetTransientParent);
}

PuglStatus puglRealize(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglRealize);
}

PuglStatus puglUnrealize(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglUnrealize);
}

PuglStatus puglShow(PuglView* const view, const PuglShowCommand command)
{
    return DGL_PUGL_FORWARD(view, puglShow, command);
}

PuglStatus puglHide(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglHide);
}

PuglStatus puglSetViewStyle(PuglView* const view, const PuglViewStyleFlags flags)
{
    return DGL_PUGL_FORWARD(view, puglSetViewStyle, flags);
}

PuglViewStyleFlags puglGetViewStyle(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetViewStyle);
}

bool puglGetVisible(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetVisible);
}

PuglNativeView puglGetNativeView(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetNativeView);
}

void* puglGetContext(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetContext);
}

PuglStatus puglObscureView(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglObscureView);
}

PuglStatus puglObscureRegion(PuglView* const view, const int x, const int y, const unsigned width, const unsigned height)
{
    return DGL_PUGL_FORWARD(view, puglObscureRegion, x, y, width, height);
}

PuglStatus puglGrabFocus(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGrabFocus);
}

bool puglHasFocus(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglHasFocus);
}

PuglStatus puglPaste(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglPaste);
}

uint32_t puglGetNumClipboardTypes(const PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglGetNumClipboardTypes);
}

const char* puglGetClipboardType(const PuglView* const view, const uint32_t typeIndex)
{
    return DGL_PUGL_FORWARD(view, puglGetClipboardType, typeIndex);
}

PuglStatus puglAcceptOffer(PuglView* const view, const PuglDataOfferEvent* const offer, const uint32_t typeIndex)
{
    return DGL_PUGL_FORWARD(view, puglAcceptOffer, offer, typeIndex);
}

PuglStatus puglSetClipboard(PuglView* const view, const char* const type, const void* const data, const size_t len)
{
    return DGL_PUGL_FORWARD(view, puglSetClipboard, type, data, len);
}

const void* puglGetClipboard(PuglView* const view, const uint32_t typeIndex, size_t* const len)
{
    return DGL_PUGL_FORWARD(view, puglGetClipboard, typeIndex, len);
}

PuglStatus puglSetCursor(PuglView* const view, const PuglCursor cursor)
{
    return DGL_PUGL_FORWARD(view, puglSetCursor, cursor);
}

PuglStatus puglStartTimer(PuglView* const view, const uintptr_t id, const double timeout)
{
    return DGL_PUGL_FORWARD(view, puglStartTimer, id, timeout);
}

PuglStatus puglStopTimer(PuglView* const view, const uintptr_t id)
{
    return DGL_PUGL_FORWARD(view, puglStopTimer, id);
}

PuglStatus puglSendEvent(PuglView* const view, const PuglEvent* const event)
{
    return DGL_PUGL_FORWARD(view, puglSendEvent, event);
}

// --------------------------------------------------------------------------------------------------------------------
// DGL extensions declared in pugl.hpp

PuglFileDrag* puglStartFileDrag(PuglView* const view, const char* const filename)
{
    return DGL_PUGL_FORWARD(view, puglStartFileDrag, filename);
}

PuglStatus puglSetAcceptsFileDrops(PuglView* const view, const bool accept)
{
    return DGL_PUGL_FORWARD(view, puglSetAcceptsFileDrops, accept);
}

bool puglBackendEnter(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglBackendEnter);
}

bool puglBackendLeave(PuglView* const view)
{
    return DGL_PUGL_FORWARD0(view, puglBackendLeave);
}

bool puglBackendIsCurrent(PuglView* const view, const bool fallback)
{
    // a null view gets the same answer from either pass
    if (view != nullptr && puglIsWayland(view))
        return pugl_wl::puglBackendIsCurrent(asWayland(view), fallback);
    return pugl_x11::puglBackendIsCurrent(asX11(view), fallback);
}

void puglSetMatchingBackendForCurrentBuild(PuglView* const view)
{
    DGL_PUGL_FORWARD0(view, puglSetMatchingBackendForCurrentBuild);
}

void puglRaiseWindow(PuglView* const view)
{
    DGL_PUGL_FORWARD0(view, puglRaiseWindow);
}

PuglStatus puglSetGeometryConstraints(PuglView* const view, const uint width, const uint height, const bool aspect)
{
    return DGL_PUGL_FORWARD(view, puglSetGeometryConstraints, width, height, aspect);
}

void puglSetResizable(PuglView* const view, const bool resizable)
{
    DGL_PUGL_FORWARD(view, puglSetResizable, resizable);
}

PuglStatus puglSetSizeAndDefault(PuglView* const view, const uint width, const uint height)
{
    return DGL_PUGL_FORWARD(view, puglSetSizeAndDefault, width, height);
}

// The backend-specific extensions do nothing, or fail, for an object of the other backend. Callers
// test DGL_VIEW_IS_X11 or DGL_VIEW_IS_WAYLAND first, so this only keeps a missed test harmless.

PuglStatus puglX11UpdateWithoutExposures(PuglWorld* const world)
{
    return puglIsWayland(world) ? PUGL_UNSUPPORTED : pugl_x11::puglX11UpdateWithoutExposures(asX11(world));
}

void puglX11SetWindowType(const PuglView* const view, const bool isStandalone)
{
    if (! puglIsWayland(view))
        pugl_x11::puglX11SetWindowType(asX11(view), isStandalone);
}

void puglX11ForwardKeyToParent(const PuglView* const view,
                               const bool press, const uint keycode, const uint mods, const uint time)
{
    if (! puglIsWayland(view))
        pugl_x11::puglX11ForwardKeyToParent(asX11(view), press, keycode, mods, time);
}

PuglStatus puglWaylandUpdateWithoutExposures(PuglWorld* const world)
{
    return puglIsWayland(world) ? pugl_wl::puglWaylandUpdateWithoutExposures(asWayland(world)) : PUGL_UNSUPPORTED;
}

void puglWaylandSetAppId(PuglView* const view, const char* const appId)
{
    if (puglIsWayland(view))
        pugl_wl::puglWaylandSetAppId(asWayland(view), appId);
}

const char* puglWaylandGetExportedHandle(const PuglView* const view)
{
    return view != nullptr && puglIsWayland(view) ? pugl_wl::puglWaylandGetExportedHandle(asWayland(view)) : nullptr;
}

#undef DGL_PUGL_FORWARD
#undef DGL_PUGL_FORWARD0

// --------------------------------------------------------------------------------------------------------------------
