/*
 * Dusk Audio Framework (DAF)
 * Copyright (C) 2026 Dusk Audio
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

// An AU's Cocoa view outliving its unit, with main() as the host. The host keeps the view for as long as it likes,
// and AppKit keeps one that was its window's first responder alive after the host has let go of it, so a UI can stay
// around after its unit is disposed either way. It kept its idle timer running and its pointer to the deleted plugin,
// and a UI reading its plugin off that pointer, as metering UIs do, read freed memory from the next tick on.
//
//   AUViewDispose <path to d_au_view_test.component> [scenario]
//
// With a scenario number (1 to 7, in the order below), that scenario alone.
//
// Loads the bundle tests/au-view builds (`make -C tests/au-view au`) by its path, nothing installed. Each scenario
// makes a unit, renders a few blocks, creates its view the way a host does (kAudioUnitProperty_CocoaUI) and runs the
// main run loop until the UI has read its plugin, then disposes the unit:
// - with the view held in a window on screen, painted again after the dispose (which edits a parameter)
// - with the view held in a window ordered out, and held in no window at all
// - with two views of the unit held
// - from another thread while the main thread is in the UI's idle tick (the UI sleeps there, plugin pointer in hand,
//   and then calls the unit), which the dispose waits for
// - from a host thread that holds a lock of its own across the dispose, which the host's observer of the view's frame
//   takes too, while the UI resizes itself from a paint: the dispose must not wait for the paint, which waits for the
//   host's lock, so that this would be a deadlock
// - after the host has let go of the view, which was its window's first responder
// The UI exits the process, failing the test, when its idle runs or it reads its plugin after the plugin was deleted
// (tests/au-view/ViewDisposeUI.cpp). This host fails it when one of the plugin's idle timers is still running once a
// unit is disposed, or when the plugin calls a unit after the unit was disposed, which libAUViewDisposeWatch watches
// (AUViewDisposeWatch.hpp). Run under AddressSanitizer as well (CI), which catches a use after free anywhere else,
// outside the UI's own reads.
//
// Not tested, as it is not guaranteed (see daf/src/DafPluginAUUILink.hpp): a unit disposed on another thread during a
// paint, an input event or the UI's destruction, which can still read the plugin as it goes.

#import <AppKit/AppKit.h>
#import <AudioToolbox/AudioToolbox.h>
#import <AudioUnit/AUCocoaUIView.h>

#include "AUViewDisposeWatch.hpp"

#include <dlfcn.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#define REQUIRE(condition, message) \
    do { if (! (condition)) { std::fprintf(stderr, "AUViewDispose: %s: %s (%s:%d)\n", gScenario, message, \
                                           __FILE__, __LINE__); std::_Exit(1); } } while (false)

namespace
{

const char* volatile gScenario = "setup";
AudioComponent gComponent = nullptr;

enum WindowMode { kOnScreen, kOrderedOut, kNoWindow };

// Once a unit is disposed, every timer the plugin made for the views of the scenario is off: no UI of it idles again.
void requireTimersStopped()
{
    size_t valid = 0;
    REQUIRE(auViewWatchPluginTimers(&valid) != 0, "the plugin's UI must make an idle timer");
    REQUIRE(valid == 0, "the UI's idle timer must stop when its unit is disposed");
}

// ---------------------------------------------------------------------------------------------------------------------

void hung(int)
{
    const char head[] = "AUViewDispose: no progress in 30 s, in: ";
    ssize_t written = write(STDERR_FILENO, head, sizeof(head) - 1);
    const char* const scenario = gScenario;
    written += write(STDERR_FILENO, scenario, std::strlen(scenario));
    written += write(STDERR_FILENO, "\n", 1);
    (void)written;
    _exit(1);
}

// A scenario starts: named in the log, so that a failure the UI reports (it knows no scenario) can be placed.
void begin(const char* const scenario)
{
    gScenario = scenario;
    std::fprintf(stderr, "AUViewDispose: %s\n", scenario);
    alarm(30);
}

OSType fourCharCode(NSString* const string)
{
    REQUIRE(string != nil && [string length] == 4, "the bundle's AudioComponents entry needs four-char codes");
    const char* const c = [string UTF8String];
    return (OSType(uint8_t(c[0])) << 24) | (OSType(uint8_t(c[1])) << 16) | (OSType(uint8_t(c[2])) << 8)
         | OSType(uint8_t(c[3]));
}

// A host's event loop: each pass in an autorelease pool of its own, drained before the next.
void spin(const double seconds)
{
    NSDate* const until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0)
    {
        @autoreleasepool {
            NSEvent* const event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                      untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                         inMode:NSDefaultRunLoopMode
                                                        dequeue:YES];
            if (event != nil)
                [NSApp sendEvent:event];
        }
    }
}

void registerComponent(const char* const bundlePath)
{
    @autoreleasepool {
        CFURLRef const url = CFURLCreateFromFileSystemRepresentation(nullptr,
                                                                      reinterpret_cast<const UInt8*>(bundlePath),
                                                                      static_cast<CFIndex>(std::strlen(bundlePath)),
                                                                      true);
        CFBundleRef const bundle = CFBundleCreate(nullptr, url);
        CFRelease(url);
        REQUIRE(bundle != nullptr && CFBundleLoadExecutableAndReturnError(bundle, nullptr),
                "the AU bundle must load");

        NSDictionary* const plist = (NSDictionary*)CFBundleGetInfoDictionary(bundle);
        NSDictionary* const entry = [plist[@"AudioComponents"] firstObject];
        REQUIRE(entry != nil, "the AU bundle must have an AudioComponents entry");

        AudioComponentDescription desc;
        std::memset(&desc, 0, sizeof(desc));
        desc.componentType = fourCharCode(entry[@"type"]);
        desc.componentSubType = fourCharCode(entry[@"subtype"]);
        desc.componentManufacturer = fourCharCode(entry[@"manufacturer"]);

        const AudioComponentFactoryFunction factory = reinterpret_cast<AudioComponentFactoryFunction>(
            CFBundleGetFunctionPointerForName(bundle, (CFStringRef)entry[@"factoryFunction"]));
        REQUIRE(factory != nullptr, "the AU bundle must export its factory");

        Dl_info info;
        REQUIRE(dladdr(reinterpret_cast<const void*>(factory), &info) != 0, "the plugin binary must be found");
        auViewWatchPluginImage(info.dli_fbase);

        // this process only, and what is instantiated is the component returned here, never an installed one
        gComponent = AudioComponentRegister(&desc, (CFStringRef)entry[@"name"],
                                            [entry[@"version"] unsignedIntValue], factory);
        REQUIRE(gComponent != nullptr, "the AU must register");
    }
}

OSStatus renderInput(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
                     AudioBufferList* const io)
{
    for (UInt32 b = 0; b < io->mNumberBuffers; ++b)
    {
        float* const data = static_cast<float*>(io->mBuffers[b].mData);
        if (data == nullptr)
            continue;
        const UInt32 count = std::min<UInt32>(frames, io->mBuffers[b].mDataByteSize / sizeof(float));
        for (UInt32 i = 0; i < count; ++i)
            data[i] = 0.5f * static_cast<float>(std::sin(0.06 * i));
    }
    return noErr;
}

// An initialized unit with a few blocks rendered, so the meter its UI reads has moved.
AudioUnit newUnit()
{
    AudioUnit unit = nullptr;
    REQUIRE(AudioComponentInstanceNew(gComponent, &unit) == noErr && unit != nullptr, "the AU must instantiate");
    auViewWatchCreated(unit);

    const UInt32 frames = 256;
    REQUIRE(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                 &frames, sizeof(frames)) == noErr, "the AU must take a block size");
    AURenderCallbackStruct callback = { renderInput, nullptr };
    REQUIRE(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                 &callback, sizeof(callback)) == noErr, "the AU must take an input");
    REQUIRE(AudioUnitInitialize(unit) == noErr, "the AU must initialize");

    float left[frames], right[frames];
    AudioBufferList* const abl = static_cast<AudioBufferList*>(
        std::calloc(1, offsetof(AudioBufferList, mBuffers) + 2 * sizeof(AudioBuffer)));
    abl->mNumberBuffers = 2;
    abl->mBuffers[0].mNumberChannels = abl->mBuffers[1].mNumberChannels = 1;
    abl->mBuffers[0].mDataByteSize = abl->mBuffers[1].mDataByteSize = sizeof(left);
    abl->mBuffers[0].mData = left;
    abl->mBuffers[1].mData = right;
    for (int block = 0; block < 4; ++block)
    {
        AudioUnitRenderActionFlags flags = 0;
        AudioTimeStamp timestamp;
        std::memset(&timestamp, 0, sizeof(timestamp));
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        timestamp.mSampleTime = block * frames;
        REQUIRE(AudioUnitRender(unit, &flags, &timestamp, 0, frames, abl) == noErr, "the AU must render");
    }
    std::free(abl);
    return unit;
}

// The view as a host makes it, with the host's own reference: it lives until releaseView(), whatever the unit does.
NSView* newView(AudioUnit const unit)
{
    NSView* view = nil;
    @autoreleasepool {
        UInt32 size = 0;
        Boolean writable = false;
        REQUIRE(AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                         &size, &writable) == noErr && size >= sizeof(AudioUnitCocoaViewInfo),
                "the AU must have a Cocoa UI");
        AudioUnitCocoaViewInfo* const info = static_cast<AudioUnitCocoaViewInfo*>(std::calloc(1, size));
        REQUIRE(AudioUnitGetProperty(unit, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
                                     info, &size) == noErr, "the AU must describe its Cocoa UI");

        NSBundle* const viewBundle = [NSBundle bundleWithURL:(NSURL*)info->mCocoaAUViewBundleLocation];
        REQUIRE(viewBundle != nil && [viewBundle load], "the view's bundle must load");
        Class const factoryClass = [viewBundle classNamed:(NSString*)info->mCocoaAUViewClass[0]];
        REQUIRE(factoryClass != nil && [factoryClass conformsToProtocol:@protocol(AUCocoaUIBase)],
                "the view's bundle must have the AUCocoaUIBase class the AU names");

        NSObject<AUCocoaUIBase>* const factory = [[factoryClass alloc] init];
        view = [[factory uiViewForAudioUnit:unit withSize:NSMakeSize(0, 0)] retain];
        [factory release];
        REQUIRE(view != nil, "the AU must make a view");

        CFRelease(info->mCocoaAUViewBundleLocation);
        CFRelease(info->mCocoaAUViewClass[0]);
        std::free(info);
    }
    return view;
}

NSWindow* newWindow(NSView* const view, const WindowMode mode)
{
    if (mode == kNoWindow)
        return nil;

    NSWindow* window = nil;
    @autoreleasepool {
        const NSRect frame = [view frame];
        window = [[NSWindow alloc] initWithContentRect:NSMakeRect(200, 200, frame.size.width, frame.size.height)
                                             styleMask:NSWindowStyleMaskTitled
                                               backing:NSBackingStoreBuffered
                                                 defer:NO];
        [window setReleasedWhenClosed:NO];
        [[window contentView] addSubview:view];
        [window orderFrontRegardless];
    }
    return window;
}

// The host lets go of the view: out of its window, its reference dropped, and the pool drained.
void releaseView(NSView*& view)
{
    @autoreleasepool {
        [view removeFromSuperview];
        [view release];
        view = nil;
    }
}

void closeWindow(NSWindow*& window)
{
    if (window == nil)
        return;
    @autoreleasepool {
        [window close];
        [window release];
        window = nil;
    }
}

void markForRepaint(NSView* const view)
{
    [view setNeedsDisplay:YES];
    for (NSView* subview in [view subviews])
        markForRepaint(subview);
}

// A scenario ends: the plugin called none of the units disposed, whether from a view the host kept, one going away, or
// one AppKit keeps. A view AppKit keeps after the host let go of it (one that was its window's first responder, or
// was painted then) outlives the scenario and the process, so nothing it does on its way out can be seen; while it
// lingers it is in no window, so it does nothing at all.
void end()
{
    const char* first = nullptr;
    if (auViewWatchCallsOnDisposedUnits(&first) != 0)
        std::fprintf(stderr, "AUViewDispose: %s: the plugin called %s on a disposed unit\n", gScenario, first);
    REQUIRE(auViewWatchCallsOnDisposedUnits(nullptr) == 0, "the plugin must not call a unit after the unit was disposed");
    auViewWatchForgetTimers();
}

// The plugin's parameters (tests/au-view/ViewDisposePlugin.hpp): what the UI has read of the plugin so far, whether
// the UI is in a slow callback, one the UI edits, and one that has the UI resize itself in its next paint.
enum { kParameterUIReads, kParameterUIBusy, kParameterEdit, kParameterResize };

float parameter(AudioUnit const unit, const AudioUnitParameterID index)
{
    AudioUnitParameterValue value = 0.0f;
    REQUIRE(AudioUnitGetParameter(unit, index, kAudioUnitScope_Global, 0, &value) == noErr,
            "the AU must report its parameters");
    return value;
}

// Runs the main run loop until the UI has read its plugin: the dispose that follows is one under a running UI.
void waitForTheUI(AudioUnit const unit)
{
    for (int i = 0; i < 300 && parameter(unit, kParameterUIReads) < 1.0f; ++i)
        spin(0.01);
    REQUIRE(parameter(unit, kParameterUIReads) >= 1.0f, "the UI must read its plugin before the unit is disposed");
}

void dispose(AudioUnit const unit)
{
    REQUIRE(AudioUnitUninitialize(unit) == noErr, "the AU must uninitialize");
    REQUIRE(AudioComponentInstanceDispose(unit) == noErr, "the AU must dispose");
    auViewWatchDisposed(unit);
}

// ---------------------------------------------------------------------------------------------------------------------

void disposeWhileTheViewIsHeld(const char* const scenario, const WindowMode mode)
{
    begin(scenario);
    AudioUnit const unit = newUnit();
    NSView* view = newView(unit);
    NSWindow* window = newWindow(view, mode);
    waitForTheUI(unit);
    if (mode == kOrderedOut)
        [window orderOut:nil];

    dispose(unit);
    requireTimersStopped();

    // idle ticks, and on screen a paint, with the view still the host's
    spin(0.3);
    if (mode == kOnScreen)
    {
        markForRepaint(view);
        [window displayIfNeeded];
    }
    spin(0.3);

    releaseView(view);
    closeWindow(window);
    spin(0.1);
    end();
}

void disposeWithTwoViewsHeld()
{
    begin("a unit disposed with two of its views held");
    AudioUnit const unit = newUnit();
    NSView* first = newView(unit);
    NSView* second = newView(unit);
    NSWindow* window = newWindow(first, kOnScreen);
    waitForTheUI(unit);

    dispose(unit);
    requireTimersStopped();
    spin(0.5);

    releaseView(first);
    releaseView(second);
    closeWindow(window);
    spin(0.1);
    end();
}

// The UI's idle tick sleeps with the plugin's pointer in hand (see ViewDisposeUI.cpp), and the unit is disposed on
// another thread right then: the dispose waits for the tick to end, during which the UI calls into the unit.
void disposeOnAnotherThreadDuringAnIdleTick()
{
    begin("a unit disposed on another thread during the UI's idle tick");
    AudioUnit const unit = newUnit();
    setenv("DAF_TEST_AU_VIEW_SLOW", "idle", 1);
    NSView* view = newView(unit);
    unsetenv("DAF_TEST_AU_VIEW_SLOW");
    NSWindow* window = newWindow(view, kOnScreen);
    waitForTheUI(unit);

    std::atomic<bool> sawBusy(false), disposed(false);
    std::thread disposer([unit, &sawBusy, &disposed] {
        for (int i = 0; i < 5000 && ! sawBusy.load(); ++i)
        {
            sawBusy.store(parameter(unit, kParameterUIBusy) > 0.5f);
            if (! sawBusy.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        auViewWatchDisposed(unit);
        disposed.store(true);
    });

    for (int i = 0; i < 500 && ! disposed.load(); ++i)
        spin(0.01);
    REQUIRE(disposed.load(), "the unit must dispose while the main thread runs the UI");
    disposer.join();
    REQUIRE(sawBusy.load(), "the unit must be disposed during the UI's idle tick");
    requireTimersStopped();
    spin(0.3);

    releaseView(view);
    closeWindow(window);
    spin(0.1);
    end();
}

// The host's own lock, see disposeOnAHostThreadWhileTheUIResizes().
std::mutex gHostLock;

// A host that disposes of the unit on a thread of its own, holding a lock of its own throughout, and whose observer of
// the plugin view's frame takes the same lock, as a host keeping its plugin window the size of the view may. The UI
// resizes itself from a paint right then, as a resize grip does, which reaches that observer and waits for the lock:
// the dispose must not wait for the paint in turn. A wrapper doing so hangs here, until the alarm fails the test.
void disposeOnAHostThreadWhileTheUIResizes()
{
    begin("a unit disposed on a host thread holding a lock its view frame observer takes, the UI resizing in a paint");
    AudioUnit const unit = newUnit();
    setenv("DAF_TEST_AU_VIEW_SLOW", "resize", 1);
    NSView* view = newView(unit);
    unsetenv("DAF_TEST_AU_VIEW_SLOW");
    NSWindow* window = newWindow(view, kOnScreen);

    id observer = nil;
    @autoreleasepool {
        observer = [[[NSNotificationCenter defaultCenter] addObserverForName:NSViewFrameDidChangeNotification
                                                                      object:view
                                                                       queue:nil
                                                                  usingBlock:^(NSNotification*) {
            const std::lock_guard<std::mutex> guard(gHostLock);
        }] retain];
    }
    waitForTheUI(unit);

    std::atomic<bool> holding(false), sawBusy(false), disposed(false);
    std::thread disposer([unit, &holding, &sawBusy, &disposed] {
        const std::lock_guard<std::mutex> guard(gHostLock);
        holding.store(true);
        for (int i = 0; i < 5000 && ! sawBusy.load(); ++i)
        {
            sawBusy.store(parameter(unit, kParameterUIBusy) > 0.5f);
            if (! sawBusy.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        AudioUnitUninitialize(unit);
        AudioComponentInstanceDispose(unit);
        auViewWatchDisposed(unit);
        disposed.store(true);
    });
    while (! holding.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    // only now, with the host's lock held, does the UI resize itself, in its next paint
    REQUIRE(AudioUnitSetParameter(unit, kParameterResize, kAudioUnitScope_Global, 0, 1.0f, 0) == noErr,
            "the AU must take a parameter");
    for (int i = 0; i < 500 && ! disposed.load(); ++i)
        spin(0.01);
    REQUIRE(disposed.load(), "the unit must dispose while the UI resizes itself");
    disposer.join();
    REQUIRE(sawBusy.load(), "the unit must be disposed while the UI is in the paint");
    requireTimersStopped();
    spin(0.3);

    @autoreleasepool {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        [observer release];
    }
    releaseView(view);
    closeWindow(window);
    spin(0.1);
    end();
}

void disposeAfterTheHostLetGo()
{
    begin("a unit disposed after the host let go of its view, its window's first responder");
    AudioUnit const unit = newUnit();
    NSView* view = newView(unit);
    NSWindow* window = newWindow(view, kOnScreen);
    waitForTheUI(unit);

    // as a click into the view makes it: AppKit then keeps the view, and its UI, alive after the host lets go
    @autoreleasepool {
        [window makeFirstResponder:view];
    }
    REQUIRE([window firstResponder] == view, "the view must become its window's first responder");

    releaseView(view);
    spin(0.2);
    dispose(unit);
    requireTimersStopped();
    spin(0.3);

    closeWindow(window);
    spin(0.1);
    end();
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc != 2 && argc != 3)
    {
        std::fprintf(stderr, "usage: %s <path to d_au_view_test.component> [scenario]\n", argv[0]);
        return 1;
    }
    const int only = argc == 3 ? std::atoi(argv[2]) : 0;
    signal(SIGALRM, hung);

    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        [NSApp finishLaunching];
    }
    registerComponent(argv[1]);

    if (only == 0 || only == 1)
        disposeWhileTheViewIsHeld("a unit disposed with its view held on screen", kOnScreen);
    if (only == 0 || only == 2)
        disposeWhileTheViewIsHeld("a unit disposed with its view held in a window ordered out", kOrderedOut);
    if (only == 0 || only == 3)
        disposeWhileTheViewIsHeld("a unit disposed with its view held in no window", kNoWindow);
    if (only == 0 || only == 4)
        disposeWithTwoViewsHeld();
    if (only == 0 || only == 5)
        disposeOnAnotherThreadDuringAnIdleTick();
    if (only == 0 || only == 6)
        disposeOnAHostThreadWhileTheUIResizes();
    if (only == 0 || only == 7)
        disposeAfterTheHostLetGo();

    alarm(0);
    std::fprintf(stderr, "AUViewDispose: passed\n");
    return 0;
}
