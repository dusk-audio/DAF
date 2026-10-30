// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after the X11 backend, inside the DGL namespace: the XDND target of a view that accepts file
// drops. See drop-targets.md, whose section on X11 gives the reason for each rule below.

/// Conversions of a file list a target can have in flight at once, each with a property of its own
#define PUGL_X11_DROP_TRANSFERS 4

// What the targets of a world share: the atoms, interned at the first opt-in, and every target.
struct PuglX11DropWorld
{
    Atom aware, enter, position, status, leave, drop, finished, selection, typeList, copy, incr;
    Atom properties[PUGL_X11_DROP_TRANSFERS]; // _DGL_XDND_DATA0 and on, what a conversion is received into
    std::vector<PuglX11DropTarget*> targets;
    char hostName[256];
    int depth; // calls into a target on the stack; targets are only freed with none

    PuglX11DropWorld() noexcept
        : aware(None), enter(None), position(None), status(None), leave(None), drop(None), finished(None),
          selection(None), typeList(None), copy(None), incr(None), properties(), targets(), hostName(),
          depth(0) {}

    bool isProperty(const Atom atom) const noexcept
    {
        for (size_t i = 0; i < PUGL_X11_DROP_TRANSFERS; ++i)
            if (properties[i] == atom)
                return true;
        return false;
    }
};

struct PuglX11DropTarget : PuglDropHost
{
    /*
       A conversion of the file list, on a property of its own: a late answer to an earlier drag, or the
       chunks of its INCR transfer, are then told apart from the current one by their property, whatever
       time the owner reports in its SelectionNotify.
    */
    struct Transfer {
        bool requested;   // its SelectionNotify is due
        bool incremental; // its INCR chunks are coming
        bool current;     // the session waits for it; any other is drained and its data thrown away
        Time time;        // of its request, which a refusal, naming no property, is matched by
        double deadline;  // when one nobody waits for any more is given up
    };

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
    Transfer transfers[PUGL_X11_DROP_TRANSFERS];
    size_t lastSlot;           // the transfer the latest conversion took; the next looks from the one after
    std::vector<char> buffer;  // the current conversion's INCR chunks so far
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
          transfers(),
          lastSlot(PUGL_X11_DROP_TRANSFERS - 1),
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

