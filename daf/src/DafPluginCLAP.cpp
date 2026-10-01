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

/* TODO items:
 * CV: write a specification
 * INFO: define url, manual url, support url and string version
 * PARAMETERS: test parameter triggers
 * States: skip DSP/UI only states as appropriate
 * UI: expose external-only UIs
 */

#include "DafPluginInternal.hpp"
#include "DafPluginStateParser.hpp"
#include "DafPluginStateUpdates.hpp"
#include "extra/ScopedPointer.hpp"

#ifndef DAF_PLUGIN_CLAP_ID
# error DAF_PLUGIN_CLAP_ID undefined!
#endif

#if DAF_PLUGIN_HAS_UI && ! defined(HAVE_DGL)
# undef DAF_PLUGIN_HAS_UI
# define DAF_PLUGIN_HAS_UI 0
#endif

#if DAF_PLUGIN_HAS_UI
# include "DafUIInternal.hpp"
# include "../extra/Mutex.hpp"
#endif

#if DAF_PLUGIN_HAS_UI && DAF_PLUGIN_WANT_MIDI_INPUT
# include "../extra/RingBuffer.hpp"
#endif

#include <atomic>
#include <map>
#include <vector>

#include "clap/entry.h"
#include "clap/factory/plugin-factory.h"
#include "clap/ext/audio-ports.h"
#include "clap/ext/audio-ports-config.h"
#include "clap/ext/latency.h"
#include "clap/ext/tail.h"
#include "clap/ext/gui.h"
#include "clap/ext/note-ports.h"
#include "clap/ext/params.h"
#include "clap/ext/state.h"
#include "clap/ext/timer-support.h"

#if defined(DAF_PLUGIN_EXTRA_IO) && ! DAF_PLUGIN_IS_SYNTH \
 && DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
# define DAF_CLAP_HAS_AUDIO_PORTS_CONFIG 1
#else
# define DAF_CLAP_HAS_AUDIO_PORTS_CONFIG 0
#endif

#if defined(DAF_OS_MAC) || defined(DAF_OS_WINDOWS)
# define DAF_CLAP_USING_HOST_TIMER 0
#else
# define DAF_CLAP_USING_HOST_TIMER 1
#endif

#ifndef DAF_CLAP_TIMER_INTERVAL
# define DAF_CLAP_TIMER_INTERVAL 16 /* ~60 fps */
#endif

/* Whether this build can only ever hand the host a plugin-created (floating) window.
 *
 * DGL links against exactly one windowing backend, picked at build time: X11 whenever the X11
 * development files are present, the native Wayland backend only when they are not. See the policy
 * comment in dgl/src/pugl.hpp for why the choice works that way.
 *
 * CLAP_WINDOW_API_WAYLAND has no embedding contract at all -- clap/ext/gui.h says outright that
 * embed is not supported for it -- and DGL's Wayland backend matches: puglSetParent() is accepted
 * and ignored, every window is an xdg toplevel. So a Wayland-backed build offers floating only.
 *
 * A build that found *both* X11 and Wayland still advertises X11 alone. It is tempting to list
 * CLAP_WINDOW_API_WAYLAND as an extra floating-only API there, but an X11-built DGL has no
 * wl_surface to hand over: that would be advertising an API it cannot honour. Supporting both at
 * once needs runtime backend dispatch inside DGL, which is deliberately out of scope.
 *
 * HAVE_X11 and HAVE_WAYLAND reach this file through DGL_FLAGS, which Makefile.plugins.mk folds into
 * BASE_FLAGS (and which cmake/DAF-plugin.cmake exposes via dgl-system-libs-definitions), so the
 * wrapper sees exactly the backend decision DGL itself was compiled with.
 */
#if defined(HAVE_WAYLAND) && ! defined(HAVE_X11) && ! defined(DAF_OS_MAC) && ! defined(DAF_OS_WINDOWS)
# define DAF_CLAP_GUI_FLOATING_ONLY 1
#else
# define DAF_CLAP_GUI_FLOATING_ONLY 0
#endif

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

#if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
struct ClapChannelConfig {
    int32_t inputs;
    int32_t outputs;
};

static constexpr const ClapChannelConfig kClapChannelConfigs[] = {
    { DAF_PLUGIN_NUM_INPUTS, DAF_PLUGIN_NUM_OUTPUTS },
    DAF_PLUGIN_EXTRA_IO
};

static constexpr clap_id kClapChannelConfigIdBase = 0xDAF00000u;

static constexpr clap_id getClapChannelConfigId(const uint32_t sourceIndex) noexcept
{
    return kClapChannelConfigIdBase + sourceIndex;
}

static const char* getClapChannelType(const uint32_t channels) noexcept
{
    if (channels == 1)
        return CLAP_PORT_MONO;
    if (channels == 2)
        return CLAP_PORT_STEREO;
    return nullptr;
}
#endif

// --------------------------------------------------------------------------------------------------------------------

typedef std::map<const String, String> StringMap;

struct ClapEventQueue
{
  #if DAF_PLUGIN_HAS_UI
    enum EventType {
        kEventGestureBegin,
        kEventGestureEnd,
        kEventParamSet
    };

    struct Event {
        EventType type;
        uint32_t index;
        float value;
    };

    struct Queue {
        RecursiveMutex lock;
        uint allocated;
        uint used;
        // How far into the queue the plugin has already been updated. Only relevant while events are
        // held back for want of an output list, see PluginCLAP::sendUIEventsToHost.
        uint appliedFromUI;
        Event* events;

        Queue()
            : allocated(0),
              used(0),
              appliedFromUI(0),
              events(nullptr) {}

        ~Queue()
        {
            // std::free, not delete[]: the buffer comes from std::malloc/std::realloc below
            std::free(events);
        }

        void addEventFromUI(const Event& event)
        {
            const RecursiveMutexLocker crml(lock);

            // realloc(nullptr, n) is malloc(n), so the first allocation and every growth after it
            // share one path. Committing the new size before checking the result would leak the old
            // block and then write through a null pointer on the next event, so keep the existing
            // buffer and lose this one event instead. That loss is not symmetric: dropping a gesture
            // begin while its end still goes through leaves the host's automation lane in touch
            // state, and a dropped value leaves the host's cache stale until the next edit. Accepted
            // as the least bad outcome at the point where allocation is already failing.
            if (used + 1 > allocated)
            {
                const uint newAllocated = allocated != 0 ? allocated * 2 : 8;
                Event* const newEvents = static_cast<Event*>(
                    std::realloc(events, sizeof(Event) * newAllocated));

                if (newEvents == nullptr)
                    return;

                events = newEvents;
                allocated = newAllocated;
            }

            std::memcpy(&events[used++], &event, sizeof(Event));
        }

        void discardParameterValuesFromUI()
        {
            const RecursiveMutexLocker crml(lock);
            uint retained = 0;
            uint applied = 0;
            for (uint i = 0; i < used; ++i)
            {
                if (events[i].type == kEventParamSet)
                    continue;
                events[retained++] = events[i];
                if (i < appliedFromUI)
                    ++applied;
            }
            // A begin may already have reached the host: preserve its queued end.
            used = retained;
            appliedFromUI = applied;
        }
    } fEventQueue;

   #if DAF_PLUGIN_WANT_MIDI_INPUT
    SmallStackBuffer fNotesBuffer;
   #endif
  #endif

   #if DAF_PLUGIN_WANT_PROGRAMS
    uint32_t fCurrentProgram;
   #endif

  #if DAF_PLUGIN_WANT_STATE
    StringMap fStateMap;
   #if DAF_PLUGIN_HAS_UI
    virtual void setStateFromUI(const char* key, const char* value) = 0;
   #endif
  #endif

    struct CachedParameters {
        uint numParams;
        std::atomic<bool>* changed;
        std::atomic<float>* values;

        CachedParameters()
            : numParams(0),
              changed(nullptr),
              values(nullptr) {}

        ~CachedParameters()
        {
            delete[] changed;
            delete[] values;
        }

        void setup(const uint numParameters)
        {
            if (numParameters == 0)
                return;

            numParams = numParameters;
            changed = new std::atomic<bool>[numParameters];
            values = new std::atomic<float>[numParameters];
            for (uint i = 0; i < numParameters; ++i)
            {
                changed[i].store(false, std::memory_order_relaxed);
                values[i].store(0.0f, std::memory_order_relaxed);
            }
        }
    } fCachedParameters;

    ClapEventQueue()
       #if DAF_PLUGIN_HAS_UI && DAF_PLUGIN_WANT_MIDI_INPUT
        : fNotesBuffer(CPP_AGGREGATE_INIT(SmallStackBuffer){0, 0, 0, false, {0}})
       #endif
    {
       #if DAF_PLUGIN_WANT_PROGRAMS
        fCurrentProgram = 0;
       #endif
    }

    virtual ~ClapEventQueue() {}
};

// --------------------------------------------------------------------------------------------------------------------

#if DAF_PLUGIN_HAS_UI

#if ! DAF_PLUGIN_WANT_STATE
static constexpr const setStateFunc setStateCallback = nullptr;
#endif
#if ! DAF_PLUGIN_WANT_MIDI_INPUT
static constexpr const sendNoteFunc sendNoteCallback = nullptr;
#endif

/**
 * CLAP UI class.
 */
class ClapUI : public DGL_NAMESPACE::IdleCallback
{
public:
    ClapUI(PluginExporter& plugin,
           ClapEventQueue* const eventQueue,
           const clap_host_t* const host,
           const clap_host_params_t* const hostParams,
           const clap_host_gui_t* const hostGui,
          #if DAF_CLAP_USING_HOST_TIMER
           const clap_host_timer_support_t* const hostTimer,
          #endif
           const bool isFloating)
        : fPlugin(plugin),
         #if DAF_PLUGIN_WANT_STATE
          fPluginEventQueue(eventQueue),
         #endif
          fEventQueue(eventQueue->fEventQueue),
          fCachedParameters(eventQueue->fCachedParameters),
         #if DAF_PLUGIN_WANT_PROGRAMS
          fCurrentProgram(eventQueue->fCurrentProgram),
         #endif
         #if DAF_PLUGIN_WANT_STATE
          fStateMap(eventQueue->fStateMap),
         #endif
          fHost(host),
          fHostParams(hostParams),
          fHostGui(hostGui),
         #if DAF_CLAP_USING_HOST_TIMER
          fTimerId(0),
          fHostTimer(hostTimer),
         #else
          fCallbackRegistered(false),
         #endif
          fIsFloating(isFloating),
          fFloatingWindowShown(false),
          fScaleFactor(0.0),
          fParentWindow(0),
          fTransientWindow(0)
    {
       #if DAF_PLUGIN_WANT_MIDI_INPUT
        fNotesRingBuffer.setRingBuffer(&eventQueue->fNotesBuffer, false);
       #endif
    }

    ~ClapUI() override
    {
        stopIdleTimer();
    }

   #ifndef DAF_OS_MAC
    bool setScaleFactor(const double scaleFactor)
    {
        if (d_isEqual(fScaleFactor, scaleFactor))
            return true;

        fScaleFactor = scaleFactor;

        if (UIExporter* const ui = fUI.get())
            ui->notifyScaleFactorChanged(scaleFactor);

        return true;
    }
   #endif

    bool getSize(uint32_t* const width, uint32_t* const height) const
    {
        if (UIExporter* const ui = fUI.get())
        {
            *width = ui->getWidth();
            *height = ui->getHeight();
           #ifdef DAF_OS_MAC
            const double scaleFactor = ui->getScaleFactor();
            *width /= scaleFactor;
            *height /= scaleFactor;
           #endif
            fReportedWidth = *width;
            fReportedHeight = *height;
            return true;
        }

        double scaleFactor = fScaleFactor;
       #if defined(DAF_UI_DEFAULT_WIDTH) && defined(DAF_UI_DEFAULT_HEIGHT)
       #ifndef DAF_OS_MAC
        // A host that never called set_scale gets a window created at the desktop scale factor
        // (see UI::PrivateData::createNextWindow), so predict the size with that same factor.
        if (d_isZero(scaleFactor))
            scaleFactor = getDesktopScaleFactor(fParentWindow);
       #endif
        if (d_isZero(scaleFactor))
            scaleFactor = 1.0;
        *width = DAF_UI_DEFAULT_WIDTH * scaleFactor;
        *height = DAF_UI_DEFAULT_HEIGHT * scaleFactor;
       #else
        UIExporter tmpUI(nullptr, 0, fPlugin.getSampleRate(),
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, d_nextBundlePath,
                         fPlugin.getInstancePointer(), scaleFactor, DGL_NAMESPACE::Application::kTypeClassic);
        *width = tmpUI.getWidth();
        *height = tmpUI.getHeight();
        scaleFactor = tmpUI.getScaleFactor();
        tmpUI.quit();
       #endif

       #ifdef DAF_OS_MAC
        *width /= scaleFactor;
        *height /= scaleFactor;
       #endif

        fReportedWidth = *width;
        fReportedHeight = *height;
        return true;
    }

    // NOTE: can_resize, get_resize_hints, adjust_size, set_size and set_parent are all documented
    // "[main-thread & !floating]" in clap/ext/gui.h: a floating window belongs to the plugin, so the
    // host neither sizes it nor embeds it. Should a host call them anyway, the honest answer is that
    // the operation is unsupported. get_size stays valid in both modes and is left alone.
    bool canResize() const noexcept
    {
        if (fIsFloating)
            return false;

       #if DAF_UI_USER_RESIZABLE
        if (UIExporter* const ui = fUI.get())
            return ui->isResizable();
        return true;
       #else
        return false;
       #endif
    }

    bool getResizeHints(clap_gui_resize_hints_t* const hints) const
    {
        if (! fIsFloating && fUI != nullptr && fUI->isResizable())
        {
            uint minimumWidth, minimumHeight;
            bool keepAspectRatio;
            fUI->getGeometryConstraints(minimumWidth, minimumHeight, keepAspectRatio);

           #ifdef DAF_OS_MAC
            const double scaleFactor = fUI->getScaleFactor();
            minimumWidth /= scaleFactor;
            minimumHeight /= scaleFactor;
           #endif

            hints->can_resize_horizontally = true;
            hints->can_resize_vertically = true;
            hints->preserve_aspect_ratio = keepAspectRatio;
            hints->aspect_ratio_width = minimumWidth;
            hints->aspect_ratio_height = minimumHeight;

            return true;
        }

        hints->can_resize_horizontally = false;
        hints->can_resize_vertically = false;
        hints->preserve_aspect_ratio = false;
        hints->aspect_ratio_width = 0;
        hints->aspect_ratio_height = 0;

        return false;
    }

