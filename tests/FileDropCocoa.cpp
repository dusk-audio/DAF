// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// Receiving file drops on macOS (dgl/src/FileDropMac.hpp, see dgl/src/drop-targets.md): the wrapper view's
// NSDraggingDestination methods, called the way AppKit calls them, with a drag described over a pasteboard
// of the test's own. Needs a window server session but no input.

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "dgl/src/pugl.mm"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#import <objc/runtime.h>

#ifndef DAF_OS_MAC
#error FileDropCocoa requires macOS
#endif

// What a destination asks of a drag, and nothing else: AppKit's own NSDraggingInfo cannot be made by hand
@interface DuskFileDropTestDrag : NSObject {
@public
    NSPasteboard* pasteboard;
    NSPoint location;
    NSDragOperation mask;
}
@end

@implementation DuskFileDropTestDrag
- (NSPasteboard*)draggingPasteboard { return pasteboard; }
- (NSPoint)draggingLocation { return location; }
- (NSDragOperation)draggingSourceOperationMask { return mask; }
@end

@interface DuskFileDropTestPromise : NSObject<NSFilePromiseProviderDelegate>
@end

@implementation DuskFileDropTestPromise
- (NSString*)filePromiseProvider:(NSFilePromiseProvider*)provider fileNameForType:(NSString*)type
{
    (void)provider; (void)type;
    return @"promised.mid";
}
- (void)filePromiseProvider:(NSFilePromiseProvider*)provider
          writePromiseToURL:(NSURL*)url
          completionHandler:(void (^)(NSError*))completionHandler
{
    (void)provider; (void)url;
    completionHandler(nil);
}
@end

static int failures = 0;

static void check(const bool ok, const char* const what)
{
    if (! ok)
    {
        ++failures;
        d_stderr2("FileDropCocoa: FAILED %s", what);
    }
}

// --------------------------------------------------------------------------------------------------------------------

// What the view heard, and what it does when it hears it
struct Recorder
{
    PuglView* view = nullptr;
    std::vector<PuglEventType> events;
    std::vector<std::string> files; // of the latest enter, motion or drop, read after its action
    double x = 0.0, y = 0.0;
    PuglStatus answer = PUGL_SUCCESS; // to enter and motion
    PuglStatus dropAnswer = PUGL_SUCCESS;
    PuglEventType freeOn = PUGL_NOTHING;   // frees the view inside this callback
    PuglEventType optOutOn = PUGL_NOTHING; // turns drops off inside this callback
    std::function<void()> during;          // runs inside the next callback
    int depth = 0;
    bool nested = false; // a callback ran inside another
};

static bool isDropEvent(const PuglEventType type)
{
    return type == PUGL_DRAG_ENTER || type == PUGL_DRAG_MOTION || type == PUGL_DRAG_LEAVE || type == PUGL_DROP;
}

static PuglStatus onEvent(PuglView* const view, const PuglEvent* const event)
{
    Recorder* const recorder = static_cast<Recorder*>(puglGetHandle(view));

    if (recorder == nullptr || ! isDropEvent(event->type))
        return PUGL_SUCCESS;

    Recorder& r = *recorder;

    const PuglDropEvent& drop = event->drop;
    const PuglEventType type = drop.type;

    if (r.depth != 0)
        r.nested = true;
    ++r.depth;
    r.events.push_back(type);

    if (r.during)
    {
        const std::function<void()> action = r.during;
        r.during = nullptr;
        action();
    }

    if (r.optOutOn == type)
    {
        r.optOutOn = PUGL_NOTHING;
        puglSetAcceptsFileDrops(view, false);
    }

    if (r.freeOn == type)
    {
        r.freeOn = PUGL_NOTHING;
        puglFreeView(r.view);
        r.view = nullptr;
    }

    // after the action: the files stay readable until this callback returns
    if (type != PUGL_DRAG_LEAVE)
    {
        r.files.clear();
        for (uint32_t i = 0; i < drop.numFiles; ++i)
            r.files.push_back(drop.files[i]);
        r.x = drop.x;
        r.y = drop.y;
    }

    --r.depth;
    return type == PUGL_DROP ? r.dropAnswer : r.answer;
}

