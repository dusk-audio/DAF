// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after the Wayland backend, inside the DGL namespace: file drops through the seat's data device,
// for the views of a world that accept them. See drop-targets.md.
//
// The data device belongs to the seat, so there is one target per world and one drag at a time. The
// target exists from the first view that accepts drops on, and from then on handles every drag of the
// world: those over a view that does not accept drops are declined as wayland.c declines them.

struct PuglWaylandDropTarget : PuglDropHost
{
    // A data device event, handled once the current call into the session returns, see post
    struct Message {
        enum Kind { kEnter, kMotion, kLeave, kDrop, kReadable, kTick } kind;
        PuglWaylandOffer* offer; // kEnter: owned by the message until handled
        PuglView* view;          // kEnter: the view under the drag, if any
        uint32_t serial;         // kEnter
        double x, y;             // kEnter, kMotion: surface coordinates
    };

    PuglWorldInternals* const impl;
    PuglDropSession session;
    PuglWaylandOffer* offer;     // the offer of the drag over the world, declined or not
    PuglView* view;              // the view the drag is over, when it accepts drops
    const char* mimeType;        // the text/uri-list type as the source offers it, owned by the offer
    uint32_t serial;             // of the enter, for wl_data_offer.accept
    bool acceptState;            // what wl_data_offer.accept last said, NULL being false
    int fd;                      // the read end of the receive in progress, or -1
    std::vector<char> received;
    std::deque<Message> queue;
    bool busy;
    char hostName[256];

    explicit PuglWaylandDropTarget(PuglWorldInternals* const i)
        : impl(i),
          session(*this),
          offer(nullptr),
          view(nullptr),
          mimeType(nullptr),
          serial(0),
          acceptState(false),
          fd(-1),
          received(),
          queue(),
          busy(false),
          hostName()
    {
        // a URI may name this machine as its host, see PuglDropFiles
        if (gethostname(hostName, sizeof(hostName)) != 0)
            hostName[0] = '\0';
        hostName[sizeof(hostName) - 1] = '\0';
    }

    ~PuglWaylandDropTarget() override
    {
        closeReceive();

        for (size_t i = 0; i < queue.size(); ++i)
            puglWaylandFreeOffer(queue[i].offer);

        puglWaylandFreeOffer(offer);
    }

    // Handle an event now, or once the call into the session on the stack returns: the view may run a
    // nested event loop from inside its callback, and the session must not be entered twice.
    void post(const Message& message)
    {
        queue.push_back(message);

        if (busy)
            return;

        busy = true;

        while (! queue.empty())
        {
            const Message next = queue.front();
            queue.pop_front();
            handle(next);

            if (fd >= 0 && ! session.wantsData())
                closeReceive();
        }

        busy = false;
    }

    // The view stopped accepting drops: a drag over it ends for it, and the rest is refused
    void optOut(PuglView* const gone)
    {
        if (view != gone)
            return;

        session.detach(true);
        refuse();
    }

    // The view is going away: nothing more reaches it. Its offer stays until the compositor's leave.
    void detachView(PuglView* const gone)
    {
        for (size_t i = 0; i < queue.size(); ++i)
            if (queue[i].view == gone)
                queue[i].view = nullptr;

        if (view != gone)
            return;

        session.abandon();

        if (! session.isActive())
            refuse();
        view = nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // PuglDropHost

    void requestData() override
    {
        if (offer == nullptr || mimeType == nullptr || fd >= 0)
            return;

        // Close-on-exec: the host may fork at any moment, and a child holding the write end would keep the
        // transfer from ever ending. Non-blocking: the poll set says when to read.
        int fds[2] = { -1, -1 };
        if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0)
            return; // the deadline refuses the drag

        wl_data_offer_receive(offer->offer, mimeType, fds[1]);
        close(fds[1]);
        wl_display_flush(impl->display);

        fd = fds[0];
        received.clear();
    }

    void answer(const bool accept) override
    {
        if (offer == nullptr || accept == acceptState)
            return;

        wl_data_offer_accept(offer->offer, serial, accept ? mimeType : NULL);
        acceptState = accept;
        wl_display_flush(impl->display);
    }