    bool adjustSize(uint32_t* const width, uint32_t* const height) const
    {
        if (! fIsFloating && fUI != nullptr && fUI->isResizable())
        {
            uint minimumWidth, minimumHeight;
            bool keepAspectRatio;
            fUI->getGeometryConstraints(minimumWidth, minimumHeight, keepAspectRatio);

           #ifdef DAF_OS_MAC
            const double scaleFactor = fUI->getScaleFactor();
            minimumWidth /= scaleFactor;
            minimumHeight /= scaleFactor;
           #endif

            if (keepAspectRatio)
            {
                if (*width < 1)
                    *width = 1;
                if (*height < 1)
                    *height = 1;

                const double ratio = static_cast<double>(minimumWidth) / static_cast<double>(minimumHeight);
                const double reqRatio = static_cast<double>(*width) / static_cast<double>(*height);

                if (d_isNotEqual(ratio, reqRatio))
                {
                    // fix width
                    if (reqRatio > ratio)
                        *width = d_roundToIntPositive(*height * ratio);
                    // fix height
                    else
                        *height = d_roundToIntPositive(static_cast<double>(*width) / ratio);
                }
            }

            if (minimumWidth > *width)
                *width = minimumWidth;
            if (minimumHeight > *height)
                *height = minimumHeight;

            return true;
        }

        return false;
    }

    bool setSizeFromHost(uint32_t width, uint32_t height)
    {
        if (fIsFloating)
            return false;

        if (UIExporter* const ui = fUI.get())
        {
           #ifdef DAF_OS_MAC
            const double scaleFactor = ui->getScaleFactor();
            width *= scaleFactor;
            height *= scaleFactor;
           #endif
            ui->setWindowSizeFromHost(width, height);
            return true;
        }

        return false;
    }

    bool setParent(const clap_window_t* const window)
    {
        if (fIsFloating)
            return false;

        fParentWindow = window->uptr;

        if (fUI == nullptr)
        {
            createUI();
            fHostGui->resize_hints_changed(fHost);

            // Should the window have come out at another size than get_size predicted, a parent on
            // a monitor with another scale factor being one way, ask the host to match it.
            if (fReportedWidth != 0 && fReportedHeight != 0)
            {
                const uint32_t reportedWidth = fReportedWidth;
                const uint32_t reportedHeight = fReportedHeight;
                uint32_t width, height;

                if (getSize(&width, &height) && (width != reportedWidth || height != reportedHeight))
                    fHostGui->request_resize(fHost, width, height);
            }
        }

        return true;
    }

    bool setTransient(const clap_window_t* const window)
    {
        if (! fIsFloating)
            return false;

        DAF_SAFE_ASSERT_RETURN(window != nullptr, false);

        // NOTE: on a Wayland build this is effectively a no-op that reports success.
        //
        // clap_window_t has no wayland member (see clap/ext/gui.h) and, more fundamentally, the only
        // thing a compositor accepts as an xdg_toplevel parent is another toplevel of the *same*
        // client -- a wl_surface pointer means nothing across a process boundary. Making the plugin
        // window stay above the host window would need the host to export its toplevel through
        // xdg-foreign and pass the resulting handle as a string, which CLAP has no way to express.
        //
        // puglSetTransientParent() already implements exactly this rule: a handle belonging to a view
        // of this process becomes a real xdg_toplevel parent, anything else is ignored with a debug
        // message. So passing the value straight through is both harmless and correct, and it keeps
        // the in-process case (host and plugin in one binary) working.
        fTransientWindow = window->uptr;

        if (UIExporter* const ui = fUI.get())
            ui->setWindowTransientWinId(window->uptr);

        return true;
    }

    void suggestTitle(const char* const title)
    {
        if (! fIsFloating)
            return;

        fWindowTitle = title;

        if (UIExporter* const ui = fUI.get())
            ui->setWindowTitle(title);
    }

    bool show()
    {
        if (fUI == nullptr)
        {
            createUI();

            // resize_hints_changed is documented "[thread-safe & !floating]"; a floating window is
            // sized by the plugin, so the host has no hints to re-read.
            if (! fIsFloating)
                fHostGui->resize_hints_changed(fHost);
        }

        if (fIsFloating)
        {
            fUI->setWindowVisible(true);
            fFloatingWindowShown = true;
        }

        startIdleTimer();
        return true;
    }

