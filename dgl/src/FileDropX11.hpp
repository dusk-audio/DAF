// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after the X11 backend, inside the DGL namespace: the XDND target of a view that accepts file
// drops. See drop-targets.md, whose section on X11 gives the reason for each rule below.

// What the targets of a world share: the atoms, interned at the first opt-in, and every target.
struct PuglX11DropWorld
{
    Atom aware, enter, position, status, leave, drop, finished, selection, typeList, copy, incr, property;
    std::vector<PuglX11DropTarget*> targets;
    char hostName[256];
    int depth; // calls into a target on the stack; targets are only freed with none

    PuglX11DropWorld() noexcept
        : aware(None), enter(None), position(None), status(None), leave(None), drop(None), finished(None),
          selection(None), typeList(None), copy(None), incr(None), property(None), targets(), hostName(),
          depth(0) {}
};

struct PuglX11DropTarget : PuglDropHost
{
    PuglWorld* const world;
    PuglX11DropWorld& shared;
    PuglView* view;            // null once the view has gone; the world frees the target at a later tick
    const ::Window window;
    PuglDropSession session;
    bool accepting;
    ::Window source;           // the source of the drag over the window, None between drags
    unsigned version;          // the XDND version spoken with it
    bool watching;             // StructureNotifyMask is selected on the source, for its DestroyNotify
    Time stamp;                // the timestamp the next conversion uses
    Time requestTime;          // that of the conversion in flight
    Time staleTime;            // that of a conversion an earlier drag left unanswered
    bool requesting;           // a SelectionNotify is due
    bool incremental;          // an INCR transfer is in progress
    bool draining;             // the chunks of an INCR transfer nobody wants any more are thrown away
    double drainDeadline;
    std::vector<char> buffer;
    std::deque<XEvent> queue; // what waits for the current call to return, see post; type 0 is a tick
    bool busy;

    PuglX11DropTarget(PuglView* const v, PuglX11DropWorld& s)
        : world(v->world),
          shared(s),
          view(v),
          window(v->impl->win),
          session(*this),
          accepting(false),
          source(None),
          version(0),
          watching(false),
          stamp(CurrentTime),
          requestTime(CurrentTime),
          staleTime(CurrentTime),
          requesting(false),
          incremental(false),
          draining(false),
          drainDeadline(0.0),
          buffer(),
          queue(),
          busy(false) {}

    Display* display() const noexcept
    {
        return world->impl->display;
    }

    // Handle an event now, or once the call into the session on the stack returns: the view may run a
    // nested event loop from inside its callback, and the session must not be entered twice.
    void post(const XEvent& event)
    {
        queue.push_back(event);

        if (busy)
            return;

        busy = true;
        ++shared.depth;

        while (! queue.empty())
        {
            const XEvent next = queue.front();
            queue.pop_front();
            handle(next);
            settleTransfer();
        }

        --shared.depth;
        busy = false;
    }

    void postTick()
    {
        // a nested event loop ticks over and over; the call it is nested in ticks when it is done
        if (busy)
            return;

        XEvent event;
        std::memset(&event, 0, sizeof(event));
        post(event);
    }

    void setAccepting(const bool accept)
    {
        if (accept == accepting || view == nullptr)
            return;

        accepting = accept;

        if (accept)
        {
            const unsigned long xdndVersion = 5;
            XChangeProperty(display(), window, shared.aware, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(&xdndVersion), 1);
        }
        else
        {
            XDeleteProperty(display(), window, shared.aware);
            session.detach(true);
            if (! busy)
                settleTransfer();
        }

        XFlush(display());
    }

