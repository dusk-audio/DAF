# Receiving file drops (drop targets)

A top-level DGL window can ask to receive files dragged onto it from other applications: a file
manager, the desktop, a browser's download list. The application hears when such a drag enters the
window, moves over it and leaves it, and when the files are dropped, and it says per position
whether it would take them. DGL has had the other direction since `Window::startFileDrag`; this note
designs the receiving side for X11, Wayland, macOS and Windows.

This is a design. Phase 1b implements X11, Wayland, the DGL API and the tests on Linux. Phase 2
implements macOS and Windows, each on its own machine. Until then those backends compile the API and
refuse the opt-in.

It is an explicit opt-in per window. A window that does not ask behaves exactly as before: no
`XdndAware` property, Wayland drag offers declined as they are today, nothing registered with AppKit
or OLE. Plugin UIs are embedded windows and are out of scope, see the end of the next section.

## The application side

```cpp
class Window {
public:
    /** A drag of files from another application over this window, valid during the call it is passed to. */
    struct FileDropEvent {
        /** Pointer position, in the coordinates the window's top-level widgets get mouse events in. */
        Point<double> pos;
        /** numFiles absolute local paths, UTF-8 and NUL-terminated, at least one. */
        const char* const* files;
        uint numFiles;
        /** Keyboard modifiers held, as far as the platform can tell (see below). @see Modifier */
        uint mod;
        /** Event timestamp in milliseconds (if any). */
        uint time;
    };

    /** Whether this window receives files dragged onto it, see setAcceptingFileDrops(). */
    bool isAcceptingFileDrops() const noexcept;

    /** Let this window receive files dragged onto it from other applications. */
    bool setAcceptingFileDrops(bool accept);

protected:
    virtual bool onFileDragEnter(const FileDropEvent& ev); // default: false, refuse
    virtual bool onFileDragMove(const FileDropEvent& ev);  // default: what onFileDragEnter answered
    virtual void onFileDragLeave();                         // default: nothing
    virtual bool onFileDrop(const FileDropEvent& ev);       // default: false, not taken
};
```

- `setAcceptingFileDrops(true)` returns false and changes nothing for an embedded window, a window
  without a view, a Wayland compositor without `wl_data_device_manager`, and on platforms without
  drop support (macOS and Windows until phase 2, Haiku, WebAssembly). `setAcceptingFileDrops(false)`
  always succeeds. It can be called at any time, a drag in progress included: turning it off ends
  the drag for the application with `onFileDragLeave` if it had seen the enter, and refuses the rest.
- Each drag gives `onFileDragEnter` once, then any number of `onFileDragMove`, then either
  `onFileDragLeave` or `onFileDrop`, never both: a drop ends the drag and no leave follows it. A drag
  the application never saw enter (nothing usable in it, the window not accepting, a modal child up)
  produces no call at all.
- Enter and move return whether the application would take the files at `ev.pos`. The answer becomes
  the platform's cursor or highlight feedback, and decides whether a drop can happen there. The
  source always sees a copy: DGL never reports a move, whatever the source asked for, so no source
  deletes the user's files because they were dropped on a DGL window.
- `onFileDrop` comes only at a position the last answer accepted. True means the files were taken;
  false reports a failed drop to the source.
- Paths are absolute, local and UTF-8: from `text/uri-list` on X11 and Wayland (rules below), file
  URLs on macOS, `CF_HDROP` on Windows. Directories arrive like files. DGL never stats the paths, a
  `stat()` on a hung network mount would block the event thread, so the application checks what it
  got when it opens it.
- On X11 and Wayland `onFileDragEnter` comes once the file list has arrived, normally milliseconds
  after the pointer entered. Until then the drag shows as refused.
- The callbacks run on the thread that runs `Application::idle` or `exec`, from inside the window's
  event dispatch, like every other Window callback. They may repaint, change acceptance, close, hide
  or delete the window. They should not block or run a modal loop: on Windows the source process waits
  for the drop call to return. Remember the paths and act on the next idle. Deleting the `Application`
  inside any pugl event is unsupported already, and stays so.
- While the window has a modal child (`runAsModal`) a drag is refused without calling the
  application, as mouse events go to the child instead. While `getClipboard()` waits for its data,
  drag events are refused the way that wait swallows input.
- `mod` carries the keyboard modifiers, best effort, because neither XDND nor the Wayland data device
  carries them: X11 asks the server for the modifier state at each position and at the drop, Wayland
  reports the seat's last modifiers only while one of the world's surfaces has the keyboard focus (0
  otherwise), macOS and Windows will take them from the drag itself (`modifierFlags`, `grfKeyState`).
  Under XWayland a drag started in a Wayland client leaves the X server's keyboard state stale, so a
  held key can be missed there. The first consumer, Dusk Studio, places a drop at the pointer when Alt
  is held and at the playhead otherwise, and a missed Alt only changes where the clip lands.

Why on `Window`: it is where DGL's operating-system callbacks live (`onClipboardDataOffer`,
`onFileSelected`, `onFocus`, `onClose`) and where the outgoing drag starts. The opt-in follows the
`setResizable`/`setIgnoringKeyRepeat` pattern. Every platform wants the answer for a position
synchronously, which the application gives by hit-testing its own layout, and `ev.pos` is in widget
coordinates already. Widget routing (a `Widget::onFileDrag...` walked like mouse events) can be added
on top later without touching a backend. Unlike the clipboard, which the application pulls
(`getClipboard` asks, `onClipboardDataOffer` picks a type), drops are pushed: the user starts them,
the type is fixed, and DGL does the negotiation.

Embedded windows and plugin UIs: XDND addresses top-level windows (lesson 5 below), and Wayland
cannot embed at all, so drops are a top-level feature. `setAcceptingFileDrops` refuses an embedded
window on every platform, and nothing is set on a child X11 window, so a host that receives drops
over its plugin area keeps doing so. The DAF `UI` class and `PluginWindow` get nothing in this phase.
A CLAP floating window is a top level and could opt in, and AppKit and OLE could deliver to embedded
views too; both are later work.

