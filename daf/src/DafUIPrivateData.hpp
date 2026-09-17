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

#ifndef DAF_UI_PRIVATE_DATA_HPP_INCLUDED
#define DAF_UI_PRIVATE_DATA_HPP_INCLUDED

#include "../DafUI.hpp"

#ifdef DAF_PLUGIN_TARGET_VST3
# include "DafPluginVST.hpp"
#endif

#include "../../dgl/src/ApplicationPrivateData.hpp"
#include "../../dgl/src/WindowPrivateData.hpp"
#include "../../dgl/src/pugl.hpp"

#if DAF_PLUGIN_WANT_STATE && DAF_UI_FILE_BROWSER
# include <map>
# include <string>
#endif

#if DAF_UI_USE_WEB_VIEW
# include "extra/WebView.hpp"
#endif

#if defined(DAF_PLUGIN_TARGET_JACK) || defined(DAF_PLUGIN_TARGET_DSSI)
# define DAF_UI_IS_STANDALONE 1
#else
# define DAF_UI_IS_STANDALONE 0
#endif

#if defined(DAF_PLUGIN_TARGET_AU)
# define DAF_UI_USES_SCHEDULED_REPAINTS 1
#else
# define DAF_UI_USES_SCHEDULED_REPAINTS 0
#endif

// A static-target UI is embedded by a host that links the plugin into its own process, and like
// CLAP and VST3 that host owns the editor's size.
#if defined(DAF_PLUGIN_TARGET_CLAP) || defined(DAF_PLUGIN_TARGET_VST3) || defined(DAF_PLUGIN_TARGET_STATIC)
# define DAF_UI_USES_SIZE_REQUEST 1
#else
# define DAF_UI_USES_SIZE_REQUEST 0
#endif

// NOTE: the AU override below clobbers the plugin's requested value, which is unrecoverable
//       afterwards. The AU wrapper still needs it to tell the host whether the plugin window may
//       be resized at all (see DafUIAU.mm), so stash it first. Expand eagerly, a lazy alias
//       would just resolve to the 0 below at its point of use.
#if defined(DAF_PLUGIN_TARGET_AU)
# if DAF_UI_USER_RESIZABLE
#  define DAF_UI_USER_RESIZABLE_AS_REQUESTED 1
# else
#  define DAF_UI_USER_RESIZABLE_AS_REQUESTED 0
# endif
#endif

#if defined(DAF_PLUGIN_TARGET_AU) || defined(DAF_PLUGIN_TARGET_VST2)
# undef DAF_UI_USER_RESIZABLE
# define DAF_UI_USER_RESIZABLE 0
#endif

START_NAMESPACE_DAF

/* define webview start */
#if defined(HAVE_X11) && defined(DAF_OS_LINUX) && DAF_UI_WEB_VIEW
# define DAF_UI_LINUX_WEBVIEW_START
int daf_webview_start(int argc, char* argv[]);
#endif

// --------------------------------------------------------------------------------------------------------------------
// Plugin Application, will set class name based on plugin details

class PluginApplication : public DGL_NAMESPACE::Application
{
public:
    explicit PluginApplication(const char* className, const Application::Type type)
        : DGL_NAMESPACE::Application(DAF_UI_IS_STANDALONE, type)
    {
       #if defined(__MOD_DEVICES__) || !defined(__EMSCRIPTEN__)
        if (className == nullptr)
        {
            className = (
               #ifdef DAF_PLUGIN_BRAND
                DAF_PLUGIN_BRAND
               #else
                DAF_MACRO_AS_STRING(DAF_NAMESPACE)
               #endif
                "-" DAF_PLUGIN_NAME
            );
        }
        setClassName(className);
       #else
        // unused
        (void)className;
       #endif
    }

    void triggerIdleCallbacks()
    {
        pData->triggerIdleCallbacks();
    }

    void repaintIfNeeeded()
    {
        pData->repaintIfNeeeded();
    }

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginApplication)
};

// --------------------------------------------------------------------------------------------------------------------
// Plugin Window, will pass some Window events to UI

class PluginWindow : public DGL_NAMESPACE::Window
{
    UI* const ui;
    bool initializing;

public:
    explicit PluginWindow(UI* const uiPtr,
                          PluginApplication& app,
                          const uintptr_t parentWindowHandle,
                          const uint width,
                          const uint height,
                          const double scaleFactor)
        : Window(app, parentWindowHandle, width, height, scaleFactor,
                 DAF_UI_USER_RESIZABLE,
                 DAF_UI_USES_SCHEDULED_REPAINTS,
                 DAF_UI_USES_SIZE_REQUEST,
                 false),
          ui(uiPtr),
          initializing(true)
    {
        if (pData->view == nullptr)
            return;

        // this is called just before creating UI, ensuring proper context to it
        if (pData->initPost())
        {
            puglBackendEnter(pData->view);
            pData->createContextIfNeeded();
        }
    }