    bool hide()
    {
        if (UIExporter* const ui = fUI.get())
        {
            fFloatingWindowShown = false;
            ui->setWindowVisible(false);
            stopIdleTimer();
        }

        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------

    void idleCallback() override
    {
        UIExporter* const ui = fUI.get();

        if (ui == nullptr)
            return;

       #if DAF_CLAP_USING_HOST_TIMER
        ui->plugin_idle();
       #else
        ui->idleFromNativeIdle();
       #endif

        // Constraints can also move without a resize following, so catch those
        // here rather than only on the resize path.
        notifyResizeHintsIfChanged();

        for (uint i=0; i<fCachedParameters.numParams; ++i)
        {
            if (fCachedParameters.changed[i].exchange(false, std::memory_order_acquire))
            {
                ui->parameterChanged(i, fCachedParameters.values[i].load(std::memory_order_relaxed));
            }
        }

        // A floating window belongs to the plugin, so the host never sees the user closing it.
        // Without this the host's "editor is open" state stays stuck on forever.
        if (fFloatingWindowShown && ! ui->isVisible())
        {
            fFloatingWindowShown = false;
            stopIdleTimer();

            // MUST be the last thing this function does. The host is allowed to call hide() or even
            // destroy() from inside closed(), and destroy() deletes this very object, so no member
            // may be touched afterwards. was_destroyed is false: the UI resources are still alive
            // and a later show() reuses them.
            fHostGui->closed(fHost, false);
            return;
        }
    }

    // ----------------------------------------------------------------------------------------------------------------

    void setParameterValueFromPlugin(const uint index, const float value)
    {
        if (UIExporter* const ui = fUI.get())
            ui->parameterChanged(index, value);
    }

   #if DAF_PLUGIN_WANT_PROGRAMS
    void setProgramFromPlugin(const uint index)
    {
        if (UIExporter* const ui = fUI.get())
            ui->programLoaded(index);
    }
   #endif

   #if DAF_PLUGIN_WANT_STATE
    void setStateFromPlugin(const char* const key, const char* const value)
    {
        if (UIExporter* const ui = fUI.get())
            ui->stateChanged(key, value);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------

private:
    // Plugin and UI
    PluginExporter& fPlugin;
   #if DAF_PLUGIN_WANT_STATE
    ClapEventQueue* const fPluginEventQueue;
   #endif
    ClapEventQueue::Queue& fEventQueue;
    ClapEventQueue::CachedParameters& fCachedParameters;
   #if DAF_PLUGIN_WANT_PROGRAMS
    uint32_t& fCurrentProgram;
   #endif
   #if DAF_PLUGIN_WANT_STATE
    StringMap& fStateMap;
   #endif
    const clap_host_t* const fHost;
    const clap_host_params_t* const fHostParams;
    const clap_host_gui_t* const fHostGui;

    // Last geometry constraints the host was told about. A CLAP host caches
    // get_resize_hints and keeps enforcing the cached aspect afterwards, so it
    // has to be told when they move or it silently applies the old ratio to
    // everything, including a size this plugin asks for.
    uint fNotifiedMinWidth = 0;
    uint fNotifiedMinHeight = 0;
    bool fNotifiedKeepAspect = false;
    bool fNotifiedAny = false;

    // Last size get_size answered with, 0 if never asked. Before the UI exists that answer is a
    // prediction, which setParent checks against the window it then creates.
    mutable uint32_t fReportedWidth = 0;
    mutable uint32_t fReportedHeight = 0;
   #if DAF_CLAP_USING_HOST_TIMER
    clap_id fTimerId;
    const clap_host_timer_support_t* const fHostTimer;
   #else
    bool fCallbackRegistered;
   #endif
   #if DAF_PLUGIN_WANT_MIDI_INPUT
    RingBufferControl<SmallStackBuffer> fNotesRingBuffer;
   #endif
    ScopedPointer<UIExporter> fUI;

    const bool fIsFloating;
    // true between a successful floating show() and the window going away again, whether that is a
    // host-side hide() or the user clicking the window's close button. Only ever set when floating.
    bool fFloatingWindowShown;

    // Temporary data
    double fScaleFactor;
    uintptr_t fParentWindow;
    uintptr_t fTransientWindow;
    String fWindowTitle;

    // ----------------------------------------------------------------------------------------------------------------

    // Idling is identical for embedded and floating windows: on Unix (X11 and Wayland alike) the
    // host timer extension drives it, everywhere else the native window idle callback does. A
    // floating window is still pumped through UIExporter::plugin_idle(), which runs the pugl event
    // loop, so the plugin-owned toplevel gets its events either way.
    void startIdleTimer()
    {
        DAF_SAFE_ASSERT_RETURN(fUI != nullptr,);

       #if DAF_CLAP_USING_HOST_TIMER
        if (fTimerId == 0)
            fHostTimer->register_timer(fHost, DAF_CLAP_TIMER_INTERVAL, &fTimerId);
       #else
        if (! fCallbackRegistered)
        {
            fCallbackRegistered = true;
            fUI->addIdleCallbackForNativeIdle(this, DAF_CLAP_TIMER_INTERVAL);
        }
       #endif
    }

    void stopIdleTimer()
    {
       #if DAF_CLAP_USING_HOST_TIMER
        if (fTimerId != 0)
        {
            fHostTimer->unregister_timer(fHost, fTimerId);
            fTimerId = 0;
        }
       #else
        if (fCallbackRegistered && fUI != nullptr)
        {
            fUI->removeIdleCallbackForNativeIdle(this);
            fCallbackRegistered = false;
        }
       #endif
    }

    void createUI()
    {
        DAF_SAFE_ASSERT_RETURN(fUI == nullptr,);

        fUI = new UIExporter(this,
                             fParentWindow,
                             fPlugin.getSampleRate(),
                             editParameterCallback,
                             setParameterCallback,
                             setStateCallback,
                             sendNoteCallback,
                             setSizeCallback,
                             nullptr, // TODO fileRequestCallback,
                             d_nextBundlePath,
                             fPlugin.getInstancePointer(),
                             fScaleFactor,
                             DGL_NAMESPACE::Application::kTypeClassic);

       #if DAF_PLUGIN_WANT_PROGRAMS
        fUI->programLoaded(fCurrentProgram);
       #endif

       #if DAF_PLUGIN_WANT_FULL_STATE
        // Update current state from plugin side
        for (StringMap::const_iterator cit=fStateMap.begin(), cite=fStateMap.end(); cit != cite; ++cit)
        {
            const String& key = cit->first;
            fStateMap[key] = fPlugin.getStateValue(key);
        }
       #endif

       #if DAF_PLUGIN_WANT_STATE
        // Set state
        for (StringMap::const_iterator cit=fStateMap.begin(), cite=fStateMap.end(); cit != cite; ++cit)
        {
            const String& key(cit->first);
            const String& value(cit->second);

            // TODO skip DSP only states

            fUI->stateChanged(key, value);
        }
       #endif

        for (uint32_t i=0; i<fCachedParameters.numParams; ++i)
        {
            const float value = fCachedParameters.values[i] = fPlugin.getParameterValue(i);
            fCachedParameters.changed[i] = false;
            fUI->parameterChanged(i, value);
        }

        if (fIsFloating)
        {
            if (fWindowTitle.isNotEmpty())
                fUI->setWindowTitle(fWindowTitle);

            if (fTransientWindow != 0)
                fUI->setWindowTransientWinId(fTransientWindow);
        }
    }

    // ----------------------------------------------------------------------------------------------------------------
    // DAF callbacks

    // The one way to queue an edit for the host. Queued events leave the plugin only from process()
    // or clap_plugin_params::flush, and the host owes it neither while it is not processing this
    // plugin (a bypassed or idle track), so every enqueue is paired with a request. Skipping that
    // for even one event means the edit is never applied nor reported, and for a bypass-designated
    // parameter it means the plugin can never switch itself back on.
    //
    // Asking per event is deliberate. The obvious optimisations are all unsound:
    //  - only on the queue's empty-to-non-empty transition: the drain bails when its try-lock finds
    //    this thread mid-enqueue, and with the queue non-empty no later edit asks again, so the
    //    events sit there. Per event, the enqueuing thread always asks after releasing the lock, so
    //    a bailed drain is always followed by a fresh request.
    //  - re-asking from idleCallback while events are pending: that timer stops at gui destroy, so
    //    an edit queued in the bail window just before the editor closes is stranded.
    //  - gating on fPlugin.isActive(): skips exactly the active-but-not-processing case this exists
    //    for. A flag driven by start/stop_processing strands events too, since stop_processing is
    //    [audio-thread] while request_flush is [!audio-thread], so the flag holder cannot ask on the
    //    way down.
    // Hosts coalesce these into a flag and a wakeup, which is the cheap half of the trade.
    void queueUIEvent(const ClapEventQueue::Event& ev) const
    {
        fEventQueue.addEventFromUI(ev);
        requestHostParamFlush();
    }

    // request_flush is [thread-safe,!audio-thread]; the only caller runs on the UI thread.
    void requestHostParamFlush() const
    {
        // Checking the member as well as the struct is defence against nonconforming hosts, not a
        // documented option: this runs on every UI edit and hosts do ship extension structs with
        // null entries.
        if (fHostParams != nullptr && fHostParams->request_flush != nullptr)
        {
            fHostParams->request_flush(fHost);
        }
        else if (fHost->request_process != nullptr)
        {
            // Scenario III in clap/ext/params.h names request_process as the other lever, and it
            // lives in clap_host_t itself, so a host without a usable params extension is still
            // reachable.
            fHost->request_process(fHost);
        }
    }

    void editParameter(const uint32_t rindex, const bool started) const
    {
        const ClapEventQueue::Event ev = {
            started ? ClapEventQueue::kEventGestureBegin : ClapEventQueue::kEventGestureEnd,
            rindex, 0.f
        };
        queueUIEvent(ev);
    }

    static void editParameterCallback(void* const ptr, const uint32_t rindex, const bool started)
    {
        static_cast<ClapUI*>(ptr)->editParameter(rindex, started);
    }

    void setParameterValue(const uint32_t rindex, const float value)
    {
        const ClapEventQueue::Event ev = {
            ClapEventQueue::kEventParamSet,
            rindex, value
        };
        queueUIEvent(ev);
    }

    static void setParameterCallback(void* const ptr, const uint32_t rindex, const float value)
    {
        static_cast<ClapUI*>(ptr)->setParameterValue(rindex, value);
    }

    // Ask the host to re-read get_resize_hints when this UI's constraints have
    // moved since it was last told. Three comparisons per call, and the host call
    // only happens on a real change, so it is cheap enough for the idle tick.
    void notifyResizeHintsIfChanged()
    {
        if (fIsFloating || fUI == nullptr)
            return;

        uint minimumWidth = 0, minimumHeight = 0;
        bool keepAspectRatio = false;
        fUI->getGeometryConstraints(minimumWidth, minimumHeight, keepAspectRatio);

        if (fNotifiedAny
            && minimumWidth == fNotifiedMinWidth
            && minimumHeight == fNotifiedMinHeight
            && keepAspectRatio == fNotifiedKeepAspect)
            return;

        fNotifiedMinWidth = minimumWidth;
        fNotifiedMinHeight = minimumHeight;
        fNotifiedKeepAspect = keepAspectRatio;
        fNotifiedAny = true;

        // Documented "[thread-safe & !floating]".
        fHostGui->resize_hints_changed(fHost);
    }

    void setSizeFromPlugin(const uint width, const uint height)
    {
        DAF_SAFE_ASSERT_RETURN(fUI != nullptr,);

        // request_resize is documented "[thread-safe & !floating]". A floating window is the
        // plugin's own, so there is nobody to ask -- just resize it.
        if (fIsFloating)
        {
            fUI->setWindowSizeFromHost(width, height);
            return;
        }

       #ifdef DAF_OS_MAC
        const double scaleFactor = fUI->getScaleFactor();
        const uint hostWidth = width / scaleFactor;
        const uint hostHeight = height / scaleFactor;
       #else
        const uint hostWidth = width;
        const uint hostHeight = height;
       #endif

        // BEFORE asking, not after: the host resolves the request against the hints
        // it currently holds. A plugin that changes its aspect (an editor with a
        // collapsible section, say) and then asks for the matching size gets a
        // window built to the OLD ratio otherwise -- Bitwig answered a 960x516
        // request with a 902x601 window, 1.5008 being the ratio from before the
        // section was collapsed, leaving the view clipped on one side and short on
        // the other.
        notifyResizeHintsIfChanged();

        if (fHostGui->request_resize(fHost, hostWidth, hostHeight))
            fUI->setWindowSizeFromHost(width, height);
    }

    static void setSizeCallback(void* const ptr, const uint width, const uint height)
    {
        static_cast<ClapUI*>(ptr)->setSizeFromPlugin(width, height);
    }

   #if DAF_PLUGIN_WANT_STATE
    void setState(const char* const key, const char* const value)
    {
        fPluginEventQueue->setStateFromUI(key, value);
    }

    static void setStateCallback(void* const ptr, const char* key, const char* value)
    {
        static_cast<ClapUI*>(ptr)->setState(key, value);
    }
   #endif

   #if DAF_PLUGIN_WANT_MIDI_INPUT
    void sendNote(const uint8_t channel, const uint8_t note, const uint8_t velocity)
    {
        uint8_t midiData[3];
        midiData[0] = (velocity != 0 ? 0x90 : 0x80) | channel;
        midiData[1] = note;
        midiData[2] = velocity;
        fNotesRingBuffer.writeCustomData(midiData, 3);
        fNotesRingBuffer.commitWrite();

        // Notes are read only in process(), and unlike parameters they cannot travel through
        // clap_plugin_params::flush, so ask the host to run one. Without this the UI keyboard is
        // silent on a track the host is not processing and the queued note-ons all arrive at once
        // when it resumes, long after their note-offs made sense.
        if (fHost->request_process != nullptr)
            fHost->request_process(fHost);
    }

    static void sendNoteCallback(void* const ptr, const uint8_t channel, const uint8_t note, const uint8_t velocity)
    {
        static_cast<ClapUI*>(ptr)->sendNote(channel, note, velocity);
    }
   #endif

    /* TODO
    bool fileRequest(const char*)
    {
        return true;
    }

    static bool fileRequestCallback(void* const ptr, const char* const key)
    {
        return static_cast<ClapUI*>(ptr)->fileRequest(key);
    }
    */
};

// --------------------------------------------------------------------------------------------------------------------

#endif // DAF_PLUGIN_HAS_UI

#if ! DAF_PLUGIN_WANT_MIDI_OUTPUT
static constexpr const writeMidiFunc writeMidiCallback = nullptr;
#endif
#if ! DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
static constexpr const requestParameterValueChangeFunc requestParameterValueChangeCallback = nullptr;
#endif
#if ! DAF_PLUGIN_WANT_STATE
static constexpr const updateStateValueFunc updateStateValueCallback = nullptr;
#endif

// --------------------------------------------------------------------------------------------------------------------

/**
 * CLAP plugin class.
 */
class PluginCLAP : ClapEventQueue
{
public:
    PluginCLAP(const clap_host_t* const host)
        : fPlugin(this,
                  writeMidiCallback,
                  requestParameterValueChangeCallback,
                  updateStateValueCallback),
          fHost(host),
          fOutputEvents(nullptr),
          fResetParameterIndex(UINT32_MAX),
         #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
          fSelectedInputChannels(DAF_PLUGIN_NUM_INPUTS),
          fSelectedOutputChannels(DAF_PLUGIN_NUM_OUTPUTS),
          fSupportsAudioPortsConfig(false),
         #endif
         #if DAF_PLUGIN_NUM_INPUTS+DAF_PLUGIN_NUM_OUTPUTS != 0
          fUsingCV(false),
         #endif
         #if DAF_PLUGIN_WANT_LATENCY
          fLatencyChanged(false),
          // seed both from the plugin so a nonzero construction-time latency is not
          // mistaken for a change on the very first activate()
          fLastKnownLatency(fPlugin.getLatency()),
          fLatencyRestartRequests(0),
          fReportedLatency(fPlugin.getLatency()),
         #endif
         #if DAF_PLUGIN_WANT_TAIL
          fLastKnownTail(fPlugin.getTail()),
         #endif
         #if DAF_PLUGIN_WANT_MIDI_INPUT
          fMidiEventCount(0),
         #endif
          fHostExtensions(host)
    {
        if (const uint32_t paramCount = fPlugin.getParameterCount())
        {
            fCachedParameters.setup(paramCount);

            for (uint32_t i=0; i<paramCount; ++i)
            {
                if (fPlugin.getParameterDesignation(i) == kParameterDesignationReset)
                {
                    fResetParameterIndex = i;
                    break;
                }
            }
        }

       #if DAF_PLUGIN_HAS_UI && DAF_PLUGIN_WANT_MIDI_INPUT
        fNotesRingBuffer.setRingBuffer(&fNotesBuffer, true);
       #endif

       #if DAF_PLUGIN_WANT_STATE
        for (uint32_t i=0, count=fPlugin.getStateCount(); i<count; ++i)
        {
            const String& dkey(fPlugin.getStateKey(i));
            fStateMap[dkey] = fPlugin.getStateDefaultValue(i);
        }
       #endif

       #if DAF_PLUGIN_NUM_INPUTS != 0
        fillInBusInfoDetails<true>();
       #endif
       #if DAF_PLUGIN_NUM_OUTPUTS != 0
        fillInBusInfoDetails<false>();
       #endif
       #if DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
        fillInBusInfoPairs();
       #endif
       #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
        fSupportsAudioPortsConfig = canUseAudioPortsConfig();
       #endif
    }

    // ----------------------------------------------------------------------------------------------------------------
    // core

    template <class T>
    const T* getHostExtension(const char* const extensionId) const
    {
        return static_cast<const T*>(fHost->get_extension(fHost, extensionId));
    }

    bool init()
    {
        if (!clap_version_is_compatible(fHost->clap_version))
            return false;

        return fHostExtensions.init();
    }

    void activate(const double sampleRate, const uint32_t maxFramesCount)
    {
        fPlugin.setSampleRate(sampleRate, true);
        fPlugin.setBufferSize(maxFramesCount, true);
        fPlugin.activate();
       #if DAF_PLUGIN_WANT_LATENCY
        // The new sample rate and buffer size can change the latency, and activation
        // is the only place where clap_host_latency::changed may be called.
        //
        // NOTE: canRequestRestart is false even though the plugin is active by now:
        // asking the host to restart us from inside activate() would tear down the
        // activation currently in progress -- exactly the activate/request_restart
        // loop clap-validator detects.
        checkForLatencyChanges(false);
        // publishes the value the host is about to read back, then tells it to look
        reportLatencyChangeIfNeeded();
       #endif
    }

    void deactivate()
    {
        fPlugin.deactivate();
       #if DAF_PLUGIN_WANT_LATENCY
        // NOTE: not reported here, it is announced on the next activate(). No restart
        // request either, the host is already tearing the activation down.
        checkForLatencyChanges(false);
       #endif
    }

    void reset()
    {
        if (fResetParameterIndex != UINT32_MAX)
        {
           #if DAF_PLUGIN_WANT_MIDI_INPUT
            fMidiEventCount = 0;
           #endif
            fPlugin.setParameterValue(fResetParameterIndex, 1.f);
            fPlugin.setParameterValue(fResetParameterIndex, 0.f);
        }
        else
        {
            fHost->request_restart(fHost);
        }
    }

   #if DAF_PLUGIN_HAS_UI
    // Reached from process() and from clap_plugin_params::flush. A host that is not processing this
    // plugin still owes it one of the two after a request_flush (see ClapUI::requestHostParamFlush),
    // which is why both entry points drain: an edit made in that state would otherwise never be
    // applied nor reported. Non-blocking: the UI keeps its own copy of the value, so a queue busy
    // with the UI thread simply drains on the next call. Guards its own arguments, as
    // flushParameters() below does with its event lists.
    //
    // A refused try_push is ignored on purpose. The report is best effort, the value is not: a host
    // whose output queue is full ends up with a stale cache entry it corrects on its next rescan,
    // whereas skipping the apply would leave the DSP on the old value with nothing to trigger a
    // retry. Retaining refused events instead was tried and is worse: a host that always rejects an
    // event type blocks the queue head forever, and a retained old value can re-push over newer host
    // automation.
    //
    // Known and deliberately left alone, so they are not rediscovered as regressions:
    //  - a host value arriving in the same call is applied after the queued UI value has already
    //    been pushed out, so the two can disagree until the next edit. Pre-existing in process(),
    //    and a protocol-level ordering question rather than something this layer can settle.
    //  - reached from the main thread while the plugin is deactivated, and a host that services
    //    request_flush inline can re-enter from the UI callback stack. Legal: DAF's
    //    setParameterValue is documented as callable from any context.
    //  - stateLoad does not cancel queued edits, so an edit made before a state load can apply
    //    after it. Pre-existing, though reachable in more situations now.
    void sendUIEventsToHost(const clap_output_events_t* const outputEvents)
    {
        const RecursiveMutexTryLocker crmtl(fEventQueue.lock);

        if (! crmtl.wasLocked())
            return;

        // reuse the same struct for gesture and parameters, they are compatible up to where it matters
        // note_id/port_index/channel/key are -1: the edit is a global parameter change, and 0 would
        // name voice 0, port 0, channel 0, key 0 (see the wildcard rules in clap/events.h).
        clap_event_param_value_t clapEvent = {
            { 0, 0, 0, 0, CLAP_EVENT_IS_LIVE },
            0, nullptr, -1, -1, -1, -1, 0.0
        };

        for (uint32_t i=0; i<fEventQueue.used; ++i)
        {
            const Event& event(fEventQueue.events[i]);

            switch (event.type)
            {
            case kEventGestureBegin:
                clapEvent.header.size = sizeof(clap_event_param_gesture_t);
                clapEvent.header.type = CLAP_EVENT_PARAM_GESTURE_BEGIN;
                clapEvent.param_id = event.index;
                break;
            case kEventGestureEnd:
                clapEvent.header.size = sizeof(clap_event_param_gesture_t);
                clapEvent.header.type = CLAP_EVENT_PARAM_GESTURE_END;
                clapEvent.param_id = event.index;
                break;
            case kEventParamSet:
                clapEvent.header.size = sizeof(clap_event_param_value_t);
                clapEvent.header.type = CLAP_EVENT_PARAM_VALUE;
                clapEvent.param_id = event.index;
                clapEvent.value = event.value;
                // Applied even when there is nowhere to report it: a host servicing a flush with no
                // output queue must still not lose the edit, or the parameter never moves at all.
                // Applied once only, though: a held-back event is re-reported later but must not be
                // re-set, because a trigger parameter fires on every set it receives.
                if (i >= fEventQueue.appliedFromUI)
                    fPlugin.setParameterValue(event.index, event.value);
                break;
            default:
                continue;
            }

            if (outputEvents != nullptr)
                outputEvents->try_push(outputEvents, &clapEvent.header);
        }

        // Held back when there was nowhere to report to, so the reports still reach the host on the
        // next drain that has a real output list. Dropping them there would strand a gesture begin
        // without its end and leave the host's automation lane stuck in touch state. The watermark
        // keeps the values from being applied a second time when that happens; the remaining cost is
        // that a held value is reported after any host automation that arrived in between, so it can
        // overwrite it host-side. Both are reachable only through a host that passes no output list
        // while asking for a flush, and losing gesture pairs outright is the worse of the two.
        if (outputEvents != nullptr)
        {
            fEventQueue.used = 0;
            fEventQueue.appliedFromUI = 0;
        }
        else
        {
            fEventQueue.appliedFromUI = fEventQueue.used;
        }
    }
   #endif

    bool process(const clap_process_t* const process)
    {
       #if DAF_PLUGIN_WANT_MIDI_INPUT
        fMidiEventCount = 0;
       #endif

       #if DAF_PLUGIN_HAS_UI
        sendUIEventsToHost(process->out_events);
       #endif

       #if DAF_PLUGIN_WANT_TIMEPOS
        if (const clap_event_transport_t* const transport = process->transport)
        {
            fTimePosition.playing = (transport->flags & CLAP_TRANSPORT_IS_PLAYING) != 0 &&
                                    (transport->flags & CLAP_TRANSPORT_IS_WITHIN_PRE_ROLL) == 0;

            fTimePosition.bpmValid = (transport->flags & CLAP_TRANSPORT_HAS_TEMPO) != 0 && transport->tempo > 0.0;

            // frame is the transport position, not steady_time (which never stops or jumps).
            // prefer the seconds timeline; failing that, derive it from beats at the current tempo.
            {
                double transportSeconds = 0.0;

                if ((transport->flags & CLAP_TRANSPORT_HAS_SECONDS_TIMELINE) != 0)
                    transportSeconds = static_cast<double>(transport->song_pos_seconds) / CLAP_SECTIME_FACTOR;
                else if ((transport->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE) != 0 && fTimePosition.bpmValid)
                    transportSeconds = static_cast<double>(transport->song_pos_beats) / CLAP_BEATTIME_FACTOR
                                     * 60.0 / transport->tempo;

                fTimePosition.frame = transportSeconds > 0.0
                                    ? static_cast<uint64_t>(transportSeconds * fPlugin.getSampleRate() + 0.5)
                                    : 0;
            }
            if (fTimePosition.bpmValid)
                fTimePosition.bbt.beatsPerMinute = transport->tempo;
            else
                fTimePosition.bbt.beatsPerMinute = 120.0;

            // ticksPerBeat is not possible with CLAP
            fTimePosition.bbt.ticksPerBeat = 1920.0;

            if ((transport->flags & (CLAP_TRANSPORT_HAS_BEATS_TIMELINE|CLAP_TRANSPORT_HAS_TIME_SIGNATURE)) == (CLAP_TRANSPORT_HAS_BEATS_TIMELINE|CLAP_TRANSPORT_HAS_TIME_SIGNATURE))
            {
                if (transport->song_pos_beats >= 0)
                {
                    const int64_t clapPos   = std::abs(transport->song_pos_beats);
                    const int64_t clapBeats = clapPos >> 31;
                    const double  clapRest  = static_cast<double>(clapPos & 0x7fffffff) / CLAP_BEATTIME_FACTOR;

                    fTimePosition.bbt.bar  = static_cast<int32_t>(clapBeats) / transport->tsig_num + 1;
                    fTimePosition.bbt.beat = static_cast<int32_t>(clapBeats % transport->tsig_num) + 1;
                    fTimePosition.bbt.tick = clapRest * fTimePosition.bbt.ticksPerBeat;
                }
                else
                {
                    fTimePosition.bbt.bar  = 1;
                    fTimePosition.bbt.beat = 1;
                    fTimePosition.bbt.tick = 0.0;
                }

                fTimePosition.bbt.valid       = true;
                fTimePosition.bbt.beatsPerBar = transport->tsig_num;
                fTimePosition.bbt.beatType    = transport->tsig_denom;
            }
            else
            {
                fTimePosition.bbt.valid       = false;
                fTimePosition.bbt.bar         = 1;
                fTimePosition.bbt.beat        = 1;
                fTimePosition.bbt.tick        = 0.0;
                fTimePosition.bbt.beatsPerBar = 4.0f;
                fTimePosition.bbt.beatType    = 4.0f;
            }

            fTimePosition.bbt.barStartTick = fTimePosition.bbt.ticksPerBeat*
                                             fTimePosition.bbt.beatsPerBar*
                                             (fTimePosition.bbt.bar-1);
        }
        else
        {
            fTimePosition.playing = false;
            fTimePosition.frame = 0;
            fTimePosition.bbt.valid          = false;
            fTimePosition.bpmValid           = false;
            fTimePosition.bbt.beatsPerMinute = 120.0;
            fTimePosition.bbt.bar            = 1;
            fTimePosition.bbt.beat           = 1;
            fTimePosition.bbt.tick           = 0.0;
            fTimePosition.bbt.beatsPerBar    = 4.f;
            fTimePosition.bbt.beatType       = 4.f;
            fTimePosition.bbt.barStartTick   = 0;
        }

        fPlugin.setTimePosition(fTimePosition);
       #endif

        if (const clap_input_events_t* const inputEvents = process->in_events)
        {
            if (const uint32_t len = inputEvents->size(inputEvents))
            {
                for (uint32_t i=0; i<len; ++i)
                {
                    const clap_event_header_t* const event = inputEvents->get(inputEvents, i);

                    switch (event->type)
                    {
                    case CLAP_EVENT_NOTE_ON:
                       #if DAF_PLUGIN_WANT_MIDI_INPUT
                        // BUG: even though we only report CLAP_NOTE_DIALECT_MIDI as supported, Bitwig sends us this anyway
                        addNoteEvent(reinterpret_cast<const clap_event_note_t*>(event), true);
                       #endif
                        break;
                    case CLAP_EVENT_NOTE_OFF:
                       #if DAF_PLUGIN_WANT_MIDI_INPUT
                        // BUG: even though we only report CLAP_NOTE_DIALECT_MIDI as supported, Bitwig sends us this anyway
                        addNoteEvent(reinterpret_cast<const clap_event_note_t*>(event), false);
                       #endif
                        break;
                    case CLAP_EVENT_NOTE_CHOKE:
                    case CLAP_EVENT_NOTE_END:
                    case CLAP_EVENT_NOTE_EXPRESSION:
                        break;
                    case CLAP_EVENT_PARAM_VALUE:
                        DAF_SAFE_ASSERT_UINT2_BREAK(event->size == sizeof(clap_event_param_value_t),
                                                        event->size, sizeof(clap_event_param_value_t));
                        if (event->space_id == CLAP_CORE_EVENT_SPACE_ID)
                            setParameterValueFromEvent(reinterpret_cast<const clap_event_param_value_t*>(event));
                        break;
                    case CLAP_EVENT_PARAM_MOD:
                    case CLAP_EVENT_PARAM_GESTURE_BEGIN:
                    case CLAP_EVENT_PARAM_GESTURE_END:
                    case CLAP_EVENT_TRANSPORT:
                        break;
                    case CLAP_EVENT_MIDI:
                        DAF_SAFE_ASSERT_UINT2_BREAK(event->size == sizeof(clap_event_midi_t),
                                                        event->size, sizeof(clap_event_midi_t));
                       #if DAF_PLUGIN_WANT_MIDI_INPUT
                        addMidiEvent(reinterpret_cast<const clap_event_midi_t*>(event));
                       #endif
                        break;
                    case CLAP_EVENT_MIDI_SYSEX:
                    case CLAP_EVENT_MIDI2:
                        break;
                    }
                }
            }
        }

       #if DAF_PLUGIN_HAS_UI && DAF_PLUGIN_WANT_MIDI_INPUT
        if (fMidiEventCount != kMaxMidiEvents && fNotesRingBuffer.isDataAvailableForReading())
        {
            uint8_t midiData[3];
            const uint32_t frame = fMidiEventCount != 0 ? fMidiEvents[fMidiEventCount-1].frame : 0;

            while (fNotesRingBuffer.isDataAvailableForReading())
            {
                if (! fNotesRingBuffer.readCustomData(midiData, 3))
                    break;

                MidiEvent& midiEvent(fMidiEvents[fMidiEventCount++]);
                midiEvent.frame = frame;
                midiEvent.size  = 3;
                std::memcpy(midiEvent.data, midiData, 3);

                if (fMidiEventCount == kMaxMidiEvents)
                    break;
            }
        }
       #endif

        if (const uint32_t frames = process->frames_count)
        {
           #if DAF_PLUGIN_NUM_INPUTS != 0
            const float** const audioInputs = fAudioInputs;

            uint32_t in=0;
            for (uint32_t i=0; i<process->audio_inputs_count; ++i)
            {
                const clap_audio_buffer_t& inputs(process->audio_inputs[i]);
                DAF_SAFE_ASSERT_CONTINUE(inputs.channel_count != 0);

                DAF_SAFE_ASSERT_RETURN(in <= DAF_PLUGIN_NUM_INPUTS
                                       && inputs.channel_count <= DAF_PLUGIN_NUM_INPUTS - in,
                                       false);

                for (uint32_t j=0; j<inputs.channel_count; ++j, ++in)
                    audioInputs[in] = const_cast<const float*>(inputs.data32[j]);
            }

            if (fUsingCV)
            {
                for (; in<DAF_PLUGIN_NUM_INPUTS; ++in)
                    audioInputs[in] = nullptr;
            }
            else
            {
               #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
                const uint32_t expectedInputs = fSupportsAudioPortsConfig
                                              ? fSelectedInputChannels
                                              : DAF_PLUGIN_NUM_INPUTS;
               #else
                constexpr uint32_t expectedInputs = DAF_PLUGIN_NUM_INPUTS;
               #endif
                DAF_SAFE_ASSERT_UINT2_RETURN(in == expectedInputs, in, expectedInputs, false);
                for (; in<DAF_PLUGIN_NUM_INPUTS; ++in)
                    audioInputs[in] = nullptr;
            }
           #else
            constexpr const float** const audioInputs = nullptr;
           #endif

           #if DAF_PLUGIN_NUM_OUTPUTS != 0
            float** const audioOutputs = fAudioOutputs;

            uint32_t out=0;
            for (uint32_t i=0; i<process->audio_outputs_count; ++i)
            {
                const clap_audio_buffer_t& outputs(process->audio_outputs[i]);
                DAF_SAFE_ASSERT_CONTINUE(outputs.channel_count != 0);

                DAF_SAFE_ASSERT_RETURN(out <= DAF_PLUGIN_NUM_OUTPUTS
                                       && outputs.channel_count <= DAF_PLUGIN_NUM_OUTPUTS - out,
                                       false);

                for (uint32_t j=0; j<outputs.channel_count; ++j, ++out)
                    audioOutputs[out] = outputs.data32[j];
            }

            if (fUsingCV)
            {
                for (; out<DAF_PLUGIN_NUM_OUTPUTS; ++out)
                    audioOutputs[out] = nullptr;
            }
            else
            {
               #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
                const uint32_t expectedOutputs = fSupportsAudioPortsConfig
                                               ? fSelectedOutputChannels
                                               : DAF_PLUGIN_NUM_OUTPUTS;
               #else
                constexpr uint32_t expectedOutputs = DAF_PLUGIN_NUM_OUTPUTS;
               #endif
                DAF_SAFE_ASSERT_UINT2_RETURN(out == expectedOutputs, out, expectedOutputs, false);
                for (; out<DAF_PLUGIN_NUM_OUTPUTS; ++out)
                    audioOutputs[out] = nullptr;
            }
           #else
            constexpr float** const audioOutputs = nullptr;
           #endif

            fOutputEvents = process->out_events;

           #if DAF_PLUGIN_WANT_MIDI_INPUT
            fPlugin.run(audioInputs, audioOutputs, frames, fMidiEvents, fMidiEventCount);
           #else
            fPlugin.run(audioInputs, audioOutputs, frames);
           #endif

            flushParameters(nullptr, process->out_events, frames - 1);

            fOutputEvents = nullptr;
        }

       #if DAF_PLUGIN_WANT_LATENCY
        // exactly once per block, and also for blocks with no frames at all: the
        // parameter events above are enough to move the latency on their own.
        // flushParameters() deliberately does not check, see flushParametersFromHost.
        checkForLatencyChanges(true);
       #endif

       #if DAF_PLUGIN_WANT_TAIL
        // clap_host_tail::changed is [audio-thread], so unlike latency the tail can be
        // announced from right here, once per block, whenever run() moved it.
        if (const uint32_t tail = fPlugin.getTail(); tail != fLastKnownTail)
        {
            fLastKnownTail = tail;
            if (fHostExtensions.tail != nullptr && fHostExtensions.tail->changed != nullptr)
                fHostExtensions.tail->changed(fHost);
        }
       #endif

        return true;
    }

    // Latency reporting used to be deferred to here, but clap_host_latency::changed is
    // [main-thread & being-activated], so it now happens in activate().
    // What remains is finishing Plugin::updateStateValue() calls, see updateState().
    void onMainThread()
    {
       #if DAF_PLUGIN_WANT_STATE
        applyStateUpdates(true);
       #endif
    }

    // ----------------------------------------------------------------------------------------------------------------
    // parameters

    uint32_t getParameterCount() const
    {
        return fPlugin.getParameterCount();
    }

    bool getParameterInfo(const uint32_t index, clap_param_info_t* const info) const
    {
        const ParameterRanges& ranges(fPlugin.getParameterRanges(index));

        switch (fPlugin.getParameterDesignation(index))
        {
        case kParameterDesignationBypass:
            info->flags = CLAP_PARAM_IS_STEPPED|CLAP_PARAM_IS_BYPASS|CLAP_PARAM_IS_AUTOMATABLE;
            std::strcpy(info->name, "Bypass");
            std::strcpy(info->module, "daf_bypass");
            break;
        case kParameterDesignationReset:
            info->flags = CLAP_PARAM_IS_STEPPED|CLAP_PARAM_IS_READONLY;
            std::strcpy(info->name, "Reset");
            std::strcpy(info->module, "daf_reset");
            break;
        default:
            const uint32_t hints = fPlugin.getParameterHints(index);
            const uint32_t groupId = fPlugin.getParameterGroupId(index);

            info->flags = 0;
            if (hints & kParameterIsOutput)
                info->flags |= CLAP_PARAM_IS_READONLY;
            else if (hints & kParameterIsAutomatable)
                info->flags |= CLAP_PARAM_IS_AUTOMATABLE;

            if (hints & (kParameterIsBoolean|kParameterIsInteger))
                info->flags |= CLAP_PARAM_IS_STEPPED;

            d_strncpy(info->name, fPlugin.getParameterName(index), CLAP_NAME_SIZE);

            uint wrtn;
            if (groupId != kPortGroupNone)
            {
                const PortGroupWithId& portGroup(fPlugin.getPortGroupById(groupId));
                strncpy(info->module, portGroup.symbol, CLAP_PATH_SIZE / 2);
                info->module[CLAP_PATH_SIZE / 2] = '\0';
                wrtn = std::strlen(info->module);
                info->module[wrtn++] = '/';
            }
            else
            {
                wrtn = 0;
            }

            d_strncpy(info->module + wrtn, fPlugin.getParameterSymbol(index), CLAP_PATH_SIZE - wrtn);
            break;
        }

        info->id = index;
        info->cookie = nullptr;
        info->min_value = ranges.min;
        info->max_value = ranges.max;
        info->default_value = ranges.def;
        return true;
    }

    bool getParameterValue(const clap_id param_id, double* const value) const
    {
        *value = fPlugin.getParameterValue(param_id);
        return true;
    }

    bool getParameterStringForValue(const clap_id param_id, double value, char* const display, const uint32_t size) const
    {
        const ParameterEnumerationValues& enumValues(fPlugin.getParameterEnumValues(param_id));
        const ParameterRanges& ranges(fPlugin.getParameterRanges(param_id));
        const uint32_t hints = fPlugin.getParameterHints(param_id);

        if (fPlugin.hasCustomParameterText(param_id))
            return fPlugin.getParameterValueText(param_id, static_cast<float>(value), display, size);

        if (hints & kParameterIsBoolean)
        {
            const float midRange = ranges.min + (ranges.max - ranges.min) * 0.5f;
            value = value > midRange ? ranges.max : ranges.min;
        }
        else if (hints & kParameterIsInteger)
        {
            value = std::round(value);
        }

        for (uint32_t i=0; i < enumValues.count; ++i)
        {
            if (d_isEqual(static_cast<double>(enumValues.values[i].value), value))
            {
                d_strncpy(display, enumValues.values[i].label, size);
                return true;
            }
        }

        if (hints & kParameterIsInteger)
            snprintf_i32(display, value, size);
        else
            snprintf_f32(display, value, size);

        return true;
    }

    bool getParameterValueForString(const clap_id param_id, const char* const display, double* const value) const
    {
        const ParameterEnumerationValues& enumValues(fPlugin.getParameterEnumValues(param_id));
        if (fPlugin.hasCustomParameterText(param_id))
        {
            float parsed;
            if (!fPlugin.getParameterValueFromText(param_id, display, parsed))
                return false;
            *value = parsed;
            return true;
        }
        const bool isInteger = fPlugin.isParameterInteger(param_id);

        for (uint32_t i=0; i < enumValues.count; ++i)
        {
            if (std::strcmp(display, enumValues.values[i].label) == 0)
            {
                *value = enumValues.values[i].value;
                return true;
            }
        }

        if (isInteger)
            *value = std::atoi(display);
        else
            *value = std::atof(display);

        return true;
    }

    void flushParameters(const clap_input_events_t* const in,
                         const clap_output_events_t* const out,
                         const uint32_t frameOffset)
    {
        if (const uint32_t len = in != nullptr ? in->size(in) : 0)
        {
            for (uint32_t i=0; i<len; ++i)
            {
                const clap_event_header_t* const event = in->get(in, i);

                if (event->type != CLAP_EVENT_PARAM_VALUE)
                    continue;
                if (event->space_id != CLAP_CORE_EVENT_SPACE_ID)
                    continue;

                DAF_SAFE_ASSERT_UINT2_BREAK(event->size == sizeof(clap_event_param_value_t),
                                                event->size, sizeof(clap_event_param_value_t));

                setParameterValueFromEvent(reinterpret_cast<const clap_event_param_value_t*>(event));
            }
        }

        // -1 for note_id/port_index/channel/key, as in sendUIEventsToHost: a parameter the plugin
        // moves reports one global value, not one belonging to voice 0 on channel 0.
        clap_event_param_value_t clapEvent = {
            { sizeof(clap_event_param_value_t), frameOffset, 0, CLAP_EVENT_PARAM_VALUE, CLAP_EVENT_IS_LIVE },
            0, nullptr, -1, -1, -1, -1, 0.0
        };

        float value;
        for (uint i=0; i<fCachedParameters.numParams; ++i)
        {
            if (! fPlugin.isParameterOutputOrTrigger(i))
                continue;

            value = fPlugin.getParameterValue(i);

            if (d_isEqual(fCachedParameters.values[i].load(std::memory_order_relaxed), value))
                continue;

            // The two kinds treat a missing output list differently, and the asymmetry is the point.
            //
            // Output parameters take the cache update unconditionally: nothing below will report
            // them anyway, and the cache is what feeds the embedded UI (ClapUI::idleCallback drains
            // changed[]), so gating it on a host connection would freeze the plugin's own meters
            // whenever a host flushes without one.
            //
            // Trigger parameters defer instead, leaving the cache untouched so the next call with a
            // real list still sees the change and reports it. Caching now would make d_isEqual
            // swallow it there, pinning the host's value at "pressed" forever, the same loss that
            // sendUIEventsToHost's watermark exists to avoid.
            //
            // The cache is atomic: the UI consumes updates without blocking the audio thread.
            if (out == nullptr && ! fPlugin.isParameterOutput(i))
                continue;

            fCachedParameters.values[i] = value;
            fCachedParameters.changed[i] = true;

            // Output parameters are deliberately never reported to the host. Note the test is
            // isParameterOutput, not the CLAP flag: getParameterInfo also stamps
            // CLAP_PARAM_IS_READONLY on the Reset designation, which is a trigger and still reports.
            //
            // clap/ext/params.h grants the host clap_plugin_params.get_value() "at any time" on the
            // main thread, and an output parameter is one "changed by the plugin and never modified
            // by the host": there is no automation lane and no MIDI mapping riding on the host's
            // cached copy. A host that wants to display the value polls for it.
            //
            // For a meter that is the whole story, and reporting one costs far more than it buys. A
            // meter moves nearly every block, so a plugin like TapeMachine 2 pushed two
            // CLAP_EVENT_PARAM_VALUE per block (750 a second at 48kHz with 128-frame buffers) into
            // the same out_events queue that carries genuine user edits from the plugin's own GUI.
            // Unlike VST3, which has a separate outputParameterChanges channel, CLAP gives a host
            // no way to tell the two apart beyond the header flags, and these events claimed
            // CLAP_EVENT_IS_LIVE ("a user turning a physical knob") while carrying no
            // CLAP_EVENT_DONT_RECORD. In Bitwig the meter kept capturing the device panel's
            // last-accessed-parameter slot and re-marked the project modified immediately after
            // every save (dusk-audio/plugins#231). Whether that host keys on the flags or on the
            // events existing at all was never isolated, which is exactly why the events go rather
            // than just their flags: dropping them fixes the symptom under either reading.
            //
            // The trade, for honesty: a low-rate informational output parameter (DAF's Info and
            // FileHandling examples publish buffer size and file sizes that way) now only reaches a
            // host that polls. That is a real regression for those, judged well worth trading for a
            // format-wide meter fix, and get_value is always there to serve it.
            //
            // Trigger parameters still report. Their reset back to default is a real value the host
            // must see to stop re-firing them, it happens once per press rather than continuously,
            // and it is the tail of an actual user gesture.
            if (fPlugin.isParameterOutput(i))
                continue;

            clapEvent.param_id = i;
            clapEvent.value = value;
            out->try_push(out, &clapEvent.header);
        }
    }

    // clap_plugin_params::flush entry point. Runs on the audio thread while the plugin
    // is active and on the main thread while it is deactivated, so unlike process()
    // this one cannot assume a restart may be requested.
    void flushParametersFromHost(const clap_input_events_t* const in, const clap_output_events_t* const out)
    {
       #if DAF_PLUGIN_HAS_UI
        // Same order as process(): UI edits are applied first, so a value the host is delivering in
        // this very call still wins over one the UI queued before it.
        sendUIEventsToHost(out);
       #endif

        flushParameters(in, out, 0);

       #if DAF_PLUGIN_WANT_LATENCY
        checkForLatencyChanges(fPlugin.isActive());
       #endif
    }

    // ----------------------------------------------------------------------------------------------------------------

   #if DAF_PLUGIN_WANT_MIDI_INPUT
    void addNoteEvent(const clap_event_note_t* const event, const bool isOn) noexcept
    {
        DAF_SAFE_ASSERT_UINT_RETURN(event->port_index == 0, event->port_index,);

        if (fMidiEventCount == kMaxMidiEvents)
            return;

        MidiEvent& midiEvent(fMidiEvents[fMidiEventCount++]);
        midiEvent.frame = event->header.time;
        midiEvent.size  = 3;
        midiEvent.data[0] = (isOn ? 0x90 : 0x80) | (event->channel & 0x0F);
        midiEvent.data[1] = std::max(0, std::min(127, static_cast<int>(event->key)));
        midiEvent.data[2] = std::max(0, std::min(127, static_cast<int>(event->velocity * 127 + 0.5)));
    }

    void addMidiEvent(const clap_event_midi_t* const event) noexcept
    {
        DAF_SAFE_ASSERT_UINT_RETURN(event->port_index == 0, event->port_index,);

        if (fMidiEventCount == kMaxMidiEvents)
            return;

        MidiEvent& midiEvent(fMidiEvents[fMidiEventCount++]);
        midiEvent.frame = event->header.time;
        midiEvent.size  = 3;
        std::memcpy(midiEvent.data, event->data, 3);
    }
   #endif

    void setParameterValueFromEvent(const clap_event_param_value_t* const event)
    {
        if (event->param_id >= fCachedParameters.numParams || fPlugin.isParameterOutput(event->param_id))
            return;
        fPlugin.setParameterValue(event->param_id, event->value);
        fCachedParameters.values[event->param_id] = fPlugin.getParameterValue(event->param_id);
        fCachedParameters.changed[event->param_id] = true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // audio ports

   #if DAF_PLUGIN_NUM_INPUTS+DAF_PLUGIN_NUM_OUTPUTS != 0
    template<bool isInput>
    uint32_t getAudioPortCount() const noexcept
    {
        return (isInput ? fAudioInputBuses : fAudioOutputBuses).size();
    }

    template<bool isInput>
    bool getAudioPortInfo(const uint32_t index, clap_audio_port_info_t* const info) const noexcept
    {
        const std::vector<BusInfo>& busInfos(isInput ? fAudioInputBuses : fAudioOutputBuses);
        DAF_SAFE_ASSERT_RETURN(index < busInfos.size(), false);

        const BusInfo& busInfo(busInfos[index]);

        info->id = busInfo.groupId;
        d_strncpy(info->name, busInfo.name, CLAP_NAME_SIZE);

        info->flags = busInfo.isMain ? CLAP_AUDIO_PORT_IS_MAIN : 0x0;
        info->channel_count = busInfo.numChannels;

       #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
        if (fSupportsAudioPortsConfig)
            info->port_type = getClapChannelType(busInfo.numChannels);
        else
       #endif
        {
            switch (busInfo.groupId)
            {
            case kPortGroupMono: info->port_type = CLAP_PORT_MONO; break;
            case kPortGroupStereo: info->port_type = CLAP_PORT_STEREO; break;
            default: info->port_type = nullptr; break;
            }
        }

        info->in_place_pair = busInfo.hasPair ? busInfo.groupId : CLAP_INVALID_ID;
        return true;
    }
   #endif

   #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
    bool supportsAudioPortsConfig() const noexcept
    {
        return fSupportsAudioPortsConfig;
    }

    uint32_t getAudioPortsConfigCount() const noexcept
    {
        if (! fSupportsAudioPortsConfig)
            return 0;

        uint32_t count = 0;
        for (uint32_t i=0; i<ARRAY_SIZE(kClapChannelConfigs); ++i)
        {
            bool duplicate = false;
            for (uint32_t j=0; j<i; ++j)
            {
                if (kClapChannelConfigs[i].inputs == kClapChannelConfigs[j].inputs
                    && kClapChannelConfigs[i].outputs == kClapChannelConfigs[j].outputs)
                {
                    duplicate = true;
                    break;
                }
            }
            if (! duplicate)
                ++count;
        }
        return count;
    }

    bool getAudioPortsConfig(const uint32_t index, clap_audio_ports_config_t* const config) const noexcept
    {
        DAF_SAFE_ASSERT_RETURN(config != nullptr, false);
        if (! fSupportsAudioPortsConfig)
            return false;

        uint32_t logicalIndex = 0;
        for (uint32_t i=0; i<ARRAY_SIZE(kClapChannelConfigs); ++i)
        {
            const ClapChannelConfig& candidate(kClapChannelConfigs[i]);
            bool duplicate = false;
            for (uint32_t j=0; j<i; ++j)
            {
                if (candidate.inputs == kClapChannelConfigs[j].inputs
                    && candidate.outputs == kClapChannelConfigs[j].outputs)
                {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate)
                continue;
            if (logicalIndex++ != index)
                continue;

            config->id = getClapChannelConfigId(i);
            std::snprintf(config->name, CLAP_NAME_SIZE, "%u input / %u output",
                          static_cast<uint32_t>(candidate.inputs),
                          static_cast<uint32_t>(candidate.outputs));
            config->input_port_count = 1;
            config->output_port_count = 1;
            config->has_main_input = true;
            config->main_input_channel_count = candidate.inputs;
            config->main_input_port_type = getClapChannelType(candidate.inputs);
            config->has_main_output = true;
            config->main_output_channel_count = candidate.outputs;
            config->main_output_port_type = getClapChannelType(candidate.outputs);
            return true;
        }
        return false;
    }

    bool selectAudioPortsConfig(const clap_id configId)
    {
        if (! fSupportsAudioPortsConfig || fPlugin.isActive())
            return false;

        uint32_t sourceIndex = 0;
        if (configId < kClapChannelConfigIdBase)
            return false;
        sourceIndex = configId - kClapChannelConfigIdBase;
        if (sourceIndex >= ARRAY_SIZE(kClapChannelConfigs))
            return false;

        const ClapChannelConfig& selected(kClapChannelConfigs[sourceIndex]);
        for (uint32_t i=0; i<sourceIndex; ++i)
        {
            if (selected.inputs == kClapChannelConfigs[i].inputs
                && selected.outputs == kClapChannelConfigs[i].outputs)
                return false;
        }

        const uint16_t inputs = static_cast<uint16_t>(selected.inputs);
        const uint16_t outputs = static_cast<uint16_t>(selected.outputs);
        fPlugin.setAudioPortIO(inputs, outputs);
        fSelectedInputChannels = inputs;
        fSelectedOutputChannels = outputs;
        rebuildSimpleAudioBuses(inputs, outputs);
        return true;
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // latency

   #if DAF_PLUGIN_WANT_LATENCY
    // clap_plugin_latency::get. Deliberately serves the value snapshotted at
    // activation rather than the plugin's live latency: "Once activated the latency
    // and port configuration must remain constant, until deactivation"
    // (clap/plugin.h), so a host polling this while active must not observe a change
    // that is still waiting for the restart below to apply it.
    //
    // Thread safety: fReportedLatency is written only by the constructor and by
    // reportLatencyChangeIfNeeded(), which activate() alone calls, and read only here.
    // clap_plugin_latency::get is [main-thread & (being-activated | active)] and
    // clap_plugin::activate is [main-thread], so the two cannot overlap and this needs
    // no atomic -- unlike the latch fields below, which the audio thread also touches.
    uint32_t getLatency() const noexcept
    {
        return fReportedLatency;
    }
   #endif

   #if DAF_PLUGIN_WANT_TAIL
    // clap_plugin_tail::get is [main-thread, audio-thread] and the tail may move at
    // any time, so serve the live value; no activation snapshot is required.
    uint32_t getTail() const noexcept
    {
        return fPlugin.getTail();
    }
   #endif

   #if DAF_PLUGIN_WANT_LATENCY
    // how many process cycles to wait before re-asking a host that dropped a
    // request_restart. The check runs once per block, so at 128 frames and 48 kHz this
    // is 512 * 128 / 48000 ~= 1.4 seconds. The exact value is not critical, it only has
    // to be long enough not to spam a host that is deliberately deferring the restart.
    static constexpr uint32_t kLatencyRestartRetryBlocks = 512;

    // Records a latency change, but never announces it to the host from here.
    //
    // The CLAP spec pins the reported latency to the active period:
    //   "Once activated the latency and port configuration must remain constant,
    //    until deactivation." -- clap/plugin.h, clap_plugin::activate
    //   "The latency is only allowed to change during plugin->activate.
    //    If the plugin is activated, call host->request_restart()
    //    [main-thread & being-activated]"
    //                                     -- clap/ext/latency.h, clap_host_latency::changed
    //
    // So the only point at which a new latency both exists and may be published is
    // activation. clap-validator enforces exactly that, rejecting any call made from
    // another entry point with "clap_host_latency::changed / Must only be called
    // within 'clap_plugin::activate'".
    //
    // Therefore: while active we only ask the host for a restart, and in every case
    // we just latch the pending change. It is announced from activate(), which is
    // where the host re-reads clap_plugin_latency::get. A change detected while the
    // plugin is deactivated is likewise held until the next activate() -- the host
    // cannot act on a new latency before activating anyway.
    //
    // canRequestRestart says whether clap_host::request_restart may be called from the
    // calling context, not whether the plugin happens to be active. activate() and
    // deactivate() pass false: the host is already changing the activation state there,
    // and asking it to restart mid-transition is what triggers the loop described above.
    void checkForLatencyChanges(const bool canRequestRestart)
    {
        const uint32_t latency = fPlugin.getLatency();

        // Read-modify-write in one step. process() and clap_plugin_params::flush run
        // on the audio thread, while clap_plugin_state::load runs on the main thread
        // and is legal while the plugin is active, so two threads can reach this
        // concurrently. A plain load/compare/store could lose the update and with it
        // both the latch and the restart request below.
        //
        // The latch is set regardless of whether the host implements clap.latency: the
        // restart request below is what actually applies the new value, and a host
        // without the extension still needs it. Only the changed() notification in
        // reportLatencyChangeIfNeeded() depends on the extension being there.
        if (fLastKnownLatency.exchange(latency) != latency)
        {
            fLatencyChanged = true;
            // ask again straight away now that the value has moved
            fLatencyRestartRequests = 0;
        }

        if (! canRequestRestart)
            return;

        // The host has to deactivate us before the new latency can take effect, and
        // the latch stays set until activate() consumes it. A host that drops the
        // request would otherwise serve a stale latency forever, since
        // fLastKnownLatency has already advanced and nothing would re-request, so
        // keep asking while the change is pending.
        //
        // This cannot become the activate/request_restart loop clap-validator watches
        // for: a host that does restart clears the latch in activate() before
        // processing resumes, so it only ever sees the one request. The interval
        // merely keeps a host that deliberately defers from being spammed every block.
        if (fLatencyChanged && (fLatencyRestartRequests++ % kLatencyRestartRetryBlocks) == 0)
            fHost->request_restart(fHost);
    }

    // must only be called from within clap_plugin::activate, see above
    void reportLatencyChangeIfNeeded()
    {
        // publish the value the host is about to read back, before telling it to look
        const uint32_t latency = fLastKnownLatency;
        const uint32_t previousLatency = fReportedLatency;
        fReportedLatency = latency;

        if (! fLatencyChanged.exchange(false))
            return;

        fLatencyRestartRequests = 0;

        // The latch only says the latency moved at some point since the last
        // activation, not that it ended up somewhere new. An A -> B -> A round trip
        // between two activations leaves the host with the value it already has, so
        // do not wake it up for that.
        if (latency == previousLatency)
            return;

        if (fHostExtensions.latency != nullptr && fHostExtensions.latency->changed != nullptr)
            fHostExtensions.latency->changed(fHost);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // state

    bool stateSave(const clap_ostream_t* const stream)
    {
       #if DAF_PLUGIN_WANT_STATE
        // save what the plugin has, even if the main-thread callback for it did not run yet
        applyStateUpdates(false);
       #endif

        const uint32_t paramCount = fPlugin.getParameterCount();
       #if DAF_PLUGIN_WANT_STATE
        const uint32_t stateCount = fPlugin.getStateCount();
       #else
        const uint32_t stateCount = 0;
       #endif

        if (stateCount == 0 && paramCount == 0)
        {
            char buffer = '\0';
            return stream->write(stream, &buffer, 1) == 1;
        }

       #if DAF_PLUGIN_WANT_FULL_STATE
        // Update current state
        for (StringMap::const_iterator cit=fStateMap.begin(), cite=fStateMap.end(); cit != cite; ++cit)
        {
            const String& key(cit->first);
            fStateMap[key] = fPlugin.getStateValue(key);
        }
       #endif

        String state;

       #if DAF_PLUGIN_WANT_PROGRAMS
        {
            String tmpStr("__daf_program__\xff");
            tmpStr += String(fCurrentProgram);
            tmpStr += "\xff";

            state += tmpStr;
        }
       #endif

       #if DAF_PLUGIN_WANT_STATE
        if (stateCount != 0)
        {
            state += "__daf_state_begin__\xff";

            for (StringMap::const_iterator cit=fStateMap.begin(), cite=fStateMap.end(); cit != cite; ++cit)
            {
                const String& key(cit->first);
                const String& value(cit->second);

                // join key and value
                String tmpStr;
                tmpStr  = key;
                tmpStr += "\xff";
                tmpStr += value;
                tmpStr += "\xff";

                state += tmpStr;
            }

            state += "__daf_state_end__\xff";
        }
       #endif

        if (paramCount != 0)
        {
            state += "__daf_parameters_begin__\xff";

            for (uint32_t i=0; i<paramCount; ++i)
            {
                if (fPlugin.isParameterOutputOrTrigger(i))
                    continue;

                // join key and value
                String tmpStr;
                tmpStr  = fPlugin.getParameterSymbol(i);
                tmpStr += "\xff";
                if (fPlugin.getParameterHints(i) & kParameterIsInteger)
                    tmpStr += String(static_cast<int>(std::round(fPlugin.getParameterValue(i))));
                else
                    tmpStr += String(fPlugin.getParameterValue(i));
                tmpStr += "\xff";

                state += tmpStr;
            }

            state += "__daf_parameters_end__\xff";
        }

        // terminator
        state += "\xfe";

        state.replace('\xff', '\0');

        // now saving state, carefully until host written bytes matches full state size
        const char* buffer = state.buffer();
        const int32_t size = static_cast<int32_t>(state.length())+1;

        // A short write is normal and simply loops again. A write of 0 is not: the stream made no
        // progress on a non-empty request, and retrying it is either an immediate spin or a hang,
        // so treat it as the failure it is rather than looping on a host that has stopped moving.
        for (int32_t wrtntotal = 0, wrtn; wrtntotal < size; wrtntotal += wrtn)
        {
            wrtn = stream->write(stream, buffer + wrtntotal, size - wrtntotal);
            DAF_SAFE_ASSERT_INT_RETURN(wrtn > 0, wrtn, false);
        }

        return true;
    }

    bool stateLoad(const clap_istream_t* const stream)
    {
       #if DAF_PLUGIN_WANT_STATE
        // settle earlier updates first, so they cannot overwrite the state being loaded
        applyStateUpdates(false);
       #endif

       #if DAF_PLUGIN_HAS_UI
        ClapUI* const ui = fUI.get();
       #endif
        PluginStateParser parser;

        // 64 KiB per host call, on the heap; tokens spanning reads are joined by the parser
        std::vector<char> buffer(65536);

        for (bool terminated = false; ! terminated;)
        {
            const int64_t read = stream->read(stream, buffer.data(), buffer.size());
            DAF_SAFE_ASSERT_INT_RETURN(read >= 0 && read <= static_cast<int64_t>(buffer.size()), static_cast<int>(read), false);

            if (read == 0)
            {
                // stateSave() has a fast path for plugins with no parameters and no states:
                // it writes a single null byte and never emits the '\xfe' terminator.
                // Accept exactly that stream here, that is a lone empty key and nothing else
                // parsed; a genuinely truncated state still fails.
                // What matters is what the stream contains, not how many parameters or states
                // this build has, so that state saved by a differently configured build of the
                // same plugin still loads.
                if (parser.isLoneEmptyKey())
                    break;

                return false;
            }

            std::size_t consumed = 0;
            switch (parser.parse(buffer.data(), static_cast<std::size_t>(read), consumed))
            {
            case PluginStateParser::kStatusNeedMoreData:
                break;
            case PluginStateParser::kStatusTerminated:
            {
                // The writer may append one NUL after the terminator.
                // Reject all other trailing bytes, including later chunks,
                // before committing any staged state.
                const std::size_t remaining = static_cast<std::size_t>(read) - consumed;
                if (remaining > 1 || (remaining == 1 && buffer[consumed] != '\0'))
                    return false;
                char tail[2];
                const int64_t tailRead = stream->read(stream, tail, sizeof(tail));
                if (tailRead != 0)
                {
                    if (remaining != 0 || tailRead != 1 || tail[0] != '\0'
                        || stream->read(stream, tail, 1) != 0)
                        return false;
                }
                terminated = true;
                break;
            }
            case PluginStateParser::kStatusInvalidTerminator:
            case PluginStateParser::kStatusInvalidSection:
                return false;
            }
        }

        // Parse and validate the entire stream before touching live sound. A complete
        // parameter snapshot is authoritative over the redundant legacy program/parameter sections.
        bool parameterSnapshot = false;
       #if DAF_PLUGIN_WANT_STATE
        for (const PluginStateParser::Entry& item : parser.entries)
        {
            if (item.type != 's' || !fPlugin.wantStateKey(item.key.c_str()))
                continue;
            if (!fPlugin.validateStateValue(item.key.c_str(), item.value.c_str()))
                return false;
            parameterSnapshot = parameterSnapshot || fPlugin.isParameterSnapshotState(item.key.c_str());
        }
       #endif
        for (const PluginStateParser::Entry& item : parser.entries)
        {
            const char* const key = item.key.c_str();
            const char* const value = item.value.c_str();
            const char queryingType = item.type;
            const bool isProgram = item.key == "__daf_program__";
            if (parameterSnapshot && (queryingType == 'p' || isProgram))
                continue;
                    if (isProgram)
                    {
                        d_debug("found program '%s'", value);

                      #if DAF_PLUGIN_WANT_PROGRAMS
                        const int program = std::atoi(value);
                        DAF_SAFE_ASSERT_CONTINUE(program >= 0);

                        fCurrentProgram = static_cast<uint32_t>(program);
                        fPlugin.loadProgram(fCurrentProgram);

                       #if DAF_PLUGIN_HAS_UI
                        if (ui != nullptr)
                            ui->setProgramFromPlugin(fCurrentProgram);
                       #endif
                      #endif
                    }
                    else if (queryingType == 's')
                    {
                        d_debug("found state '%s' '%s'", key, value);

                       #if DAF_PLUGIN_WANT_STATE
                        if (fPlugin.wantStateKey(key))
                        {
                            fStateMap[String(key)] = value;
                            fPlugin.setState(key, value);

                           #if DAF_PLUGIN_HAS_UI
                            if (ui != nullptr)
                                ui->setStateFromPlugin(key, value);
                           #endif
                        }
                       #endif
                    }
                    else if (queryingType == 'p')
                    {
                        d_debug("found parameter '%s' '%s'", key, value);
                        float fvalue;

                        // find parameter with this symbol, and set its value
                        for (uint32_t j=0; j<fCachedParameters.numParams; ++j)
                        {
                            if (fPlugin.isParameterOutputOrTrigger(j))
                                continue;
                            if (fPlugin.getParameterSymbol(j) != key)
                                continue;

                            if (fPlugin.getParameterHints(j) & kParameterIsInteger)
                            {
                                fvalue = std::atoi(value);
                            }
                            else
                            {
                                const ScopedSafeLocale ssl;
                                fvalue = std::atof(value);
                            }

                            fCachedParameters.values[j] = fvalue;
                           #if DAF_PLUGIN_HAS_UI
                            if (ui != nullptr)
                            {
                                // UI parameter updates are handled outside the read loop (after host param restart)
                                fCachedParameters.changed[j] = true;
                            }
                           #endif
                            fPlugin.setParameterValue(j, fvalue);
                            break;
                        }
                    }

        }
        if (parameterSnapshot)
        {
           #if DAF_PLUGIN_WANT_PROGRAMS
            const int32_t program = fPlugin.getCurrentProgram();
            if (program >= 0 && static_cast<uint32_t>(program) < fPlugin.getProgramCount())
                fCurrentProgram = static_cast<uint32_t>(program);
           #endif
            for (uint i = 0; i < fCachedParameters.numParams; ++i)
            {
                if (fPlugin.isParameterOutputOrTrigger(i)) continue;
                fCachedParameters.values[i] = fPlugin.getParameterValue(i);
                fCachedParameters.changed[i] = true;
            }
        }

        if (fHostExtensions.params != nullptr)
            fHostExtensions.params->rescan(fHost, CLAP_PARAM_RESCAN_VALUES|CLAP_PARAM_RESCAN_TEXT);

       #if DAF_PLUGIN_WANT_LATENCY
        // NOTE: not reported here, it is announced on the next activate()
        checkForLatencyChanges(fPlugin.isActive());
       #endif

       #if DAF_PLUGIN_HAS_UI
        if (ui != nullptr)
        {
            for (uint32_t i=0; i<fCachedParameters.numParams; ++i)
            {
                if (fPlugin.isParameterOutputOrTrigger(i))
                    continue;
                fCachedParameters.changed[i] = false;
                ui->setParameterValueFromPlugin(i, fCachedParameters.values[i]);
            }
        }
       #endif

        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // gui

   #if DAF_PLUGIN_HAS_UI
    bool createUI(const bool isFloating)
    {
        const clap_host_gui_t* const hostGui = getHostExtension<clap_host_gui_t>(CLAP_EXT_GUI);
        DAF_SAFE_ASSERT_RETURN(hostGui != nullptr, false);

       #if DAF_CLAP_USING_HOST_TIMER
        const clap_host_timer_support_t* const hostTimer = getHostExtension<clap_host_timer_support_t>(CLAP_EXT_TIMER_SUPPORT);
        DAF_SAFE_ASSERT_RETURN(hostTimer != nullptr, false);
       #endif

        fUI = new ClapUI(fPlugin, this, fHost, fHostExtensions.params, hostGui,
                        #if DAF_CLAP_USING_HOST_TIMER
                         hostTimer,
                        #endif
                         isFloating);
        return true;
    }

    void destroyUI()
    {
        fUI = nullptr;
    }

    ClapUI* getUI() const noexcept
    {
        return fUI.get();
    }
   #endif

   #if DAF_PLUGIN_HAS_UI && DAF_PLUGIN_WANT_STATE
    void setStateFromUI(const char* const key, const char* const value) override
    {
        if (!fPlugin.validateStateValue(key, value)) return;
        const bool snapshot = fPlugin.isParameterSnapshotState(key);
        if (snapshot)
            fEventQueue.discardParameterValuesFromUI();

        fPlugin.setState(key, value);

        if (fPlugin.wantStateKey(key))
        {
            const String dkey(key);
            fStateMap[dkey] = value;

            // an earlier Plugin::updateStateValue() must not put its value back later
            fStateUpdates.supersede(key);
        }

        if (snapshot)
        {
           #if DAF_PLUGIN_WANT_PROGRAMS
            const int32_t program = fPlugin.getCurrentProgram();
            if (program >= 0 && static_cast<uint32_t>(program) < fPlugin.getProgramCount())
                fCurrentProgram = static_cast<uint32_t>(program);
           #endif
            for (uint i=0; i<fCachedParameters.numParams; ++i)
            {
                if (fPlugin.isParameterOutputOrTrigger(i))
                    continue;
                fCachedParameters.values[i] = fPlugin.getParameterValue(i);
                // Deliver on idle, after the editor has completed its state transaction.
                fCachedParameters.changed[i] = true;
            }

            if (fHostExtensions.params != nullptr)
                fHostExtensions.params->rescan(fHost, CLAP_PARAM_RESCAN_VALUES|CLAP_PARAM_RESCAN_TEXT);
            if (fHostExtensions.state != nullptr && fHostExtensions.state->mark_dirty != nullptr)
                fHostExtensions.state->mark_dirty(fHost);
        }
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------

private:
    // Plugin and UI
    PluginExporter fPlugin;
   #if DAF_PLUGIN_HAS_UI
    ScopedPointer<ClapUI> fUI;
   #endif

    // CLAP stuff
    const clap_host_t* const fHost;
    const clap_output_events_t* fOutputEvents;

    uint32_t fResetParameterIndex;
   #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
    uint16_t fSelectedInputChannels;
    uint16_t fSelectedOutputChannels;
    bool fSupportsAudioPortsConfig;
   #endif
   #if DAF_PLUGIN_NUM_INPUTS != 0
    const float* fAudioInputs[DAF_PLUGIN_NUM_INPUTS];
   #endif
   #if DAF_PLUGIN_NUM_OUTPUTS != 0
    float* fAudioOutputs[DAF_PLUGIN_NUM_OUTPUTS];
   #endif
   #if DAF_PLUGIN_NUM_INPUTS+DAF_PLUGIN_NUM_OUTPUTS != 0
    bool fUsingCV;
   #endif
   #if DAF_PLUGIN_WANT_LATENCY
    // reachable from both the audio thread (process, params flush) and the main
    // thread (state load), see checkForLatencyChanges
    std::atomic<bool> fLatencyChanged;
    std::atomic<uint32_t> fLastKnownLatency;
    std::atomic<uint32_t> fLatencyRestartRequests;
    // main-thread only, see getLatency
    uint32_t fReportedLatency;
   #endif
   #if DAF_PLUGIN_WANT_TAIL
    // last value announced through clap_host_tail::changed; seeded from the plugin in
    // the constructor (after fPlugin, see the member order above) so a construction-time
    // tail is not mistaken for a change on the first block.
    uint32_t fLastKnownTail;
   #endif
  #if DAF_PLUGIN_WANT_MIDI_INPUT
    uint32_t fMidiEventCount;
    MidiEvent fMidiEvents[kMaxMidiEvents];
   #if DAF_PLUGIN_HAS_UI
    RingBufferControl<SmallStackBuffer> fNotesRingBuffer;
   #endif
  #endif
   #if DAF_PLUGIN_WANT_TIMEPOS
    TimePosition fTimePosition;
   #endif
   #if DAF_PLUGIN_WANT_STATE
    PluginStateUpdates fStateUpdates;
   #endif

    struct HostExtensions {
        const clap_host_t* const host;
        const clap_host_params_t* params;
       #if DAF_PLUGIN_WANT_STATE
        const clap_host_state_t* state;
       #endif
       #if DAF_PLUGIN_WANT_LATENCY
        const clap_host_latency_t* latency;
       #endif
       #if DAF_PLUGIN_WANT_TAIL
        const clap_host_tail_t* tail;
       #endif

        HostExtensions(const clap_host_t* const host)
            : host(host),
              params(nullptr)
           #if DAF_PLUGIN_WANT_STATE
            , state(nullptr)
           #endif
           #if DAF_PLUGIN_WANT_LATENCY
            , latency(nullptr)
           #endif
           #if DAF_PLUGIN_WANT_TAIL
            , tail(nullptr)
           #endif
        {}

        bool init()
        {
            params = static_cast<const clap_host_params_t*>(host->get_extension(host, CLAP_EXT_PARAMS));
           #if DAF_PLUGIN_WANT_STATE
            state = static_cast<const clap_host_state_t*>(host->get_extension(host, CLAP_EXT_STATE));
           #endif
           #if DAF_PLUGIN_WANT_LATENCY
            DAF_SAFE_ASSERT_RETURN(host->request_restart != nullptr, false);
            latency = static_cast<const clap_host_latency_t*>(host->get_extension(host, CLAP_EXT_LATENCY));
           #endif
           #if DAF_PLUGIN_WANT_TAIL
            tail = static_cast<const clap_host_tail_t*>(host->get_extension(host, CLAP_EXT_TAIL));
           #endif
            return true;
        }
    } fHostExtensions;

    // ----------------------------------------------------------------------------------------------------------------
    // helper functions for dealing with buses

   #if DAF_PLUGIN_NUM_INPUTS+DAF_PLUGIN_NUM_OUTPUTS != 0
    struct BusInfo {
        char name[CLAP_NAME_SIZE];
        uint32_t numChannels;
        bool hasPair;
        bool isCV;
        bool isMain;
        uint32_t groupId;
    };
    std::vector<BusInfo> fAudioInputBuses, fAudioOutputBuses;

   #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
    bool canUseAudioPortsConfig() const noexcept
    {
        if (fUsingCV || fAudioInputBuses.size() != 1 || fAudioOutputBuses.size() != 1)
            return false;
        if (! fAudioInputBuses[0].isMain || ! fAudioOutputBuses[0].isMain)
            return false;
        if (fAudioInputBuses[0].isCV || fAudioOutputBuses[0].isCV)
            return false;
        if (fAudioInputBuses[0].numChannels != DAF_PLUGIN_NUM_INPUTS
            || fAudioOutputBuses[0].numChannels != DAF_PLUGIN_NUM_OUTPUTS)
            return false;

        for (uint32_t i=0; i<ARRAY_SIZE(kClapChannelConfigs); ++i)
        {
            const ClapChannelConfig& config(kClapChannelConfigs[i]);
            if (config.inputs <= 0 || config.outputs <= 0
                || config.inputs > DAF_PLUGIN_NUM_INPUTS
                || config.outputs > DAF_PLUGIN_NUM_OUTPUTS)
                return false;
        }
        return true;
    }

    void rebuildSimpleAudioBuses(const uint16_t inputs, const uint16_t outputs)
    {
        fAudioInputBuses.clear();
        fAudioOutputBuses.clear();
        fillInBusInfoDetails<true>();
        fillInBusInfoDetails<false>();
        fAudioInputBuses[0].numChannels = inputs;
        fAudioOutputBuses[0].numChannels = outputs;
        fillInBusInfoPairs();
    }
   #endif

    template<bool isInput>
    void fillInBusInfoDetails()
    {
        constexpr const uint32_t numPorts = isInput ? DAF_PLUGIN_NUM_INPUTS : DAF_PLUGIN_NUM_OUTPUTS;
        std::vector<BusInfo>& busInfos(isInput ? fAudioInputBuses : fAudioOutputBuses);

        enum {
            kPortTypeNull,
            kPortTypeAudio,
            kPortTypeSidechain,
            kPortTypeCV,
            kPortTypeGroup
        } lastSeenPortType = kPortTypeNull;
        uint32_t lastSeenGroupId = kPortGroupNone;
        uint32_t nonGroupAudioId = 0;
        uint32_t nonGroupSidechainId = 0x20000000;

        for (uint32_t i=0; i<numPorts; ++i)
        {
            const AudioPortWithBusId& port(fPlugin.getAudioPort(isInput, i));

            if (port.groupId != kPortGroupNone)
            {
                if (lastSeenPortType != kPortTypeGroup || lastSeenGroupId != port.groupId)
                {
                    lastSeenPortType = kPortTypeGroup;
                    lastSeenGroupId = port.groupId;

                    BusInfo busInfo = {
                        {}, 1, false, false,
                        // if this is the first port, set it as main
                        busInfos.empty(),
                        // user given group id with extra safety offset
                        port.groupId + 0x80000000
                    };

                    const PortGroupWithId& group(fPlugin.getPortGroupById(port.groupId));

                    switch (port.groupId)
                    {
                    case kPortGroupStereo:
                    case kPortGroupMono:
                        if (busInfo.isMain)
                        {
                            d_strncpy(busInfo.name, isInput ? "Audio Input" : "Audio Output", CLAP_NAME_SIZE);
                            break;
                        }
                    // fall-through
                    default:
                        if (group.name.isNotEmpty())
                            d_strncpy(busInfo.name, group.name, CLAP_NAME_SIZE);
                        else
                            d_strncpy(busInfo.name, port.name, CLAP_NAME_SIZE);
                        break;
                    }

                    busInfos.push_back(busInfo);
                }
                else
                {
                    ++busInfos.back().numChannels;
                }
            }
            else if (port.hints & kAudioPortIsCV)
            {
                // TODO
                lastSeenPortType = kPortTypeCV;
                lastSeenGroupId = kPortGroupNone;
                fUsingCV = true;
            }
            else if (port.hints & kAudioPortIsSidechain)
            {
                if (lastSeenPortType != kPortTypeSidechain)
                {
                    lastSeenPortType = kPortTypeSidechain;
                    lastSeenGroupId = kPortGroupNone;

                    BusInfo busInfo = {
                        {}, 1, false, false,
                        // not main
                        false,
                        // give unique id
                        nonGroupSidechainId++
                    };

                    d_strncpy(busInfo.name, port.name, CLAP_NAME_SIZE);

                    busInfos.push_back(busInfo);
                }
                else
                {
                    ++busInfos.back().numChannels;
                }
            }
            else
            {
                if (lastSeenPortType != kPortTypeAudio)
                {
                    lastSeenPortType = kPortTypeAudio;
                    lastSeenGroupId = kPortGroupNone;

                    BusInfo busInfo = {
                        {}, 1, false, false,
                        // if this is the first port, set it as main
                        busInfos.empty(),
                        // give unique id
                        nonGroupAudioId++
                    };

                    if (busInfo.isMain)
                    {
                        d_strncpy(busInfo.name, isInput ? "Audio Input" : "Audio Output", CLAP_NAME_SIZE);
                    }
                    else
                    {
                        d_strncpy(busInfo.name, port.name, CLAP_NAME_SIZE);
                    }

                    busInfos.push_back(busInfo);
                }
                else
                {
                    ++busInfos.back().numChannels;
                }
            }
        }
    }
   #endif

   #if DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
    void fillInBusInfoPairs()
    {
        const size_t numChannels = std::min(fAudioInputBuses.size(), fAudioOutputBuses.size());

        for (size_t i=0; i<numChannels; ++i)
        {
            if (fAudioInputBuses[i].groupId != fAudioOutputBuses[i].groupId)
                break;
            if (fAudioInputBuses[i].numChannels != fAudioOutputBuses[i].numChannels)
                break;
            if (fAudioInputBuses[i].isMain != fAudioOutputBuses[i].isMain)
                break;

            fAudioInputBuses[i].hasPair = fAudioOutputBuses[i].hasPair = true;
        }
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // DAF callbacks

   #if DAF_PLUGIN_WANT_MIDI_OUTPUT
    bool writeMidi(const MidiEvent& midiEvent)
    {
        DAF_SAFE_ASSERT_RETURN(fOutputEvents != nullptr, false);

        if (midiEvent.size > 3)
            return true;

        const clap_event_midi clapEvent = {
            { sizeof(clap_event_midi), midiEvent.frame, 0, CLAP_EVENT_MIDI, CLAP_EVENT_IS_LIVE },
            0, { midiEvent.data[0],
                 static_cast<uint8_t>(midiEvent.size >= 2 ? midiEvent.data[1] : 0),
                 static_cast<uint8_t>(midiEvent.size >= 3 ? midiEvent.data[2] : 0) }
        };
        return fOutputEvents->try_push(fOutputEvents, &clapEvent.header);
    }

    static bool writeMidiCallback(void* const ptr, const MidiEvent& midiEvent)
    {
        return static_cast<PluginCLAP*>(ptr)->writeMidi(midiEvent);
    }
   #endif

   #if DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
    bool requestParameterValueChange(uint32_t, float)
    {
        return true;
    }

    static bool requestParameterValueChangeCallback(void* const ptr, const uint32_t index, const float value)
    {
        return static_cast<PluginCLAP*>(ptr)->requestParameterValueChange(index, value);
    }
   #endif

   #if DAF_PLUGIN_WANT_STATE
    // any thread but the audio one
    bool updateState(const char* const key, const char* const value)
    {
        if (! fStateUpdates.update(fPlugin, key, value))
            return false;

        // the state map, the UI and clap_host_state are main-thread only, continue in onMainThread()
        fHost->request_callback(fHost);
        return true;
    }

    // main thread, as are all of clap_plugin_state. A host that is saving or loading state at the time
    // needs no telling: what it saves already has the update, and what it loads replaces it.
    void applyStateUpdates(const bool hostNeedsTelling)
    {
        PluginStateUpdates::Map updates;
        bool markDirty = false;
        if (! fStateUpdates.takeForMainThread(updates, markDirty))
            return;

       #if DAF_PLUGIN_HAS_UI
        ClapUI* const ui = fUI.get();
       #endif

        for (PluginStateUpdates::Map::const_iterator cit=updates.begin(), cite=updates.end(); cit != cite; ++cit)
        {
            const String& key(cit->first);
            const String& value(cit->second);

            fStateMap[key] = value;

           #if DAF_PLUGIN_HAS_UI
            if (ui != nullptr && isStateForUI(fPlugin, key))
                ui->setStateFromPlugin(key, value);
           #endif
        }

        if (hostNeedsTelling && markDirty &&
            fHostExtensions.state != nullptr && fHostExtensions.state->mark_dirty != nullptr)
            fHostExtensions.state->mark_dirty(fHost);
    }

    static bool updateStateValueCallback(void* const ptr, const char* const key, const char* const value)
    {
        return static_cast<PluginCLAP*>(ptr)->updateState(key, value);
    }
   #endif
};

// --------------------------------------------------------------------------------------------------------------------

static ScopedPointer<PluginExporter> sPlugin;

// --------------------------------------------------------------------------------------------------------------------
// plugin gui

#if DAF_PLUGIN_HAS_UI

// The single window API this build is able to honour; see DAF_CLAP_GUI_FLOATING_ONLY above.
static const char* const kSupportedAPIs[] = {
#if defined(DAF_OS_WINDOWS)
    CLAP_WINDOW_API_WIN32,
#elif defined(DAF_OS_MAC)
    CLAP_WINDOW_API_COCOA,
#elif DAF_CLAP_GUI_FLOATING_ONLY
    CLAP_WINDOW_API_WAYLAND,
#else
    CLAP_WINDOW_API_X11,
#endif
};

// TODO DAF external UI
static bool CLAP_ABI clap_gui_is_api_supported(const clap_plugin_t*, const char* const api, const bool is_floating)
{
    // might be unused
    (void)is_floating;

   #if DAF_CLAP_GUI_FLOATING_ONLY
    // Wayland cannot embed, so only the floating variant of the advertised API is real.
    if (! is_floating)
        return false;
   #endif

    if (api != nullptr)
    {
        for (size_t i=0; i<ARRAY_SIZE(kSupportedAPIs); ++i)
        {
            if (std::strcmp(kSupportedAPIs[i], api) == 0)
                return true;
        }
    }

    return false;
}

// TODO DAF external UI
static bool CLAP_ABI clap_gui_get_preferred_api(const clap_plugin_t*, const char** const api, bool* const is_floating)
{
    *api = kSupportedAPIs[0];
    *is_floating = DAF_CLAP_GUI_FLOATING_ONLY != 0;
    return true;
}

static bool CLAP_ABI clap_gui_create(const clap_plugin_t* const plugin, const char* const api, const bool is_floating)
{
   #if DAF_CLAP_GUI_FLOATING_ONLY
    if (! is_floating)
        return false;
   #endif

    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);

    // "api may be null or blank for floating window", as per clap/ext/gui.h
    if (is_floating && (api == nullptr || api[0] == '\0'))
        return instance->createUI(true);

    DAF_SAFE_ASSERT_RETURN(api != nullptr, false);

    for (size_t i=0; i<ARRAY_SIZE(kSupportedAPIs); ++i)
    {
        if (std::strcmp(kSupportedAPIs[i], api) == 0)
            return instance->createUI(is_floating);
    }

    return false;
}

static void CLAP_ABI clap_gui_destroy(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    instance->destroyUI();
}

static bool CLAP_ABI clap_gui_set_scale(const clap_plugin_t* const plugin, const double scale)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
   #ifndef DAF_OS_MAC
    return gui->setScaleFactor(scale);
   #else
    return true;
    // unused
    (void)scale;
   #endif
}

static bool CLAP_ABI clap_gui_get_size(const clap_plugin_t* const plugin, uint32_t* const width, uint32_t* const height)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->getSize(width, height);
}

static bool CLAP_ABI clap_gui_can_resize(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->canResize();
}

static bool CLAP_ABI clap_gui_get_resize_hints(const clap_plugin_t* const plugin, clap_gui_resize_hints_t* const hints)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->getResizeHints(hints);
}

static bool CLAP_ABI clap_gui_adjust_size(const clap_plugin_t* const plugin, uint32_t* const width, uint32_t* const height)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->adjustSize(width, height);
}

static bool CLAP_ABI clap_gui_set_size(const clap_plugin_t* const plugin, const uint32_t width, const uint32_t height)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->setSizeFromHost(width, height);
}

static bool CLAP_ABI clap_gui_set_parent(const clap_plugin_t* const plugin, const clap_window_t* const window)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->setParent(window);
}

static bool CLAP_ABI clap_gui_set_transient(const clap_plugin_t* const plugin, const clap_window_t* const window)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->setTransient(window);
}

static void CLAP_ABI clap_gui_suggest_title(const clap_plugin_t* const plugin, const char* const title)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr,);
    return gui->suggestTitle(title);
}

