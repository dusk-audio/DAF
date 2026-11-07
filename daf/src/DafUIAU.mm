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

#include "DafUIInternal.hpp"
#include "DafPluginAUUILink.hpp"

#define Point AudioUnitPoint
#define Size AudioUnitSize

#include <AudioUnit/AudioUnit.h>
#include <AudioUnit/AUCocoaUIView.h>
#include <Cocoa/Cocoa.h>

#undef Point
#undef Size

#ifndef DAF_PLUGIN_BRAND_ID
# error DAF_PLUGIN_BRAND_ID undefined!
#endif

#ifndef DAF_PLUGIN_UNIQUE_ID
# error DAF_PLUGIN_UNIQUE_ID undefined!
#endif

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

#if ! DAF_PLUGIN_WANT_STATE
static constexpr const setStateFunc setStateCallback = nullptr;
#endif
#if ! DAF_PLUGIN_WANT_MIDI_INPUT
static constexpr const sendNoteFunc sendNoteCallback = nullptr;
#endif

// unsupported in AU
static constexpr const fileRequestFunc fileRequestCallback = nullptr;

// --------------------------------------------------------------------------------------------------------------------
// Static data, see DafPlugin.cpp

extern double d_nextSampleRate;
extern const char* d_nextBundlePath;

// --------------------------------------------------------------------------------------------------------------------

class DAF_UI_AU
{
public:
    DAF_UI_AU(const AudioUnit component,
              NSView* const view,
              const double sampleRate,
              void* const instancePointer)
        : fComponent(component),
          fLink(nullptr),
          fParentView(view),
          fTimerRef(nullptr),
          fFrameObserver(nullptr),
          fInFrameSizeChange(false),
          fUI(this,
              reinterpret_cast<uintptr_t>(view),
              sampleRate,
              editParameterCallback,
              setParameterCallback,
              setStateCallback,
              sendNoteCallback,
              setSizeCallback,
              fileRequestCallback,
              d_nextBundlePath,
              instancePointer)
    {
        // the tie to the instance, which detaches this UI before its plugin goes away (see DafPluginAUUILink.hpp)
        {
            AUUILink* link = nullptr;
            UInt32 dataSize = sizeof(AUUILink*);
            if (AudioUnitGetProperty(component, 'DPFu', kAudioUnitScope_Global, 0, &link, &dataSize) == noErr
                && dataSize == sizeof(AUUILink*) && link != nullptr)
            {
                if (link->attach(detachCallback, this))
                    fLink = link;
                else
                {
                    // the instance went away while this was being made: detached from the start
                    link->release();
                    unitDisposed();
                    return;
                }
            }
        }

       #if DAF_PLUGIN_WANT_STATE
        // create state keys
        {
            CFArrayRef keysRef = nullptr;
            UInt32 dataSize = sizeof(CFArrayRef);
            if (AudioUnitGetProperty(component, 'DPFl', kAudioUnitScope_Global, 0, &keysRef, &dataSize) == noErr
                && dataSize == sizeof(CFArrayRef))
            {
                const CFIndex numStates = CFArrayGetCount(keysRef);
                char* key = nullptr;
                CFIndex keyLen = -1;

                fStateKeys.resize(numStates);

                for (CFIndex i=0; i<numStates; ++i)
                {
                    const CFStringRef keyRef = static_cast<CFStringRef>(CFArrayGetValueAtIndex(keysRef, i));
                    DAF_SAFE_ASSERT_BREAK(CFGetTypeID(keyRef) == CFStringGetTypeID());

                    const CFIndex keyRefLen = CFStringGetLength(keyRef);
                    if (keyLen < keyRefLen)
                    {
                        keyLen = keyRefLen;
                        key = static_cast<char*>(std::realloc(key, keyLen + 1));
                    }
                    DAF_SAFE_ASSERT_BREAK(CFStringGetCString(keyRef, key, keyLen + 1, kCFStringEncodingASCII));

                    fStateKeys[i] = key;
                }

                CFRelease(keysRef);
                std::free(key);
            }
        }
       #endif

        // setup idle timer
        constexpr const CFTimeInterval interval = 60 * 0.0001;

        CFRunLoopTimerContext context = {};
        context.info = this;
        fTimerRef = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + interval, interval, 0, 0,
                                         _idleCallback, &context);
        DAF_SAFE_ASSERT_RETURN(fTimerRef != nullptr,);