    ~PluginWindow() override
    {
        if (pData->view != nullptr)
            puglBackendLeave(pData->view);
    }

    // called after creating UI, restoring proper context
    void leaveContext()
    {
        if (pData->view == nullptr)
            return;

        initializing = false;
        puglBackendLeave(pData->view);
    }

    /* Called once the UI is fully constructed and no longer initializing, to hand the widget
     * the size the window already has.
     *
     * This window is realized in its own constructor, which runs as part of constructing the
     * UI, so where realizing a window is synchronous, as it is on Windows, the configure event
     * that follows finds an empty top-level widget list and its size is dropped. An embedded
     * view that the host never resizes afterwards gets no other configure, so the widget keeps
     * the size the UI constructor asked for while the window, and with it the framebuffer, is
     * at the scale factor multiplied size. Drawing code takes its clip and viewport region from
     * the widget, so the plugin ends up painting into a fraction of its own window.
     *
     * Do here what that configure would have done. Deliberately not done any earlier: only
     * from this point on does onResize reach an override in the concrete UI class, which is
     * format independent, and only from this point on does UI::onResize hand the new size to
     * the host on the formats that compile that branch in, VST2 and the other non size
     * request ones. Size request formats do not need it, there the host reads the window size
     * through UIExporter::getWidth and getHeight, which is correct throughout.
     *
     * Where the configure is merely deferred rather than lost, X11 being the case in point
     * since realizing a view does not dispatch one synchronously, this does the same work the
     * configure would have done, only earlier; the configure that follows then finds the sizes
     * equal and changes nothing. It is a no-op whenever a configure did already land, and at
     * scale 1.0 everywhere.
     */
    void reconcileWidgetSize()
    {
        if (pData->view == nullptr)
            return;

        /* Automatic scaling is reconciled by the configure event on purpose: setGeometryConstraints
         * turns the mode on but leaves autoScaleFactor alone, and only onPuglConfigure derives it
         * from the window and the minimum size. Reconciling here would divide by a factor of 1.0
         * that is not yet the real one, hand the widget a size the next configure has to correct,
         * and so report two resizes with a wrong one in between. Leave the mode alone; its own
         * no-configure hole on Windows is a separate pre-existing defect, and half fixing it with
         * a stale factor is worse than not touching it.
         */
        if (pData->autoScaling)
            return;

        const DGL_NAMESPACE::Size<uint> windowSize(getSize());

        if (! windowSize.isValid())
            return;

        const double autoScaleFactor = pData->autoScaleFactor;
        const DGL_NAMESPACE::Size<uint> widgetSize(
            d_roundToUnsignedInt(windowSize.getWidth() / autoScaleFactor),
            d_roundToUnsignedInt(windowSize.getHeight() / autoScaleFactor));

        if (widgetSize == ui->getSize())
            return;

        puglBackendEnter(pData->view);
        static_cast<DGL_NAMESPACE::Widget*>(ui)->setSize(widgetSize);
        puglBackendLeave(pData->view);
    }

    // used for temporary windows (VST/CLAP get size without active/visible view)
    void setIgnoreIdleCallbacks(const bool ignore = true)
    {
        pData->ignoreIdleCallbacks = ignore;
    }

    // called right before deleting UI, ensuring correct context
    void enterContextForDeletion()
    {
        if (pData->view != nullptr)
            puglBackendEnter(pData->view);
    }

    // NOTE: AU has no size request/negotiation, but hosts do resize the parent view on their own
   #if DAF_UI_USES_SIZE_REQUEST || defined(DAF_PLUGIN_TARGET_AU)
    void setSizeFromHost(const uint width, const uint height)
    {
        puglSetSizeAndDefault(pData->view, width, height);
    }
   #endif

    std::vector<DGL_NAMESPACE::ClipboardDataOffer> getClipboardDataOfferTypes()
    {
        return Window::getClipboardDataOfferTypes();
    }

protected:
    uint32_t onClipboardDataOffer() override
    {
        DAF_SAFE_ASSERT_RETURN(ui != nullptr, 0);

        if (initializing)
            return 0;

        return ui->uiClipboardDataOffer();
    }