static bool CLAP_ABI clap_gui_show(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->show();
}

static bool CLAP_ABI clap_gui_hide(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr, false);
    return gui->hide();
}

static const clap_plugin_gui_t clap_plugin_gui = {
    clap_gui_is_api_supported,
    clap_gui_get_preferred_api,
    clap_gui_create,
    clap_gui_destroy,
    clap_gui_set_scale,
    clap_gui_get_size,
    clap_gui_can_resize,
    clap_gui_get_resize_hints,
    clap_gui_adjust_size,
    clap_gui_set_size,
    clap_gui_set_parent,
    clap_gui_set_transient,
    clap_gui_suggest_title,
    clap_gui_show,
    clap_gui_hide
};

// --------------------------------------------------------------------------------------------------------------------
// plugin timer

#if DAF_CLAP_USING_HOST_TIMER
static void CLAP_ABI clap_plugin_on_timer(const clap_plugin_t* const plugin, clap_id)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    ClapUI* const gui = instance->getUI();
    DAF_SAFE_ASSERT_RETURN(gui != nullptr,);
    return gui->idleCallback();
}

static const clap_plugin_timer_support_t clap_timer = {
    clap_plugin_on_timer
};
#endif

#endif // DAF_PLUGIN_HAS_UI

// --------------------------------------------------------------------------------------------------------------------
// plugin audio ports