    // The view is going away. A drop being dispatched still gets its answer to the source.
    void orphan()
    {
        session.abandon();

        if (! session.isActive())
            forgetSource();

        requesting = incremental = draining = false;
        buffer.clear();
        accepting = false;
        view = nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // PuglDropHost

    void requestData() override
    {
        if (view == nullptr)
            return;

        if (requesting)
            staleTime = requestTime;

        requestTime = stamp;
        requesting = true;
        incremental = draining = false;
        buffer.clear();

        // lesson 2: the timestamp of the message that allows the conversion, never another field
        XConvertSelection(display(), shared.selection, world->impl->atoms.text_uri_list, shared.property,
                          window, stamp);
        XFlush(display());
    }

    void answer(const bool accept) override
    {
        // lesson 3: a copy, whatever the source asked for. Bit 1 asks for every position, with no rectangle.
        send(shared.status, (accept ? 1L : 0L) | 2L, 0, 0, accept ? static_cast<long>(shared.copy) : 0L);
    }

    void finish(const bool taken) override
    {
        // lesson 3 again; the accepted flag and the action arrived with version 5
        const bool told = version >= 5 && taken;
        send(shared.finished, told ? 1L : 0L, told ? static_cast<long>(shared.copy) : 0L, 0, 0);
        forgetSource();
    }

    PuglStatus dispatch(const PuglDropEvent& event) override
    {
        if (view == nullptr)
            return PUGL_FAILURE;

        PuglEvent wrapped;
        std::memset(&wrapped, 0, sizeof(wrapped));
        wrapped.drop = event;
        return puglDispatchEvent(view, &wrapped);
    }

    // ----------------------------------------------------------------------------------------------------------------

private:
    void handle(const XEvent& event)
    {
        switch (event.type)
        {
        case 0:
            tick(puglGetTime(world));
            break;
        case ClientMessage:
            onClientMessage(event.xclient);
            break;
        case SelectionNotify:
            onSelectionNotify(event.xselection);
            break;
        case PropertyNotify:
            onPropertyNotify(event.xproperty);
            break;
        case DestroyNotify:
            if (watching && event.xdestroywindow.window == source)
            {
                watching = false;
                endDrag();
            }
            break;
        }
    }

    void tick(const double now)
    {
        session.tick(now);

        if (draining && now >= drainDeadline)
        {
            draining = false;
            if (view != nullptr)
                XDeleteProperty(display(), window, shared.property);
        }
    }

    void onClientMessage(const XClientMessageEvent& message)
    {
        const Atom type = message.message_type;
        const ::Window from = static_cast<::Window>(static_cast<uint32_t>(message.data.l[0]));

        if (type == shared.enter)
            return onEnter(message, from);

        // a source the window has had no XdndEnter from, or one it ignored
        if (from == None || from != source || ! session.isActive())
            return;

        if (type == shared.position)
            onPosition(message);
        else if (type == shared.drop)
            onDrop(message);
        else if (type == shared.leave)
            endDrag();
    }

    void onEnter(const XClientMessageEvent& message, const ::Window from)
    {
        // a new drag ends any before it
        endDrag();

        // a window that does not accept drops stays silent, as it always was
        if (! accepting || view == nullptr || from == None)
            return;

        const unsigned long word = static_cast<uint32_t>(message.data.l[1]);
        const unsigned speaks = static_cast<unsigned>(word >> 24);

        // lesson 1: Mutter speaks version 5 whatever the window advertises, so anything from 3 up is taken
        if (speaks < 3)
            return;

        source = from;
        version = std::min(speaks, 5U);

        XSelectInput(display(), source, StructureNotifyMask);
        watching = true;

        // lesson 4: the enter has no position, so nothing reaches the view yet and no status is sent
        session.begin(offersUriList(message, (word & 1U) != 0), true);
    }

    bool offersUriList(const XClientMessageEvent& message, const bool typeList)
    {
        const Atom uri = world->impl->atoms.text_uri_list;

        if (! typeList)
        {
            for (int i = 2; i <= 4; ++i)
                if (static_cast<Atom>(static_cast<uint32_t>(message.data.l[i])) == uri)
                    return true;
            return false;
        }

        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;
        bool found = false;

        if (XGetWindowProperty(display(), source, shared.typeList, 0, 1024, False, XA_ATOM,
                               &type, &format, &count, &after, &data) == Success
            && type == XA_ATOM && format == 32 && data != nullptr)
        {
            // format 32 property data is an array of long
            for (unsigned long i = 0; i < count && ! found; ++i)
            {
                long atom = 0;
                std::memcpy(&atom, data + i * sizeof(long), sizeof(long));
                found = static_cast<Atom>(atom) == uri;
            }
        }

        if (data != nullptr)
            XFree(data);

        return found;
    }

    void onPosition(const XClientMessageEvent& message)
    {
        const uint32_t packed = static_cast<uint32_t>(message.data.l[2]);
        const int rootX = static_cast<int>(packed >> 16);
        const int rootY = static_cast<int>(packed & 0xffffU);

        // lesson 2: the position's timestamp is l[3]; l[2] holds the coordinates
        stamp = static_cast<Time>(static_cast<uint32_t>(message.data.l[3]));

        double x = 0.0, y = 0.0;
        PuglMods mods = 0;
        locate(rootX, rootY, x, y, mods);

        session.position(x, y, static_cast<double>(stamp) / 1e3, mods, puglGetTime(world));
    }

    void onDrop(const XClientMessageEvent& message)
    {
        // lesson 2: the drop's timestamp is l[2]
        stamp = static_cast<Time>(static_cast<uint32_t>(message.data.l[2]));

        double x = 0.0, y = 0.0;
        PuglMods mods = 0;
        locate(0, 0, x, y, mods);

        session.drop(static_cast<double>(stamp) / 1e3, mods, puglGetTime(world));
    }

    // The view position of a root position, and the modifiers held. One XQueryPointer gives both: the pointer's
    // root position minus its window position is the window's origin, wherever the pointer is.
    void locate(const int rootX, const int rootY, double& x, double& y, PuglMods& mods)
    {
        if (view == nullptr)
            return;

        ::Window root = None, child = None;
        int pointerRootX = 0, pointerRootY = 0, pointerX = 0, pointerY = 0;
        unsigned mask = 0;

        const Bool sameScreen = XQueryPointer(display(), window, &root, &child,
                                              &pointerRootX, &pointerRootY, &pointerX, &pointerY, &mask);
        mods = translateModifiers(mask);

        if (sameScreen)
        {
            x = rootX - (pointerRootX - pointerX);
            y = rootY - (pointerRootY - pointerY);
            return;
        }

        int translatedX = 0, translatedY = 0;
        if (XTranslateCoordinates(display(), RootWindow(display(), view->impl->screen), window,
                                  rootX, rootY, &translatedX, &translatedY, &child))
        {
            x = translatedX;
            y = translatedY;
        }
    }

    void onSelectionNotify(const XSelectionEvent& event)
    {
        const bool current = requesting && event.target == world->impl->atoms.text_uri_list
                          && (event.time == requestTime || event.time != staleTime);

        if (! current)
        {
            // the answer to a conversion of an earlier drag
            if (event.property != None && view != nullptr)
                XDeleteProperty(display(), window, event.property);
            return;
        }

        requesting = false;

        if (event.property == None)
            return session.dataFailed();

        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(display(), window, event.property, 0, static_cast<long>(PUGL_DROP_MAX_PAYLOAD / 4),
                               True, AnyPropertyType, &type, &format, &count, &after, &data) != Success)
        {
            if (data != nullptr)
                XFree(data);
            return session.dataFailed();
        }

        // ICCCM 2.7.2: deleting the INCR property, which the read above did, asks for the first chunk
        if (type == shared.incr)
        {
            if (data != nullptr)
                XFree(data);
            incremental = true;
            buffer.clear();
            return;
        }

        if (after != 0 || format != 8)
        {
            if (data != nullptr)
                XFree(data);
            XDeleteProperty(display(), window, event.property);
            return session.dataFailed();
        }

        session.data(reinterpret_cast<const char*>(data), count, shared.hostName);

        if (data != nullptr)
            XFree(data);
    }