    void onFocus(const bool focus, const DGL_NAMESPACE::CrossingMode mode) override
    {
        DAF_SAFE_ASSERT_RETURN(ui != nullptr,);

        if (initializing)
            return;

        ui->uiFocus(focus, mode);
    }

    void onReshape(const uint width, const uint height) override
    {
        DAF_SAFE_ASSERT_RETURN(ui != nullptr,);

        /* No initializing guard here, unlike the handlers below. Those are reachable from a
         * configure event, which a window realized inside the UI constructor can receive while
         * that constructor is still running, with no override in place to dispatch to yet. This
         * one is dispatched from the expose handler instead, and an expose needs a realized,
         * mapped window and a running event loop: the size a configure recorded during
         * construction is delivered here afterwards, once the UI is whole.
         */
        ui->uiReshape(width, height);
    }

    void onScaleFactorChanged(const double scaleFactor) override
    {
        DAF_SAFE_ASSERT_RETURN(ui != nullptr,);

        if (initializing)
            return;

        ui->uiScaleFactorChanged(scaleFactor);
    }

   #if DAF_UI_FILE_BROWSER
    void onFileSelected(const char* filename) override;
   #endif

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginWindow)
};

// --------------------------------------------------------------------------------------------------------------------
// UI callbacks

typedef void (*editParamFunc)   (void* ptr, uint32_t rindex, bool started);
typedef void (*setParamFunc)    (void* ptr, uint32_t rindex, float value);
typedef void (*setStateFunc)    (void* ptr, const char* key, const char* value);
typedef void (*sendNoteFunc)    (void* ptr, uint8_t channel, uint8_t note, uint8_t velo);
typedef void (*setSizeFunc)     (void* ptr, uint width, uint height);
typedef bool (*fileRequestFunc) (void* ptr, const char* key);

// --------------------------------------------------------------------------------------------------------------------
// UI private data

struct UI::PrivateData {
    // DGL
    PluginApplication app;
    ScopedPointer<PluginWindow> window;
   #if DAF_UI_USE_WEB_VIEW
    WebViewHandle webview;
   #endif

    // DSP
    double   sampleRate;
    uint32_t parameterOffset;
    void*    dspPtr;

    // UI
    uint bgColor;
    uint fgColor;
    double scaleFactor;
    uintptr_t winId;
   #if DAF_PLUGIN_WANT_STATE && DAF_UI_FILE_BROWSER
    char* uiStateFileKeyRequest;
    std::map<std::string,std::string> lastUsedDirnames;
   #endif
    char* bundlePath;

    // Ignore initial resize events while initializing
    bool initializing;

    // Callbacks
    void*           callbacksPtr;
    editParamFunc   editParamCallbackFunc;
    setParamFunc    setParamCallbackFunc;
    setStateFunc    setStateCallbackFunc;
    sendNoteFunc    sendNoteCallbackFunc;
    setSizeFunc     setSizeCallbackFunc;
    fileRequestFunc fileRequestCallbackFunc;

    PrivateData(const char* const appClassName, const DGL_NAMESPACE::Application::Type appType) noexcept
        : app(appClassName, appType),
          window(nullptr),
         #if DAF_UI_USE_WEB_VIEW
          webview(nullptr),
         #endif
          sampleRate(0),
          parameterOffset(0),
          dspPtr(nullptr),
          bgColor(0),
          fgColor(0xffffffff),
          scaleFactor(1.0),
          winId(0),
         #if DAF_PLUGIN_WANT_STATE && DAF_UI_FILE_BROWSER
          uiStateFileKeyRequest(nullptr),
         #endif
          bundlePath(nullptr),
          initializing(true),
          callbacksPtr(nullptr),
          editParamCallbackFunc(nullptr),
          setParamCallbackFunc(nullptr),
          setStateCallbackFunc(nullptr),
          sendNoteCallbackFunc(nullptr),
          setSizeCallbackFunc(nullptr),
          fileRequestCallbackFunc(nullptr)
    {
      #if defined(DAF_PLUGIN_TARGET_DSSI) || defined(DAF_PLUGIN_TARGET_LV2)
        parameterOffset += DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS;
       #if DAF_PLUGIN_WANT_LATENCY
        parameterOffset += 1;
       #endif
      #endif

      #ifdef DAF_PLUGIN_TARGET_LV2
       #if (DAF_PLUGIN_WANT_MIDI_INPUT || DAF_PLUGIN_WANT_TIMEPOS || DAF_PLUGIN_WANT_STATE)
        parameterOffset += 1;
       #endif
       #if (DAF_PLUGIN_WANT_MIDI_OUTPUT || DAF_PLUGIN_WANT_STATE)
        parameterOffset += 1;
       #endif
      #endif

       #ifdef DAF_PLUGIN_TARGET_VST3
        parameterOffset += kVst3InternalParameterCount;
       #endif
    }