#if DAF_PLUGIN_NUM_INPUTS+DAF_PLUGIN_NUM_OUTPUTS != 0
static uint32_t CLAP_ABI clap_plugin_audio_ports_count(const clap_plugin_t* const plugin, const bool is_input)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return is_input ? instance->getAudioPortCount<true>()
                    : instance->getAudioPortCount<false>();
}

static bool CLAP_ABI clap_plugin_audio_ports_get(const clap_plugin_t* const plugin,
                                        const uint32_t index,
                                        const bool is_input,
                                        clap_audio_port_info_t* const info)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return is_input ? instance->getAudioPortInfo<true>(index, info)
                    : instance->getAudioPortInfo<false>(index, info);
}

static const clap_plugin_audio_ports_t clap_plugin_audio_ports = {
    clap_plugin_audio_ports_count,
    clap_plugin_audio_ports_get
};
#endif

#if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
static uint32_t CLAP_ABI clap_plugin_audio_ports_config_count(const clap_plugin_t* const plugin)
{
    const PluginCLAP* const instance = static_cast<const PluginCLAP*>(plugin->plugin_data);
    return instance->getAudioPortsConfigCount();
}

static bool CLAP_ABI clap_plugin_audio_ports_config_get(const clap_plugin_t* const plugin,
                                                        const uint32_t index,
                                                        clap_audio_ports_config_t* const config)
{
    const PluginCLAP* const instance = static_cast<const PluginCLAP*>(plugin->plugin_data);
    return instance->getAudioPortsConfig(index, config);
}