        CFRunLoopAddTimer(CFRunLoopGetCurrent(), fTimerRef, kCFRunLoopCommonModes);

        // setup property listeners
        AudioUnitAddPropertyListener(component, kAudioUnitProperty_SampleRate, auPropertyChangedCallback, this);
        AudioUnitAddPropertyListener(component, 'DPFp', auPropertyChangedCallback, this);
       #if DAF_PLUGIN_WANT_PROGRAMS
        AudioUnitAddPropertyListener(component, 'DPFo', auPropertyChangedCallback, this);
       #endif
       #if DAF_PLUGIN_WANT_STATE
        AudioUnitAddPropertyListener(component, 'DPFs', auPropertyChangedCallback, this);
       #endif

        // setup parent view frame listener
        //
        // AU has no size negotiation of its own, hosts simply resize the view we hand them.
        // Logic Pro does this right after creating the UI, restoring the plugin window size it
        // saved with the session. Size otherwise only ever flows plugin -> host in this wrapper,
        // so without listening for this the UI would stay at its creation size while the rest of
        // the host window is left empty.
        [fParentView setPostsFrameChangedNotifications:YES];

        NSNotificationCenter* const notificationCenter = [NSNotificationCenter defaultCenter];

        fFrameObserver = [notificationCenter addObserverForName:NSViewFrameDidChangeNotification
                                                         object:fParentView
                                                          queue:nil
                                                     usingBlock:^(NSNotification*)
        {
            parentViewFrameChanged();
        }];

        // the returned token is autoreleased, keep it alive until we remove the observer again
        [fFrameObserver retain];
    }

    ~DAF_UI_AU()
    {
        // stop the frame listener first, its block calls back into us and into fUI
        if (fFrameObserver != nullptr)
        {
            [[NSNotificationCenter defaultCenter] removeObserver:fFrameObserver];
            [fFrameObserver release];
            fFrameObserver = nullptr;
        }

        // off the instance's list: it does not detach this UI from here on, and a detach in progress on the thread
        // disposing it has ended
        if (fLink != nullptr)
        {
            fLink->remove(this);
            fLink->release();
            fLink = nullptr;
        }

        // the listeners died with the instance if it is gone, otherwise they are taken off it
        if (const AudioUnit component = fComponent.load())
        {
            AudioUnitRemovePropertyListenerWithUserData(component, kAudioUnitProperty_SampleRate, auPropertyChangedCallback, this);
            AudioUnitRemovePropertyListenerWithUserData(component, 'DPFp', auPropertyChangedCallback, this);
           #if DAF_PLUGIN_WANT_PROGRAMS
            AudioUnitRemovePropertyListenerWithUserData(component, 'DPFo', auPropertyChangedCallback, this);
           #endif
           #if DAF_PLUGIN_WANT_STATE
            AudioUnitRemovePropertyListenerWithUserData(component, 'DPFs', auPropertyChangedCallback, this);
           #endif
        }

        if (fTimerRef != nullptr)
        {
            CFRunLoopTimerInvalidate(fTimerRef);
            CFRelease(fTimerRef);
        }
    }

    void postSetup()
    {
        resizeParentView(fUI.getWidth(), fUI.getHeight());

        [fParentView setHidden:NO];
    }

    // called by the parent view when its backing scale factor changes, see COCOA_VIEW_CLASS_NAME
    //
    // pugl has no viewDidChangeBackingProperties handler of its own (grep mac.m), so nothing
    // dispatches a configure event when the backing scale changes under an already realized view.
    // The point size of every view in the hierarchy survives such a change untouched, so the frame
    // listener does not fire either, yet the pixel size the UI has to render at just halved or
    // doubled. Push the unchanged point size back through the host resize path to re-derive it.
    //
    // Two cases reach this in practice:
    //  - the host window migrating between a 1x and a 2x display
    //  - the view being added to its window, which is the first moment the real display is known.
    //    Until then both this wrapper and pugl fall back to [NSScreen mainScreen], which is not
    //    necessarily the screen the plugin window ends up on.
    void backingScaleChanged()
    {
        parentViewFrameChanged();
    }