// A top-level view that accepts drops, its wrapper kept for calls after the view has gone
struct Fixture
{
    PuglWorld* world;
    Recorder recorder;
    PuglWrapperView* wrapper;
    double scale;

    explicit Fixture(const bool accept = true)
        : world(puglNewWorld(PUGL_MODULE, 0)),
          recorder(),
          wrapper(nil),
          scale(1.0)
    {
        PuglView* const view = puglNewView(world);
        recorder.view = view;
        puglSetHandle(view, &recorder);
        puglSetEventFunc(view, onEvent);
        puglSetBackend(view, puglStubBackend());
        puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 320, 240);
        check(puglRealize(view) == PUGL_SUCCESS, "a top-level view realizes");
        check(puglShow(view, PUGL_SHOW_PASSIVE) == PUGL_SUCCESS, "it shows");
        wrapper = [view->impl->wrapperView retain];
        scale = viewScaleFactor(view);
        check(scale == 2.0, "the test window is a Retina one");

        if (accept)
            check(puglSetAcceptsFileDrops(view, true) == PUGL_SUCCESS, "a top-level view accepts drops");
    }

    ~Fixture()
    {
        if (recorder.view != nullptr)
            puglFreeView(recorder.view);
        [wrapper release];
        puglFreeWorld(world);
    }

    bool registered() const
    {
        return [[wrapper registeredDraggedTypes] containsObject:NSPasteboardTypeFileURL];
    }

    // A drag at x, y in view pixels, as AppKit gives it: in the window's base coordinates, in points
    void moveTo(DuskFileDropTestDrag* const drag, const double x, const double y) const
    {
        drag->location = [wrapper convertPoint:NSMakePoint(x / scale, y / scale) toView:nil];
    }
};

// every pasteboard made, given back to the pasteboard server at the end
static NSMutableArray* pasteboards = nil;

static NSPasteboard* uniquePasteboard()
{
    NSPasteboard* const pasteboard = [NSPasteboard pasteboardWithUniqueName];
    [pasteboards addObject:pasteboard];
    [pasteboard clearContents];
    return pasteboard;
}

static NSPasteboard* pasteboardWith(NSArray* const objects)
{
    NSPasteboard* const pasteboard = uniquePasteboard();
    [pasteboard writeObjects:objects];
    return pasteboard;
}

static DuskFileDropTestDrag* dragOf(NSPasteboard* const pasteboard, const NSDragOperation mask = NSDragOperationCopy)
{
    DuskFileDropTestDrag* const drag = [[DuskFileDropTestDrag new] autorelease];
    drag->pasteboard = pasteboard;
    drag->location = NSZeroPoint;
    drag->mask = mask;
    return drag;
}

// Counts the closes of a window, from AppKit's own notification
struct CloseCounter
{
    NSWindow* const window;
    int* const closes;
    id observer;