    ~PrivateData() noexcept
    {
       #if DAF_PLUGIN_WANT_STATE && DAF_UI_FILE_BROWSER
        std::free(uiStateFileKeyRequest);
       #endif
        std::free(bundlePath);
    }

    void editParamCallback(const uint32_t rindex, const bool started)
    {
        if (editParamCallbackFunc != nullptr)
            editParamCallbackFunc(callbacksPtr, rindex, started);
    }

    void setParamCallback(const uint32_t rindex, const float value)
    {
        if (setParamCallbackFunc != nullptr)
            setParamCallbackFunc(callbacksPtr, rindex, value);
    }

    void setStateCallback(const char* const key, const char* const value)
    {
        DAF_SAFE_ASSERT_RETURN(key != nullptr && key[0] != '\0',);
        DAF_SAFE_ASSERT_RETURN(value != nullptr,);

        if (setStateCallbackFunc != nullptr)
            setStateCallbackFunc(callbacksPtr, key, value);
    }

    void sendNoteCallback(const uint8_t channel, const uint8_t note, const uint8_t velocity)
    {
        if (sendNoteCallbackFunc != nullptr)
            sendNoteCallbackFunc(callbacksPtr, channel, note, velocity);
    }

    void setSizeCallback(const uint width, const uint height)
    {
        DAF_SAFE_ASSERT_RETURN(width != 0 && height != 0,);

        if (setSizeCallbackFunc != nullptr)
            setSizeCallbackFunc(callbacksPtr, width, height);
    }

    // implemented below, after PluginWindow
    bool fileRequestCallback(const char* key);

    static UI::PrivateData* s_nextPrivateData;
    static PluginWindow& createNextWindow(UI* ui, uint width, uint height);
   #if DAF_UI_USE_WEB_VIEW
    static void webViewMessageCallback(void* arg, char* msg);
   #endif
};

// --------------------------------------------------------------------------------------------------------------------
// UI private data fileRequestCallback, which requires PluginWindow definitions

inline bool UI::PrivateData::fileRequestCallback(const char* const key)
{
    if (fileRequestCallbackFunc != nullptr)
        return fileRequestCallbackFunc(callbacksPtr, key);

   #if DAF_PLUGIN_WANT_STATE && DAF_UI_FILE_BROWSER
    std::free(uiStateFileKeyRequest);
    uiStateFileKeyRequest = strdup(key);
    DAF_SAFE_ASSERT_RETURN(uiStateFileKeyRequest != nullptr, false);

    char title[0xff];
    snprintf(title, sizeof(title)-1u, DAF_PLUGIN_NAME ": %s", key);
    title[sizeof(title)-1u] = '\0';

    DGL_NAMESPACE::FileBrowserOptions opts;
    opts.title = title;
    if  (lastUsedDirnames.count(key))
        opts.startDir = lastUsedDirnames[key].c_str();
    return window->openFileBrowser(opts);
   #endif

    return false;
}

// --------------------------------------------------------------------------------------------------------------------
// PluginWindow onFileSelected that require UI::PrivateData definitions

#if DAF_UI_FILE_BROWSER
inline void PluginWindow::onFileSelected(const char* const filename)
{
    DAF_SAFE_ASSERT_RETURN(ui != nullptr,);

    if (initializing)
        return;

   #if DAF_PLUGIN_WANT_STATE
    if (char* const key = ui->uiData->uiStateFileKeyRequest)
    {
        ui->uiData->uiStateFileKeyRequest = nullptr;
        if (filename != nullptr)
        {
            // notify DSP
            ui->setState(key, filename);

            // notify UI
            ui->stateChanged(key, filename);

            // save dirname for next time
            if (const char* const lastsep = std::strrchr(filename, DAF_OS_SEP))
                ui->uiData->lastUsedDirnames[key] = std::string(filename, lastsep-filename);
        }
        std::free(key);
        return;
    }
   #endif

    puglBackendEnter(pData->view);
    ui->uiFileBrowserSelected(filename);
    puglBackendLeave(pData->view);
}
#endif

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_UI_PRIVATE_DATA_HPP_INCLUDED