    void onPropertyNotify(const XPropertyEvent& event)
    {
        if (event.state != PropertyNewValue || view == nullptr)
            return;

        // the source writing the answer to a conversion; its SelectionNotify follows
        if (requesting)
            return;

        // a chunk nobody asked for
        if (! incremental && ! draining)
        {
            XDeleteProperty(display(), window, event.atom);
            return;
        }

        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;

        const bool read = XGetWindowProperty(display(), window, event.atom, 0,
                                             static_cast<long>(PUGL_DROP_MAX_PAYLOAD / 4), True, AnyPropertyType,
                                             &type, &format, &count, &after, &data) == Success;

        if (draining)
        {
            if (data != nullptr)
                XFree(data);
            if (! read || after != 0)
                XDeleteProperty(display(), window, event.atom);
            else if (count == 0)
                draining = false;
            return;
        }

        // the zero-length chunk ends the transfer
        if (read && count == 0 && after == 0)
        {
            if (data != nullptr)
                XFree(data);

            incremental = false;
            std::vector<char> list;
            list.swap(buffer);
            session.data(list.empty() ? nullptr : list.data(), list.size(), shared.hostName);
            return;
        }

        if (! read || after != 0 || format != 8 || buffer.size() + count > PUGL_DROP_MAX_PAYLOAD)
        {
            if (data != nullptr)
                XFree(data);
            if (read && after != 0)
                XDeleteProperty(display(), window, event.atom);

            incremental = false;
            buffer.clear();
            startDraining();
            return session.dataFailed();
        }

        buffer.insert(buffer.end(), data, data + count);
        XFree(data);
    }