private:
    // the instance this UI was made for, nullptr once it is disposed: nothing is called on it after that
    std::atomic<AudioUnit> fComponent;
    // shared with the instance, see DafPluginAUUILink.hpp
    AUUILink* fLink;
    NSView* const fParentView;
    CFRunLoopTimerRef fTimerRef;

    // parent view frame listener, see constructor
    id fFrameObserver;
    bool fInFrameSizeChange;

    UIExporter fUI;

   #if DAF_PLUGIN_WANT_STATE
    std::vector<String> fStateKeys;
   #endif

    // ----------------------------------------------------------------------------------------------------------------
    // Pixel <-> point conversion
    //
    // DGL and pugl work in backing pixels, AppKit view frames are in points. On a 2x display the UI
    // reports twice the size of the frame the host has to be given.
    //
    // NOTE: fUI.getScaleFactor() must not be used for this. It is informational state captured
    //       while the DGL window is initialized, so it can be based on the main screen and does
    //       not track a later move to a display with a different backing scale.

    // mirrors pugl's viewScreen() fallback exactly (see dgl/src/pugl-upstream/src/mac.m), so that
    // both layers always agree on the conversion, including before the view is added to a window
    double backingScaleFactor() const
    {
        NSWindow* const window = [fParentView window];
        const double scaleFactor = window != nil ? [[window screen] backingScaleFactor]
                                                 : [[NSScreen mainScreen] backingScaleFactor];

        return scaleFactor > 0.0 ? scaleFactor : 1.0;
    }

    // resize the host-owned parent view to match the pugl view, plugin -> host
    void resizeParentView(const uint width, const uint height)
    {
        // pugl keeps its own wrapper view at the point size matching its pixel size, so copy that
        // frame outright and leave pugl as the single source of truth.
        //
        // The ordering works out for both callers: puglRealize() sizes the wrapper view before
        // postSetup() runs, and puglSetWindowSize() resizes it *before* dispatching the configure
        // event that ends up here as setSize() (see dispatchCurrentChildViewConfiguration in mac.m,
        // which derives the very pixel size we are given from that same wrapper frame).
        NSSize sizePt = NSMakeSize(0.0, 0.0);

        if (NSView* const wrapperView = reinterpret_cast<NSView*>(fUI.getNativeWindowHandle()))
            sizePt = [wrapperView frame].size;

        // fallback in case pugl has no wrapper view for us, keeping the conversion rule identical
        if (sizePt.width <= 0.0 || sizePt.height <= 0.0)
        {
            const double scaleFactor = backingScaleFactor();
            sizePt = NSMakeSize(width / scaleFactor, height / scaleFactor);
        }

        const bool wasInFrameSizeChange = fInFrameSizeChange;
        fInFrameSizeChange = true;
        [fParentView setFrameSize:sizePt];
        fInFrameSizeChange = wasInFrameSizeChange;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Idle setup

    void idleCallback()
    {
        // under the link's lock: an instance disposed on another thread waits for this tick to end
        std::unique_lock<std::recursive_mutex> guard;
        if (fLink != nullptr)
            guard = std::unique_lock<std::recursive_mutex>(fLink->lock());

        if (fComponent.load() == nullptr)
            return;

        fUI.idleFromNativeIdle();
    }

    static void _idleCallback(CFRunLoopTimerRef, void* const info)
    {
        static_cast<DAF_UI_AU*>(info)->idleCallback();
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Instance disposed

    // Called by the link, on the thread disposing the instance and under the link's lock, so no idle tick is running.
    // The view stays for as long as the host or AppKit keeps it: the idle timer stops, and the plugin and the handle
    // are dropped, so that nothing of either is read or called again.
    void unitDisposed()
    {
        fComponent.store(nullptr);
        fUI.clearPluginInstancePointer();

        // any thread: off every run loop it is on
        if (fTimerRef != nullptr)
            CFRunLoopTimerInvalidate(fTimerRef);
    }

    static void detachCallback(void* const ptr)
    {
        static_cast<DAF_UI_AU*>(ptr)->unitDisposed();
    }

    // ----------------------------------------------------------------------------------------------------------------
    // AU callbacks

    void auSampleRateChanged(const AudioUnit component, const AudioUnitScope scope)
    {
        Float64 sampleRate = 0;
        UInt32 dataSize = sizeof(Float64);
        if (AudioUnitGetProperty(component, kAudioUnitProperty_SampleRate, scope, 0, &sampleRate, &dataSize) == noErr
            && dataSize == sizeof(Float64))
        {
            fUI.setSampleRate(sampleRate, true);
        }
    }

    void auParameterChanged(const AudioUnit component, const AudioUnitElement elem)
    {
        float value = 0;
        UInt32 dataSize = sizeof(float);
        if (AudioUnitGetProperty(component, 'DPFp', kAudioUnitScope_Global, elem, &value, &dataSize) == noErr
            && dataSize == sizeof(float))
        {
            fUI.parameterChanged(elem, value);
        }
    }

   #if DAF_PLUGIN_WANT_PROGRAMS
    void auProgramChanged(const AudioUnit component)
    {
        uint32_t program = 0;
        UInt32 dataSize = sizeof(uint32_t);
        if (AudioUnitGetProperty(component, 'DPFo', kAudioUnitScope_Global, 0, &program, &dataSize) == noErr
            && dataSize == sizeof(uint32_t))
        {
            fUI.programLoaded(program);
        }
    }
   #endif

   #if DAF_PLUGIN_WANT_STATE
    void auStateChanged(const AudioUnit component, const AudioUnitElement elem)
    {
        DAF_SAFE_ASSERT_RETURN(elem < fStateKeys.size(),);

        CFStringRef valueRef = nullptr;
        UInt32 dataSize = sizeof(valueRef);
        if (AudioUnitGetProperty(component, 'DPFs', kAudioUnitScope_Global, elem, &valueRef, &dataSize) == noErr
            && dataSize == sizeof(CFStringRef)
            && valueRef != nullptr
            && CFGetTypeID(valueRef) == CFStringGetTypeID())
        {
            const CFIndex valueLen = CFStringGetLength(valueRef);
            char* const value = static_cast<char*>(std::malloc(valueLen + 1));
            DAF_SAFE_ASSERT_RETURN(value != nullptr,);
            DAF_SAFE_ASSERT_RETURN(CFStringGetCString(valueRef, value, valueLen + 1, kCFStringEncodingUTF8),);

            fUI.stateChanged(fStateKeys[elem], value);

            CFRelease(valueRef);
            std::free(value);
        }
    }
   #endif

    static void auPropertyChangedCallback(void* const userData,
                                          const AudioUnit component,
                                          const AudioUnitPropertyID prop,
                                          const AudioUnitScope scope,
                                          const AudioUnitElement elem)
    {
        DAF_UI_AU* const self = static_cast<DAF_UI_AU*>(userData);

        DAF_SAFE_ASSERT_RETURN(self != nullptr,);

        // detached: a notification on its way when the instance was disposed (listeners are told from a copy)
        const AudioUnit unit = self->fComponent.load();
        if (unit == nullptr)
            return;
        DAF_SAFE_ASSERT_RETURN(unit == component,);

        switch (prop)
        {
        case kAudioUnitProperty_SampleRate:
            DAF_SAFE_ASSERT_UINT_RETURN(scope == kAudioUnitScope_Input || scope == kAudioUnitScope_Output, scope,);
            self->auSampleRateChanged(component, scope);
            break;
        case 'DPFp':
            DAF_SAFE_ASSERT_UINT_RETURN(scope == kAudioUnitScope_Global, scope,);
            self->auParameterChanged(component, elem);
            break;
       #if DAF_PLUGIN_WANT_PROGRAMS
        case 'DPFo':
            DAF_SAFE_ASSERT_UINT_RETURN(scope == kAudioUnitScope_Global, scope,);
            self->auProgramChanged(component);
            break;
       #endif
       #if DAF_PLUGIN_WANT_STATE
        case 'DPFs':
            DAF_SAFE_ASSERT_UINT_RETURN(scope == kAudioUnitScope_Global, scope,);
            self->auStateChanged(component, elem);
            break;
       #endif
        }
    }

    // ----------------------------------------------------------------------------------------------------------------
    // DAF callbacks

    void editParameter(const uint32_t rindex, const bool started) const
    {
        const AudioUnit component = fComponent.load();
        if (component == nullptr)
            return;

        const uint8_t flag = started ? 1 : 2;
        AudioUnitSetProperty(component, 'DPFe', kAudioUnitScope_Global, rindex, &flag, sizeof(uint8_t));

        if (! started)
        {
            const uint8_t cancel = 0;
            AudioUnitSetProperty(component, 'DPFe', kAudioUnitScope_Global, rindex, &cancel, sizeof(uint8_t));
        }
    }

    static void editParameterCallback(void* const ptr, const uint32_t rindex, const bool started)
    {
        static_cast<DAF_UI_AU*>(ptr)->editParameter(rindex, started);
    }

    // ----------------------------------------------------------------------------------------------------------------

    void setParameter(const uint32_t rindex, const float value)
    {
        if (const AudioUnit component = fComponent.load())
            AudioUnitSetProperty(component, 'DPFp', kAudioUnitScope_Global, rindex, &value, sizeof(float));
    }

    static void setParameterCallback(void* const ptr, const uint32_t rindex, const float value)
    {
        static_cast<DAF_UI_AU*>(ptr)->setParameter(rindex, value);
    }

    // ----------------------------------------------------------------------------------------------------------------

   #if DAF_PLUGIN_WANT_STATE
    void setState(const char* const key, const char* const value)
    {
        const AudioUnit component = fComponent.load();
        if (component == nullptr)
            return;

        CFStringRef keyRef = CFStringCreateWithCString(nullptr, key, kCFStringEncodingASCII);
        CFStringRef valueRef = CFStringCreateWithCString(nullptr, value, kCFStringEncodingUTF8);

        if (const CFDictionaryRef dictRef = CFDictionaryCreate(nullptr,
                                                               reinterpret_cast<const void**>(&keyRef),
                                                               reinterpret_cast<const void**>(&valueRef),
                                                               1,
                                                               &kCFTypeDictionaryKeyCallBacks,
                                                               &kCFTypeDictionaryValueCallBacks))
        {
            AudioUnitSetProperty(component, 'DPFs', kAudioUnitScope_Global, 0, &dictRef, sizeof(dictRef));
            CFRelease(dictRef);
        }

        CFRelease(keyRef);
        CFRelease(valueRef);
    }

    static void setStateCallback(void* const ptr, const char* const key, const char* const value)
    {
        static_cast<DAF_UI_AU*>(ptr)->setState(key, value);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------

   #if DAF_PLUGIN_WANT_MIDI_INPUT
    void sendNote(const uint8_t channel, const uint8_t note, const uint8_t velocity)
    {
        const AudioUnit component = fComponent.load();
        if (component == nullptr)
            return;

        const uint8_t data[3] = { static_cast<uint8_t>((velocity != 0 ? 0x90 : 0x80) | channel), note, velocity };
        AudioUnitSetProperty(component, 'DPFn', kAudioUnitScope_Global, 0, data, sizeof(data));

        const uint8_t cancel[3] = { 0, 0, 0 };
        AudioUnitSetProperty(component, 'DPFn', kAudioUnitScope_Global, 0, cancel, sizeof(cancel));
    }

    static void sendNoteCallback(void* const ptr, const uint8_t channel, const uint8_t note, const uint8_t velocity)
    {
        static_cast<DAF_UI_AU*>(ptr)->sendNote(channel, note, velocity);
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------

    void setSize(const uint width, const uint height)
    {
        resizeParentView(width, height);
    }

    static void setSizeCallback(void* const ptr, const uint width, const uint height)
    {
        static_cast<DAF_UI_AU*>(ptr)->setSize(width, height);
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Host callbacks

    // called from the parent view frame listener, always on the main thread
    void parentViewFrameChanged()
    {
        // ignore the notifications caused by our own plugin -> host resizes
        if (fInFrameSizeChange)
            return;

        const NSSize sizePt = [fParentView frame].size;

        // hosts can momentarily set an empty frame while tearing the view down
        if (sizePt.width <= 0.0 || sizePt.height <= 0.0)
            return;

        const double scaleFactor = backingScaleFactor();
        const uint width = d_roundToUnsignedInt(sizePt.width * scaleFactor);
        const uint height = d_roundToUnsignedInt(sizePt.height * scaleFactor);

        // pt -> px and px -> pt do not round the same way, so never trust the flag alone
        if (width == fUI.getWidth() && height == fUI.getHeight())
            return;

        d_debug("host->plugin frame change %u %u", width, height);
        fUI.setWindowSizeFromHost(width, height);
    }
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

#define MACRO_NAME2(a, b, c, d, e, f) a ## b ## c ## d ## e ## f
#define MACRO_NAME(a, b, c, d, e, f) MACRO_NAME2(a, b, c, d, e, f)

// --------------------------------------------------------------------------------------------------------------------

#define COCOA_VIEW_CLASS_NAME \
    MACRO_NAME(CocoaView_, DAF_PLUGIN_AU_TYPE, _, DAF_PLUGIN_UNIQUE_ID, _, DAF_PLUGIN_BRAND_ID)

using DAF_NAMESPACE::DAF_UI_AU;
using DAF_NAMESPACE::d_nextSampleRate;

@interface COCOA_VIEW_CLASS_NAME : NSView
{
@public
    DAF_UI_AU* ui;
}
@end

@implementation COCOA_VIEW_CLASS_NAME

- (id) initWithPreferredSize:(NSSize)size
{
    ui = nullptr;
    self = [super initWithFrame: NSMakeRect(0, 0, size.width, size.height)];
    [self setHidden:YES];

   #if DAF_UI_USER_RESIZABLE_AS_REQUESTED
    // AU v2 has no resizability flag of its own, hosts key the plugin window resize affordance off
    // the autoresizing mask of the view we hand them. Logic Pro only shows a resize grip when this
    // view is both width and height sizable, and leaves it out entirely for NSViewNotSizable.
    // This just makes the host willing to resize us, the actual resizing is handled by the parent
    // view frame change observer in DAF_UI_AU, see NSViewFrameDidChangeNotification there.
    [self setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
   #endif

    return self;
}

- (BOOL) acceptsFirstResponder
{
  return YES;
}

- (void) viewDidChangeBackingProperties
{
    [super viewDidChangeBackingProperties];

    // fires when the host window moves between displays of different backing scale, and when this
    // view is moved into a window with a different one. pugl does not handle either on its own.
    if (ui != nullptr)
        ui->backingScaleChanged();
}

- (void) dealloc
{
    delete ui;
    ui = nullptr;

	[super dealloc];
}

- (BOOL) isFlipped
{
  return YES;
}

@end

// --------------------------------------------------------------------------------------------------------------------

#define COCOA_UI_CLASS_NAME \
    MACRO_NAME(CocoaAUView_, DAF_PLUGIN_AU_TYPE, _, DAF_PLUGIN_UNIQUE_ID, _, DAF_PLUGIN_BRAND_ID)

@interface COCOA_UI_CLASS_NAME : NSObject<AUCocoaUIBase>
{
    COCOA_VIEW_CLASS_NAME* view;
}
@end

@implementation COCOA_UI_CLASS_NAME

- (NSString*) description
{
    return @DAF_PLUGIN_NAME;
}

- (unsigned) interfaceVersion
{
    return 0;
}

- (NSView*) uiViewForAudioUnit:(AudioUnit)component withSize:(NSSize)inPreferredSize
{
    Float64 sampleRate = d_nextSampleRate;
    void* instancePointer = nullptr;
    AudioUnitScope scope;
    UInt32 dataSize;

    // fetch direct access pointer
   #if DAF_PLUGIN_WANT_DIRECT_ACCESS
    dataSize = sizeof(void*);
    AudioUnitGetProperty(component, 'DPFa', kAudioUnitScope_Global, 0, &instancePointer, &dataSize);
   #endif

    // fetch current sample rate
   #if DAF_PLUGIN_NUM_INPUTS != 0
    dataSize = sizeof(Float64);
    AudioUnitGetProperty(component, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &sampleRate, &dataSize);
   #elif DAF_PLUGIN_NUM_OUTPUTS != 0
    dataSize = sizeof(Float64);
    AudioUnitGetProperty(component, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sampleRate, &dataSize);
   #endif

   #if defined(DAF_UI_DEFAULT_WIDTH) && defined(DAF_UI_DEFAULT_HEIGHT)
    inPreferredSize = NSMakeSize(DAF_UI_DEFAULT_WIDTH, DAF_UI_DEFAULT_HEIGHT);
   #endif

    // create view
    view = [[[COCOA_VIEW_CLASS_NAME alloc] initWithPreferredSize:inPreferredSize] autorelease];
    view->ui = new DAF_UI_AU(component, view, sampleRate, instancePointer);
    view->ui->postSetup();

    // request data from DSP side
    {
        const uint16_t magic = 1337;
        AudioUnitSetProperty(component, 'DPFi', kAudioUnitScope_Global, 0, &magic, sizeof(uint16_t));
        const uint16_t cancel = 0;
        AudioUnitSetProperty(component, 'DPFi', kAudioUnitScope_Global, 0, &cancel, sizeof(uint16_t));
    }

    return view;

    // maybe unused
    (void)scope;
}

@end

// --------------------------------------------------------------------------------------------------------------------

#undef MACRO_NAME
#undef MACRO_NAME2

// --------------------------------------------------------------------------------------------------------------------