    explicit CloseCounter(NSWindow* const w)
        : window([w retain]),
          closes(new int(0))
    {
        int* const count = closes;
        observer = [[[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification
                                                                     object:window
                                                                      queue:nil
                                                                 usingBlock:^(NSNotification*) { ++*count; }] retain];
    }

    ~CloseCounter()
    {
        [[NSNotificationCenter defaultCenter] removeObserver:observer];
        [observer release];
        [window release];
        delete closes;
    }

    int count() const { return *closes; }
};

// Lets what was scheduled for the next turn of the run loop run
static void turnRunLoop()
{
    [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
}

static NSString* const kPath = @"/tmp/Dusk drop/take 1.wav";

static DuskFileDropTestDrag* fileDrag(const NSDragOperation mask = NSDragOperationCopy)
{
    return dragOf(pasteboardWith(@[[NSURL fileURLWithPath:kPath]]), mask);
}

static std::vector<PuglEventType> eventsOf(std::initializer_list<PuglEventType> types)
{
    return std::vector<PuglEventType>(types);
}

// --------------------------------------------------------------------------------------------------------------------

static void testAccept()
{
    Fixture f;
    PuglWrapperView* const w = f.wrapper;
    check(f.registered(), "opting in registers the wrapper for file URLs");
    check([w wantsPeriodicDraggingUpdates] == NO, "no periodic updates");

    DuskFileDropTestDrag* const drag = fileDrag(NSDragOperationEvery);
    f.moveTo(drag, 30, 40);
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "an accepted enter answers copy, never more");
    check(f.recorder.events == eventsOf({ PUGL_DRAG_ENTER }), "the view hears the enter");
    check(f.recorder.files.size() == 1 && f.recorder.files[0] == [kPath fileSystemRepresentation], "the enter carries the file");
    check(f.recorder.x == 30 && f.recorder.y == 40, "the enter is in view pixels, top left origin");

    f.moveTo(drag, 100, 7);
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "an accepted motion answers copy");
    check(f.recorder.events == eventsOf({ PUGL_DRAG_ENTER, PUGL_DRAG_MOTION }), "the view hears the motion");
    check(f.recorder.x == 100 && f.recorder.y == 7, "the motion is in view pixels");

    check([w prepareForDragOperation:(id<NSDraggingInfo>)drag] == YES, "an accepted drag is prepared for");
    check([w performDragOperation:(id<NSDraggingInfo>)drag] == YES, "a drop the view takes reports success");
    check(f.recorder.events == eventsOf({ PUGL_DRAG_ENTER, PUGL_DRAG_MOTION, PUGL_DROP }), "the view hears the drop");
    check(f.recorder.files.size() == 1 && f.recorder.files[0] == [kPath fileSystemRepresentation], "the drop carries the file");

    [w draggingExited:(id<NSDraggingInfo>)drag];
    [w draggingEnded:(id<NSDraggingInfo>)drag];
    check(f.recorder.events.size() == 3, "no leave after a drop");

    // a drop the view does not take
    DuskFileDropTestDrag* const second = fileDrag();
    f.recorder.dropAnswer = PUGL_FAILURE;
    check([w draggingEntered:(id<NSDraggingInfo>)second] == NSDragOperationCopy, "a second drag is accepted");
    check([w performDragOperation:(id<NSDraggingInfo>)second] == NO, "a drop the view does not take reports failure");
    check(f.recorder.events.back() == PUGL_DROP, "the view heard the drop it did not take");
}

static void testRefuse()
{
    Fixture f;
    PuglWrapperView* const w = f.wrapper;
    f.recorder.answer = PUGL_FAILURE;

    DuskFileDropTestDrag* const drag = fileDrag();
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationNone, "a refused enter answers none");
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationNone, "a refused motion answers none");
    check([w prepareForDragOperation:(id<NSDraggingInfo>)drag] == NO, "a refused drag is not prepared for");
    [w draggingExited:(id<NSDraggingInfo>)drag];
    check(f.recorder.events == eventsOf({ PUGL_DRAG_ENTER, PUGL_DRAG_MOTION, PUGL_DRAG_LEAVE }), "a refused drag leaves");

    // a source that drops where the view said no gets a failure, and the view a leave
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationNone, "a second refused enter");
    check([w performDragOperation:(id<NSDraggingInfo>)drag] == NO, "a drop where the view refused fails");
    check(f.recorder.events.size() == 5 && f.recorder.events[4] == PUGL_DRAG_LEAVE, "a refused drop is a leave");

    // an answer that changes within a drag
    f.recorder.answer = PUGL_SUCCESS;
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "a third drag is accepted");
    f.recorder.answer = PUGL_FAILURE;
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationNone, "the answer follows the view");
    check([w prepareForDragOperation:(id<NSDraggingInfo>)drag] == NO, "the last answer decides the drop");
    [w draggingExited:(id<NSDraggingInfo>)drag];
}

