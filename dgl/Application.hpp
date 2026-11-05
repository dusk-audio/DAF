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

#ifndef DGL_APP_HPP_INCLUDED
#define DGL_APP_HPP_INCLUDED

#include "Base.hpp"
#include "WaylandInput.hpp"

#include <cstddef>

#ifdef DAF_NAMESPACE
START_NAMESPACE_DAF
class PluginApplication;
END_NAMESPACE_DAF
#endif

START_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------
// build config sentinels

/**
   This set of static variables act as a build sentinel that detects a configuration error.

   Usually this means the way DGL was built and how it is being used and linked into your program is different,
   we want to avoid such combinations as memory layout would then also be different
   leading to all sort of subtle but very nasty memory corruption issues.

   Make sure the flags used to build DGL match the ones used by your program and the link errors should go away.
 */
#define BUILD_CONFIG_SENTINEL(NAME) \
   static struct DAF_JOIN_MACRO(_, NAME) { bool ok; DAF_JOIN_MACRO(_, NAME)() noexcept; } NAME;

#ifdef DAF_DEBUG
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_daf_debug_on)
#else
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_daf_debug_off)
#endif

#ifdef DGL_USE_FILE_BROWSER
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_use_file_browser_on)
#else
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_use_file_browser_off)
#endif

#ifdef DGL_USE_WEB_VIEW
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_use_web_view_on)
#else
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_use_web_view_off)
#endif

#ifdef DGL_NO_SHARED_RESOURCES
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_no_shared_resources_on)
#else
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_no_shared_resources_off)
#endif

#ifdef DGL_RUNTIME_BACKEND
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_runtime_backend_on)
#else
BUILD_CONFIG_SENTINEL(fail_to_link_is_mismatch_dgl_runtime_backend_off)
#endif

#undef BUILD_CONFIG_SENTINEL

// --------------------------------------------------------------------------------------------------------------------

/**
   Base DGL Application class.

   One application instance is required for creating a window.
   There's no single/global application instance in DGL, and multiple windows can share the same app instance.

   In standalone mode an application will automatically quit its event-loop when all its windows are closed.

   Unless stated otherwise, functions within this class are not thread-safe.
 */
class DAF_API Application
{
public:
   /**
      Type of application to setup, either "classic" or "modern".

      What this means depends on the OS.
    */
    enum Type {
        kTypeAuto,
        kTypeClassic,
        kTypeModern,
    };

   /**
      Windowing system to open the application's windows on.

      Only Linux and the other X11/Wayland systems have one to choose. A DGL_BACKEND=both build picks X11 or
      Wayland at runtime, by the rule in dgl/src/runtime-backend.md, unless the application asks for one.
      The request is final: if that system cannot be used, or the build does not have it, no window opens
      and a message says why, rather than the application ending up on the other one.
      It takes the place of the DAF_WINDOW_BACKEND environment variable.

      On macOS, Windows and the other platforms with a single windowing system the request is ignored.
    */
    enum WindowSystem {
        kWindowSystemAuto,
        kWindowSystemX11,
        kWindowSystemWayland,
    };

   /**
      Constructor for standalone or plugin application.
    */
    Application(bool isStandalone = true, Type type = kTypeAuto, WindowSystem windowSystem = kWindowSystemAuto);

   /**
      Constructor for a standalone application.
      This specific constructor is required if using web views in standalone applications.
      On Linux a web view needs X11, see WindowSystem.
    */
    Application(int argc, char* argv[], WindowSystem windowSystem = kWindowSystemAuto);

    struct ExternalWaylandDisplay {
        explicit ExternalWaylandDisplay(void* nativeDisplay, void* nativeInputSeat = nullptr)
            : display(nativeDisplay), inputSeat(nativeInputSeat) {}

        void* display; // wl_display*, owned by the host
        void* inputSeat; // optional wl_seat*, owned by the host
    };

   /**
      Construct a module application borrowing the host's Wayland connection.

      Create one Application per borrowed connection and share it among its Windows. The host must
      keep the display alive until those Windows and this Application are destroyed. Construction,
      idle and window operations run on the display owner's thread with no prepared read outstanding.
      The host reads the connection; idle dispatches only this application's pending events.
      Initialization and synchronous clipboard requests may read the socket while dispatching only
      this application's events.

      Supplying inputSeat selects host-managed input and prevents DAF from creating another pointer
      or keyboard stream. The seat must belong to this display and outlive this Application.
      Forward host input through getWaylandInput().

      A host using EGL must initialize it before creating DAF windows and keep it initialized until
      all those windows are destroyed. DAF preserves existing EGL resources and current bindings;
      if EGL is initially uninitialized, DAF owns its initialization until its last window is destroyed.

      A null display, an unavailable Wayland backend or initialization failure opens no connection;
      getWindowSystem() returns kWindowSystemAuto. There is no fallback to another backend.
    */
    explicit Application(ExternalWaylandDisplay display);