        if (! busy)
            exclusively([] {});
    }

    // Do some work that may call the view, then handle what arrived meanwhile, in order. Inside a call already
    // under way the work runs as it is, and the queue is left to that call.
    template <class Work>
    void exclusively(const Work& work)
    {
        if (busy)
            return work();

        busy = true;
        ++shared.depth;
        work();
        settleTransfer();

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
            // the view's leave may run a nested event loop: what arrives meanwhile waits, as for any call to it
            XDeleteProperty(display(), window, shared.aware);
            exclusively([this] { session.detach(true); });
        }

        XFlush(display());
    }

    // The view is going away. A drop being dispatched still gets its answer to the source.
    void orphan()
    {
        session.abandon();

        if (! session.isActive())
            forgetSource();

        // the properties go with the window
        for (size_t i = 0; i < PUGL_X11_DROP_TRANSFERS; ++i)
            transfers[i] = Transfer();
        buffer.clear();
        accepting = false;
        view = nullptr;
    }

    // whether a conversion nobody waits for any more is still running, which the tick drains
    bool lingering() const noexcept
    {
        for (size_t i = 0; i < PUGL_X11_DROP_TRANSFERS; ++i)
            if (! transfers[i].current && busyTransfer(transfers[i]))
                return true;
        return false;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // PuglDropHost

    void requestData() override
    {
        if (view == nullptr)
            return;

        abandonTransfer();

        // A property no conversion is using, taken in turn: one given up at its deadline, whose owner may still
        // answer, is the last to be used again. With every one busy, the one closest to being given up.
        size_t slot = PUGL_X11_DROP_TRANSFERS;
        for (size_t i = 1; i <= PUGL_X11_DROP_TRANSFERS && slot == PUGL_X11_DROP_TRANSFERS; ++i)
        {
            const size_t next = (lastSlot + i) % PUGL_X11_DROP_TRANSFERS;
            if (! busyTransfer(transfers[next]))
                slot = next;
        }

        if (slot == PUGL_X11_DROP_TRANSFERS)
        {
            slot = 0;
            for (size_t i = 1; i < PUGL_X11_DROP_TRANSFERS; ++i)
                if (transfers[i].deadline < transfers[slot].deadline)
                    slot = i;
            XDeleteProperty(display(), window, shared.properties[slot]);
        }

        lastSlot = slot;
        Transfer& transfer = transfers[slot];
        transfer.requested = true;
        transfer.incremental = false;
        transfer.current = true;
        transfer.time = stamp;
        transfer.deadline = 0.0;
        buffer.clear();

        // lesson 2: the timestamp of the message that allows the conversion, never another field
        XConvertSelection(display(), shared.selection, world->impl->atoms.text_uri_list, shared.properties[slot],
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
            // nothing more goes to a source that is gone, not even the finish of a drop it held
            if (watching && event.xdestroywindow.window == source)
            {
                watching = false;
                source = None;
                endDrag();
            }
            break;
        }
    }

    void tick(const double now)
    {
        session.tick(now);

        for (size_t i = 0; i < PUGL_X11_DROP_TRANSFERS; ++i)
        {
            Transfer& transfer = transfers[i];

            if (transfer.current || ! busyTransfer(transfer) || now < transfer.deadline)
                continue;

            transfer.requested = transfer.incremental = false;
            if (view != nullptr)
                XDeleteProperty(display(), window, shared.properties[i]);
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
        // a window that has gone took its conversions and their properties with it
        if (event.target != world->impl->atoms.text_uri_list || view == nullptr)
            return;

        // A refusal names no property, so it is matched by its request's time. One that matches no request in
        // flight is left alone: the deadline settles a conversion that never answers.
        Transfer* transfer = nullptr;
        if (event.property != None)
            transfer = transferOf(event.property);
        else
            for (size_t i = 0; i < PUGL_X11_DROP_TRANSFERS && transfer == nullptr; ++i)
                if (transfers[i].requested && transfers[i].time == event.time)
                    transfer = &transfers[i];

        if (transfer == nullptr)
            return;

        // an answer nobody waits for on one of the target's properties
        if (! transfer->requested)
        {
            XDeleteProperty(display(), window, event.property);
            return;
        }

        transfer->requested = false;

        if (event.property == None)
            return failTransfer(*transfer);

        Atom type = None;
        int format = 0;
        unsigned long count = 0, after = 0;
        unsigned char* data = nullptr;

        if (XGetWindowProperty(display(), window, event.property, 0, static_cast<long>(PUGL_DROP_MAX_PAYLOAD / 4),
                               True, AnyPropertyType, &type, &format, &count, &after, &data) != Success)
        {
            if (data != nullptr)
                XFree(data);
            XDeleteProperty(display(), window, event.property);
            return failTransfer(*transfer);
        }

        // ICCCM 2.7.2: deleting the INCR property, which the read above did, asks for the first chunk
        if (type == shared.incr)
        {
            if (data != nullptr)
                XFree(data);
            transfer->incremental = true;
            if (transfer->current)
                buffer.clear();
            return;
        }

        if (after != 0 || format != 8)
        {
            if (data != nullptr)
                XFree(data);
            XDeleteProperty(display(), window, event.property);
            return failTransfer(*transfer);
        }

        // the late answer of an earlier drag, read and thrown away
        if (! transfer->current)
        {
            if (data != nullptr)
                XFree(data);
            return;
        }

        transfer->current = false;
        session.data(reinterpret_cast<const char*>(data), count, shared.hostName);

        if (data != nullptr)
            XFree(data);
    }

    void onPropertyNotify(const XPropertyEvent& event)
    {
        if (event.state != PropertyNewValue || view == nullptr)
            return;

        Transfer* const transfer = transferOf(event.atom);
        if (transfer == nullptr)
            return;

        // the owner writing the answer to a conversion; its SelectionNotify follows
        if (transfer->requested)
            return;

        // a chunk nobody asked for
        if (! transfer->incremental)
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

        // a chunk the read could not take whole is deleted all the same, so that the owner goes on
        if (! read || after != 0)
        {
            if (data != nullptr)
                XFree(data);
            XDeleteProperty(display(), window, event.atom);
            return failTransfer(*transfer);
        }

        // the zero-length chunk ends the transfer
        if (count == 0)
        {
            if (data != nullptr)
                XFree(data);

            transfer->incremental = false;
            if (! transfer->current)
                return;

            transfer->current = false;
            std::vector<char> list;
            list.swap(buffer);
            session.data(list.empty() ? nullptr : list.data(), list.size(), shared.hostName);
            return;
        }

        // drained: a chunk of a transfer nobody waits for any more
        if (! transfer->current)
        {
            XFree(data);
            return;
        }

        if (format != 8 || buffer.size() + count > PUGL_DROP_MAX_PAYLOAD)
        {
            XFree(data);
            return failTransfer(*transfer);
        }

        buffer.insert(buffer.end(), data, data + count);
        XFree(data);
    }

    static bool busyTransfer(const Transfer& transfer) noexcept
    {
        return transfer.requested || transfer.incremental;
    }

    Transfer* transferOf(const Atom property) noexcept
    {
        for (size_t i = 0; i < PUGL_X11_DROP_TRANSFERS; ++i)
            if (shared.properties[i] == property)
                return &transfers[i];
        return nullptr;
    }

    // The conversion the session waits for is no longer wanted: it is left to finish on its own, and drained
    void abandonTransfer()
    {
        for (size_t i = 0; i < PUGL_X11_DROP_TRANSFERS; ++i)
        {
            Transfer& transfer = transfers[i];

            if (transfer.current && busyTransfer(transfer))
            {
                transfer.current = false;
                transfer.deadline = puglGetTime(world) + PUGL_DROP_HELD_TIMEOUT;
                buffer.clear();
            }
        }
    }

    // A conversion that came to nothing; one still sending INCR chunks is drained
    void failTransfer(Transfer& transfer)
    {
        const bool wanted = transfer.current;
        transfer.current = false;
        transfer.deadline = puglGetTime(world) + PUGL_DROP_HELD_TIMEOUT;

        if (wanted)
        {
            buffer.clear();
            session.dataFailed();
        }
    }

    void settleTransfer()
    {
        if (! session.wantsData())
            abandonTransfer();
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
        if (! shared->isProperty(event->xproperty.atom))
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
        if (shared->targets[i]->session.getDeadline() > 0.0 || shared->targets[i]->lingering())
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

        char names[11 + PUGL_X11_DROP_TRANSFERS][16] = {
            "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop",
            "XdndFinished", "XdndSelection", "XdndTypeList", "XdndActionCopy", "INCR",
        };
        char* list[11 + PUGL_X11_DROP_TRANSFERS];
        for (int i = 0; i < 11 + PUGL_X11_DROP_TRANSFERS; ++i)
        {
            if (i >= 11)
                std::snprintf(names[i], sizeof(names[i]), "_DGL_XDND_DATA%d", i - 11);
            list[i] = names[i];
        }

        Atom atoms[11 + PUGL_X11_DROP_TRANSFERS] = {};
        if (! XInternAtoms(wimpl->display, list, 11 + PUGL_X11_DROP_TRANSFERS, False, atoms))
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
        for (int i = 0; i < PUGL_X11_DROP_TRANSFERS; ++i)
            shared->properties[i] = atoms[11 + i];

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