static void testCopyOnly()
{
    Fixture f;
    PuglWrapperView* const w = f.wrapper;

    // Cmd held over Finder leaves move alone in the mask
    DuskFileDropTestDrag* const move = fileDrag(NSDragOperationMove);
    check([w draggingEntered:(id<NSDraggingInfo>)move] == NSDragOperationNone, "a move-only source is refused");
    check([w draggingUpdated:(id<NSDraggingInfo>)move] == NSDragOperationNone, "and stays refused");
    check([w prepareForDragOperation:(id<NSDraggingInfo>)move] == NO, "a move-only drag is not prepared for");
    check([w performDragOperation:(id<NSDraggingInfo>)move] == NO, "a move-only drop fails");
    check(std::find(f.recorder.events.begin(), f.recorder.events.end(), PUGL_DROP) == f.recorder.events.end(),
          "a move-only drop never reaches the view");
    check(! f.recorder.events.empty() && f.recorder.events.back() == PUGL_DRAG_LEAVE, "the view that had the enter leaves");

    // the mask follows the keys held during the drag
    DuskFileDropTestDrag* const drag = fileDrag(NSDragOperationCopy | NSDragOperationMove | NSDragOperationGeneric);
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "copy among others answers copy");
    drag->mask = NSDragOperationMove | NSDragOperationGeneric;
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationNone, "copy gone mid-drag is refused");
    check([w prepareForDragOperation:(id<NSDraggingInfo>)drag] == NO, "nor prepared for");
    drag->mask = NSDragOperationCopy;
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "copy back is accepted again");
    drag->mask = NSDragOperationMove;
    const size_t before = f.recorder.events.size();
    check([w performDragOperation:(id<NSDraggingInfo>)drag] == NO, "a drop after copy went fails");
    check(f.recorder.events.size() == before + 1 && f.recorder.events.back() == PUGL_DRAG_LEAVE, "and is a leave");
}

static void testNoFiles()
{
    Fixture f;
    PuglWrapperView* const w = f.wrapper;

    // Mail and Photos promise files they have yet to write
    NSFilePromiseProvider* const provider = [[NSFilePromiseProvider alloc]
        initWithFileType:@"public.midi-audio" delegate:[[DuskFileDropTestPromise new] autorelease]];
    DuskFileDropTestDrag* const promise = dragOf(pasteboardWith(@[provider]));
    [provider release];
    check([w draggingEntered:(id<NSDraggingInfo>)promise] == NSDragOperationNone, "a file promise is refused");
    check([w draggingUpdated:(id<NSDraggingInfo>)promise] == NSDragOperationNone, "and stays refused");
    check([w performDragOperation:(id<NSDraggingInfo>)promise] == NO, "its drop fails");

    DuskFileDropTestDrag* const web = dragOf(pasteboardWith(@[[NSURL URLWithString:@"https://example.com/a.wav"]]));
    check([w draggingEntered:(id<NSDraggingInfo>)web] == NSDragOperationNone, "a web URL is refused");
    [w draggingExited:(id<NSDraggingInfo>)web];

    NSPasteboard* const text = uniquePasteboard();
    [text setString:@"file:///tmp/a.wav" forType:NSPasteboardTypeString];
    check([w draggingEntered:(id<NSDraggingInfo>)dragOf(text)] == NSDragOperationNone, "text naming a file is refused");
    [w draggingExited:(id<NSDraggingInfo>)dragOf(text)];

    check(f.recorder.events.empty(), "the view never hears of a drag without files");

    // the web URL among files is skipped
    DuskFileDropTestDrag* const mixed = dragOf(pasteboardWith(@[
        [NSURL URLWithString:@"https://example.com/a.wav"], [NSURL fileURLWithPath:@"/tmp/b.wav"],
        [NSURL fileURLWithPath:@"/tmp/c.wav"] ]));
    check([w draggingEntered:(id<NSDraggingInfo>)mixed] == NSDragOperationCopy, "files among other URLs are accepted");
    check(f.recorder.files.size() == 2 && f.recorder.files[0] == "/tmp/b.wav" && f.recorder.files[1] == "/tmp/c.wav",
          "only the files, in order");
    [w draggingExited:(id<NSDraggingInfo>)mixed];
}