static bool CLAP_ABI clap_plugin_audio_ports_config_select(const clap_plugin_t* const plugin,
                                                           const clap_id configId)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->selectAudioPortsConfig(configId);
}

static const clap_plugin_audio_ports_config_t clap_plugin_audio_ports_config = {
    clap_plugin_audio_ports_config_count,
    clap_plugin_audio_ports_config_get,
    clap_plugin_audio_ports_config_select
};
#endif

// --------------------------------------------------------------------------------------------------------------------
// plugin note ports

#if DAF_PLUGIN_WANT_MIDI_INPUT+DAF_PLUGIN_WANT_MIDI_OUTPUT != 0
static uint32_t CLAP_ABI clap_plugin_note_ports_count(const clap_plugin_t*, const bool is_input)
{
    return (is_input ? DAF_PLUGIN_WANT_MIDI_INPUT : DAF_PLUGIN_WANT_MIDI_OUTPUT) != 0 ? 1 : 0;
}

static bool CLAP_ABI clap_plugin_note_ports_get(const clap_plugin_t*, uint32_t,
                                                const bool is_input, clap_note_port_info_t* const info)
{
    if (is_input)
    {
      #if DAF_PLUGIN_WANT_MIDI_INPUT
        info->id = 0;
       #if DAF_PLUGIN_WANT_MIDI_AS_MPE
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI_MPE;
       #else
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
       #endif
        std::strcpy(info->name, "Event/MIDI Input");
        return true;
      #endif
    }
    else
    {
      #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        info->id = 0;
       #if DAF_PLUGIN_WANT_MIDI_AS_MPE
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI_MPE;
       #else
        info->supported_dialects = CLAP_NOTE_DIALECT_MIDI;
        info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
       #endif
        std::strcpy(info->name, "Event/MIDI Output");
        return true;
      #endif
    }

    return false;
}

