// Copyright 2012-2022 David Robillard <d@drobilla.net>
// SPDX-License-Identifier: ISC

#ifndef PUGL_SRC_WIN_H
#define PUGL_SRC_WIN_H

#include "internal.h"

#include <pugl/pugl.h>

#include <windows.h>

#include <stdbool.h>

typedef PIXELFORMATDESCRIPTOR PuglWinPFD;

// DGL: the OLE target of a view that accepts file drops, see FileDropWin.hpp
struct PuglWinDropTarget;

struct PuglWorldInternalsImpl {
  double timerFrequency;
};

struct PuglInternalsImpl {
  PuglWinPFD      pfd;
  int             pfId;
  HWND            hwnd;
  HCURSOR         cursor;
  HWND            prevFocus; // outside window that last lost the focus to this view
  HDC             hdc;
  WINDOWPLACEMENT oldPlacement;
  PAINTSTRUCT     paint;
  PuglBlob        clipboard;
  PuglSurface*    surface;
  PuglWinDropTarget* drop; // DGL: null until the view first accepts file drops
  double          scaleFactor;
  bool            mapped;
  bool            flashing;
  bool            mouseTracked;
  bool            minimized;
  bool            maximized;
  bool            fullscreen;
};

PUGL_API PuglWinPFD
puglWinGetPixelFormatDescriptor(const PuglHints hints);

PUGL_WARN_UNUSED_RESULT PUGL_API PuglStatus
puglWinCreateWindow(PuglView* view, const char* title, HWND* hwnd, HDC* hdc);

PUGL_WARN_UNUSED_RESULT PUGL_API PuglStatus
puglWinConfigure(PuglView* view);

PUGL_WARN_UNUSED_RESULT PUGL_API PuglStatus
puglWinEnter(PuglView* view, const PuglExposeEvent* expose);

PUGL_WARN_UNUSED_RESULT PUGL_API PuglStatus
puglWinLeave(PuglView* view, const PuglExposeEvent* expose);

// DGL: defined in FileDropWin.hpp, which follows this backend. The first revokes and frees the view's drop
// target before its window goes; the second makes the OleUninitialize calls a drop target put off; the
// third is the apartment's OLE window while a drop target of the thread holds OLE, and null otherwise.
void
puglWinForgetDropTarget(PuglView* view);

void
puglWinSettleDropOle(void);

HWND
puglWinDropOleWindow(void);

#endif // PUGL_SRC_WIN_H