    // A transfer the session no longer wants is left to finish on its own, or drained.
    void settleTransfer()
    {
        if (session.wantsData())
            return;

        if (requesting)
        {
            requesting = false;
            staleTime = requestTime;
        }

        if (incremental)
        {
            incremental = false;
            buffer.clear();
            startDraining();
        }
    }

    void startDraining()
    {
        draining = true;
        drainDeadline = puglGetTime(world) + PUGL_DROP_HELD_TIMEOUT;
    }

    void endDrag()
    {
        if (session.isActive())
            session.leave();

        forgetSource();
    }

    void forgetSource()
    {
        if (watching && source != None)
            XSelectInput(display(), source, NoEventMask);

        watching = false;
        source = None;
        version = 0;
    }

    void send(const Atom type, const long l1, const long l2, const long l3, const long l4)
    {
        if (source == None)
            return;

        const ::Window to = source;
        XEvent event;
        std::memset(&event, 0, sizeof(event));
        event.xclient.type = ClientMessage;
        event.xclient.display = display();
        event.xclient.window = to;
        event.xclient.message_type = type;
        event.xclient.format = 32;
        event.xclient.data.l[0] = static_cast<long>(window);
        event.xclient.data.l[1] = l1;
        event.xclient.data.l[2] = l2;
        event.xclient.data.l[3] = l3;
        event.xclient.data.l[4] = l4;

        // a source gone since is a BadWindow for the error trap every caller is inside
        XSendEvent(display(), to, False, NoEventMask, &event);
        XFlush(display());
    }

    DAF_DECLARE_NON_COPYABLE(PuglX11DropTarget)
};

// --------------------------------------------------------------------------------------------------------------------
// hooks declared in x11.h

static PuglX11DropTarget* puglX11DropFind(PuglX11DropWorld* const shared, const ::Window window)
{
    for (size_t i = 0; i < shared->targets.size(); ++i)
        if (shared->targets[i]->view != nullptr && shared->targets[i]->window == window)
            return shared->targets[i];

    return nullptr;
}

static bool puglX11DropFilter(PuglWorld* const world, const XEvent* const event)
{
    PuglX11DropWorld* const shared = world->impl->drop;

    // nothing ever opted in: every event goes on as before
    if (shared == nullptr)
        return false;

    PuglX11DropTarget* target = nullptr;

    switch (event->type)
    {
    case ClientMessage:
    {
        const Atom type = event->xclient.message_type;
        if (event->xclient.format != 32
            || (type != shared->enter && type != shared->position && type != shared->leave && type != shared->drop))
            return false;

        // not a window that ever accepted drops: ignored, as before
        if ((target = puglX11DropFind(shared, event->xclient.window)) == nullptr)
            return false;

        target->post(*event);
        return true;
    }

    case SelectionNotify:
        if (event->xselection.selection != shared->selection)
            return false;
        if ((target = puglX11DropFind(shared, event->xselection.requestor)) != nullptr)
            target->post(*event);
        return true;

    case PropertyNotify:
        if (event->xproperty.atom != shared->property)
            return false;
        if ((target = puglX11DropFind(shared, event->xproperty.window)) != nullptr)
            target->post(*event);
        return true;

    case DestroyNotify:
    {
        // a source window, not one of ours, so findView would not know it
        bool taken = false;
        for (size_t i = 0; i < shared->targets.size(); ++i)
        {
            target = shared->targets[i];
            if (target->watching && target->source == event->xdestroywindow.window)
            {
                taken = true;
                target->post(*event);
            }
        }
        return taken;
    }
    }

    return false;
}