    void finish(const bool taken) override
    {
        if (offer != nullptr)
        {
            // finish after a NULL accept, or with no action selected, is a protocol error
            if (taken && acceptState && offer->dndAction == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                && wl_data_offer_get_version(offer->offer) >= WL_DATA_OFFER_FINISH_SINCE_VERSION)
                wl_data_offer_finish(offer->offer);

            puglWaylandFreeOffer(offer);
            offer = nullptr;
            wl_display_flush(impl->display);
        }

        closeReceive();
        view = nullptr;
        mimeType = nullptr;
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
    // Modifiers are the seat's only while one of the world's views has the keyboard focus
    PuglMods mods() const noexcept
    {
        return impl->keyboardFocus != nullptr ? impl->mods : 0U;
    }

    double scale() const noexcept
    {
        return view != nullptr ? view->impl->scale : 1.0;
    }

    void handle(const Message& message)
    {
        switch (message.kind)
        {
        case Message::kEnter:
            onEnter(message);
            break;
        case Message::kMotion:
            // the world's clock for every drop event: an enter and a drop carry no time of the compositor's
            session.position(message.x * scale(), message.y * scale(), puglWaylandTime(impl->world), mods(),
                             puglWaylandTime(impl->world));
            break;
        case Message::kLeave:
            endDrag();
            break;
        case Message::kDrop:
            onDrop();
            break;
        case Message::kReadable:
            onReadable();
            break;
        case Message::kTick:
            session.tick(puglWaylandTime(impl->world));
            break;
        }
    }

    void onEnter(const Message& message)
    {
        // a drag that never had its leave
        endDrag();

        offer = message.offer;
        serial = message.serial;
        acceptState = false;

        if (offer == nullptr)
            return;

        // as wayland.c does for every drag when nothing accepts drops: declined, kept until the leave or drop
        if (message.view == nullptr || ! message.view->impl->acceptsDrops)
        {
            wl_data_offer_accept(offer->offer, serial, NULL);
            wl_display_flush(impl->display);
            return;
        }

        view = message.view;
        mimeType = uriListType(offer);

        // copy is the only action this target takes, and below version 3 there are no actions to speak of
        const bool actions = wl_data_offer_get_version(offer->offer) >= WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION;
        const bool copies = ! actions || (offer->sourceActions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) != 0;
        const bool offersFiles = mimeType != nullptr && copies;

        // With copy the only action the destination supports, the compositor can only ever select copy or
        // none, whatever modifier the user holds. A preferred action the source lacks would be an error.
        if (offersFiles && actions)
            wl_data_offer_set_actions(offer->offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                      WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

        wl_data_offer_accept(offer->offer, serial, NULL);
        wl_display_flush(impl->display);

        session.begin(offersFiles, true);

        // the enter carries a position, and the drag's first one asks for the files
        session.position(message.x * scale(), message.y * scale(), puglWaylandTime(impl->world), mods(),
                         puglWaylandTime(impl->world));
    }

    void onDrop()
    {
        if (session.isActive())
        {
            session.drop(puglWaylandTime(impl->world), mods(), puglWaylandTime(impl->world));

            // a drop that came before its files waits for them
            if (session.isActive())
                return;
        }

        // a declined drag, or one whose offer finish has not taken yet
        endDrag();
    }

    void onReadable()
    {
        if (fd < 0)
            return;

        for (;;)
        {
            char chunk[16384];
            const ssize_t length = read(fd, chunk, sizeof(chunk));

            if (length > 0)
            {
                if (received.size() + static_cast<size_t>(length) > PUGL_DROP_MAX_PAYLOAD)
                {
                    closeReceive();
                    return session.dataFailed();
                }

                received.insert(received.end(), chunk, chunk + length);
                continue;
            }

            if (length == 0)
            {
                // the source closed its end: the list is complete
                std::vector<char> list;
                list.swap(received);
                closeReceive();
                return session.data(list.empty() ? nullptr : list.data(), list.size(), hostName);
            }

            if (errno == EINTR)
                continue;

            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                closeReceive();
                session.dataFailed();
            }

            return;
        }
    }

    void endDrag()
    {
        if (session.isActive())
            session.leave();

        closeReceive();

        if (offer != nullptr)
        {
            puglWaylandFreeOffer(offer);
            offer = nullptr;
        }

        view = nullptr;
        mimeType = nullptr;
    }

    // The drag goes on without the view: accept nothing more, and stop reading
    void refuse()
    {
        answer(false);
        closeReceive();
        view = nullptr;
    }

    void closeReceive()
    {
        if (fd >= 0)
        {
            close(fd);
            fd = -1;
        }

        received.clear();
    }

    // the offered type that is text/uri-list, in any case and with any parameters
    static const char* uriListType(const PuglWaylandOffer* const candidate)
    {
        static const char kType[] = "text/uri-list";

        for (uint32_t i = 0; i < candidate->numMimeTypes; ++i)
        {
            const char* const type = candidate->mimeTypes[i];
            size_t length = 0;

            for (; kType[length] != '\0'; ++length)
            {
                char c = type[length];
                if (c >= 'A' && c <= 'Z')
                    c = static_cast<char>(c - 'A' + 'a');
                if (c != kType[length])
                    break;
            }

            if (kType[length] == '\0' && (type[length] == '\0' || type[length] == ';'))
                return type;
        }

        return nullptr;
    }

    DAF_DECLARE_NON_COPYABLE(PuglWaylandDropTarget)
};

// --------------------------------------------------------------------------------------------------------------------
// hooks declared in wayland.h

static bool puglWaylandDropEnter(PuglWorldInternals* const impl,
                                 const uint32_t serial,
                                 struct wl_surface* const surface,
                                 const wl_fixed_t x,
                                 const wl_fixed_t y,
                                 PuglWaylandOffer* const offer)
{
    if (impl->drop == nullptr)
        return false;

    PuglWaylandDropTarget::Message message;
    message.kind = PuglWaylandDropTarget::Message::kEnter;
    message.offer = offer;
    message.view = puglWaylandViewForSurface(surface);
    message.serial = serial;
    message.x = wl_fixed_to_double(x);
    message.y = wl_fixed_to_double(y);
    impl->drop->post(message);
    return true;
}

static void puglWaylandDropPost(PuglWorldInternals* const impl,
                                const PuglWaylandDropTarget::Message::Kind kind,
                                const double x = 0.0,
                                const double y = 0.0)
{
    if (impl->drop == nullptr)
        return;

    PuglWaylandDropTarget::Message message;
    message.kind = kind;
    message.offer = nullptr;
    message.view = nullptr;
    message.serial = 0;
    message.x = x;
    message.y = y;
    impl->drop->post(message);
}

static void puglWaylandDropMotion(PuglWorldInternals* const impl, const wl_fixed_t x, const wl_fixed_t y)
{
    puglWaylandDropPost(impl, PuglWaylandDropTarget::Message::kMotion, wl_fixed_to_double(x), wl_fixed_to_double(y));
}

static void puglWaylandDropLeave(PuglWorldInternals* const impl)
{
    puglWaylandDropPost(impl, PuglWaylandDropTarget::Message::kLeave);
}

static void puglWaylandDropDrop(PuglWorldInternals* const impl)
{
    puglWaylandDropPost(impl, PuglWaylandDropTarget::Message::kDrop);
}

static int puglWaylandDropFd(const PuglWorldInternals* const impl)
{
    return impl->drop != nullptr ? impl->drop->fd : -1;
}

static void puglWaylandDropReadable(PuglWorldInternals* const impl)
{
    puglWaylandDropPost(impl, PuglWaylandDropTarget::Message::kReadable);
}

static void puglWaylandDropTick(PuglWorldInternals* const impl)
{
    // a nested event loop ticks over and over; the call it is nested in ticks when it is done
    if (impl->drop != nullptr && ! impl->drop->busy && impl->drop->session.getDeadline() > 0.0)
        puglWaylandDropPost(impl, PuglWaylandDropTarget::Message::kTick);
}

static void puglWaylandDropDetach(PuglView* const view)
{
    if (PuglWaylandDropTarget* const target = view->world->impl->drop)
        target->detachView(view);
}

static void puglWaylandDropFreeWorld(PuglWorldInternals* const impl)
{
    delete impl->drop;
    impl->drop = nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// DGL extension declared in pugl.hpp

PuglStatus puglSetAcceptsFileDrops(PuglView* const view, const bool accept)
{
    PuglWorldInternals* const wimpl = view->world->impl;

    if (! accept)
    {
        if (view->impl->acceptsDrops)
        {
            view->impl->acceptsDrops = false;
            if (wimpl->drop != nullptr)
                wimpl->drop->optOut(view);
        }
        return PUGL_SUCCESS;
    }

    // Wayland has no embedding; a compositor without a data device has no drops either
    if (view->parent != 0 || wimpl->dataDevice == nullptr)
        return PUGL_UNSUPPORTED;

    if (wimpl->drop == nullptr)
    {
        wimpl->drop = new (std::nothrow) PuglWaylandDropTarget(wimpl);
        if (wimpl->drop == nullptr)
            return PUGL_NO_MEMORY;
    }

    view->impl->acceptsDrops = true;
    return PUGL_SUCCESS;
}
