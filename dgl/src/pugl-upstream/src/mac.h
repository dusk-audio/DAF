// Copyright 2012-2020 David Robillard <d@drobilla.net>
// Copyright 2017 Hanspeter Portner <dev@open-music-kontrollers.ch>
// SPDX-License-Identifier: ISC

#ifndef PUGL_SRC_MAC_H
#define PUGL_SRC_MAC_H

#include <pugl/pugl.h>

#import <Cocoa/Cocoa.h>

#include <mach/mach_time.h>

#include <stdint.h>

@interface PuglWrapperView : NSView<NSTextInputClient>

- (void)dispatchExpose:(NSRect)rect;
- (void)setReshaped;

@end

@interface PuglWindow : NSWindow
@end

// Receiving file drops, a DGL extension in FileDropMac.hpp, which DGL's unity build includes after this
struct PuglMacDropTarget;
static NSDragOperation puglMacDropEntered(PuglWrapperView* wrapper, id<NSDraggingInfo> info);
static NSDragOperation puglMacDropUpdated(PuglWrapperView* wrapper, id<NSDraggingInfo> info);
static void puglMacDropExited(PuglWrapperView* wrapper);
static BOOL puglMacDropPrepare(PuglWrapperView* wrapper, id<NSDraggingInfo> info);
static BOOL puglMacDropPerform(PuglWrapperView* wrapper, id<NSDraggingInfo> info);
static void puglMacDropDetach(PuglWrapperView* wrapper);
static void puglMacDropFree(PuglWrapperView* wrapper);

struct PuglWorldInternalsImpl {
  NSApplication*            app;
  NSAutoreleasePool*        autoreleasePool;
  struct mach_timebase_info timebaseInfo;
};

struct PuglInternalsImpl {
  NSApplication*   app;
  PuglWrapperView* wrapperView;
  NSView*          drawView;
  NSCursor*        cursor;
  PuglWindow*      window;
  NSEvent*         fileDragEvent;
  uint32_t         mods;
  bool             mouseTracked;
  bool             lastDispatchedFocus;
};

#endif // PUGL_SRC_MAC_H