static void testPaths()
{
    Fixture f;
    PuglWrapperView* const w = f.wrapper;

    // Finder can hand a name in decomposed Unicode, and names may hold anything a URI escapes. A URL made
    // from a string keeps the precomposed form, which the file system does not.
    NSString* const decomposed = @"/tmp/Dusk C# e\u0301 100%?\U0001F3B5.mid";
    DuskFileDropTestDrag* const drag = dragOf(pasteboardWith(@[
        [NSURL fileURLWithPath:decomposed], [NSURL URLWithString:@"file:///tmp/caf%C3%A9.mid"] ]));
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "unusual names are accepted");
    check(f.recorder.files.size() == 2, "both names arrive");
    check(f.recorder.files.size() > 0 && f.recorder.files[0] == "/tmp/Dusk C# e\xCC\x81 100%?\xF0\x9F\x8E\xB5.mid",
          "a decomposed name arrives byte for byte");
    check(f.recorder.files.size() > 1 && f.recorder.files[1] == "/tmp/cafe\xCC\x81.mid",
          "a name arrives in the form the file system keeps");
    [w draggingExited:(id<NSDraggingInfo>)drag];

    // a file reference URL, as Finder writes them, names a path only once resolved
    NSString* const directory = [NSTemporaryDirectory() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
    [[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:nil error:nil];
    NSString* const file = [directory stringByAppendingPathComponent:@"referenced.wav"];
    check([[NSData data] writeToFile:file atomically:NO], "a referenced file is made");
    NSURL* const reference = [[NSURL fileURLWithPath:file] fileReferenceURL];
    check(reference != nil && [reference isFileReferenceURL], "it has a reference URL");
    DuskFileDropTestDrag* const referenced = dragOf(pasteboardWith(@[reference]));
    check([w draggingEntered:(id<NSDraggingInfo>)referenced] == NSDragOperationCopy, "a file reference is accepted");
    // resolved, the temporary directory is under /private
    char real[PATH_MAX] = {};
    check(realpath([file fileSystemRepresentation], real) != nullptr, "the referenced file has a real path");
    check(f.recorder.files.size() == 1 && f.recorder.files[0] == real, "a file reference arrives as its path");
    [w draggingExited:(id<NSDraggingInfo>)referenced];
    [[NSFileManager defaultManager] removeItemAtPath:directory error:nil];
}

// A drag driven to the point where the view hears @a type
static void driveTo(PuglWrapperView* const w, DuskFileDropTestDrag* const drag, const PuglEventType type,
                    NSDragOperation& answer, BOOL& performed)
{
    answer = NSDragOperationCopy;
    performed = YES;

    switch (type)
    {
    case PUGL_DRAG_ENTER:
        answer = [w draggingEntered:(id<NSDraggingInfo>)drag];
        break;
    case PUGL_DRAG_MOTION:
        [w draggingEntered:(id<NSDraggingInfo>)drag];
        answer = [w draggingUpdated:(id<NSDraggingInfo>)drag];
        break;
    case PUGL_DRAG_LEAVE:
        [w draggingEntered:(id<NSDraggingInfo>)drag];
        [w draggingExited:(id<NSDraggingInfo>)drag];
        break;
    case PUGL_DROP:
        [w draggingEntered:(id<NSDraggingInfo>)drag];
        performed = [w performDragOperation:(id<NSDraggingInfo>)drag];
        break;
    default:
        break;
    }
}

static const char* nameOf(const PuglEventType type)
{
    switch (type)
    {
    case PUGL_DRAG_ENTER: return "enter";
    case PUGL_DRAG_MOTION: return "motion";
    case PUGL_DRAG_LEAVE: return "leave";
    case PUGL_DROP: return "drop";
    default: return "?";
    }
}

// Every call a retired wrapper can get answers none and reaches no view
static void checkSilent(Fixture& f, DuskFileDropTestDrag* const drag, const std::string& when)
{
    PuglWrapperView* const w = f.wrapper;
    const size_t before = f.recorder.events.size();
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationNone, (when + ": a late enter answers none").c_str());
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationNone, (when + ": a late motion answers none").c_str());
    check([w prepareForDragOperation:(id<NSDraggingInfo>)drag] == NO, (when + ": a late drag is not prepared for").c_str());
    check([w performDragOperation:(id<NSDraggingInfo>)drag] == NO, (when + ": a late drop fails").c_str());
    [w draggingExited:(id<NSDraggingInfo>)drag];
    [w draggingEnded:(id<NSDraggingInfo>)drag];
    check(f.recorder.events.size() == before, (when + ": nothing reaches the view").c_str());
    check(! f.registered(), (when + ": the wrapper is no longer registered").c_str());
}