## Pugl level

### Events

Four event types go at the end of `PuglEventType` in `pugl/pugl.h`, after `PUGL_DATA` so that nothing
renumbers, with one struct in the `PuglEvent` union:

```c
  PUGL_DRAG_ENTER,  ///< Files dragged into a view that accepts drops, a #PuglDropEvent
  PUGL_DRAG_MOTION, ///< Files dragged within the view, a #PuglDropEvent
  PUGL_DRAG_LEAVE,  ///< Drag left the view or ended without a drop, a #PuglDropEvent
  PUGL_DROP,        ///< Files dropped on the view, a #PuglDropEvent

typedef struct {
  PuglEventType      type;     ///< #PUGL_DRAG_ENTER, #PUGL_DRAG_MOTION, #PUGL_DRAG_LEAVE or #PUGL_DROP
  PuglEventFlags     flags;    ///< Bitwise OR of #PuglEventFlag values
  double             time;     ///< Time in seconds
  double             x;        ///< View-relative X coordinate, as in #PuglMotionEvent
  double             y;        ///< View-relative Y coordinate
  PuglMods           mods;     ///< Keyboard modifiers, best effort, 0 where unknown
  uint32_t           numFiles; ///< Entries in files, 0 for #PUGL_DRAG_LEAVE
  const char* const* files;    ///< Absolute local paths in UTF-8, valid during dispatch only
} PuglDropEvent;
```

The answer comes back as the event function's status, which `puglDispatchEvent` already returns. For
`PUGL_DRAG_ENTER` and `PUGL_DRAG_MOTION`, `PUGL_SUCCESS` accepts a drop at that position and any other
status refuses. For `PUGL_DROP`, `PUGL_SUCCESS` means the files were taken. The status of
`PUGL_DRAG_LEAVE` is ignored. Refusal has to be what anything that does not handle the events
answers, so DGL's clipboard wait returns `PUGL_FAILURE` for them rather than the `PUGL_SUCCESS` it
returns for the input it swallows. The backends interpret the status themselves and never pass it up
as an error of `puglUpdate` (x11.c's dispatch loop stops on the first non-zero status of an event it
dispatches, which is why the drop code dispatches these itself).

Why events rather than a callback interface: runtime-backend.md already describes drops as pugl
events at DGL's one event function, and events keep DGL's types out of the backends, pass through the
filters DGL already has (modal child, clipboard wait, debug printing), and need no forwarder beyond
the opt-in. Every switch over `PuglEventType` in DGL handles the four values (the `-Werror` CI leg
would say so otherwise). The examples and the C++ wrapper inside `pugl-upstream` are not built and
are left alone.

### Opting in

One DGL extension in `pugl.hpp`, next to `puglStartFileDrag`:

```cpp
// DGL specific, whether a top-level view receives files dragged onto it from other applications
// PUGL_UNSUPPORTED for an embedded view and wherever drops cannot be received.
PuglStatus puglSetAcceptsFileDrops(PuglView* view, bool accept);
```

X11 sets or deletes `XdndAware` on the view's window, so the view has to be realized first (DGL
realizes its windows as it creates them). Wayland sets a flag on the view's internals. macOS registers
the wrapper view for file URLs, Windows an `IDropTarget`. `pugl-dispatch.hpp` forwards it with
`DGL_PUGL_FORWARD` like the other DGL extensions. Nothing changes in the selection rule or the Wayland
loader. The new code calls no public pugl function from inside a pass that the pass has not defined
already, so the table of pass-to-DGL-scope calls in runtime-backend.md stays as it is: the listing
described there (an unoptimised build, every call from a pass into DGL scope) finds the same eleven.

### Where the code lives

The same shape as the outgoing drag:

| File | Compiled | Holds |
|---|---|---|
| `dgl/src/FileDrop.hpp` (new) | once, at DGL scope, before the passes (pugl.cpp; phase 2 also pugl.mm and the Windows arm) | `text/uri-list` decoding, the file list (`PuglDropFiles`), the drag session (`PuglDropSession`) and the interface a backend implements for it (`PuglDropHost`) |
| `dgl/src/FileDropX11.hpp` (new) | X11 pass, after x11.c, like FileDragX11.hpp | the XDND target and the X11 `puglSetAcceptsFileDrops` |
| `x11.c`, `x11.h` | X11 pass | hooks: an event filter ahead of `findView`, a deadline tick at the top of `dispatchX11Events`, a detach in `puglUnrealize`; the per-view pointer and the world's drop atoms |
| `dgl/src/FileDropWayland.hpp` (new) | Wayland pass, after wayland.c | the data-device drag session, the receive pipe, the Wayland `puglSetAcceptsFileDrops` |
| `wayland.c`, `wayland.h` | Wayland pass | hooks in the data device and data offer listeners, the poll set and its dispatch, `puglUnrealize`, world teardown; the per-world pointer |
| `FileDropMac.hpp`, `FileDropWin.hpp` (phase 2) | Cocoa and Win32 backends | `NSDraggingDestination`, `IDropTarget` |

The hooks are declared in `x11.h` and `wayland.h`, inside the pass, and defined by the header that
follows the backend, so a both build has one set per pass and a single-backend build only its own.
Until a view opts in the X11 filter returns at its first test (no drop atoms interned yet), so a
program that never opts in runs what it runs today. No build file changes: the new headers ride in
the pugl.cpp unity build for Make and CMake alike.

### The shared drag session

X11 and Wayland differ in how a target asks for the data and how it answers, not in what the
application should see. The sequencing therefore lives once, in `PuglDropSession`, and each backend
supplies four primitives through a DGL-scope base class, as the passes already share `PuglFileDrag`:

```cpp
struct PuglDropHost {
    virtual ~PuglDropHost() {}
    virtual void requestData(double time) = 0;  // X11: XConvertSelection; Wayland: wl_data_offer.receive
    virtual void answer(bool accept) = 0;       // X11: XdndStatus when one is owed; Wayland: accept()
    virtual void finish(bool taken) = 0;        // X11: XdndFinished; Wayland: finish (v3) and destroy
    virtual PuglStatus dispatch(const PuglDropEvent& event) = 0; // pugl's puglDispatchEvent, per pass
};
```

The session takes the protocol's events (`begin(offersFiles)`, `position(x, y, time, mods)`,
`data(bytes, size)`, `dataFailed()`, `leave()`, `drop(time, mods)`, `tick(now)`, `detach(notify)`) and
guarantees:

- The data is requested once, at the first position, never at enter: XDND Enter has neither a
  position nor a timestamp. Wayland's enter carries a position, so its adapter reports one with it.
- The application's enter waits until there is both a position and a decoded list, and never comes
  for a drag without a usable file.
- The current answer goes to the host whenever one is owed (X11 owes one status per position,
  coalesced while the data is on its way) and whenever it changes (Wayland's accept state).
- Leave only after enter, and no leave after a drop. A source that drops where the view refused (one
  that drops whatever the status said) gets a failed drop, and the view that had the enter a leave.
- A drop that arrives before the data, from a source that did not wait for the status, is held until
  the data or the deadline, then becomes enter and, if accepted, drop; otherwise `finish(false)`.
- Deadlines: 2 s for the data once requested (the drag is refused from then on), 5 s for a held drop.
- Nothing is freed during a dispatch, so an application may delete its window inside any callback, and
  the event's files stay readable until that callback returns. A view that goes away, or stops
  accepting, only detaches the session, which then refuses the rest of the drag at the protocol level.
  The X11 target belongs to its view, so `puglUnrealize` hands it to the world, which frees it at the
  next tick, after any dispatch on the stack has returned. Once a drop is being taken or refused, the
  drag stays with the session until its end has told the source: a drop dispatched still finishes with
  the application's answer, and one refused (in the late enter of a drop that came before its files, or
  in the leave of a drop where the view had said no) still tells the source it failed, whether the view
  went or opted out inside that callback. The X11 target keeps its source until then, and is freed at
  the tick after.
- Callbacks cannot nest. While the backend is inside the session, a protocol event that arrives (an
  application running a nested event loop, `getClipboard()` say) is queued and handled in order once
  the outer call returns, the deadline tick included. Turning drops off from outside a callback runs the
  same way, since the leave it sends may run such a loop: a drag that moves on to another view meanwhile
  is handled once the opt-out is done, rather than torn down by it.

The unit tests drive exactly this with a fake host and no display. macOS and Windows can use it in
phase 2, with data that arrives synchronously at enter.

## Paths from text/uri-list

`PuglDropFiles::setFromUriList` builds the list, after RFC 2483 and RFC 8089:

- The payload stops at the first NUL; some sources append one.
- Lines end in CRLF; a bare LF is accepted and the last line need not end at all. Spaces and tabs at
  either end of a line are trimmed (a real space in a file name is `%20`).
- Empty lines, and lines starting with `#`, are skipped.
- The scheme is `file:`, in any case. The authority must be empty (`file:///p`), `localhost`, or this
  machine's name as `gethostname()` gives it, both compared without case (`file://localhost/p`). The
  backend passes the name in, so the decoder stays portable. `file:/p`, with no authority, is a local
  file too. Any other host and every other scheme (`http:`, `trash:`, `smb:`, `sftp:`, GVfs locations)
  are ignored.
- Percent escapes are decoded, `%2F` included. `%00`, and a malformed escape (`%G1`, a truncated `%4`),
  reject the line rather than guess. `?` and `#` after the authority stay part of the path: no file
  manager sends a query or a fragment, and cutting a mis-encoded `C#.mid` would name another file.
- The decoded path must start with `/` and be valid UTF-8 (no overlong forms, no surrogates, nothing
  past U+10FFFF), or the line is ignored. An application that stores paths as JSON then never gets
  one it cannot serialise.
- Order and duplicates are kept.
- A payload with no usable path is refused at the protocol level, and the application never hears of
  the drag.

The payload is bounded at 16 MiB (`PUGL_DROP_MAX_PAYLOAD`), far past any real list: ten thousand files
take about 1 MB.

## X11 (XDND)

The X11 target speaks the protocol whose mishandling in JUCE kept GNOME's drops out of Dusk Studio, so
each lesson from that bug is a rule here.

Receiver, lesson 5: the top-level window. `puglSetAcceptsFileDrops` refuses an embedded view
(`view->parent`) and sets `XdndAware` = 5 (`XA_ATOM`, format 32) on the view's own window, which for a
DGL top level is the client window the window manager reparents. Mutter ignores `XdndAware` and
`XdndProxy` for XWayland clients and sends to the top-level client window regardless, so messages
arrive at `impl->win` whatever its properties say, and `findView` already resolves that. DGL sets no
`XdndProxy`. A window that has not opted in ignores Xdnd messages exactly as today (Mutter sends them
anyway); one that opts out again deletes the property.

Atoms: interned at the first opt-in with a single `XInternAtoms` call and kept on the world:
`XdndAware`, `XdndEnter`, `XdndPosition`, `XdndStatus`, `XdndLeave`, `XdndDrop`, `XdndFinished`,
`XdndSelection`, `XdndTypeList`, `XdndActionCopy`, `INCR`, and `_DGL_XDND_DATA0` to `_DGL_XDND_DATA3`,
the properties DGL receives into. Fixed names, since the server never frees an atom. `text/uri-list` is
interned by x11.c already.

Messages, all format 32, XIDs read as `::Window(uint32_t(l[0]))` like FileDragX11.hpp:

| Message | Fields | Rule |
|---|---|---|
| XdndEnter | l[0] source; l[1] bits 24-31 version, bit 0 more than three types; l[2..4] types | Lesson 1: every version from 3 up is taken, above ours included, and the exchange runs at min(source, 5). Below 3 is ignored. Types come from l[2..4], or from the source's `XdndTypeList` (a bounded read) when bit 0 is set. Without `text/uri-list` the session refuses. Lesson 4: Enter is not a position: nothing reaches the application and no status is sent. |
| XdndPosition | l[0] source; l[2] root x << 16 \| y; l[3] timestamp; l[4] requested action | One `XQueryPointer` on the window gives both its origin on the root (pointer root position minus its window position, exact wherever the pointer is) and the modifier mask; `XTranslateCoordinates` stands in when the pointer is on another screen. The first position starts the conversion with l[3] (lesson 2). l[4] is ignored. |
| XdndStatus, sent | l[0] our window; l[1] bit 0 accept, bit 1 keep sending positions; l[2] = l[3] = 0, no rectangle; l[4] action | Lesson 3: l[4] is `XdndActionCopy` when accepting and None otherwise, also when the source asked for Move, Link or Ask. One per position, held back while the data is on its way: XDND sources wait for the status before the next position, and positions from one that does not are coalesced into one answer. |
| XdndLeave | l[0] source | Ends the session. A conversion in flight becomes stale. |
| XdndDrop | l[0] source; l[2] timestamp | Lesson 2: the drop's timestamp is l[2], never another field. The conversion already started at the first position; a drop with no position before it is refused. Always answered with XdndFinished. |
| XdndFinished, sent | l[0] our window; v5: l[1] bit 0 accepted, l[2] action | Lesson 3: at v5, l[2] is `XdndActionCopy` when the application took the files and None otherwise. Below v5, l[1] = l[2] = 0. |

Messages count only from the session's source. A Position before any Enter, from another window, or
from a source below version 3, is ignored and gets no answer, as XDND has it. A new Enter, from any
source, ends the current session as if it had left: the application gets its leave if it had its enter,
and a held drop gets `XdndFinished` refusing. A source window that is destroyed ends its drag the same
way, without anything being sent to it.

The conversion: `XConvertSelection(XdndSelection, text/uri-list, property, window, time)` with the first
Position's l[3], and back to the event loop. Nothing waits. A drop that arrives while it is in flight
waits for it, up to the held drop's deadline. Each conversion takes the first of the four properties no
other conversion is using, so that the late answer of an earlier drag (one that left, one a new drag
replaced) and the chunks of its INCR transfer are told from the current conversion by their property,
whatever time the owner reports: some answer with CurrentTime rather than the request's time. Four
properties cover four drags in a row whose owners have not answered within the 5 s a conversion nobody
waits for is kept; a fifth takes over the property of the one closest to being given up, whose answer,
should it still come, would pass for the fifth's. The filter takes the `SelectionNotify` before
`handleSelectionNotify`, which knows only `CLIPBOARD`. A late answer is read, thrown away and its
property deleted. A refusal (property None) names no property, so it is matched by its request's time,
and one that matches no request in flight is left to the deadline: a current answer is never mistaken
for a stale one. JUCE converted with l[2] of the Position, which is the packed coordinates; the test
below checks the time the source sees.

Reading: `XGetWindowProperty(..., delete True, AnyPropertyType)`, bounded by the payload ceiling, and
refused when `bytes_after` is not zero. An `INCR` type starts an incremental transfer (ICCCM 2.7.2):
the delete asks the owner for the first chunk, and each `PropertyNotify` with `PropertyNewValue` for that
conversion's property on the window (pugl selects `PropertyChangeMask` already) is read with delete and
appended, up to a zero-length chunk. A transfer past the ceiling, or one nobody waits for any more, is
drained: each further chunk is deleted unread, up to the zero-length one or the deadline, so the
source is not left waiting on a property nobody deletes and nothing stays on the window. Another
conversion meanwhile uses another property, so the chunks never land in its list. A
`PropertyNewValue` on one of the properties with no transfer in progress is deleted as well. No DGL
property outlives its conversion.

Deadlines are checked at the top of `dispatchX11Events`. It runs on every pass of `puglUpdate` and
`puglX11UpdateWithoutExposures`, events or not, and DGL never calls `puglUpdate` without a timeout.
The data has 2 s after the request (the owed status goes out refusing and the session stays refused
until it leaves), a held drop 5 s (`XdndFinished` refusing), a conversion nobody waits for any more 5 s,
after which its property is deleted. A source that converts slowly cannot make DAF's own outgoing drag
give up first: `X11FileDrag` waits 5 s after the release.

A source that dies: its window is selected for `StructureNotifyMask` on pugl's connection for as long
as the session lasts (under the error trap, deselected at the end), and its `DestroyNotify`, which
the filter takes before `findView` since the window is not ours, ends the session at once as a leave,
a held drop included. The source is forgotten first, so nothing goes to the dead window, not even the
`XdndFinished` refusing a drop it held. Its conversion in flight goes stale like a left drag's.
Everything sent to a source goes through `XSendEvent` inside pugl's error trap (`puglUpdate` and the
clipboard loop already open one), so a source gone between the test and the message costs a trapped
`BadWindow` and nothing else.

A window destroyed mid-drag: `puglUnrealize`, which `puglFreeView` reaches, detaches the session
before `XDestroyWindow`. An owed status goes out refusing, a held drop gets `XdndFinished` refusing,
the source window is deselected and the property deleted. The application gets no leave from its own
window going away.

The round trips that remain (`XQueryPointer` per position and at the drop, the `XdndTypeList` read,
the data property read) go to the X server, which answers whether or not the source is alive; none
waits on the source.

Checked on Mutter 50.5, headless, with a Wayland source's drag over an X11 window under its Xwayland
(the Dusk Studio case): Mutter relays it at XDND version 5 to the top-level client window, answers the
conversion at the first position long before the drop, and takes the copy in both status and finished,
Shift held or not. The default path is the one Mutter needs, and the fallback of decision 4 is not
implemented. Mutter addresses drags over the window's frame (its title bar) to the client window too,
so a position there is outside the window, negative above it: the test compared the drop position with
the X server's own pointer position once the drag let go of it. Applications hit-test as they would
for any position.

## Wayland (wl_data_device)

The data device belongs to the seat, so the session is per world: one drag at a time, over the
surface its enter names. pugl binds `wl_data_device_manager` at version 3 already.

Opted-out views keep today's path. An enter over a view that has not opted in, over a surface that is
not a view, or with an offer lacking `text/uri-list`, gets `wl_data_offer.accept(serial, NULL)`, and
the offer is kept until leave or drop and destroyed then.

For an opted-in view:

- `wl_data_offer.offer` (before the enter) lists the MIME types, compared without parameters.
  `source_actions` (v3, also sent before the enter) is recorded on `PuglWaylandOffer`, which also
  records `action`.
- enter(serial, surface, x, y, offer): the session takes the offer and keeps the serial for accept.
  At v3, a source that offers copy gets `set_actions(COPY, COPY)` once. With copy the only action the
  destination supports, the compositor can only ever select copy or nothing, whatever modifier the
  user holds. A source without copy is refused, and gets no `set_actions` at all: a preferred action
  outside the source's set is an error. Then `accept(serial, NULL)` until the application has
  decided, and the receive starts at once. The protocol says a destination "may preemptively fetch
  data or examine it more closely to determine acceptance": `pipe2(O_CLOEXEC | O_NONBLOCK)`,
  `wl_data_offer_receive(offer, "text/uri-list", write end)`, close the write end, flush.
- The read end joins the world's poll set. `puglWaylandBuildPollFds` gets a third kind of entry next
  to the display, key repeat and timers, and the loop in `puglWaylandDispatchEvents` that treats every
  other ready descriptor as a timer learns to tell it apart (the end of the list can show as a hang-up
  with nothing left to read). Each wake reads what is there, until the pipe would block; the clipboard's
  blocking reader stays as it is. EOF decodes and calls `data()`; a read error or the ceiling,
  `dataFailed()`.
- The answer is `accept(serial, "text/uri-list")` or `accept(serial, NULL)`, sent only when it
  changes.
- motion(time, x, y) is a position. Modifiers are the seat's (`impl->mods`) while one of the world's
  views has the keyboard focus, 0 otherwise. Every drop event takes its time from the world's clock,
  since enter and drop carry none of the compositor's.
- drop: the compositor sends it only after an accept with an action, which here means the files were
  in hand. `PUGL_DROP` goes out; then, at v3, `wl_data_offer.finish` if the application took the files
  and the last `action` was copy, and never otherwise (finish after a NULL accept or without an action
  is a protocol error); then `wl_data_offer.destroy`. Below v3: destroy only. The session ends at the
  drop, so the leave that compositors send after it finds none and does nothing.
- leave: a receive in flight is closed and its bytes dropped, `PUGL_DRAG_LEAVE` goes out if the
  application had its enter, and the offer is destroyed.
- The deadline tick runs in `puglWaylandDispatchEvents`: 2 s for the data, after which the drag stays
  refused until it leaves.
- A view destroyed mid-drag (`puglUnrealize`): the receive is closed, `accept(serial, NULL)` sent and
  the view forgotten. The offer stays until the compositor's leave, and is destroyed then. World
  teardown destroys whatever offer and descriptor remain, next to the `dndOffer` bookkeeping in
  `puglWaylandDestroyWorldInternals`.
- A source that dies: the compositor sends leave. The read may reach EOF with a partial list just
  before, and the leave ends that session anyway.
- A paste (`puglWaylandReadPipe`) dispatches display events while it waits, so a drag can start during
  one. Its receive begins and is serviced by the main poll once the paste is done.

| Data device version | `set_actions` | `accept` | `finish` |
|---|---|---|---|
| 1, 2 | never sent | feedback only | never sent; destroy after the drop |
| 3 | `COPY, COPY` at enter, if the source offers copy | decides the outcome | after a drop the application took, with action copy; then destroy |

No new libwayland function: each request above is an inline protocol wrapper over
`wl_proxy_marshal_flags`, `wl_proxy_get_version` and `wl_proxy_destroy`, already in
`wayland-loader-names.h`. A both build loads nothing new, and the CI checks of what a build links and
exports stay as they are.

## macOS (phase 2)

- `NSDraggingDestination` on `PuglWrapperView`, registered with
  `registerForDraggedTypes:@[NSPasteboardTypeFileURL]` by `puglSetAcceptsFileDrops` and unregistered on
  opt-out, in `detachPuglView` and before the view is freed. Embedded views are refused for
  consistency, though AppKit could deliver to a view inside a host's window.
- `draggingEntered:` reads `[[sender draggingPasteboard] readObjectsForClasses:@[NSURL.class]
  options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}]`, synchronously as a destination does, and
  converts each URL with `fileSystemRepresentation`: UTF-8, in the form the file system keeps (Finder
  can hand decomposed Unicode, which `FileDragCocoaNative.cpp` already allows for). File promises
  (`NSFilePromiseReceiver`, from Mail or Photos) are not files yet and are refused.
- The answer is `NSDragOperationCopy` when accepted and `[sender draggingSourceOperationMask]` has
  copy, `NSDragOperationNone` otherwise. Cmd held over Finder leaves move alone in the mask: refused,
  never turned into a move.
- `draggingUpdated:` is motion and `draggingExited:` leave; `prepareForDragOperation:` says YES when
  the last answer accepted; `performDragOperation:` dispatches `PUGL_DROP` and returns whether the
  files were taken; `wantsPeriodicDraggingUpdates` is NO.
- Position: `[self convertPoint:[sender draggingLocation] fromView:nil]`, then `nsPointFromPoints`,
  which is what `eventLocation:` does to mouse events. The view is flipped, so the origin is top left.
- Every method tests `puglview` first: AppKit can still call a retired view (`detachPuglView`), which
  then answers none.
- The wrapper view still declares `dragSource`, `dragOperation`, `dragTypeIndex` and `droppedUriList`
  from an older drag-and-drop attempt, and `puglGetClipboard` has a `public.file-url` branch that
  reads `droppedUriList`. Nothing ever sets them. Phase 2 removes them rather than builds on them.
- Everything runs on the main thread, as AppKit does.

## Windows (phase 2)

- An `IDropTarget` in `FileDropWin.hpp`, a COM object like those in `FileDragWin.hpp`, registered with
  `RegisterDragDrop(hwnd)` by `puglSetAcceptsFileDrops`, revoked on opt-out and in `puglUnrealize`
  and `puglFreeViewInternals` before `DestroyWindow`. It needs `OleInitialize` on the thread, a
  single-threaded apartment, matched by one `OleUninitialize` when revoked. `RPC_E_CHANGED_MODE`, a
  thread already in the multi-threaded apartment, fails the opt-in with a message.
- `DragEnter` asks the `IDataObject` for `CF_HDROP` in `TYMED_HGLOBAL`, reads it with `DragQueryFileW`
  (length first, paths can pass `MAX_PATH`) and converts with `WideCharToMultiByte(CP_UTF8,
  WC_ERR_INVALID_CHARS)`, skipping a path with an unpaired surrogate. Virtual files
  (`CFSTR_FILEDESCRIPTOR`, from Outlook or a zip folder) are refused.
- `*pdwEffect` is `DROPEFFECT_COPY` when accepted and the source allows copy, `DROPEFFECT_NONE`
  otherwise, whatever `grfKeyState` says: Shift asks Explorer for a move. `Drop` reports copy when the
  application took the files.
- Position: the `POINTL` is in screen coordinates, and `ScreenToClient(hwnd)` gives client pixels, what
  `WM_MOUSEMOVE` reports.
- The target object keeps a pointer to the view that `puglUnrealize` clears; a call after that answers
  `DROPEFFECT_NONE`.
- The message loop. A drag from another process (Explorer) calls the target through COM, which reaches
  a single-threaded apartment as messages to OLE's hidden window on the target thread. pugl's Windows
  loop only peeks the messages of the world's own windows (`PeekMessage(&msg, view->impl->hwnd, ...)`
  in `puglDispatchViewEvents`), so in a standalone program nothing dispatches those calls and the drag
  stalls. A plugin is fine: its host pumps the whole thread. Phase 2 has to test a drag from another
  process, not only the same-process one `FileDragWinNative.cpp` does. A program world dispatches the
  rest of its thread's messages as well, so that OLE reaches the target; a module world keeps the
  filtered loop, since its host pumps the thread and the other windows on it are the host's.
- `WM_DROPFILES` (`DragAcceptFiles`) would avoid COM and the loop question, but it only reports the
  drop: no enter, no motion, no answer per position.
- An elevated process does not get drops from a non-elevated Explorer (UIPI). That is the system's
  rule.

## Lifetime and robustness

| Case | X11 | Wayland | macOS, Windows |
|---|---|---|---|
| Drag leaves during the read | conversion stale, late `SelectionNotify` ignored, property deleted, INCR drained | descriptor closed, offer destroyed | data read synchronously at enter |
| New drag before the last finished | new Enter ends the old session: leave, or `XdndFinished` refusing a held drop | the protocol ends a drag with leave or drop before the next enter | native sessions are sequential |
| Source dies | `DestroyNotify` ends it at once as a leave, a held drop included, with nothing sent to the dead window; its conversion goes stale; sends trapped | compositor sends leave | AppKit and OLE end the session (exited, `DragLeave`) |
| Window destroyed mid-drag | detach in `puglUnrealize`: owed status refusing, `XdndFinished` refusing, deselect, delete property | descriptor closed, `accept(NULL)`, offer kept until leave | unregister or revoke; late calls answer none |
| Window deleted inside a callback | the session checks the alive flag after each dispatch | same | same |
| Application refuses | status accept 0, action None | `accept(serial, NULL)` | `NSDragOperationNone`, `DROPEFFECT_NONE` |
| No usable file | refused, application never called | same | same |
| Source asks for a move | status and finished say copy | the destination supports copy only | copy only; a move-only source is refused |
| Opt-out during a drag | `XdndAware` deleted, application leave, the rest refused, a drop finished refusing | `accept(NULL)`, application leave, read closed | unregister or revoke, application leave |

Nothing blocks on the source. X11 waits for `SelectionNotify` and `PropertyNotify` in the event loop;
Wayland reads a non-blocking pipe from the poll set; both stop at a deadline. macOS and Windows read
their native file types synchronously, as their destination APIs expect.

## Coordinates

`PuglDropEvent.x` and `.y` are the view coordinates `PuglMotionEvent` uses on the same backend, so DGL
treats them like a mouse position: `FileDropEvent.pos` divides them by the auto-scale factor while
`setGeometryConstraints(..., automaticallyScale = true)` is in effect, which is what `TopLevelWidget`
does to mouse events before its widgets see them.

| Backend | Protocol gives | View coordinates |
|---|---|---|
| X11 | `XdndPosition` root coordinates | minus the window's origin on the root (from `XQueryPointer`): X pixels, as `MotionNotify`. The X11 scale factor (`Xft.dpi`) does not enter, as it does not for mouse events. |
| Wayland | surface-local `wl_fixed` | times `view->impl->scale`, the factor the pointer handlers use: buffer pixels, fractional scale included |
| macOS | window base coordinates | `convertPoint:fromView:nil`, then `nsPointFromPoints`, as `eventLocation:` |
| Windows | screen `POINTL` | `ScreenToClient`: client pixels, as `WM_MOUSEMOVE` |

## Dear ImGui

Nothing changes in `widgets/`. An `ImGuiStandaloneWindow` is a `Window` and overrides the callbacks
itself; an `ImGuiTopLevelWidget` lives in a `Window` subclass that handles them. Two things matter.
During a drag from another application no platform sends the window mouse motion (the source, or the
compositor, has the pointer), so ImGui never sees a hover. And the answer is due inside the callback,
before the next frame. An ImGui application answers from rectangles it kept from its last frame (the
timeline's, say), keeps the drag position for its own drawing, and calls `repaint()`. Positions need
no conversion: ImGui takes widget coordinates (`io.AddMousePosEvent(event.pos)`), which
`FileDropEvent.pos` already is. Feeding the drag into ImGui's own drag and drop
(`BeginDragDropSource(ImGuiDragDropFlags_SourceExtern)`) is application code, if wanted.

## Tests

Each platform is tested on that platform.

In CI:

| Test | Covers | Runs in |
|---|---|---|
| `tests/FileDrop.cpp` (new) | The decoder: CRLF, LF, no final newline, comments, NUL, `file:///`, `localhost` in any case, `file:/p`, other hosts and schemes, escapes valid, malformed, `%00` and `%2F`, UTF-8 valid, invalid and overlong, relative paths, `#` and `?`, the ceiling. The session with a fake host: no request before a position, enter waits for the files, coalesced answers, leave only after enter and none after a drop, a drop before the data, both deadlines, the view deleted inside each callback, the late enter of a held drop and the leave of a refused one included, and opting out there; the files of a drop outliving a view deleted inside `onFileDrop`; opt-out during a drag. | every `make -C tests run`, on every platform: header only, no display |
| `tests/Window.cpp` (extended) | DGL with synthesised pugl events: refusal by default, move keeping enter's answer, the auto-scale division, refusal under a modal child and during the clipboard wait, `setAcceptingFileDrops` false for an embedded window | every `Window.*` variant |
| `tests/X11Drop.cpp` (new) | A DGL window on the X server, and an XDND source inside the test on a second connection, as X11Focus has its host: it sends Mutter-style client messages straight to the top level, serves the selection and can play INCR. Sources at v3, v4, v5 and v6; copy and move requested, status and finished saying copy; types inline and through `XdndTypeList`; no `text/uri-list`; Enter alone sending nothing; the `SelectionRequest` time equal to the Position's l[3], and the Drop's l[2] on the held-drop path, never the packed coordinates; root to window coordinates at a known window position, with and without auto-scaling; answers per position; leave during the conversion; a new drag before the last ended, its late answer arriving while the next conversion is in flight; three drags, two of which left, answering out of order; an owner answering with CurrentTime; a conversion the owner refuses, refused at once; the source window destroyed during a drag and during a held drop, both ending at once, with nothing sent to it; INCR above 256 KiB, above the ceiling (refused well before the data deadline), and left mid-transfer while the next drag's list arrives; the window destroyed after the drop; the window deleted inside `onFileDrop`, inside the late enter of a held drop and inside the leave of a refused one, every drop still finished; opting out inside those callbacks; no DGL property left on the window; a window that has not opted in having no `XdndAware` and sending nothing. | build.yml X11 suite and runtime-backend, and wayland.yml runtime-backend-without-wayland, all under Xvfb; added to `SKIP_UNIT_TESTS` where Weston runs alone (wayland.yml runtime-backend), as X11Focus is |
| `tests/WaylandDrop.cpp` (new) | A stand-in compositor on a socket pair handed over through `WAYLAND_SOCKET`, extending `CompositorWithoutShell` in RuntimeBackend.cpp: hand-written wire protocol, `recvmsg` with `SCM_RIGHTS` for the receive descriptor. It offers `wl_compositor`, `xdg_wm_base`, `wl_seat`, `wl_output` (scale 2, for the coordinates) and `wl_data_device_manager`, once at v3 and once at v1, and checks the requests: `accept(serial, NULL)` then `accept(serial, "text/uri-list")` once the data is in, `set_actions(1, 1)` once, receive, finish before destroy after a taken drop, no finish after a refusal and none at all at v1; a move-only source refused with no `set_actions`; leave during the read (descriptor closed, no enter); enter right after a leave; a partial write and then leave; the view freed mid-drag, inside the late enter of a held drop and inside the leave of a refused one; a view turning drops off while its leave runs a nested event loop in which the drag moves on to another view, that drag surviving; an opted-out view getting today's `accept(serial, NULL)` and nothing else; every offer destroyed and no descriptor left open (`/proc/self/fd`). | wayland.yml wayland-only and runtime-backend (under Weston alone), build.yml runtime-backend (under Xvfb): no compositor needed, the socket is handed over. Listed in `SKIP_UNIT_TESTS` in runtime-backend-without-wayland, where the libraries are gone. |

Why a stand-in and not libwayland-server: the both jobs fail any test binary that needs a `libwayland`
library, which a server would be. Neither CI compositor takes input either: Weston's test plugin is
not packaged and Mutter's injection needs a session bus. The stand-in costs a few hundred lines and
runs the real protocol code, Wayland-only and both alike.

Local only, on this box and never on the user's display:

- X11 end to end on a private `Xvfb -displayfd 3 -nolisten tcp`, the pointer driven by `xdotool` on
  that display only. `tests/FileDropDemo.cpp`, a window that opts in and prints its events, receives
  (a) DAF's own `X11FileDrag`, started from a second DGL window under a real XTEST press, which also
  checks the outgoing drag against the new target, and (b) `/var/tmp/dusk-xdnd/tools/xdnd_probe` at
  versions 3 and 5, with and without `PROBE_ACTION=move`, whose `status:` and `RESULT finished` lines
  must say copy, and once with Alt held (`xdotool keydown alt` on the private display), whose drop must
  carry Alt in `mod`. Both protocol tests also run here under AddressSanitizer and
  UndefinedBehaviorSanitizer.
- Wayland end to end on a private headless Mutter: `env -i PATH=/usr/bin:/bin HOME=/var/tmp/<dir>
  XDG_RUNTIME_DIR=<private 0700 dir> GSETTINGS_BACKEND=memory LIBGL_ALWAYS_SOFTWARE=1 timeout <s>
  dbus-run-session -- mutter --wayland --headless --virtual-monitor 1280x720 --wayland-display
  daf-drop-N -- inner.sh`, with input through `org.gnome.Mutter.RemoteDesktop` on that private bus
  (`CreateSession`, `Start`, `NotifyPointerMotionRelative`, `NotifyPointerButton`, one throwaway event
  first). The source is a small `wl_data_source` client offering `text/uri-list` with copy and move:
  it maps a window, takes the serial of an injected press on it and calls `start_drag`. A Wayland
  client cannot know where windows are, so relative motion walks the pointer until the target prints
  a drag enter, then the button is released. Checks: enter, move and drop at the right coordinates,
  the source's `dnd_finished` with action copy, Shift held still giving copy. Afterwards, kill any
  `gvfsd-fuse` left in the private runtime directory (`fusermount3 -u`).
- The XWayland case on the same Mutter, if it starts Xwayland headless: a DGL window on X11
  (`DAF_WINDOW_BACKEND=x11`) receiving the Wayland source's drag, which Mutter relays as XDND v5 to
  the top level. That is the Dusk Studio bug itself, on real Mutter, and the only check that Mutter
  answers a conversion before the drop (decision 4).
- macOS, phase 2 on the Air: a unit test, CI-able, calling the wrapper view's destination methods with
  a fake `NSDraggingInfo` over `[NSPasteboard pasteboardWithUniqueName]`, and an interactive drop of
  the existing `CocoaFileDrag` onto an opted-in DGL window (the `tests/native-file-drag` pattern,
  `DAF_FILE_DRAG_INTERACTIVE`), then Finder by hand.
- Windows, phase 2 on the VM: a unit test, CI-able, calling the `IDropTarget` methods with a fake
  `IDataObject` holding `CF_HDROP`; the interactive same-process drop (`WindowsFileDrag` onto an
  opted-in DGL window); and a drag from another process, a helper or Explorer, for the message loop.

## Files

Phase 1b, X11, Wayland, the DGL API and the tests, in commits small enough to review:

1. pugl events, the DGL API and the shared session: `dgl/src/pugl-upstream/include/pugl/pugl.h`,
   `dgl/src/pugl.hpp`, `dgl/src/pugl-dispatch.hpp`, `dgl/src/pugl-platform.hpp` (the includes, and
   `PUGL_UNSUPPORTED` for macOS, Windows, Haiku and WebAssembly), `dgl/src/pugl.cpp`,
   `dgl/src/FileDrop.hpp` (new), `dgl/Window.hpp`, `dgl/src/Window.cpp`,
   `dgl/src/WindowPrivateData.hpp`, `dgl/src/WindowPrivateData.cpp`, `tests/FileDrop.cpp` (new),
   `tests/Window.cpp`, `tests/Makefile`.
2. X11: `dgl/src/FileDropX11.hpp` (new), `dgl/src/pugl-upstream/src/x11.c`,
   `dgl/src/pugl-upstream/src/x11.h`, `tests/X11Drop.cpp` (new), `tests/Makefile`,
   `.github/workflows/wayland.yml` (the skip list).
3. Wayland: `dgl/src/FileDropWayland.hpp` (new), `dgl/src/pugl-extra/wayland.c`,
   `dgl/src/pugl-extra/wayland.h`, `tests/WaylandDrop.cpp` (new), `tests/Makefile`,
   `.github/workflows/wayland.yml` (the skip list in runtime-backend-without-wayland).
4. This note, the drops paragraph and extension list in `dgl/src/runtime-backend.md`, and the manual
   program the local runs use, `tests/FileDropDemo.cpp`, a `MANUAL_TESTS` entry drawn with Cairo, since a
   Wayland window shows only once something has drawn into it.

Phase 2:

- macOS: `dgl/src/FileDropMac.hpp` (new), `dgl/src/pugl-upstream/src/mac.m` and `mac.h`,
  `dgl/src/pugl-platform.hpp`, `dgl/src/pugl.mm`, `tests/FileDropCocoa.cpp` (new),
  `tests/native-file-drag/`, `tests/Makefile`.
- Windows: `dgl/src/FileDropWin.hpp` (new), `dgl/src/pugl-upstream/src/win.c` and `win.h`,
  `dgl/src/pugl-platform.hpp`, `dgl/src/pugl.cpp`, `tests/FileDropWin.cpp` (new),
  `tests/native-file-drag/`, `tests/Makefile`.

## Decided

The owner's answers to the questions this design raised:

1. File names that are not valid UTF-8 are skipped.
2. A `file://` URI names a local file when its host is empty, `localhost`, or this machine's name
   from `gethostname()`, compared without case. Every other host is ignored.
3. The list is read once, at the first position. There is no second conversion at the drop.
4. Implemented as designed, then checked against real headless Mutter with an X11 window under
   Xwayland. Had Mutter not answered a conversion before the drop, the fallback was to accept
   provisionally while the type list has `text/uri-list` and convert at the drop with Drop l[2]. Mutter
   50.5 answers it (see the section on X11), so the fallback is not needed and not implemented.
5. Windows, phase 2: a program world dispatches all of its thread's messages, so that OLE reaches the
   drop target; module (plugin) worlds keep the filtered loop their hosts expect.
6. Plugin UIs (the DAF `UI` class, CLAP floating windows, embedded views on macOS and Windows) are
   later work, not part of this.
7. Modifier keys are in the event, best effort, as described under the application side.
8. The 16 MiB ceiling and the 2 s and 5 s deadlines stay.
9. A window that has not opted in stays silent towards Mutter's XDND, as it is today.
10. Directories pass through unchecked.

Still open: nothing for phase 1b. Phase 2 owes the checks the macOS and Windows sections list,
above all a drag from another process on Windows.
