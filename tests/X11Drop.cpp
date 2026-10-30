// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// File drops on X11 (dgl/src/FileDropX11.hpp, see dgl/src/drop-targets.md): a DGL window that accepts drops,
// and an XDND source inside the test on a connection of its own, which sends its messages straight to the
// window the way Mutter does for an XWayland window, and serves the selection, INCR included.
// Needs an X server; CI runs the unit tests under xvfb-run.

#include "tests.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#define DAF_TEST_POINT_CPP
#define DAF_TEST_WINDOW_CPP
#include "dgl/src/pugl.cpp"
#include "dgl/src/Application.cpp"
#include "dgl/src/ApplicationPrivateData.cpp"
#include "dgl/src/Geometry.cpp"
#include "dgl/src/Widget.cpp"
#include "dgl/src/WidgetPrivateData.cpp"
#include "dgl/src/Window.cpp"
#include "dgl/src/WindowPrivateData.cpp"

using DGL_NAMESPACE::Application;

static int failures = 0;

static void check(const bool ok, const char* const what)
{
    if (! ok)
    {
        ++failures;
        d_stderr2("X11Drop: FAILED %s", what);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// the window: accepts drops left of x = 100, and writes down what it hears

class DropWindow : public DGL_NAMESPACE::Window
{
public:
    std::string log;
    std::vector<std::string> files;
    double x = -1.0, y = -1.0;
    uint mod = 0xffff, time = 0;
    bool takeDrop = true;
    std::function<void()> duringDrop;

    explicit DropWindow(Application& app) : Window(app) {}
    DropWindow(Application& app, const uintptr_t parent) : Window(app, parent, 120, 80, 1.0, false) {}

    std::string take()
    {
        const std::string result(log);
        log.clear();
        return result;
    }

protected:
    void note(const char* const what, const FileDropEvent& ev)
    {
        log += what;
        x = ev.pos.getX();
        y = ev.pos.getY();
        mod = ev.mod;
        time = ev.time;
        files.clear();
        for (uint i = 0; i < ev.numFiles; ++i)
            files.push_back(ev.files[i]);
    }

    bool onFileDragEnter(const FileDropEvent& ev) override
    {
        note("ENTER;", ev);
        return ev.pos.getX() < 100.0;
    }

    bool onFileDragMove(const FileDropEvent& ev) override
    {
        note("MOTION;", ev);
        return ev.pos.getX() < 100.0;
    }

    void onFileDragLeave() override
    {
        log += "LEAVE;";
    }

    bool onFileDrop(const FileDropEvent& ev) override
    {
        note("DROP;", ev);
        const bool answer = takeDrop;
        const std::function<void()> step(duringDrop);
        if (step)
            step();
        return answer;
    }
};

// --------------------------------------------------------------------------------------------------------------------
// the source

struct Source
{
    Display* const dpy;
    ::Window win = None;
    Atom enter, position, status, leave, drop, finished, selection, typeList, copy, move, uri, incr, text, targets;
    std::string payload;
    bool serve = true;            // answer SelectionRequests as they come
    bool incremental = false;     // answer through INCR
    size_t chunk = 64 * 1024;
    std::vector<XEvent> messages; // the ClientMessages the window sent
    std::vector<XSelectionRequestEvent> requests;
    Time stamp = CurrentTime;

    // INCR in progress
    ::Window incrRequestor = None;
    Atom incrProperty = None, incrType = None;
    size_t incrOffset = 0;
    bool incrDone = true;

    explicit Source(Display* const d) : dpy(d)
    {
        enter     = XInternAtom(dpy, "XdndEnter", False);
        position  = XInternAtom(dpy, "XdndPosition", False);
        status    = XInternAtom(dpy, "XdndStatus", False);
        leave     = XInternAtom(dpy, "XdndLeave", False);
        drop      = XInternAtom(dpy, "XdndDrop", False);
        finished  = XInternAtom(dpy, "XdndFinished", False);
        selection = XInternAtom(dpy, "XdndSelection", False);
        typeList  = XInternAtom(dpy, "XdndTypeList", False);
        copy      = XInternAtom(dpy, "XdndActionCopy", False);
        move      = XInternAtom(dpy, "XdndActionMove", False);
        uri       = XInternAtom(dpy, "text/uri-list", False);
        incr      = XInternAtom(dpy, "INCR", False);
        text      = XInternAtom(dpy, "text/plain", False);
        targets   = XInternAtom(dpy, "TARGETS", False);
        open();
    }

    ~Source()
    {
        if (win != None)
            XDestroyWindow(dpy, win);
        XSync(dpy, False);
    }

    void open()
    {
        win = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 1, 1, 0, 0, 0);
        XSelectInput(dpy, win, PropertyChangeMask);
        // a real server time, as an owner must use
        const Atom clock = XInternAtom(dpy, "DAF_TEST_DROP_TIME", False);
        const unsigned char byte = 0;
        XChangeProperty(dpy, win, clock, XA_INTEGER, 8, PropModeReplace, &byte, 1);
        XEvent event;
        for (;;)
        {
            XNextEvent(dpy, &event);
            if (event.type == PropertyNotify && event.xproperty.atom == clock)
                break;
        }
        stamp = event.xproperty.time;
        XSetSelectionOwner(dpy, selection, win, stamp);
        XSync(dpy, False);
    }

    void reopen()
    {
        XDestroyWindow(dpy, win);
        open();
    }

    void send(const ::Window to, const Atom type, const long l1, const long l2, const long l3, const long l4)
    {
        XEvent event;
        std::memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.display = dpy;
        event.xclient.window = to;
        event.xclient.message_type = type;
        event.xclient.format = 32;
        event.xclient.data.l[0] = static_cast<long>(win);
        event.xclient.data.l[1] = l1;
        event.xclient.data.l[2] = l2;
        event.xclient.data.l[3] = l3;
        event.xclient.data.l[4] = l4;
        XSendEvent(dpy, to, False, NoEventMask, &event);
        XFlush(dpy);
    }

    void sendEnter(const ::Window to, const long version, const std::vector<Atom>& types)
    {
        if (types.size() > 3)
        {
            XChangeProperty(dpy, win, typeList, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(types.data()), static_cast<int>(types.size()));
            send(to, enter, version << 24 | 1, 0, 0, 0);
        }
        else
        {
            send(to, enter, version << 24,
                 types.size() > 0 ? static_cast<long>(types[0]) : 0,
                 types.size() > 1 ? static_cast<long>(types[1]) : 0,
                 types.size() > 2 ? static_cast<long>(types[2]) : 0);
        }
    }

    void sendPosition(const ::Window to, const int rootX, const int rootY, const Time time, const Atom action)
    {
        send(to, position, 0, static_cast<long>(rootX) << 16 | rootY, static_cast<long>(time), static_cast<long>(action));
    }

    void reply(const XSelectionRequestEvent& request, const bool supplied)
    {
        XEvent event;
        std::memset(&event, 0, sizeof(event));
        event.xselection.type = SelectionNotify;
        event.xselection.display = dpy;
        event.xselection.requestor = request.requestor;
        event.xselection.selection = request.selection;
        event.xselection.target = request.target;
        event.xselection.time = request.time;
        event.xselection.property = supplied ? request.property : None;
        XSendEvent(dpy, request.requestor, False, NoEventMask, &event);
        XFlush(dpy);
    }

    void answer(const XSelectionRequestEvent& request)
    {
        if (request.target != uri)
            return reply(request, false);

        if (! incremental)
        {
            XChangeProperty(dpy, request.requestor, request.property, uri, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(payload.data()), static_cast<int>(payload.size()));
            return reply(request, true);
        }

        // ICCCM 2.7.2: announce INCR, then a chunk each time the requestor deletes the property
        XSelectInput(dpy, request.requestor, PropertyChangeMask);
        const long size = static_cast<long>(payload.size());
        XChangeProperty(dpy, request.requestor, request.property, incr, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(&size), 1);
        incrRequestor = request.requestor;
        incrProperty = request.property;
        incrType = uri;
        incrOffset = 0;
        incrDone = false;
        reply(request, true);
    }

    void nextChunk()
    {
        const size_t length = std::min(chunk, payload.size() - incrOffset);
        XChangeProperty(dpy, incrRequestor, incrProperty, incrType, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(payload.data() + incrOffset), static_cast<int>(length));
        incrOffset += length;
        if (length == 0)
            incrDone = true;
        XFlush(dpy);
    }

    void read()
    {
        while (XPending(dpy) > 0)
        {
            XEvent event;
            XNextEvent(dpy, &event);

            if (event.type == SelectionRequest)
            {
                requests.push_back(event.xselectionrequest);
                if (serve)
                    answer(event.xselectionrequest);
            }
            else if (event.type == ClientMessage)
            {
                messages.push_back(event);
            }
            else if (event.type == PropertyNotify && ! incrDone && event.xproperty.window == incrRequestor
                     && event.xproperty.atom == incrProperty && event.xproperty.state == PropertyDelete)
            {
                nextChunk();
            }
        }
    }

    // the last ClientMessage of a type the window sent, or null
    const XClientMessageEvent* last(const Atom type) const
    {
        for (size_t i = messages.size(); i > 0; --i)
            if (messages[i - 1].xclient.message_type == type)
                return &messages[i - 1].xclient;
        return nullptr;
    }

    size_t count(const Atom type) const
    {
        size_t n = 0;
        for (size_t i = 0; i < messages.size(); ++i)
            if (messages[i].xclient.message_type == type)
                ++n;
        return n;
    }
};

// run both sides until the condition holds or the time is up
static bool pump(Application& app, Source& source, const std::function<bool()>& done, const int ms = 2000)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;)
    {
        app.idle();
        source.read();
        if (done())
            return true;
        if (std::chrono::steady_clock::now() >= end)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static void settle(Application& app, Source& source, const int ms = 150)
{
    pump(app, source, [] { return false; }, ms);
}

static bool hasProperty(Display* const dpy, const ::Window window, const Atom property)
{
    int count = 0;
    Atom* const atoms = XListProperties(dpy, window, &count);
    bool found = false;
    for (int i = 0; i < count; ++i)
        found = found || atoms[i] == property;
    if (atoms != nullptr)
        XFree(atoms);
    return found;
}

static long awareVersion(Display* const dpy, const ::Window window, const Atom aware)
{
    Atom type = None;
    int format = 0;
    unsigned long count = 0, after = 0;
    unsigned char* data = nullptr;
    long version = -1;
    if (XGetWindowProperty(dpy, window, aware, 0, 1, False, XA_ATOM, &type, &format, &count, &after, &data) == Success
        && type == XA_ATOM && format == 32 && count == 1)
        std::memcpy(&version, data, sizeof(long));
    if (data != nullptr)
        XFree(data);
    return version;
}

static uint32_t low32(const long value)
{
    return static_cast<uint32_t>(value);
}

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    Display* const dpy = XOpenDisplay(nullptr);
    DAF_ASSERT_NOT_EQUAL(dpy, nullptr, "an X server is reachable");

    const Atom aware = XInternAtom(dpy, "XdndAware", False);
    const Atom dataProperty = XInternAtom(dpy, "_DGL_XDND_DATA", False);

    Application app(true, Application::kTypeAuto, Application::kWindowSystemX11);
    DAF_ASSERT_EQUAL(app.getWindowSystem(), Application::kWindowSystemX11, "the application opens its windows on X11");

    std::unique_ptr<DropWindow> win(new DropWindow(app));
    win->setSize(300, 200);
    win->show();
    app.idle();

    ::Window target = static_cast<::Window>(win->getNativeWindowHandle());
    Source source(dpy);
    source.payload = "file:///tmp/a.wav\r\nfile:///tmp/caf%C3%A9.mid\r\nhttps://example.com/x\r\n";

    const std::vector<Atom> uriOnly = { source.uri };
    const std::vector<Atom> fourTypes = { source.text, source.targets, source.copy, source.uri };
    const std::vector<Atom> noFiles = { source.text };

    // a window that has not opted in: no XdndAware, and silence, as before
    check(awareVersion(dpy, target, aware) == -1, "no XdndAware before opting in");
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, 10, 10, source.stamp, source.copy);
    settle(app, source);
    check(source.messages.empty() && source.requests.empty(), "a window that did not opt in stays silent");
    check(win->take().empty(), "and hears nothing");
    source.send(target, source.leave, 0, 0, 0, 0);

    check(win->setAcceptingFileDrops(true), "a top-level window opts in");
    check(win->isAcceptingFileDrops(), "and accepts drops");
    check(awareVersion(dpy, target, aware) == 5, "with XdndAware 5 on its window");

    // where the window is on the root
    int originX = 0, originY = 0;
    {
        ::Window child = None;
        XTranslateCoordinates(dpy, target, DefaultRootWindow(dpy), 0, 0, &originX, &originY, &child);
    }

    // version 5, copy
    source.messages.clear();
    source.sendEnter(target, 5, uriOnly);
    settle(app, source, 100);
    check(source.messages.empty() && source.requests.empty(), "an enter alone sends nothing and asks nothing");
    check(win->take().empty(), "and the window hears nothing of it");

    const Time positionTime = source.stamp + 1;
    source.sendPosition(target, originX + 40, originY + 30, positionTime, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 1; }), "the position is answered");
    check(source.requests.size() == 1, "after one conversion");
    if (source.requests.size() == 1)
    {
        check(source.requests[0].time == positionTime, "lesson 2: converted with the position's l[3]");
        check(source.requests[0].target == source.uri, "for text/uri-list");
    }
    if (const XClientMessageEvent* const status = source.last(source.status))
    {
        check(low32(status->data.l[0]) == target, "the status names the window");
        check((status->data.l[1] & 1) == 1, "and accepts");
        check(static_cast<Atom>(low32(status->data.l[4])) == source.copy, "a copy");
    }
    check(win->take() == "ENTER;", "the window hears the enter once the files are in");
    check(win->x == 40.0 && win->y == 30.0, "at the root position translated to the window");
    check(win->files.size() == 2 && win->files[0] == "/tmp/a.wav" && win->files[1] == "/tmp/caf\xc3\xa9.mid",
          "with the local files, decoded");
    check(win->time == positionTime, "with the position's time");
    check(win->mod == 0, "with no modifier held");

    source.sendPosition(target, originX + 150, originY + 30, positionTime + 1, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 2; }), "a refused position is answered");
    if (const XClientMessageEvent* const status = source.last(source.status))
        check((status->data.l[1] & 1) == 0 && status->data.l[4] == 0, "with a refusal and no action");
    check(win->take() == "MOTION;", "after asking the window");

    source.sendPosition(target, originX + 50, originY + 60, positionTime + 2, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 3; }), "an accepted position again");
    win->take();

    const Time dropTime = positionTime + 3;
    source.send(target, source.drop, 0, static_cast<long>(dropTime), 0, 0);
    check(pump(app, source, [&] { return source.count(source.finished) == 1; }), "the drop is finished");
    if (const XClientMessageEvent* const finished = source.last(source.finished))
    {
        check((finished->data.l[1] & 1) == 1, "v5: accepted");
        check(static_cast<Atom>(low32(finished->data.l[2])) == source.copy, "lesson 3: v5 finished says copy");
    }
    check(win->take() == "DROP;", "the window hears the drop");
    check(win->x == 50.0 && win->y == 60.0 && win->files.size() == 2, "at the last position, with the files");
    check(source.requests.size() == 1, "no second conversion at the drop");
    source.send(target, source.leave, 0, 0, 0, 0);
    settle(app, source, 50);
    check(win->take().empty(), "no leave after a drop");
    check(! hasProperty(dpy, target, dataProperty), "no DGL property left on the window");

    // a move requested, through a type list, at version 4: still a copy, and v4 has no finished fields
    source.messages.clear();
    source.requests.clear();
    source.sendEnter(target, 4, fourTypes);
    source.sendPosition(target, originX + 10, originY + 10, positionTime + 10, source.move);
    check(pump(app, source, [&] { return source.count(source.status) == 1; }), "a type list with text/uri-list is taken");
    if (const XClientMessageEvent* const status = source.last(source.status))
        check((status->data.l[1] & 1) == 1 && static_cast<Atom>(low32(status->data.l[4])) == source.copy,
              "lesson 3: a move asked for is answered with a copy");
    source.send(target, source.drop, 0, static_cast<long>(positionTime + 11), 0, 0);
    check(pump(app, source, [&] { return source.count(source.finished) == 1; }), "the v4 drop is finished");
    if (const XClientMessageEvent* const finished = source.last(source.finished))
        check(finished->data.l[1] == 0 && finished->data.l[2] == 0, "below v5 the finished fields stay 0");
    win->take();

    // versions 3 and 6 are both taken (lesson 1), version 2 is not
    for (const long version : { 3L, 6L, 2L })
    {
        source.messages.clear();
        source.sendEnter(target, version, uriOnly);
        source.sendPosition(target, originX + 10, originY + 10, positionTime + 20, source.copy);
        const bool answered = pump(app, source, [&] { return source.count(source.status) == 1; }, version == 2 ? 200 : 2000);
        check(answered == (version != 2), version == 2 ? "version 2 is ignored" : "versions 3 and 6 are answered");
        source.send(target, source.leave, 0, 0, 0, 0);
        settle(app, source, 50);
        win->take();
    }

    // a source that drops where the window refused: the drop fails, and the window hears the drag leave
    source.messages.clear();
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, originX + 150, originY + 10, positionTime + 25, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 1; }), "a position the window refuses is answered");
    source.send(target, source.drop, 0, static_cast<long>(positionTime + 26), 0, 0);
    check(pump(app, source, [&] { return source.count(source.finished) == 1; }), "a drop there is finished");
    if (const XClientMessageEvent* const finished = source.last(source.finished))
        check((finished->data.l[1] & 1) == 0 && finished->data.l[2] == 0, "as refused");
    check(win->take() == "ENTER;LEAVE;", "and the window that had the enter hears a leave");

    // no text/uri-list: refused, nothing asked for, and the window never hears of it
    source.messages.clear();
    source.requests.clear();
    source.sendEnter(target, 5, noFiles);
    source.sendPosition(target, originX + 10, originY + 10, positionTime + 30, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 1; }), "a drag without files is answered");
    if (const XClientMessageEvent* const status = source.last(source.status))
        check((status->data.l[1] & 1) == 0, "with a refusal");
    check(source.requests.empty(), "without a conversion");
    source.send(target, source.drop, 0, static_cast<long>(positionTime + 31), 0, 0);
    check(pump(app, source, [&] { return source.count(source.finished) == 1; }), "its drop is finished");
    if (const XClientMessageEvent* const finished = source.last(source.finished))
        check((finished->data.l[1] & 1) == 0 && finished->data.l[2] == 0, "as refused");
    check(win->take().empty(), "and the window heard nothing");

    // nothing usable in the list
    {
        const std::string files(source.payload);
        source.payload = "https://example.com/a\r\nfile://elsewhere/b\r\n";
        source.messages.clear();
        source.sendEnter(target, 5, uriOnly);
        source.sendPosition(target, originX + 10, originY + 10, positionTime + 40, source.copy);
        check(pump(app, source, [&] { return source.count(source.status) == 1; }), "a list without local files is answered");
        if (const XClientMessageEvent* const status = source.last(source.status))
            check((status->data.l[1] & 1) == 0, "with a refusal");
        source.send(target, source.leave, 0, 0, 0, 0);
        settle(app, source, 50);
        check(win->take().empty(), "and the window heard nothing");
        source.payload = files;
    }

    // the drag leaves while the conversion is in flight: the late answer is ignored and cleaned up
    source.messages.clear();
    source.requests.clear();
    source.serve = false;
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, originX + 10, originY + 10, positionTime + 50, source.copy);
    check(pump(app, source, [&] { return source.requests.size() == 1; }), "a drag that will leave asks for its files");
    source.send(target, source.leave, 0, 0, 0, 0);
    settle(app, source, 50);
    source.answer(source.requests[0]);
    settle(app, source, 100);
    source.serve = true;
    check(win->take().empty(), "a drag that left during the conversion never reaches the window");
    check(source.count(source.status) == 0, "nor is its position answered after it left");
    check(! hasProperty(dpy, target, dataProperty), "and its late answer leaves nothing on the window");

    // a new drag before the last one ended
    source.messages.clear();
    source.requests.clear();
    source.serve = false;
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, originX + 10, originY + 10, positionTime + 60, source.copy);
    check(pump(app, source, [&] { return source.requests.size() == 1; }), "the first drag asks for its files");
    Display* const otherDisplay = XOpenDisplay(nullptr);
    {
        Source other(otherDisplay);
        other.payload = "file:///tmp/other.wav\r\n";
        other.sendEnter(target, 5, uriOnly);
        const Time otherTime = other.stamp + 1;
        other.sendPosition(target, originX + 20, originY + 20, otherTime, other.copy);
        check(pump(app, other, [&] { return other.count(other.status) == 1; }), "the second drag is answered");
        source.answer(source.requests[0]);
        settle(app, source, 100);
        check(win->take() == "ENTER;", "the window hears the second drag only");
        check(win->files.size() == 1 && win->files[0] == "/tmp/other.wav", "with its files");
        other.send(target, other.drop, 0, static_cast<long>(otherTime + 1), 0, 0);
        check(pump(app, other, [&] { return other.count(other.finished) == 1; }), "and its drop is finished");
        check(win->take() == "DROP;", "and dropped");
    }
    XCloseDisplay(otherDisplay);
    source.serve = true;

    // the other source took the selection and died with it
    source.reopen();

    // the source window dies before it answers the conversion
    source.messages.clear();
    source.requests.clear();
    source.serve = false;
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, originX + 10, originY + 10, positionTime + 70, source.copy);
    check(pump(app, source, [&] { return source.requests.size() == 1; }), "a source that will die is asked for its files");
    source.reopen();
    settle(app, source, 100);
    check(win->take().empty(), "a dead source's drag never reaches the window");
    source.serve = true;

    // INCR, for a list well past what one request carries
    {
        std::string list;
        for (int i = 0; i < 6000; ++i)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "file:///tmp/clip%05d.wav\r\n", i);
            list += line;
        }
        source.payload = list;
        source.incremental = true;
        source.chunk = 32 * 1024;
        source.messages.clear();
        source.requests.clear();
        source.sendEnter(target, 5, uriOnly);
        source.sendPosition(target, originX + 10, originY + 10, source.stamp + 1, source.copy);
        check(pump(app, source, [&] { return source.count(source.status) == 1; }, 5000), "an INCR transfer completes");
        check(win->take() == "ENTER;", "the window hears the enter");
        check(win->files.size() == 6000 && win->files[5999] == "/tmp/clip05999.wav", "with every file of the list");
        source.send(target, source.leave, 0, 0, 0, 0);
        settle(app, source, 50);
        check(win->take() == "LEAVE;", "and its leave");
        check(! hasProperty(dpy, target, dataProperty), "no DGL property left after INCR");

        // past the ceiling: refused, and the rest of the transfer drained
        list.clear();
        const std::string line(1000, 'x');
        while (list.size() <= PUGL_DROP_MAX_PAYLOAD)
            list += "file:///tmp/" + line + "\r\n";
        source.payload = list;
        source.chunk = 1024 * 1024;
        source.messages.clear();
        source.requests.clear();
        source.sendEnter(target, 5, uriOnly);
        source.sendPosition(target, originX + 10, originY + 10, source.stamp + 2, source.copy);
        check(pump(app, source, [&] { return source.count(source.status) == 1; }, 10000), "an oversized list is answered");
        if (const XClientMessageEvent* const status = source.last(source.status))
            check((status->data.l[1] & 1) == 0, "with a refusal");
        pump(app, source, [&] { return source.incrDone; }, 10000);
        check(source.incrDone, "the source got to finish its transfer");
        source.send(target, source.leave, 0, 0, 0, 0);
        settle(app, source, 100);
        check(win->take().empty(), "and the window heard nothing");
        check(! hasProperty(dpy, target, dataProperty), "no DGL property left after a drained transfer");

        source.incremental = false;
        source.payload = "file:///tmp/a.wav\r\n";
    }

    // a drop before its files, then the window goes: the source still hears the drop failed
    source.messages.clear();
    source.requests.clear();
    source.serve = false;
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, originX + 10, originY + 10, source.stamp + 3, source.copy);
    check(pump(app, source, [&] { return source.requests.size() == 1; }), "a drag that will drop early asks for its files");
    source.send(target, source.drop, 0, static_cast<long>(source.stamp + 4), 0, 0);
    settle(app, source, 50);
    check(source.count(source.finished) == 0, "a drop before its files waits for them");
    win->close();
    win.reset();
    source.read();
    pump(app, source, [&] { return source.count(source.finished) == 1; }, 500);
    check(source.count(source.finished) == 1, "the window going fails the held drop");
    if (const XClientMessageEvent* const finished = source.last(source.finished))
        check((finished->data.l[1] & 1) == 0, "as not accepted");
    source.serve = true;

    // a window deleting itself from inside its drop: the source still gets the window's answer
    win.reset(new DropWindow(app));
    win->setSize(300, 200);
    win->show();
    app.idle();
    target = static_cast<::Window>(win->getNativeWindowHandle());
    {
        ::Window child = None;
        XTranslateCoordinates(dpy, target, DefaultRootWindow(dpy), 0, 0, &originX, &originY, &child);
    }
    check(win->setAcceptingFileDrops(true), "a new window opts in");
    source.messages.clear();
    source.requests.clear();
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, originX + 10, originY + 10, source.stamp + 5, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 1; }), "its position is answered");
    win->duringDrop = [&win] { win->close(); win.reset(); };
    source.send(target, source.drop, 0, static_cast<long>(source.stamp + 6), 0, 0);
    check(pump(app, source, [&] { return source.count(source.finished) == 1; }), "the drop is finished");
    check(win.get() == nullptr, "by a window that deleted itself");
    if (const XClientMessageEvent* const finished = source.last(source.finished))
        check((finished->data.l[1] & 1) == 1, "with the answer it gave");

    // opting out removes XdndAware and ends a drag for the window with a leave
    win.reset(new DropWindow(app));
    win->setSize(300, 200);
    win->show();
    app.idle();
    target = static_cast<::Window>(win->getNativeWindowHandle());
    {
        ::Window child = None;
        XTranslateCoordinates(dpy, target, DefaultRootWindow(dpy), 0, 0, &originX, &originY, &child);
    }
    check(win->setAcceptingFileDrops(true), "the window opts in");
    source.messages.clear();
    source.sendEnter(target, 5, uriOnly);
    source.sendPosition(target, originX + 10, originY + 10, source.stamp + 7, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 1; }), "its position is answered");
    win->take();
    check(win->setAcceptingFileDrops(false), "and opts out");
    check(win->take() == "LEAVE;", "which ends the drag for it with a leave");
    check(awareVersion(dpy, target, aware) == -1, "and removes XdndAware");
    source.sendPosition(target, originX + 11, originY + 11, source.stamp + 8, source.copy);
    check(pump(app, source, [&] { return source.count(source.status) == 2; }), "the rest of the drag is answered");
    if (const XClientMessageEvent* const status = source.last(source.status))
        check((status->data.l[1] & 1) == 0, "with refusals");
    source.send(target, source.leave, 0, 0, 0, 0);
    settle(app, source, 50);

    // an embedded window cannot take drops: XDND addresses top-level windows
    {
        const ::Window host = XCreateSimpleWindow(dpy, DefaultRootWindow(dpy), 0, 0, 200, 200, 0, 0, 0);
        XMapWindow(dpy, host);
        XSync(dpy, False);
        DropWindow embedded(app, static_cast<uintptr_t>(host));
        check(! embedded.setAcceptingFileDrops(true), "an embedded window refuses to opt in");
        check(awareVersion(dpy, static_cast<::Window>(embedded.getNativeWindowHandle()), aware) == -1,
              "and gets no XdndAware");
        embedded.close();
        XDestroyWindow(dpy, host);
        XSync(dpy, False);
    }

    win->close();
    win.reset();

    if (failures != 0)
    {
        d_stderr2("X11Drop: %d checks failed", failures);
        return 1;
    }

    d_stdout("X11Drop: all checks passed");
    return 0;
}