   /**
      Destructor.
    */
    virtual ~Application();

   /**
      Idle function.
      This runs the application event-loop once.
    */
    void idle();

   /**
      Run the application event-loop until all Windows are closed.
      idle() is called at regular intervals.
      @note This function is meant for standalones only, *never* call this from plugins.
    */
    void exec(uint idleTimeInMs = 30);

   /**
      Quit the application.
      This stops the event-loop and closes all Windows.
      This function is thread-safe.
    */
    void quit();

   /**
      Check if the application is about to quit.
      Returning true means there's no event-loop running at the moment (or it's just about to stop).
      This function is thread-safe.
    */
    bool isQuitting() const noexcept;

   /**
      Check if the application is standalone, otherwise running as a module or plugin.
      This function is thread-safe.
    */
    bool isStandalone() const noexcept;

   /**
      Return the time in seconds.

      This is a monotonically increasing clock with high resolution.@n
      The returned time is only useful to compare against other times returned by this function,
      its absolute value has no meaning.
   */
    double getTime() const;

   /**
      Return the application type, either kTypeClassic or kTypeModern.
      This function never return kTypeAuto.
   */
    Type getType() const noexcept;

   /**
      Return the windowing system the application's windows open on, kWindowSystemX11 or kWindowSystemWayland.
      Returns kWindowSystemAuto where the platform has no choice of one, and when none could be opened.
   */
    WindowSystem getWindowSystem() const noexcept;

   /**
      Return callbacks for the host-managed input seat supplied at construction.
      All fields are null for other applications and failed initialization.
      See WaylandInput for forwarding, reset and lifetime requirements.
   */
    WaylandInput getWaylandInput() const noexcept;

   /**
      Submit an activation token for a live toplevel surface on this application's Wayland connection.

      The host owns the mapped wl_surface and must call from the display-owner thread. Neither the
      surface nor the token is retained. This does not read the environment, obtain a token, create
      a window, or dispatch events. Consume the single-use token before calling, even on failure.
      True means the request was submitted or queued, not that the compositor granted focus.
      False means invalid arguments, an unavailable backend/protocol, or a broken connection.
   */
    bool activateWaylandSurface(void* surface, const char* token) noexcept;

    struct ClipboardCallbacks {
        void* context = nullptr;
        const void* (*get)(void*, size_t&) = nullptr;
        bool (*set)(void*, const char* mimeType, const void*, size_t) = nullptr;
    };

   /**
      Route every window's clipboard operations through the host. Set or clear both callbacks together;
      a partial pair is rejected without changing registration. Null/false callback results are final,
      with no native fallback. An empty pair restores native clipboard handling.

      The application copies the record but does not own context. Registration and callbacks run on
      the event thread; callbacks must not pump events or recursively call DGL clipboard methods.
      Returned storage stays valid until the next clipboard operation or removal. The setter must
      copy supplied bytes before returning. A null MIME type is passed as "text/plain".
    */
    bool setClipboardCallbacks(const ClipboardCallbacks& callbacks) noexcept;

   /**
      Add a callback function to be triggered on every idle cycle.
      You can add more than one, and remove them at anytime with removeIdleCallback().
      Idle callbacks trigger right after OS event handling and Window idle events (within the same cycle).
      There are no guarantees in terms of timing, use Window::addIdleCallback for time-relative callbacks.
      Registrations added during dispatch start on the next idle call. Removing a registration
      cancels its pending call immediately; nested idle calls do not re-enter an active registration.
    */
    void addIdleCallback(IdleCallback* callback);

   /**
      Remove an idle callback previously added via addIdleCallback().
    */
    void removeIdleCallback(IdleCallback* callback);

   /**
      Get the class name of the application.

      This is a stable identifier for the application, used as the window class/instance name on X11 and Windows.
      It is not displayed to the user, but can be used in scripts and by window managers,
      so it should be the same for every instance of the application, but different from other applications.

      Plugins created with DAF have their class name automatically set based on DGL_NAMESPACE and plugin name.
      Returns null if no windowing system could be opened.
    */
    const char* getClassName() const noexcept;

   /**
      Set the class name of the application.
      @see getClassName
    */
    void setClassName(const char* name);

private:
    struct PrivateData;
    PrivateData* const pData;
    friend class Window;
   #ifdef DAF_NAMESPACE
    friend class DAF_NAMESPACE::PluginApplication;
   #endif

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Application)
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL

#endif // DGL_APP_HPP_INCLUDED
