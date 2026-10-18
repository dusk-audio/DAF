/*
 * DAF - Dusk Audio Framework
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

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "dgl/src/pugl.cpp"

#ifdef DAF_OS_MAC

// Control key status without requiring foreground activation in a build session.
@interface FocusTestWindow : NSWindow {
@public
    BOOL keyForTest;
}
@end

@implementation FocusTestWindow
- (BOOL)isKeyWindow { return keyForTest; }
@end

struct EventCounts {
    unsigned total = 0;
    unsigned unrealized = 0;
    unsigned focusIn = 0;
    unsigned focusOut = 0;
    bool aliveDuringUnrealize = false;
    PuglEventType retireOn = PUGL_NOTHING;
    unsigned retiringEvents = 0;
    bool retiredInCallback = false;
};

static PuglStatus onEvent(PuglView* const view, const PuglEvent* const event)
{
    EventCounts& counts = *static_cast<EventCounts*>(puglGetHandle(view));
    ++counts.total;
    if (event->type == PUGL_FOCUS_IN) ++counts.focusIn;
    if (event->type == PUGL_FOCUS_OUT) ++counts.focusOut;
    if (event->type == PUGL_UNREALIZE)
    {
        ++counts.unrealized;
        counts.aliveDuringUnrealize = view->impl->wrapperView->puglview == view;
    }
    if (event->type == counts.retireOn)
    {
        counts.retireOn = PUGL_NOTHING;
        ++counts.retiringEvents;
        counts.retiredInCallback = puglUnrealize(view) == PUGL_SUCCESS;
        return PUGL_UNSUPPORTED;
    }
    return PUGL_SUCCESS;
}

static NSEvent* crossingEvent()
{
    return [NSEvent enterExitEventWithType:NSEventTypeMouseEntered
                                   location:NSMakePoint(10, 10)
                              modifierFlags:0
                                  timestamp:0
                               windowNumber:0
                                    context:nil
                                eventNumber:0
                             trackingNumber:0
                                   userData:NULL];
}

static int lateCallbacks(PuglWrapperView* const wrapper, NSTimer* const timer,
                         const EventCounts& counts)
{
    const unsigned before = counts.total;
    NSEvent* const crossing = crossingEvent();
    DAF_ASSERT_NOT_EQUAL(crossing, nil, "tracking event exists");

    // AppKit may retain the removed wrapper while its tracking event is queued.
    [wrapper mouseEntered:crossing];
    DAF_ASSERT_EQUAL(counts.total, before, "late mouse entry dispatches no event");
    [wrapper mouseExited:crossing];
    [wrapper cursorUpdate:crossing];
    [wrapper mouseMoved:crossing];
    [wrapper mouseDragged:crossing];
    [wrapper rightMouseDragged:crossing];
    [wrapper otherMouseDragged:crossing];
    [wrapper mouseDown:crossing];
    [wrapper mouseUp:crossing];
    [wrapper rightMouseDown:crossing];
    [wrapper rightMouseUp:crossing];
    [wrapper otherMouseDown:crossing];
    [wrapper otherMouseUp:crossing];
    [wrapper scrollWheel:crossing];
    [wrapper keyDown:crossing];
    [wrapper keyUp:crossing];
    [wrapper flagsChanged:crossing];
    [wrapper insertText:@"late" replacementRange:NSMakeRange(NSNotFound, 0)];
    [wrapper dispatchExpose:NSMakeRect(0, 0, 10, 10)];
    [wrapper viewWillDraw];
    [wrapper viewWillStartLiveResize];
    [wrapper viewDidEndLiveResize];
    [wrapper timerTick:timer];

    DAF_ASSERT_EQUAL(counts.total, before, "a retired wrapper dispatches no events");
    DAF_ASSERT_EQUAL(wrapper->puglview, nullptr, "the retired wrapper has no C view");
    DAF_ASSERT_EQUAL([[wrapper trackingAreas] count], 0u, "retirement removes tracking areas");
    DAF_ASSERT_EQUAL([timer isValid], NO, "retirement invalidates timers");
    DAF_ASSERT_EQUAL([wrapper->userTimers count], 0u, "retirement drops timer ownership");
    [wrapper updateTrackingAreas];
    DAF_ASSERT_EQUAL([[wrapper trackingAreas] count], 0u, "late updates do not recreate tracking areas");
    const NSSize intrinsic = [wrapper intrinsicContentSize];
    DAF_ASSERT_EQUAL(intrinsic.width, NSViewNoInstrinsicMetric, "retired intrinsic width is unspecified");
    DAF_ASSERT_EQUAL(intrinsic.height, NSViewNoInstrinsicMetric, "retired intrinsic height is unspecified");
    return 0;
}

static int runRetirement(const bool unrealize)
{
    PuglWorld* const world = puglNewWorld(PUGL_MODULE, 0);
    DAF_ASSERT_NOT_EQUAL(world, nullptr, "Cocoa world exists");
    NSView* const parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    PuglView* const view = puglNewView(world);
    DAF_ASSERT_NOT_EQUAL(view, nullptr, "view exists");
    EventCounts counts;
    puglSetHandle(view, &counts);
    puglSetEventFunc(view, onEvent);
    puglSetBackend(view, puglStubBackend());
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 320, 240);
    puglSetParent(view, reinterpret_cast<PuglNativeView>(parent));
    DAF_ASSERT_EQUAL(puglRealize(view), PUGL_SUCCESS, "embedded view realizes without a top-level window");

    PuglWrapperView* const wrapper = [view->impl->wrapperView retain];
    DAF_ASSERT_NOT_EQUAL([[wrapper trackingAreas] count], 0u, "live view has a tracking area");
    const unsigned beforeLive = counts.total;
    NSEvent* const crossing = crossingEvent();
    DAF_ASSERT_NOT_EQUAL(crossing, nil, "live tracking event exists");
    [wrapper mouseEntered:crossing];
    DAF_ASSERT_EQUAL(counts.total, beforeLive + 1, "live mouse entry still dispatches");
    DAF_ASSERT_EQUAL(puglStartTimer(view, 77, 60.0), PUGL_SUCCESS, "live view starts a timer");
    NSTimer* const timer = [wrapper->userTimers[@77] retain];
    DAF_ASSERT_NOT_EQUAL(timer, nil, "timer is retained for late delivery");
    DAF_ASSERT_EQUAL([timer isValid], YES, "live timer is valid");

    if (unrealize)
    {
        DAF_ASSERT_EQUAL(puglUnrealize(view), PUGL_SUCCESS, "view unrealizes");
        DAF_ASSERT_EQUAL(counts.unrealized, 1u, "unrealize dispatches exactly once");
        DAF_ASSERT_EQUAL(counts.aliveDuringUnrealize, true, "unrealize callback precedes retirement");
        if (lateCallbacks(wrapper, timer, counts) != 0)
            return 1;

        DAF_ASSERT_EQUAL(puglRealize(view), PUGL_SUCCESS, "same C view realizes a new wrapper");
        DAF_ASSERT_NOT_EQUAL(view->impl->wrapperView, wrapper, "the new wrapper is distinct");
        if (lateCallbacks(wrapper, timer, counts) != 0)
            return 1;
        const unsigned beforeReplacement = counts.total;
        [view->impl->wrapperView mouseEntered:crossing];
        DAF_ASSERT_EQUAL(counts.total, beforeReplacement + 1, "replacement wrapper remains interactive");
    }

    puglFreeView(view);
    const int result = lateCallbacks(wrapper, timer, counts);
    [timer release];
    [wrapper release];
    [parent release];
    puglFreeWorld(world);
    return result;
}

enum class RetiringCallback { Flags, KeyDown, KeyUp, Text, Expose };

static NSEvent* keyEvent(const NSEventType type, const NSEventModifierFlags modifiers = 0)
{
    return [NSEvent keyEventWithType:type
                           location:NSMakePoint(10, 10)
                      modifierFlags:modifiers
                          timestamp:0
                       windowNumber:0
                            context:nil
                         characters:@"a"
        charactersIgnoringModifiers:@"a"
                          isARepeat:NO
                            keyCode:type == NSEventTypeFlagsChanged ? 0x38 : 0];
}

static int runRetiringCallback(const RetiringCallback which)
{
    PuglWorld* const world = puglNewWorld(PUGL_MODULE, 0);
    DAF_ASSERT_NOT_EQUAL(world, nullptr, "reentrant Cocoa world exists");
    NSView* const parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
    PuglView* const view = puglNewView(world);
    DAF_ASSERT_NOT_EQUAL(view, nullptr, "reentrant view exists");
    EventCounts counts;
    puglSetHandle(view, &counts);
    puglSetEventFunc(view, onEvent);
    puglSetBackend(view, puglStubBackend());
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 320, 240);
    puglSetParent(view, reinterpret_cast<PuglNativeView>(parent));
    DAF_ASSERT_EQUAL(puglRealize(view), PUGL_SUCCESS, "reentrant view realizes");
    PuglWrapperView* const wrapper = [view->impl->wrapperView retain];
    const unsigned before = counts.total;

    switch (which)
    {
    case RetiringCallback::Flags:
        counts.retireOn = PUGL_KEY_PRESS;
        [wrapper flagsChanged:keyEvent(NSEventTypeFlagsChanged, NSEventModifierFlagShift)];
        break;
    case RetiringCallback::KeyDown:
        counts.retireOn = PUGL_KEY_PRESS;
        [wrapper keyDown:keyEvent(NSEventTypeKeyDown)];
        break;
    case RetiringCallback::KeyUp:
        counts.retireOn = PUGL_KEY_RELEASE;
        [wrapper keyUp:keyEvent(NSEventTypeKeyUp)];
        break;
    case RetiringCallback::Text:
        counts.retireOn = PUGL_TEXT;
        [wrapper insertText:@"xyz" replacementRange:NSMakeRange(NSNotFound, 0)];
        break;
    case RetiringCallback::Expose:
        counts.retireOn = PUGL_CONFIGURE;
        [wrapper setReshaped];
        [wrapper dispatchExpose:NSMakeRect(0, 0, 10, 10)];
        break;
    }

    DAF_ASSERT_EQUAL(counts.retiringEvents, 1u, "the input or configure callback ran once");
    DAF_ASSERT_EQUAL(counts.retiredInCallback, true, "callback can unrealize its C view");
    DAF_ASSERT_EQUAL(counts.unrealized, 1u, "reentrant unrealize dispatches once");
    DAF_ASSERT_EQUAL(counts.aliveDuringUnrealize, true, "reentrant unrealize precedes retirement");
    DAF_ASSERT_EQUAL(counts.total, before + 2, "retirement stops further dispatch including remaining text");
    DAF_ASSERT_EQUAL(wrapper->puglview, nullptr, "callback retired its native wrapper");
    DAF_ASSERT_EQUAL(view->stage, PUGL_VIEW_STAGE_ALLOCATED, "C view remains valid and unrealized");
    puglFreeView(view);
    [wrapper mouseEntered:crossingEvent()];
    DAF_ASSERT_EQUAL(counts.total, before + 2, "the retired wrapper stays harmless after C view free");
    [wrapper release];
    [parent release];
    puglFreeWorld(world);
    return 0;
}

static int runEmbeddedFocus()
{
    PuglWorld* const world = puglNewWorld(PUGL_MODULE, 0);
    DAF_ASSERT_NOT_EQUAL(world, nullptr, "embedded focus world exists");
    FocusTestWindow* const window = [[FocusTestWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 320, 240)
                  styleMask:NSWindowStyleMaskTitled
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [window setReleasedWhenClosed:NO];
    window->keyForTest = YES;
    [window makeKeyAndOrderFront:nil];
    PuglView* const view = puglNewView(world);
    DAF_ASSERT_NOT_EQUAL(view, nullptr, "embedded focus view exists");
    EventCounts counts;
    puglSetHandle(view, &counts);
    puglSetEventFunc(view, onEvent);
    puglSetBackend(view, puglStubBackend());
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 320, 240);
    puglSetParent(view, reinterpret_cast<PuglNativeView>([window contentView]));
    DAF_ASSERT_EQUAL(puglRealize(view), PUGL_SUCCESS, "embedded focus view realizes");
    DAF_ASSERT_EQUAL(puglGrabFocus(view), PUGL_SUCCESS, "embedded view requests native focus");
    DAF_ASSERT_EQUAL([window isKeyWindow], YES, "native parent is a key window");
    DAF_ASSERT_EQUAL([window firstResponder], view->impl->wrapperView, "native child is the first responder");
    DAF_ASSERT_EQUAL(puglHasFocus(view), true, "embedded view is the key window's first responder");
    DAF_ASSERT_EQUAL(counts.focusIn, 0u, "initial focus waits for the event pump");
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "initial focus pump succeeds");
    DAF_ASSERT_EQUAL(counts.focusIn, 1u, "initial embedded focus reaches its callback");
    DAF_ASSERT_EQUAL(counts.focusOut, 0u, "initial focus has no loss");
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "unchanged focus pump succeeds");
    DAF_ASSERT_EQUAL(counts.focusIn, 1u, "unchanged focus is not dispatched twice");

    DAF_ASSERT_EQUAL([window makeFirstResponder:nil], YES, "the parent takes responder focus");
    DAF_ASSERT_EQUAL([window firstResponder], window, "the native parent is the first responder");
    DAF_ASSERT_EQUAL(puglHasFocus(view), false, "the child no longer owns responder focus");
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "responder transfer pump succeeds");
    DAF_ASSERT_EQUAL(counts.focusOut, 1u, "responder transfer dispatches focus loss");
    DAF_ASSERT_EQUAL(puglGrabFocus(view), PUGL_SUCCESS, "the child takes responder focus again");
    DAF_ASSERT_EQUAL([window firstResponder], view->impl->wrapperView, "the native child regains the first responder");
    DAF_ASSERT_EQUAL(puglHasFocus(view), true, "the child owns responder focus again");
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "responder return pump succeeds");
    DAF_ASSERT_EQUAL(counts.focusIn, 2u, "responder return dispatches focus gain");

    window->keyForTest = NO;
    DAF_ASSERT_EQUAL(puglHasFocus(view), false, "a non-key parent gives no child keyboard focus");
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "key-window loss pump succeeds");
    DAF_ASSERT_EQUAL(counts.focusOut, 2u, "key-window loss dispatches focus loss");
    window->keyForTest = YES;
    DAF_ASSERT_EQUAL(puglHasFocus(view), true, "the key parent restores child focus");
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "key-window return pump succeeds");
    DAF_ASSERT_EQUAL(counts.focusIn, 3u, "key-window return dispatches focus gain");

    DAF_ASSERT_EQUAL(puglUnrealize(view), PUGL_SUCCESS, "focused view unrealizes");
    DAF_ASSERT_EQUAL(puglRealize(view), PUGL_SUCCESS, "focused view realizes a replacement");
    DAF_ASSERT_EQUAL(puglGrabFocus(view), PUGL_SUCCESS, "replacement requests native focus");
    DAF_ASSERT_EQUAL([window firstResponder], view->impl->wrapperView, "the native replacement is the first responder");
    DAF_ASSERT_EQUAL(puglHasFocus(view), true, "replacement has native focus");
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "replacement focus pump succeeds");
    DAF_ASSERT_EQUAL(counts.focusIn, 4u, "replacement receives its own initial focus gain");

    counts.retireOn = PUGL_FOCUS_OUT;
    DAF_ASSERT_EQUAL([window makeFirstResponder:nil], YES, "the parent takes focus before retirement");
    DAF_ASSERT_EQUAL([window firstResponder], window, "the native parent owns focus before retirement");
    DAF_ASSERT_EQUAL(puglHasFocus(view), false, "native focus is gone before retirement");
    const unsigned beforeRetirement = counts.total;
    DAF_ASSERT_EQUAL(puglUpdate(world, 0.0), PUGL_SUCCESS, "retiring focus callback returns safely");
    DAF_ASSERT_EQUAL(counts.focusOut, 3u, "retiring focus loss dispatches once");
    DAF_ASSERT_EQUAL(counts.retiredInCallback, true, "focus callback can unrealize its view");
    DAF_ASSERT_EQUAL(view->stage, PUGL_VIEW_STAGE_ALLOCATED, "retired focus view stays unrealized");
    DAF_ASSERT_EQUAL(counts.total, beforeRetirement + 2, "retirement prevents update and draw callbacks");

    puglFreeView(view);
    [window close];
    [window release];
    puglFreeWorld(world);
    return 0;
}

int main(int argc, char** argv)
{
    @autoreleasepool
    {
        if (argc > 1 && std::strcmp(argv[1], "--focus") == 0)
            return runEmbeddedFocus();
        if (argc > 1 && std::strcmp(argv[1], "--free") == 0)
            return runRetirement(false);
        if (argc > 1 && std::strcmp(argv[1], "--unrealize") == 0)
            return runRetirement(true);
        const struct { const char* argument; RetiringCallback which; } callbacks[] = {
            { "--flags", RetiringCallback::Flags },
            { "--key-down", RetiringCallback::KeyDown },
            { "--key-up", RetiringCallback::KeyUp },
            { "--text", RetiringCallback::Text },
            { "--expose", RetiringCallback::Expose }
        };
        if (argc > 1)
        {
            for (const auto& callback : callbacks)
                if (std::strcmp(argv[1], callback.argument) == 0)
                    return runRetiringCallback(callback.which);
            return 1;
        }
        if (runEmbeddedFocus() != 0 || runRetirement(false) != 0 || runRetirement(true) != 0)
            return 1;
        for (const auto& callback : callbacks)
            if (runRetiringCallback(callback.which) != 0)
                return 1;
    }
    return 0;
}

#else
#error CocoaRetiredView requires macOS
#endif
