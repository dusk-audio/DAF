// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Interactive test: a DGL window drags a file with Window::startFileDrag onto DGL windows that accept drops
// (see dgl/src/drop-targets.md). The first drag goes to a window that takes it, with Option held, and deletes
// itself inside onFileDrop; the second to one that refuses.
//
//   file-drop-native                  injects its own gestures: needs existing event-posting access and the
//                                     foreground, and a desktop with no mouse button held
//   file-drop-native --external N     waits up to N seconds for those two drags from real input of another
//                                     origin (a remote desktop), which needs no access; prints where to drag
//   file-drop-native --wait N [--delete]
//                                     only a window that accepts drops, printing what it hears for N seconds;
//                                     with --delete it deletes itself inside its first onFileDrop
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "dgl/Application.hpp"
#include "dgl/TopLevelWidget.hpp"
#include "dgl/Window.hpp"
#import <Cocoa/Cocoa.h>
#import <ApplicationServices/ApplicationServices.h>
#import <objc/runtime.h>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using DGL_NAMESPACE::Application;
using DGL_NAMESPACE::TopLevelWidget;
using DGL_NAMESPACE::Window;

static int checks = 0, failures = 0;
static void check(const bool ok, const char* const text)
{
    ++checks;
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    if (!ok) ++failures;
}

// What the source was told at the end of its drag, from the DGL drag source's own callback
static std::vector<NSDragOperation> sourceOperations;
static IMP sourceEnded = nullptr;
static void recordSourceEnded(id self, SEL selector, NSDraggingSession* session, NSPoint point, NSDragOperation operation)
{
    sourceOperations.push_back(operation);
    std::printf("SOURCE ended operation %lu\n", (unsigned long)operation);
    std::fflush(stdout);
    reinterpret_cast<void (*)(id, SEL, NSDraggingSession*, NSPoint, NSDragOperation)>(sourceEnded)(self, selector, session, point, operation);
}
static bool watchSource()
{
    const SEL selector = @selector(draggingSession:endedAtPoint:operation:);
    unsigned count = 0;
    Class* const classes = objc_copyClassList(&count);
    bool found = false;
    for (unsigned i = 0; i < count && !found; ++i)
    {
        const char* const name = class_getName(classes[i]);
        const size_t length = std::strlen(name);
        if (length < 15 || std::strcmp(name + length - 15, "_FileDragSource") != 0) continue;
        if (Method method = class_getInstanceMethod(classes[i], selector))
        {
            sourceEnded = method_setImplementation(method, reinterpret_cast<IMP>(recordSourceEnded));
            found = true;
        }
    }
    std::free(classes);
    return found;
}

// A window that starts a drag of its file when pressed
class Source : public TopLevelWidget
{
public:
    std::string path;
    int started = 0;
    bool verbose = false;
    explicit Source(Window& window) : TopLevelWidget(window) {}
protected:
    void onDisplay() override {}
    bool onMouse(const MouseEvent& ev) override
    {
        if (ev.press && ev.button == 1 && getWindow().startFileDrag(path.c_str())) ++started;
        if (verbose) { std::printf("SOURCE %s button %u, drags started %d\n", ev.press ? "press" : "release", ev.button, started); std::fflush(stdout); }
        return true;
    }
};

struct Heard {
    int enters = 0, moves = 0, leaves = 0, drops = 0;
    std::vector<std::string> files;
    unsigned mod = 0;
    double x = 0, y = 0;
    double expectedX = -1, expectedY = -1; // where the pointer was at the drop, in view pixels
    bool deleted = false;
};

class Target : public Window
{
public:
    Heard& heard;
    const bool accept, deleteOnDrop, print;
    Target(Application& app, Heard& h, const bool a, const bool d, const bool p = false)
        : Window(app), heard(h), accept(a), deleteOnDrop(d), print(p)
    {
        setTitle(accept ? "Dusk drop target (accepts)" : "Dusk drop target (refuses)");
        setSize(240, 220);
        setAcceptingFileDrops(true);
    }
protected:
    void record(const char* const what, const FileDropEvent& ev)
    {
        heard.files.assign(ev.files, ev.files + ev.numFiles);
        heard.mod = ev.mod;
        heard.x = ev.pos.getX();
        heard.y = ev.pos.getY();
        if (!print) return;
        std::printf("%s x=%.1f y=%.1f mod=0x%x files=%u\n", what, heard.x, heard.y, ev.mod, ev.numFiles);
        for (uint i = 0; i < ev.numFiles; ++i) std::printf("  file %s\n", ev.files[i]);
        std::fflush(stdout);
    }
    bool onFileDragEnter(const FileDropEvent& ev) override { ++heard.enters; record("ENTER", ev); return accept; }
    bool onFileDragMove(const FileDropEvent& ev) override { ++heard.moves; record("MOVE", ev); return accept; }
    void onFileDragLeave() override { ++heard.leaves; if (print) { std::printf("LEAVE\n"); std::fflush(stdout); } }
    bool onFileDrop(const FileDropEvent& ev) override
    {
        ++heard.drops;
        NSWindow* const window = [reinterpret_cast<NSView*>(getNativeWindowHandle()) window];
        const NSPoint local = [window convertPointFromScreen:[NSEvent mouseLocation]];
        const double scale = [window backingScaleFactor];
        heard.expectedX = local.x * scale;
        heard.expectedY = ([[window contentView] frame].size.height - local.y) * scale;
        Heard& h = heard;
        const bool printing = print;
        if (deleteOnDrop) { close(); delete this; h.deleted = true; }
        // the files stay readable until this callback returns, the window gone or not
        h.files.assign(ev.files, ev.files + ev.numFiles);
        h.mod = ev.mod;
        h.x = ev.pos.getX();
        h.y = ev.pos.getY();
        if (printing)
        {
            std::printf("DROP x=%.1f y=%.1f mod=0x%x files=%u\n", h.x, h.y, h.mod, ev.numFiles);
            for (uint i = 0; i < ev.numFiles; ++i) std::printf("  file %s\n", ev.files[i]);
            std::fflush(stdout);
        }
        return true;
    }
};

static void mouse(const CGEventType type, const CGPoint point, const CGEventFlags flags = 0)
{
    CGEventRef event = CGEventCreateMouseEvent(nullptr, type, point, kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, 1);
    CGEventSetFlags(event, flags);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}
static void option(const bool down)
{
    CGEventRef event = CGEventCreateKeyboardEvent(nullptr, 58, down); // kVK_Option
    CGEventSetFlags(event, down ? kCGEventFlagMaskAlternate : 0);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}
static NSWindow* nativeWindow(Window& window)
{
    return [reinterpret_cast<NSView*>(window.getNativeWindowHandle()) window];
}
// A point of a window's content, given from its top left in points, on the screen from its top left
static CGPoint screenPoint(NSWindow* window, const double x, const double y)
{
    const NSPoint point = [window convertPointToScreen:NSMakePoint(x, [[window contentView] frame].size.height - y)];
    return CGPointMake(point.x, CGDisplayBounds(CGMainDisplayID()).size.height - point.y);
}
static void pump(Application& app, const int times)
{
    for (int i = 0; i < times; ++i) { app.idle(); std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
}

// Press on the source, move onto the target and let go there
static void inject(Application& app, NSWindow* const source, NSWindow* const target, const bool holdOption,
                   const std::function<bool()>& done)
{
    const CGPoint start = screenPoint(source, 70, 70);
    const CGPoint end = screenPoint(target, 90, 80);
    mouse(kCGEventMouseMoved, start);
    mouse(kCGEventLeftMouseDown, start);
    std::thread driver([=] {
        const CGEventFlags flags = holdOption ? kCGEventFlagMaskAlternate : 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        if (holdOption) option(true);
        for (int i = 1; i <= 10; ++i)
        {
            const CGPoint p = CGPointMake(start.x + (end.x - start.x) * i / 10, start.y + (end.y - start.y) * i / 10);
            mouse(kCGEventLeftMouseDragged, p, flags);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        mouse(kCGEventLeftMouseUp, end, flags);
        if (holdOption) option(false);
    });
    for (int i = 0; i < 400 && !done(); ++i) pump(app, 1);
    driver.join();
    pump(app, 30);
}

static int waitForDrops(const int seconds, const bool deleteOnDrop)
{
    Application app(true);
    Heard heard;
    Target* const target = new Target(app, heard, true, deleteOnDrop, true);
    target->show();
    std::printf("waiting %d s for drops\n", seconds);
    std::fflush(stdout);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < until && !heard.deleted) pump(app, 1);
    pump(app, 30);
    if (!heard.deleted) { target->close(); delete target; }
    app.quit();
    return 0;
}

int main(int argc, char** argv)
{
    NSAutoreleasePool* pool = [NSAutoreleasePool new];
    if (argc > 2 && std::strcmp(argv[1], "--wait") == 0)
    {
        const int r = waitForDrops(std::atoi(argv[2]), argc > 3 && std::strcmp(argv[3], "--delete") == 0);
        [pool drain];
        return r;
    }
    const int external = argc > 2 && std::strcmp(argv[1], "--external") == 0 ? std::atoi(argv[2]) : 0;

    if (external == 0)
    {
        check(CGPreflightPostEventAccess(), "native event posting is already authorized");
        if (failures) return 1;
    }
    NSRunningApplication* previous = [[[NSWorkspace sharedWorkspace] frontmostApplication] retain];
    CGEventRef cursor = CGEventCreate(nullptr);
    const CGPoint saved = CGEventGetLocation(cursor);
    CFRelease(cursor);

    NSString* directory = [NSTemporaryDirectory() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
    check([[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:NO attributes:nil error:nil],
          "private test directory creates");
    NSString* file = [directory stringByAppendingPathComponent:@"Dusk drop C# é \U0001F3B5.mid"];
    check([[NSData dataWithBytes:"MThd" length:4] writeToFile:file atomically:YES], "owned Unicode file fixture creates");

    std::unique_ptr<Application> app(new Application(true));
    std::unique_ptr<Window> source(new Window(*app));
    source->setTitle("Dusk drag source");
    source->setSize(220, 180);
    std::unique_ptr<Source> sourceWidget(new Source(*source));
    sourceWidget->path = [file fileSystemRepresentation];
    source->show();
    Heard taking, refusing;
    sourceWidget->verbose = external != 0;
    Target* taker = new Target(*app, taking, true, true, external != 0);
    std::unique_ptr<Target> refuser(new Target(*app, refusing, false, false, external != 0));
    check(taker->isAcceptingFileDrops() && refuser->isAcceptingFileDrops(), "top-level windows accept drops on macOS");
    taker->show();
    refuser->show();
    NSWindow* const sourceWindow = nativeWindow(*source);
    NSWindow* const takerWindow = nativeWindow(*taker);
    NSWindow* const refuserWindow = nativeWindow(*refuser);
    [sourceWindow setFrameOrigin:NSMakePoint(40, 300)];
    [takerWindow setFrameOrigin:NSMakePoint(420, 300)];
    [refuserWindow setFrameOrigin:NSMakePoint(720, 300)];
    check(watchSource(), "the DGL drag source can be watched");
    pump(*app, 30);

    const auto takenDone = [&] { return taking.drops > 0 && sourceOperations.size() > 0; };
    const auto refusedDone = [&] { return refusing.leaves > 0 && sourceOperations.size() > 1; };

    if (external != 0)
    {
        const CGPoint s = screenPoint(sourceWindow, 70, 70), t = screenPoint(takerWindow, 90, 80),
                      r = screenPoint(refuserWindow, 90, 80);
        std::printf("DRAG from %.0f,%.0f to %.0f,%.0f with Option held, then from %.0f,%.0f to %.0f,%.0f\n",
                    s.x, s.y, t.x, t.y, s.x, s.y, r.x, r.y);
        std::fflush(stdout);
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(external);
        while (std::chrono::steady_clock::now() < until && !(takenDone() && refusedDone())) pump(*app, 1);
        pump(*app, 30);
    }
    else
    {
        // a click of the test's own makes it the active application wherever it was started from
        [NSApp activateIgnoringOtherApps:YES];
        [sourceWindow makeKeyAndOrderFront:nil];
        pump(*app, 10);
        const bool foreground = [NSApp isActive] && [NSApp keyWindow] == sourceWindow;
        check(foreground, "test owns the foreground source before injecting its gesture");
        if (foreground && !CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft))
        {
            inject(*app, sourceWindow, takerWindow, true, takenDone);
            inject(*app, sourceWindow, refuserWindow, false, refusedDone);
        }
        CGWarpMouseCursorPosition(saved);
    }

    check(sourceWidget->started == 2, "the source starts a drag from each real press");
    check(taking.enters == 1, "the accepting window hears one enter");
    check(taking.drops == 1 && taking.leaves == 0, "and the drop, with no leave");
    check(taking.deleted, "it deletes itself inside onFileDrop");
    check(taking.files.size() == 1 && taking.files[0] == [file fileSystemRepresentation],
          "the drop carries the exact path, read after the window went");
    check((taking.mod & DGL_NAMESPACE::kModifierAlt) != 0, "the drop carries Option");
    check(std::abs(taking.x - taking.expectedX) <= 2 && std::abs(taking.y - taking.expectedY) <= 2,
          "the drop is at the pointer, in view pixels from the top left");
    std::printf("  drop at %.1f,%.1f (pointer %.1f,%.1f) mod=0x%x\n", taking.x, taking.y, taking.expectedX,
                taking.expectedY, taking.mod);
    check(refusing.enters == 1 && refusing.leaves == 1, "the refusing window hears enter and leave");
    check(refusing.drops == 0, "and no drop");
    check(sourceOperations.size() == 2, "the source hears the end of both drags");
    check(sourceOperations.size() > 0 && sourceOperations[0] == NSDragOperationCopy, "the source of the taken drop is told copy");
    check(sourceOperations.size() > 1 && sourceOperations[1] == NSDragOperationNone, "the source of the refused drag is told none");

    refuser->close();
    refuser.reset();
    if (!taking.deleted) { taker->close(); delete taker; }
    sourceWidget.reset();
    source->close();
    source.reset();
    app->quit();
    app.reset();
    [[NSFileManager defaultManager] removeItemAtPath:directory error:nil];
    [previous activateWithOptions:0];
    [previous release];
    std::printf("Native Cocoa DGL file drop: %d checks, %d failures\n", checks, failures);
    [pool drain];
    return failures ? 1 : 0;
}