static const clap_plugin_note_ports_t clap_plugin_note_ports = {
    clap_plugin_note_ports_count,
    clap_plugin_note_ports_get
};
#endif

// --------------------------------------------------------------------------------------------------------------------
// plugin parameters

static uint32_t CLAP_ABI clap_plugin_params_count(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->getParameterCount();
}

static bool CLAP_ABI clap_plugin_params_get_info(const clap_plugin_t* const plugin, const uint32_t index, clap_param_info_t* const info)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->getParameterInfo(index, info);
}

static bool CLAP_ABI clap_plugin_params_get_value(const clap_plugin_t* const plugin, const clap_id param_id, double* const value)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->getParameterValue(param_id, value);
}

static bool CLAP_ABI clap_plugin_params_value_to_text(const clap_plugin_t* plugin, const clap_id param_id, const double value, char* const display, const uint32_t size)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->getParameterStringForValue(param_id, value, display, size);
}

static bool CLAP_ABI clap_plugin_params_text_to_value(const clap_plugin_t* plugin, const clap_id param_id, const char* const display, double* const value)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->getParameterValueForString(param_id, display, value);
}

static void CLAP_ABI clap_plugin_params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t* out)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->flushParametersFromHost(in, out);
}

static const clap_plugin_params_t clap_plugin_params = {
    clap_plugin_params_count,
    clap_plugin_params_get_info,
    clap_plugin_params_get_value,
    clap_plugin_params_value_to_text,
    clap_plugin_params_text_to_value,
    clap_plugin_params_flush
};

