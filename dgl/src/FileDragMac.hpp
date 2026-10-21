// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after Cocoa's backend, at global Objective-C++ scope.
#define DuskFileDragSource DAF_MACOS_NAMESPACE_MACRO(DGL_NAMESPACE, DAF_MACOS_NAMESPACE_TIME, FileDragSource)
@interface DuskFileDragSource : NSObject<NSDraggingSource>
{
@public
    BOOL finished;
    BOOL cancelled;
    BOOL retainedForSession;
}
@end
@implementation DuskFileDragSource
- (NSDragOperation)draggingSession:(NSDraggingSession*)session sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    (void)session; (void)context;
    return cancelled ? NSDragOperationNone : NSDragOperationCopy;
}
- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession*)session
{ (void)session; return YES; }
- (void)draggingSession:(NSDraggingSession*)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation
{
    (void)session; (void)point; (void)operation;
    finished = YES;
    // The native session can finish after the C++ Window has been destroyed.
    if (retainedForSession) { retainedForSession = NO; [self release]; }
}
@end

class CocoaFileDrag : public PuglFileDrag
{
    NSView* view;
    NSEvent* event;
    NSURL* url;
    DuskFileDragSource* source;
    NSDraggingSession* session = nil;
    bool begun = false;
public:
    CocoaFileDrag(NSView* owner, NSEvent* trigger, NSURL* file)
        : view([owner retain]), event([trigger retain]), url([file retain]), source([DuskFileDragSource new]) {}
    ~CocoaFileDrag() override
    { [session release]; [source release]; [url release]; [event release]; [view release]; }
    void cancel() override { source->cancelled = YES; }
    bool idle() override
    {
        if (source->cancelled || source->finished) return false;
        if (!begun)
        {
            begun = true;
            if ([view window] == nil) return false;
            NSDraggingItem* item = [[NSDraggingItem alloc] initWithPasteboardWriter:url];
            NSImage* image = [[[NSWorkspace sharedWorkspace] iconForFile:[url path]] copy];
            [image setSize:NSMakeSize(32, 32)];
            const NSPoint point = [view convertPoint:[event locationInWindow] fromView:nil];
            [item setDraggingFrame:NSMakeRect(point.x - 16, point.y - 16, 32, 32) contents:image];
            [image release];
            source->retainedForSession = YES; [source retain];
            @try { session = [[view beginDraggingSessionWithItems:@[item] event:event source:source] retain]; }
            @catch (NSException* exception) { (void)exception; session = nil; }
            [item release];
            if (session == nil)
            {
                if (source->retainedForSession) { source->retainedForSession = NO; [source release]; }
                return false;
            }
        }
        return !source->finished && !source->cancelled;
    }
};

PuglFileDrag* puglStartFileDrag(PuglView* view, const char* filename)
{
    NSString* path = [NSString stringWithUTF8String:filename]; BOOL directory = NO;
    if (path == nil || ![path isAbsolutePath] || ![[NSFileManager defaultManager] fileExistsAtPath:path isDirectory:&directory] || directory)
        return nullptr;
    // Rendering can run after AppKit has dequeued an unrelated event.
    NSEvent* event = view->impl->fileDragEvent;
    NSView* native = view->impl->wrapperView;
    if (event == nil || ([event type] != NSEventTypeLeftMouseDown && [event type] != NSEventTypeLeftMouseDragged)
        || [event window] != [native window]) return nullptr;
    CocoaFileDrag* operation = new CocoaFileDrag(native, event, [NSURL fileURLWithPath:path]);
    view->impl->fileDragEvent = nil;
    [event release];
    return operation;
}
#undef DuskFileDragSource
