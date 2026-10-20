// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after the X11 backend, inside the DGL namespace.
class X11FileDrag : public PuglFileDrag
{
    Display* display = nullptr;
    ::Window source = None, target = None, destination = None;
    Atom enter, position, status, leave, drop, finished, selection, uri, copy, targets, timestamp, proxy, aware;
    std::string payload;
    Time acquired = CurrentTime, time = CurrentTime;
    int x = 0, y = 0;
    unsigned version = 0;
    bool grabbed = false, released = false, pending = false, queued = true;
    bool accepted = false, dropped = false, done = false;
    Cursor allowed = None, denied = None;
    typedef std::chrono::steady_clock Clock;
    Clock::time_point deadline;

    Atom atom(const char* name) { return XInternAtom(display, name, False); }
    unsigned long property(::Window window, Atom key, Atom expected)
    {
        Atom actual = None; int format = 0;
        unsigned long count = 0, remaining = 0, value = 0; unsigned char* bytes = nullptr;
        if (XGetWindowProperty(display, window, key, 0, 1, False, expected,
                               &actual, &format, &count, &remaining, &bytes) == Success
            && actual == expected && format == 32 && count == 1 && remaining == 0 && bytes)
            value = *reinterpret_cast<unsigned long*>(bytes);
        if (bytes) XFree(bytes);
        return value;
    }
    void message(Atom kind, long a = 0, long b = 0, long c = 0, long d = 0)
    {
        if (destination == None) return;
        XEvent event{};
        event.xclient.type = ClientMessage; event.xclient.display = display;
        event.xclient.window = target; event.xclient.message_type = kind; event.xclient.format = 32;
        event.xclient.data.l[0] = source; event.xclient.data.l[1] = a;
        event.xclient.data.l[2] = b; event.xclient.data.l[3] = c; event.xclient.data.l[4] = d;
        XSendEvent(display, destination, False, NoEventMask, &event);
    }
    void releaseInput()
    {
        if (!grabbed) return;
        XUngrabPointer(display, CurrentTime); XUngrabKeyboard(display, CurrentTime); grabbed = false;
    }
    void stop()
    {
        if (!done && !dropped) message(leave);
        releaseInput(); done = true;
    }
    void locate()
    {
        ::Window candidate = None, receiver = None, node = DefaultRootWindow(display);
        unsigned candidateVersion = 0;
        for (unsigned depth = 0; depth < 64 && node != None; ++depth)
        {
            ::Window route = node;
            const ::Window proposed = property(node, proxy, XA_WINDOW);
            if (proposed != None && property(proposed, proxy, XA_WINDOW) == proposed) route = proposed;
            const auto supported = property(route, aware, XA_ATOM);
            if (supported >= 3 && supported <= 255 && node != source)
            { candidate = node; receiver = route; candidateVersion = unsigned(std::min(5UL, supported)); }
            ::Window root = None, child = None; int rootX, rootY, localX, localY; unsigned mask;
            if (!XQueryPointer(display, node, &root, &child, &rootX, &rootY, &localX, &localY, &mask)) break;
            x = rootX; y = rootY; node = child;
        }
        if (target == candidate && destination == receiver) return;
        message(leave);
        target = candidate; destination = receiver; version = candidateVersion;
        pending = accepted = false; queued = true;
        if (target != None) message(enter, long(version) << 24, long(uri));
        if (grabbed) XChangeActivePointerGrab(display, PointerMotionMask | ButtonPressMask | ButtonReleaseMask, denied, CurrentTime);
    }
    void offer(const XSelectionRequestEvent& request)
    {
        if (request.owner != source || request.selection != selection) return;
        // ICCCM permits old requestors to omit a destination property.
        const Atom output = request.property != None ? request.property : request.target;
        bool supplied = false;
        if (request.target == uri)
        {
            XChangeProperty(display, request.requestor, output, uri, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(payload.data()), int(payload.size()));
            supplied = true;
        }
        else if (request.target == targets)
        {
            const Atom types[] = {uri, targets, timestamp};
            XChangeProperty(display, request.requestor, output, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(types), 3);
            supplied = true;
        }
        else if (request.target == timestamp)
        {
            const unsigned long value = acquired;
            XChangeProperty(display, request.requestor, output, XA_INTEGER, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(&value), 1);
            supplied = true;
        }
        XEvent reply{};
        reply.xselection.type = SelectionNotify; reply.xselection.display = display;
        reply.xselection.requestor = request.requestor; reply.xselection.selection = selection;
        reply.xselection.target = request.target; reply.xselection.time = request.time;
        reply.xselection.property = supplied ? output : None;
        XSendEvent(display, request.requestor, False, NoEventMask, &reply);
    }

public:
    bool init(PuglView* view, const char* filename)
    {
        struct stat info{};
        if (filename[0] != '/' || stat(filename, &info) || !S_ISREG(info.st_mode)) return false;
        payload = "file://";
        const char hex[] = "0123456789ABCDEF";
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(filename); *p; ++p)
        {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')
                || *p == '/' || *p == '-' || *p == '_' || *p == '.' || *p == '~') payload += char(*p);
            else { payload += '%'; payload += hex[*p >> 4]; payload += hex[*p & 15]; }
        }
        payload += "\r\n";
        Display* const original = view->world->impl->display;
        display = XOpenDisplay(DisplayString(original)); if (!display) return false;
        PuglX11ErrorTrap trap; puglX11BeginErrorTrap(&trap, display);
        enter=atom("XdndEnter"); position=atom("XdndPosition"); status=atom("XdndStatus");
        leave=atom("XdndLeave"); drop=atom("XdndDrop"); finished=atom("XdndFinished");
        selection=atom("XdndSelection"); uri=atom("text/uri-list"); copy=atom("XdndActionCopy");
        targets=atom("TARGETS"); timestamp=atom("TIMESTAMP"); proxy=atom("XdndProxy"); aware=atom("XdndAware");
        ::Window root, child; int rootX, rootY, localX, localY; unsigned mask = 0;
        const bool held = XQueryPointer(display, DefaultRootWindow(display), &root, &child,
                                       &rootX, &rootY, &localX, &localY, &mask) && (mask & Button1Mask);
        if (!held || payload.size() > std::size_t(XMaxRequestSize(display) * 4 - 128))
        { puglX11EndErrorTrap(&trap); return false; }
        XSetWindowAttributes attributes{}; attributes.override_redirect = True;
        attributes.event_mask = PropertyChangeMask;
        source = XCreateWindow(display, DefaultRootWindow(display), -10, -10, 1, 1, 0, 0, InputOnly,
                               CopyFromParent, CWOverrideRedirect | CWEventMask, &attributes);
        XMapWindow(display, source);
        // Get a real server timestamp rather than using Pugl's elapsed time.
        const Atom clock = atom("DGL_FILE_DRAG_TIME"); const unsigned char byte = 0;
        XChangeProperty(display, source, clock, XA_INTEGER, 8, PropModeReplace, &byte, 1);
        XEvent event{}; XSync(display, False);
        if (trap.errorCode || !XCheckWindowEvent(display, source, PropertyChangeMask, &event))
        { puglX11EndErrorTrap(&trap); return false; }
        acquired = time = event.xproperty.time;
        XSetSelectionOwner(display, selection, source, acquired);
        allowed = XCreateFontCursor(display, XC_hand2); denied = XCreateFontCursor(display, XC_X_cursor);
        // Only the original connection can release its implicit ButtonPress grab.
        XUngrabPointer(original, CurrentTime); XSync(original, False);
        const bool pointer = XGrabPointer(display, source, False, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                                         GrabModeAsync, GrabModeAsync, None, denied, CurrentTime) == GrabSuccess;
        const bool keyboard = pointer && XGrabKeyboard(display, source, False, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess;
        grabbed = pointer;
        const bool ok = pointer && keyboard && XGetSelectionOwner(display, selection) == source;
        if (!ok) releaseInput();
        if (ok) locate();
        const bool clean = puglX11EndErrorTrap(&trap) == 0;
        return ok && clean;
    }
    ~X11FileDrag() override
    {
        if (!display) return;
        PuglX11ErrorTrap trap; puglX11BeginErrorTrap(&trap, display);
        stop();
        if (allowed) XFreeCursor(display, allowed);
        if (denied) XFreeCursor(display, denied);
        if (source) XDestroyWindow(display, source);
        puglX11EndErrorTrap(&trap); XCloseDisplay(display);
    }
    void cancel() override
    {
        if (!display) return;
        PuglX11ErrorTrap trap; puglX11BeginErrorTrap(&trap, display);
        stop(); puglX11EndErrorTrap(&trap);
    }
    bool idle() override
    {
        if (done) return false;
        PuglX11ErrorTrap trap; puglX11BeginErrorTrap(&trap, display);
        bool moved = queued && !dropped;
        while (XPending(display) && !done)
        {
            XEvent event{}; XNextEvent(display, &event);
            if (event.type == SelectionRequest) offer(event.xselectionrequest);
            else if (event.type == SelectionClear) stop();
            else if (event.type == KeyPress && XLookupKeysym(&event.xkey, 0) == XK_Escape) stop();
            else if (event.type == ButtonPress && event.xbutton.button != Button1) stop();
            else if (event.type == MotionNotify && !released)
            { time = event.xmotion.time; moved = queued = true; }
            else if (event.type == ButtonRelease && event.xbutton.button == Button1 && !released)
            {
                time = event.xbutton.time; moved = queued = released = true;
                deadline = Clock::now() + std::chrono::seconds(5); releaseInput();
            }
            else if (event.type == ClientMessage && event.xclient.format == 32
                     && ::Window(uint32_t(event.xclient.data.l[0])) == target)
            {
                if (event.xclient.message_type == status && pending && !dropped)
                {
                    pending = false;
                    accepted = (event.xclient.data.l[1] & 1) && Atom(uint32_t(event.xclient.data.l[4])) == copy;
                    if (grabbed) XChangeActivePointerGrab(display, PointerMotionMask | ButtonPressMask | ButtonReleaseMask,
                                                         accepted ? allowed : denied, CurrentTime);
                }
                else if (event.xclient.message_type == finished && dropped) done = true;
            }
        }
        if (!done && !dropped)
        {
            if (moved) locate();
            if (target != None && queued && !pending)
            {
                const uint32_t coordinates = (uint32_t(uint16_t(x)) << 16) | uint16_t(y);
                message(position, 0, long(coordinates), long(time), long(copy));
                queued = false; pending = true;
            }
            if (released && !pending)
            {
                if (accepted && target != None)
                { message(drop, 0, long(time)); dropped = true; deadline = Clock::now() + std::chrono::seconds(30); }
                else stop();
            }
        }
        if (released && Clock::now() >= deadline) stop();
        XFlush(display);
        const bool clean = puglX11EndErrorTrap(&trap) == 0;
        if (!clean) { cancel(); return false; }
        return !done;
    }
};

PuglFileDrag* puglStartFileDrag(PuglView* view, const char* filename)
{
    std::unique_ptr<X11FileDrag> operation(new X11FileDrag);
    return operation->init(view, filename) ? operation.release() : nullptr;
}