static void puglX11DropTick(PuglWorld* const world)
{
    PuglX11DropWorld* const shared = world->impl->drop;

    if (shared == nullptr)
        return;

    // the targets of views that went, once nothing of theirs is on the stack
    if (shared->depth == 0)
    {
        for (size_t i = shared->targets.size(); i > 0; --i)
        {
            PuglX11DropTarget* const target = shared->targets[i - 1];
            if (target->view == nullptr && ! target->busy && ! target->session.isActive())
            {
                shared->targets.erase(shared->targets.begin() + static_cast<std::ptrdiff_t>(i - 1));
                delete target;
            }
        }
    }

    for (size_t i = 0; i < shared->targets.size(); ++i)
        if (shared->targets[i]->session.getDeadline() > 0.0 || shared->targets[i]->draining)
            shared->targets[i]->postTick();
}

static void puglX11DropDetach(PuglView* const view)
{
    PuglX11DropTarget* const target = view->impl->drop;

    if (target == nullptr)
        return;

    view->impl->drop = nullptr;
    target->orphan();
}

static void puglX11DropFreeWorld(PuglWorld* const world)
{
    PuglX11DropWorld* const shared = world->impl->drop;

    if (shared == nullptr)
        return;

    world->impl->drop = nullptr;

    for (size_t i = 0; i < shared->targets.size(); ++i)
    {
        PuglX11DropTarget* const target = shared->targets[i];
        if (target->view != nullptr)
            target->view->impl->drop = nullptr;
        delete target;
    }

    delete shared;
}

// --------------------------------------------------------------------------------------------------------------------
// DGL extension declared in pugl.hpp

PuglStatus puglSetAcceptsFileDrops(PuglView* const view, const bool accept)
{
    PuglInternals* const impl = view->impl;
    PuglWorldInternals* const wimpl = view->world->impl;

    if (! accept)
    {
        if (impl->drop != nullptr)
        {
            PuglX11ErrorTrap trap;
            puglX11BeginErrorTrap(&trap, wimpl->display);
            impl->drop->setAccepting(false);
            puglX11EndErrorTrap(&trap);
        }
        return PUGL_SUCCESS;
    }

    // XDND addresses top-level windows (lesson 5), so an embedded view could never be reached
    if (view->parent != 0)
        return PUGL_UNSUPPORTED;

    if (impl->win == None)
        return PUGL_FAILURE;

    if (wimpl->drop == nullptr)
    {
        PuglX11DropWorld* const shared = new (std::nothrow) PuglX11DropWorld;
        if (shared == nullptr)
            return PUGL_NO_MEMORY;

        char names[12][16] = {
            "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop",
            "XdndFinished", "XdndSelection", "XdndTypeList", "XdndActionCopy", "INCR", "_DGL_XDND_DATA",
        };
        char* list[12];
        for (int i = 0; i < 12; ++i)
            list[i] = names[i];

        Atom atoms[12] = {};
        if (! XInternAtoms(wimpl->display, list, 12, False, atoms))
        {
            delete shared;
            return PUGL_FAILURE;
        }

        shared->aware     = atoms[0];
        shared->enter     = atoms[1];
        shared->position  = atoms[2];
        shared->status    = atoms[3];
        shared->leave     = atoms[4];
        shared->drop      = atoms[5];
        shared->finished  = atoms[6];
        shared->selection = atoms[7];
        shared->typeList  = atoms[8];
        shared->copy      = atoms[9];
        shared->incr      = atoms[10];
        shared->property  = atoms[11];

        // a URI may name this machine as its host, see PuglDropFiles
        if (gethostname(shared->hostName, sizeof(shared->hostName)) != 0)
            shared->hostName[0] = '\0';
        shared->hostName[sizeof(shared->hostName) - 1] = '\0';

        wimpl->drop = shared;
    }

    if (impl->drop == nullptr)
    {
        PuglX11DropTarget* const target = new (std::nothrow) PuglX11DropTarget(view, *wimpl->drop);
        if (target == nullptr)
            return PUGL_NO_MEMORY;

        wimpl->drop->targets.push_back(target);
        impl->drop = target;
    }

    PuglX11ErrorTrap trap;
    puglX11BeginErrorTrap(&trap, wimpl->display);
    impl->drop->setAccepting(true);
    puglX11EndErrorTrap(&trap);
    return PUGL_SUCCESS;
}