static void testDeletedInside(const PuglEventType type)
{
    const std::string when = std::string("view freed inside its ") + nameOf(type);
    Fixture f;
    f.recorder.freeOn = type;
    const CloseCounter window([f.wrapper window]);
    check([window.window isVisible], (when + ": the window is on screen").c_str());

    DuskFileDropTestDrag* const drag = fileDrag();
    NSDragOperation answer;
    BOOL performed;
    driveTo(f.wrapper, drag, type, answer, performed);

    check(f.recorder.view == nullptr, (when + ": the view went").c_str());
    check(! f.recorder.events.empty() && f.recorder.events.back() == type, (when + ": the callback ran last").c_str());

    // AppKit tells the source Generic when the window closes during the drop
    check(window.count() == 0 && ! [window.window isVisible], (when + ": its window is hidden, not yet closed").c_str());
    turnRunLoop();
    check(window.count() == 1, (when + ": and closed once the call has returned").c_str());

    // cut off from the view that went, whatever AppKit still sends it
    PuglWindow* const native = static_cast<PuglWindow*>(window.window);
    check(native->puglview == nullptr && [native delegate] == nil, (when + ": the window no longer knows the view").c_str());
    [native setIsVisible:NO];
    [native setIsZoomed:NO];
    [native dispatchCurrentConfiguration];
    if (type == PUGL_DRAG_ENTER || type == PUGL_DRAG_MOTION)
        check(answer == NSDragOperationNone, (when + ": the drag is refused").c_str());
    if (type == PUGL_DROP)
        check(performed == YES, (when + ": the view's answer stands").c_str());
    if (type != PUGL_DRAG_LEAVE)
        check(f.recorder.files.size() == 1 && f.recorder.files[0] == [kPath fileSystemRepresentation],
              (when + ": the files outlive the view until the callback returns").c_str());

    checkSilent(f, drag, when);
}

// AppKit need not hold the wrapper while it calls in: freeing the view inside a callback can let it go
static void testDeletedAlone(const PuglEventType type)
{
    const std::string when = std::string("view freed inside its ") + nameOf(type) + ", wrapper held by nobody else";
    Fixture f;
    f.recorder.freeOn = type;

    PuglWrapperView* const w = f.wrapper;
    [f.wrapper release];
    f.wrapper = nil;

    NSDragOperation answer;
    BOOL performed;
    driveTo(w, fileDrag(), type, answer, performed);

    check(f.recorder.view == nullptr, (when + ": the view went").c_str());
    if (type == PUGL_DRAG_ENTER || type == PUGL_DRAG_MOTION)
        check(answer == NSDragOperationNone, (when + ": the drag is refused").c_str());
    if (type == PUGL_DROP)
        check(performed == YES, (when + ": the view's answer stands").c_str());
}

