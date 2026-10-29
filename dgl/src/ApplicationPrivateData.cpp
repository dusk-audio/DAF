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

#include "ApplicationPrivateData.hpp"
#include "../Window.hpp"

#include "pugl.hpp"

#include <ctime>

START_NAMESPACE_DGL

typedef std::list<DGL_NAMESPACE::Window*>::iterator WindowListIterator;
typedef std::list<DGL_NAMESPACE::Window*>::reverse_iterator WindowListReverseIterator;

static d_ThreadHandle getCurrentThreadHandle() noexcept
{
   #ifdef DAF_OS_WINDOWS
    return GetCurrentThread();
   #else
    return pthread_self();
   #endif
}

static bool isThisTheMainThread(const d_ThreadHandle mainThreadHandle) noexcept
{
   #ifdef DAF_OS_WINDOWS
    return GetCurrentThread() == mainThreadHandle; // IsGUIThread ?
   #else
    return pthread_equal(getCurrentThreadHandle(), mainThreadHandle) != 0;
   #endif
}

// --------------------------------------------------------------------------------------------------------------------

const char* Application::getClassName() const noexcept
{
    return puglGetWorldString(pData->world, PUGL_CLASS_NAME);
}

Application::WindowSystem Application::getWindowSystem() const noexcept
{
    if (pData->world == nullptr)
        return kWindowSystemAuto;

   #if defined(DGL_RUNTIME_BACKEND)
    return puglWorldIsX11(pData->world) ? kWindowSystemX11 : kWindowSystemWayland;
   #elif defined(DGL_USING_X11)
    return kWindowSystemX11;
   #elif defined(DGL_USING_WAYLAND)
    return kWindowSystemWayland;
   #else
    return kWindowSystemAuto;
   #endif
}

// --------------------------------------------------------------------------------------------------------------------

// A world on the windowing system the application asked for, see Application::WindowSystem
static PuglWorld* newWorld(const bool standalone, const Application::WindowSystem windowSystem)
{
    const PuglWorldType type = standalone ? PUGL_PROGRAM : PUGL_MODULE;
    const PuglWorldFlags flags = standalone ? PUGL_WORLD_THREADS : 0;

   #if defined(DGL_RUNTIME_BACKEND)
    if (windowSystem != Application::kWindowSystemAuto)
        return puglNewWorldOnSystem(type, flags, windowSystem == Application::kWindowSystemX11);
   #elif defined(DGL_USING_X11) || defined(DGL_USING_WAYLAND)
   # ifdef DGL_USING_X11
    const Application::WindowSystem missing = Application::kWindowSystemWayland;
   # else
    const Application::WindowSystem missing = Application::kWindowSystemX11;
   # endif
    if (windowSystem == missing)
    {
        d_stderr2("DGL: the application asked for %s, which this build does not have, see DGL_BACKEND",
                  missing == Application::kWindowSystemX11 ? "X11" : "Wayland");
        return nullptr;
    }
   #else
    // a single windowing system, nothing to choose
    (void)windowSystem;
   #endif

    return puglNewWorld(type, flags);
}

// --------------------------------------------------------------------------------------------------------------------

Application::PrivateData::PrivateData(const bool standalone, const Type type, const WindowSystem windowSystem)
    : world(newWorld(standalone, windowSystem)),
      isModern(false),
      isStandalone(standalone),
      isStarting(true),
      isQuitting(false),
      isQuittingInNextCycle(false),
      needsRepaint(false),
      visibleWindows(0),
      mainThreadHandle(getCurrentThreadHandle()),
      windows(),
      idleCallbacks()
{
    DAF_SAFE_ASSERT_RETURN(world != nullptr,);

   #ifdef __EMSCRIPTEN__
    puglSetWorldString(world, PUGL_CLASS_NAME, "canvas");
   #else
    puglSetWorldString(world, PUGL_CLASS_NAME, DAF_MACRO_AS_STRING(DGL_NAMESPACE));
   #endif
}

Application::PrivateData::~PrivateData()
{
    DAF_SAFE_ASSERT(isStarting || isQuitting);
    DAF_SAFE_ASSERT(visibleWindows == 0);

    windows.clear();
    idleCallbacks.clear();

    if (world != nullptr)
        puglFreeWorld(world);
}

// --------------------------------------------------------------------------------------------------------------------

void Application::PrivateData::oneWindowShown() noexcept
{
    if (++visibleWindows == 1)
    {
        isQuitting = false;
        isStarting = false;
    }
}

void Application::PrivateData::oneWindowClosed() noexcept
{
    DAF_SAFE_ASSERT_RETURN(visibleWindows != 0,);

    if (--visibleWindows == 0)
        isQuitting = true;
}

// --------------------------------------------------------------------------------------------------------------------

void Application::PrivateData::idle(const uint timeoutInMs)
{
    if (isQuittingInNextCycle)
    {
        quit();
        isQuittingInNextCycle = false;
    }

    if (world != nullptr)
    {
        const double timeoutInSeconds = timeoutInMs != 0
                                      ? static_cast<double>(timeoutInMs) / 1000.0
                                      : 0.0;

        puglUpdate(world, timeoutInSeconds);
    }

    triggerIdleCallbacks();
}

void Application::PrivateData::triggerIdleCallbacks()
{
    idleCallbacks.trigger();
}

void Application::PrivateData::repaintIfNeeeded()
{
    if (needsRepaint)
    {
        needsRepaint = false;

        for (WindowListIterator it = windows.begin(), ite = windows.end(); it != ite; ++it)
        {
            DGL_NAMESPACE::Window* const window(*it);
            window->repaint();
        }
    }
}

void Application::PrivateData::quit()
{
    if (! isThisTheMainThread(mainThreadHandle))
    {
        if (! isQuittingInNextCycle)
        {
            isQuittingInNextCycle = true;
            return;
        }
    }

    isQuitting = true;

   #ifndef DAF_TEST_APPLICATION_CPP
    for (WindowListReverseIterator rit = windows.rbegin(), rite = windows.rend(); rit != rite; ++rit)
    {
        DGL_NAMESPACE::Window* const window(*rit);
        window->close();
    }
   #endif
}

double Application::PrivateData::getTime() const
{
    return world != nullptr ? puglGetTime(world) : 0.0;
}

void Application::PrivateData::setClassName(const char* const name)
{
    DAF_SAFE_ASSERT_RETURN(name != nullptr && name[0] != '\0',);

    if (world != nullptr)
        puglSetWorldString(world, PUGL_CLASS_NAME, name);
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL
