// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Interactive test: requires existing event-posting access and its own foreground windows.
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "dgl/src/pugl.mm"
#import <ApplicationServices/ApplicationServices.h>
#include <thread>
#include <chrono>
#include <memory>
#include <string>
#include <unistd.h>

static int checks=0,failures=0;
static void check(bool ok,const char* text)
{++checks;std::printf("%s: %s\n",ok?"PASS":"FAIL",text);std::fflush(stdout);if(!ok)++failures;}
struct DropState {
    PuglView* view;NSString* path;bool dropped=false;int enters=0;
    DropState(PuglView* owner,NSString* file):view(owner),path(file){}
};
@interface DuskNativeDropFixture : NSView <NSDraggingDestination> {
@public DropState* state;
}
@end
@implementation DuskNativeDropFixture
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info
{
    ++state->enters;
    check([info draggingSourceOperationMask]==NSDragOperationCopy,"native Cocoa source offers copy only");
    return NSDragOperationCopy;
}
- (BOOL)prepareForDragOperation:(id<NSDraggingInfo>)info {(void)info;return YES;}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)info
{
    state->dropped=true;
    puglFreeView(state->view);state->view=nullptr;
    NSArray* files=[[info draggingPasteboard] readObjectsForClasses:@[[NSURL class]]
        options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
    check([files count]==1,"native destination receives exactly one file URL after source closure");
    NSURL* url=[files count]?[files objectAtIndex:0]:nil;
    // The native pasteboard returns the filesystem's decomposed Unicode form.
    check([[[url path] precomposedStringWithCanonicalMapping] isEqualToString:
        [state->path precomposedStringWithCanonicalMapping]],"native destination preserves the complete Unicode path under canonical normalization");
    NSData* bytes=[NSData dataWithContentsOfURL:url];
    check([bytes length]==4&&std::memcmp([bytes bytes],"MThd",4)==0,"destination reads original file after native source destruction");
    return YES;
}
@end

static void mouse(CGEventType type,CGPoint point)
{
    CGEventRef event=CGEventCreateMouseEvent(nullptr,type,point,kCGMouseButtonLeft);
    CGEventSetIntegerValueField(event,kCGMouseEventClickState,1);CGEventPost(kCGHIDEventTap,event);CFRelease(event);
}
static CGPoint screenPoint(NSWindow* window,NSPoint local)
{
    NSPoint point=[window convertPointToScreen:local];
    return CGPointMake(point.x,CGDisplayBounds(CGMainDisplayID()).size.height-point.y);
}
int main()
{
    NSAutoreleasePool* pool=[NSAutoreleasePool new];
    check(CGPreflightPostEventAccess(),"native event posting is already authorized");if(failures)return 1;
    NSRunningApplication* previous=[[[NSWorkspace sharedWorkspace] frontmostApplication] retain];
    CGEventRef cursor=CGEventCreate(nullptr);const CGPoint saved=CGEventGetLocation(cursor);CFRelease(cursor);
    NSString* directory=[NSTemporaryDirectory() stringByAppendingPathComponent:[[NSUUID UUID] UUIDString]];
    const bool created=[[NSFileManager defaultManager] createDirectoryAtPath:directory
        withIntermediateDirectories:NO attributes:nil error:nil];
    check(created,"private native test directory creates");if(!created)return 1;
    NSString* path=[directory stringByAppendingPathComponent:@"Dusk native C# é 🎵.mid"];
    check([[NSData dataWithBytes:"MThd" length:4] writeToFile:path atomically:YES],"owned Unicode file fixture creates");
    PuglWorld* world=puglNewWorld(PUGL_PROGRAM,0);
    PuglView* view=puglNewView(world);puglSetBackend(view,puglStubBackend());puglSetSizeHint(view,PUGL_DEFAULT_SIZE,220,180);
    puglSetEventFunc(view,[](PuglView*,const PuglEvent*){return PUGL_SUCCESS;});
    check(puglRealize(view)==PUGL_SUCCESS,"native source realizes");puglShow(view,PUGL_SHOW_RAISE);
    NSWindow* source=[view->impl->wrapperView window];[source setFrameOrigin:NSMakePoint(40,200)];
    DropState state{view,path};
    NSWindow* destination=[[NSWindow alloc] initWithContentRect:NSMakeRect(420,200,240,220)
        styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    [destination setTitle:@"Dusk isolated file destination"];
    DuskNativeDropFixture* target=[[DuskNativeDropFixture alloc] initWithFrame:NSMakeRect(0,0,240,220)];
    target->state=&state;[target registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
    [destination setContentView:target];[target release];[destination orderFront:nil];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];[NSApp activateIgnoringOtherApps:YES];[source makeKeyAndOrderFront:nil];
    for(int i=0;i<30;++i){puglUpdate(world,0);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
    const bool foreground=[NSApp isActive]&&[NSApp keyWindow]==source;
    check(foreground,"test owns the foreground source before injecting its gesture");
    std::unique_ptr<PuglFileDrag> operation;
    bool pressed=false;
    if(foreground&&!CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState,kCGMouseButtonLeft))
    {
        const CGPoint start=screenPoint(source,NSMakePoint(70,70)),end=screenPoint(destination,NSMakePoint(90,80));
        mouse(kCGEventMouseMoved,start);mouse(kCGEventLeftMouseDown,start);pressed=true;
        for(int i=0;i<20;++i){puglUpdate(world,0);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        operation.reset(puglStartFileDrag(view,[path fileSystemRepresentation]));
        check(bool(operation),"native source accepts its real physical primary gesture");
        if(operation)
        {
            std::thread driver([=]{std::this_thread::sleep_for(std::chrono::milliseconds(200));
                mouse(kCGEventLeftMouseDragged,end);std::this_thread::sleep_for(std::chrono::milliseconds(300));mouse(kCGEventLeftMouseUp,end);});
            bool active=operation->idle();
            for(int i=0;i<250&&active;++i){puglUpdate(world,0);active=operation->idle();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
            driver.join();check(state.enters>0&&state.dropped,"real Cocoa dragging session delivers to native destination");
            check(state.view==nullptr,"native drop destroys the source view during delivery");
        }
    }
    if(pressed)mouse(kCGEventLeftMouseUp,saved);
    if(operation)operation->cancel();operation.reset();
    if(state.view)puglFreeView(state.view);puglFreeWorld(world);
    [destination orderOut:nil];[destination release];
    [[NSFileManager defaultManager] removeItemAtPath:directory error:nil];
    CGWarpMouseCursorPosition(saved);[previous activateWithOptions:0];[previous release];
    std::printf("Native Cocoa file drop: %d checks, %d failures\n",checks,failures);
    [pool drain];return failures?1:0;
}
