// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "dgl/src/pugl.mm"
#include <cstdio>
#include <memory>

int main()
{
    NSAutoreleasePool* pool=[NSAutoreleasePool new];
    NSString* fixture = [NSTemporaryDirectory() stringByAppendingPathComponent:
        [NSString stringWithFormat:@"Dusk file drag %d C# é.mid", [[NSProcessInfo processInfo] processIdentifier]]];
    if (![[NSData dataWithBytes:"MThd" length:4] writeToFile:fixture atomically:YES]) return 1;
    const char* filename = [fixture fileSystemRepresentation];
    int checks=0,failures=0;
    const auto check=[&](bool ok,const char* text){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL: %s\n",text);}};
    PuglWorld* world=puglNewWorld(PUGL_PROGRAM,0);check(world!=nullptr,"native Cocoa world creates");if(!world)return 1;
    PuglView* view=puglNewView(world);if(!view)return 1;
    puglSetBackend(view,puglStubBackend());puglSetSizeHint(view,PUGL_DEFAULT_SIZE,160,160);
    puglSetEventFunc(view,[](PuglView*,const PuglEvent*){return PUGL_SUCCESS;});
    check(puglRealize(view)==PUGL_SUCCESS,"native Cocoa source view realizes");
    check(puglShow(view,PUGL_SHOW_RAISE)==PUGL_SUCCESS,"source view shows");puglUpdate(world,0);
    NSView* native=view->impl->wrapperView;NSWindow* window=[native window];
    check(window!=nil,"source view belongs to a native window");
    auto makeEvent=[&](NSEventType type){return [NSEvent mouseEventWithType:type location:NSMakePoint(40,40)
        modifierFlags:0 timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber]
        context:nil eventNumber:1 clickCount:1 pressure:1];};
    std::unique_ptr<PuglFileDrag> before(puglStartFileDrag(view,filename));
    check(!before,"a source without a primary gesture cannot start a drag");
    [native mouseDown:makeEvent(NSEventTypeLeftMouseDown)];
    std::unique_ptr<PuglFileDrag> down(puglStartFileDrag(view,filename));
    check(bool(down),"the actual native primary press is retained for a later UI idle/render callback");
    if(down){down->cancel();down.reset();}
    [native mouseDragged:makeEvent(NSEventTypeLeftMouseDragged)];
    std::unique_ptr<PuglFileDrag> moved(puglStartFileDrag(view,filename));
    check(bool(moved),"the actual native drag event can start the outgoing file handoff");
    if(moved){moved->cancel();moved.reset();}
    [native mouseDragged:makeEvent(NSEventTypeLeftMouseDragged)];
    NSEvent* other=[NSEvent otherEventWithType:NSEventTypeApplicationDefined location:NSZeroPoint modifierFlags:0
        timestamp:[[NSProcessInfo processInfo] systemUptime] windowNumber:[window windowNumber]
        context:nil subtype:0 data1:183 data2:0];
    [NSApp postEvent:other atStart:YES];
    [NSApp nextEventMatchingMask:NSEventMaskApplicationDefined untilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]
        inMode:NSDefaultRunLoopMode dequeue:YES];
    std::unique_ptr<PuglFileDrag> delayed(puglStartFileDrag(view,filename));
    check(bool(delayed),"an unrelated most-recent AppKit event does not lose this view's drag gesture");
    if(delayed){delayed->cancel();delayed.reset();}
    [native mouseUp:makeEvent(NSEventTypeLeftMouseUp)];
    std::unique_ptr<PuglFileDrag> released(puglStartFileDrag(view,filename));
    check(!released,"primary release clears the native gesture");
    [native mouseDown:makeEvent(NSEventTypeLeftMouseDown)];
    std::unique_ptr<PuglFileDrag> pending(puglStartFileDrag(view,filename));
    check(bool(pending),"a fresh primary gesture remains usable after cancellation");
    if(pending)pending->cancel();
    puglFreeView(view);
    if(pending){check(!pending->idle(),"a cancelled handoff remains safe after native source destruction");pending->cancel();pending.reset();}
    puglFreeWorld(world);
    std::printf("Native Cocoa file gesture: %d checks, %d failures\n",checks,failures);
    [[NSFileManager defaultManager] removeItemAtPath:fixture error:nil];
    [pool drain];return failures?1:0;
}