static void testOptOutInside(const PuglEventType type)
{
    const std::string when = std::string("drops turned off inside the ") + nameOf(type);
    Fixture f;
    f.recorder.optOutOn = type;

    DuskFileDropTestDrag* const drag = fileDrag();
    NSDragOperation answer;
    BOOL performed;
    driveTo(f.wrapper, drag, type, answer, performed);

    const size_t heard = f.recorder.events.size();
    check(! f.registered(), (when + ": the wrapper is no longer registered").c_str());
    if (type == PUGL_DRAG_ENTER || type == PUGL_DRAG_MOTION)
        check(answer == NSDragOperationNone, (when + ": the drag is refused").c_str());
    if (type == PUGL_DROP)
        check(performed == YES, (when + ": the view's answer stands").c_str());

    PuglWrapperView* const w = f.wrapper;
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationNone, (when + ": the rest is refused").c_str());
    check([w prepareForDragOperation:(id<NSDraggingInfo>)drag] == NO, (when + ": and not prepared for").c_str());
    check([w performDragOperation:(id<NSDraggingInfo>)drag] == NO, (when + ": and its drop fails").c_str());
    [w draggingExited:(id<NSDraggingInfo>)drag];
    check(f.recorder.events.size() == heard, (when + ": the view hears nothing more").c_str());
    check(std::count(f.recorder.events.begin(), f.recorder.events.end(), PUGL_DRAG_LEAVE) <= 1,
          (when + ": at most one leave").c_str());

    check([w draggingEntered:(id<NSDraggingInfo>)fileDrag()] == NSDragOperationNone,
          (when + ": a later drag is refused").c_str());
    check(f.recorder.events.size() == heard, (when + ": and never heard of").c_str());
}

// Turning drops off from outside a callback, during a drag; the leave it sends runs a nested event loop
static void testOptOutOutside()
{
    Fixture f;
    PuglWrapperView* const w = f.wrapper;

    DuskFileDropTestDrag* const drag = fileDrag();
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "a drag is accepted before the opt-out");

    DuskFileDropTestDrag* const next = fileDrag();
    NSDragOperation nestedEnter = NSDragOperationCopy, nestedMotion = NSDragOperationCopy;
    BOOL nestedPrepare = YES;
    size_t heardInLeave = 0;
    PuglView* const view = f.recorder.view;
    f.recorder.during = [&] {
        // the leave turns drops back on, and AppKit delivers the next drag meanwhile
        check(puglSetAcceptsFileDrops(view, true) == PUGL_SUCCESS, "drops turn back on inside the leave");
        nestedEnter = [w draggingEntered:(id<NSDraggingInfo>)next];
        nestedMotion = [w draggingUpdated:(id<NSDraggingInfo>)next];
        nestedPrepare = [w prepareForDragOperation:(id<NSDraggingInfo>)next];
        heardInLeave = f.recorder.events.size();
    };

    check(puglSetAcceptsFileDrops(view, false) == PUGL_SUCCESS, "turning drops off succeeds");
    check(f.recorder.events == eventsOf({ PUGL_DRAG_ENTER, PUGL_DRAG_LEAVE, PUGL_DRAG_ENTER, PUGL_DRAG_MOTION }),
          "the view leaves, and hears the next drag once the opt-out is done");
    check(! f.recorder.nested, "no callback runs inside another");
    check(heardInLeave == 2, "nothing reaches the view inside its leave");
    check(nestedEnter == NSDragOperationNone && nestedMotion == NSDragOperationNone && nestedPrepare == NO,
          "a drag inside the leave is refused until it is handled");
    check(f.registered(), "the view is registered again");
    check([w draggingUpdated:(id<NSDraggingInfo>)next] == NSDragOperationCopy, "the next drag is accepted after");
    check([w performDragOperation:(id<NSDraggingInfo>)next] == YES, "and dropped");
    check(f.recorder.events.back() == PUGL_DROP, "the drop reaches the view");

    // plain: the rest of the drag is refused
    check([w draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "a third drag is accepted");
    check(puglSetAcceptsFileDrops(f.recorder.view, false) == PUGL_SUCCESS, "turning drops off again");
    const size_t heard = f.recorder.events.size();
    check(f.recorder.events.back() == PUGL_DRAG_LEAVE, "the view that had the enter leaves");
    check(! f.registered(), "the wrapper is no longer registered");
    check([w draggingUpdated:(id<NSDraggingInfo>)drag] == NSDragOperationNone, "the rest is refused");
    check([w performDragOperation:(id<NSDraggingInfo>)drag] == NO, "its drop fails");
    [w draggingExited:(id<NSDraggingInfo>)drag];
    check(f.recorder.events.size() == heard, "and the view hears nothing more");
}