#if DAF_PLUGIN_WANT_LATENCY
// --------------------------------------------------------------------------------------------------------------------
// plugin latency

static uint32_t CLAP_ABI clap_plugin_latency_get(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->getLatency();
}

static const clap_plugin_latency_t clap_plugin_latency = {
    clap_plugin_latency_get
};
#endif

#if DAF_PLUGIN_WANT_TAIL
// --------------------------------------------------------------------------------------------------------------------
// plugin tail

static uint32_t CLAP_ABI clap_plugin_tail_get(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->getTail();
}

static const clap_plugin_tail_t clap_plugin_tail = {
    clap_plugin_tail_get
};
#endif

// --------------------------------------------------------------------------------------------------------------------
// plugin state

static bool CLAP_ABI clap_plugin_state_save(const clap_plugin_t* const plugin, const clap_ostream_t* const stream)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->stateSave(stream);
}

static bool CLAP_ABI clap_plugin_state_load(const clap_plugin_t* const plugin, const clap_istream_t* const stream)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->stateLoad(stream);
}

static const clap_plugin_state_t clap_plugin_state = {
    clap_plugin_state_save,
    clap_plugin_state_load
};

// --------------------------------------------------------------------------------------------------------------------
// plugin

static bool CLAP_ABI clap_plugin_init(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->init();
}

static void CLAP_ABI clap_plugin_destroy(const clap_plugin_t* const plugin)
{
    delete static_cast<PluginCLAP*>(plugin->plugin_data);
    std::free(const_cast<clap_plugin_t*>(plugin));
}

static bool CLAP_ABI clap_plugin_activate(const clap_plugin_t* const plugin,
                                          const double sample_rate,
                                          uint32_t,
                                          const uint32_t max_frames_count)
{
    d_nextBufferSize = max_frames_count;
    d_nextSampleRate = sample_rate;

    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    instance->activate(sample_rate, max_frames_count);
    return true;
}

static void CLAP_ABI clap_plugin_deactivate(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    instance->deactivate();
}

static bool CLAP_ABI clap_plugin_start_processing(const clap_plugin_t*)
{
    // nothing to do
    return true;
}

static void CLAP_ABI clap_plugin_stop_processing(const clap_plugin_t*)
{
    // nothing to do
}

static void CLAP_ABI clap_plugin_reset(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    instance->reset();
}

static clap_process_status CLAP_ABI clap_plugin_process(const clap_plugin_t* const plugin, const clap_process_t* const process)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    return instance->process(process) ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_ERROR;
}

static const void* CLAP_ABI clap_plugin_get_extension(const clap_plugin_t* const plugin, const char* const id)
{
   #if ! DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
    // unused
    (void)plugin;
   #endif

    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)
        return &clap_plugin_params;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &clap_plugin_state;
   #if DAF_PLUGIN_NUM_INPUTS+DAF_PLUGIN_NUM_OUTPUTS != 0
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &clap_plugin_audio_ports;
   #endif
   #if DAF_CLAP_HAS_AUDIO_PORTS_CONFIG
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS_CONFIG) == 0)
    {
        const PluginCLAP* const instance = static_cast<const PluginCLAP*>(plugin->plugin_data);
        return instance->supportsAudioPortsConfig() ? &clap_plugin_audio_ports_config : nullptr;
    }
   #endif
   #if DAF_PLUGIN_WANT_MIDI_INPUT+DAF_PLUGIN_WANT_MIDI_OUTPUT != 0
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
        return &clap_plugin_note_ports;
   #endif
   #if DAF_PLUGIN_WANT_LATENCY
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0)
        return &clap_plugin_latency;
   #endif
   #if DAF_PLUGIN_WANT_TAIL
    if (std::strcmp(id, CLAP_EXT_TAIL) == 0)
        return &clap_plugin_tail;
   #endif
  #if DAF_PLUGIN_HAS_UI
    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &clap_plugin_gui;
   #if DAF_CLAP_USING_HOST_TIMER
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
        return &clap_timer;
   #endif
  #endif
    return nullptr;
}

static void CLAP_ABI clap_plugin_on_main_thread(const clap_plugin_t* const plugin)
{
    PluginCLAP* const instance = static_cast<PluginCLAP*>(plugin->plugin_data);
    instance->onMainThread();
}

// --------------------------------------------------------------------------------------------------------------------
// plugin factory

static uint32_t CLAP_ABI clap_get_plugin_count(const clap_plugin_factory_t*)
{
    return 1;
}

static const clap_plugin_descriptor_t* CLAP_ABI clap_get_plugin_descriptor(const clap_plugin_factory_t*,
                                                                           const uint32_t index)
{
    DAF_SAFE_ASSERT_UINT_RETURN(index == 0, index, nullptr);

    static const char* features[] = {
       #ifdef DAF_PLUGIN_CLAP_FEATURES
        DAF_PLUGIN_CLAP_FEATURES,
       #elif DAF_PLUGIN_IS_SYNTH
        "instrument",
       #else
        "audio-effect",
       #endif
        nullptr
    };

    static const clap_plugin_descriptor_t descriptor = {
        CLAP_VERSION,
        DAF_PLUGIN_CLAP_ID,
        sPlugin->getName(),
        sPlugin->getMaker(),
        // TODO url
        "",
        // TODO manual url
        "",
        // TODO support url
        "",
        // TODO version string
        "",
        sPlugin->getDescription(),
        features
    };

    return &descriptor;
}

static const clap_plugin_t* CLAP_ABI clap_create_plugin(const clap_plugin_factory_t* const factory,
                                                        const clap_host_t* const host,
                                                        const char* const plugin_id)
{
    if (plugin_id == nullptr || std::strcmp(plugin_id, DAF_PLUGIN_CLAP_ID) != 0)
        return nullptr;

    clap_plugin_t* const pluginptr = static_cast<clap_plugin_t*>(std::malloc(sizeof(clap_plugin_t)));
    DAF_SAFE_ASSERT_RETURN(pluginptr != nullptr, nullptr);

    // default early values
    if (d_nextBufferSize == 0)
        d_nextBufferSize = 1024;
    if (d_nextSampleRate <= 0.0)
        d_nextSampleRate = 44100.0;

    d_nextCanRequestParameterValueChanges = true;

    const clap_plugin_t plugin = {
        clap_get_plugin_descriptor(factory, 0),
        new PluginCLAP(host),
        clap_plugin_init,
        clap_plugin_destroy,
        clap_plugin_activate,
        clap_plugin_deactivate,
        clap_plugin_start_processing,
        clap_plugin_stop_processing,
        clap_plugin_reset,
        clap_plugin_process,
        clap_plugin_get_extension,
        clap_plugin_on_main_thread
    };

    std::memcpy(pluginptr, &plugin, sizeof(clap_plugin_t));
    return pluginptr;
}

static const clap_plugin_factory_t clap_plugin_factory = {
    clap_get_plugin_count,
    clap_get_plugin_descriptor,
    clap_create_plugin
};

// --------------------------------------------------------------------------------------------------------------------
// plugin entry

static bool CLAP_ABI clap_plugin_entry_init(const char* const plugin_path)
{
    static String bundlePath;
    bundlePath = plugin_path;
    d_nextBundlePath = bundlePath.buffer();

    // init dummy plugin
    if (sPlugin == nullptr)
    {
        // set valid but dummy values
        d_nextBufferSize = 512;
        d_nextSampleRate = 44100.0;
        d_nextPluginIsDummy = true;
        d_nextCanRequestParameterValueChanges = true;

        // Create dummy plugin to get data from
        sPlugin = new PluginExporter(nullptr, nullptr, nullptr, nullptr);

        // unset
        d_nextBufferSize = 0;
        d_nextSampleRate = 0.0;
        d_nextPluginIsDummy = false;
        d_nextCanRequestParameterValueChanges = false;
    }

    return true;
}

static void CLAP_ABI clap_plugin_entry_deinit(void)
{
    sPlugin = nullptr;
}

static const void* CLAP_ABI clap_plugin_entry_get_factory(const char* const factory_id)
{
    if (std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &clap_plugin_factory;
    return nullptr;
}

static const clap_plugin_entry_t clap_plugin_entry = {
    CLAP_VERSION,
    clap_plugin_entry_init,
    clap_plugin_entry_deinit,
    clap_plugin_entry_get_factory
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

DAF_PLUGIN_EXPORT
const clap_plugin_entry_t clap_entry = DAF_NAMESPACE::clap_plugin_entry;

// --------------------------------------------------------------------------------------------------------------------