static void testRetired()
{
    {
        Fixture f;
        const CloseCounter window([f.wrapper window]);
        DuskFileDropTestDrag* const drag = fileDrag();
        check([f.wrapper draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "a drag before unrealize");
        check(puglUnrealize(f.recorder.view) == PUGL_SUCCESS, "the view unrealizes mid-drag");
        check(window.count() == 1, "outside a call of ours the window closes at once");
        check(f.recorder.events == eventsOf({ PUGL_DRAG_ENTER }), "a view going away hears no leave");
        checkSilent(f, drag, "unrealized");
    }
    {
        Fixture f;
        DuskFileDropTestDrag* const drag = fileDrag();
        check([f.wrapper draggingEntered:(id<NSDraggingInfo>)drag] == NSDragOperationCopy, "a drag before free");
        puglFreeView(f.recorder.view);
        f.recorder.view = nullptr;
        checkSilent(f, drag, "freed");
    }
}

static void testOptIn()
{
    {
        Fixture f(false);
        check(! f.registered(), "a view that never asks is not registered");
        check([f.wrapper draggingEntered:(id<NSDraggingInfo>)fileDrag()] == NSDragOperationNone, "and refuses drags");
        check(f.recorder.events.empty(), "without hearing of them");
        check(puglSetAcceptsFileDrops(f.recorder.view, false) == PUGL_SUCCESS, "turning drops off always succeeds");
    }
    {
        PuglWorld* const world = puglNewWorld(PUGL_MODULE, 0);
        PuglView* const view = puglNewView(world);
        puglSetBackend(view, puglStubBackend());
        puglSetEventFunc(view, onEvent);
        check(puglSetAcceptsFileDrops(view, true) == PUGL_FAILURE, "an unrealized view cannot accept drops");

        NSView* const parent = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 320, 240)];
        puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 320, 240);
        puglSetParent(view, reinterpret_cast<PuglNativeView>(parent));
        check(puglRealize(view) == PUGL_SUCCESS, "an embedded view realizes");
        check(puglSetAcceptsFileDrops(view, true) == PUGL_UNSUPPORTED, "an embedded view cannot accept drops");
        check(! [[view->impl->wrapperView registeredDraggedTypes] containsObject:NSPasteboardTypeFileURL],
              "an embedded view is not registered");
        puglFreeView(view);
        [parent release];
        puglFreeWorld(world);
    }
}

// A Retina window wherever the test runs, so that points and pixels differ
static CGFloat doubledBackingScaleFactor(id self, SEL selector)
{
    (void)self; (void)selector;
    return 2.0;
}

int main()
{
    @autoreleasepool
    {
        class_replaceMethod([PuglWindow class], @selector(backingScaleFactor),
                            reinterpret_cast<IMP>(doubledBackingScaleFactor), "d@:");
        pasteboards = [NSMutableArray array];
        testAccept();
        testRefuse();
        testCopyOnly();
        testNoFiles();
        testPaths();

        const PuglEventType callbacks[] = { PUGL_DRAG_ENTER, PUGL_DRAG_MOTION, PUGL_DRAG_LEAVE, PUGL_DROP };
        for (const PuglEventType type : callbacks)
        {
            testDeletedInside(type);
            testDeletedAlone(type);
            testOptOutInside(type);
        }

        testOptOutOutside();
        testRetired();
        testOptIn();

        for (NSPasteboard* pasteboard in pasteboards)
            [pasteboard releaseGlobally];
    }

    if (failures != 0)
    {
        d_stderr2("FileDropCocoa: %d checks failed", failures);
        return 1;
    }

    d_stdout("FileDropCocoa: all checks passed");
    return 0;
}
