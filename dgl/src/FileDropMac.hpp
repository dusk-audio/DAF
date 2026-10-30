// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after Cocoa's backend, at global Objective-C++ scope: the NSDraggingDestination of a view that
// accepts file drops. See drop-targets.md.
//
// The target belongs to the wrapper view AppKit sends the drag to, and lives as long as the wrapper does.
// AppKit can still message a wrapper whose PuglView has gone, and every call in keeps the wrapper alive
// until it returns, so a view deleted inside a callback leaves the session, and the files it was handed,
// in place until that callback returns.

struct PuglMacDropTarget : PuglDropHost
{
    // A dragging message, handled once the current call into the session returns, see post
    struct Message {
        enum Kind { kEnter, kMotion, kLeave } kind;
        std::vector<std::string> paths; // kEnter: the files
        double x, y;                    // kEnter, kMotion: view coordinates
        PuglMods mods;
    };

    PuglWrapperView* const wrapper; // owns the target
    PuglView* view;                 // null once the wrapper is retired
    PuglDropSession session;
    bool accepting;                 // registered for file URLs
    bool wanted;                    // the session asked for the list
    bool accepted;                  // what the session last answered
    bool taken;                     // how the latest drop ended
    std::deque<Message> queue;
    bool busy;

    PuglMacDropTarget(PuglWrapperView* const w, PuglView* const v)
        : wrapper(w),
          view(v),
          session(*this),
          accepting(false),
          wanted(false),
          accepted(false),
          taken(false),
          queue(),
          busy(false) {}

    void setAccepting(const bool accept)
    {
        if (accept == accepting || view == nullptr)
            return;

        accepting = accept;

        if (accept)
        {
            [wrapper registerForDraggedTypes:@[NSPasteboardTypeFileURL]];
            return;
        }

        // The view's leave may run a nested event loop: what arrives meanwhile waits, as for any call to it.
        // The session stays active, refusing, until the drag ends.
        [wrapper unregisterDraggedTypes];
        accepted = false;
        exclusively([this] { session.detach(true); });
    }

    // The wrapper is retired: nothing more reaches the view, and the rest of the drag is refused
    void detachView()
    {
        if (view == nullptr)
            return;

        if (accepting)
            [wrapper unregisterDraggedTypes];

        accepting = false;
        session.abandon();
        view = nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // NSDraggingDestination, with the wrapper alive and attached

    NSDragOperation entered(id<NSDraggingInfo> const info)
    {
        Message message;
        message.kind = Message::kEnter;
        message.paths = readFiles([info draggingPasteboard]);
        locate(info, message);

        // a drag that arrives inside a call of ours is refused until it has been handled
        if (! post(message))
            return NSDragOperationNone;

        return operation(info);
    }

    NSDragOperation updated(id<NSDraggingInfo> const info)
    {
        Message message;
        message.kind = Message::kMotion;
        locate(info, message);
        post(message);
        return operation(info);
    }

    void exited()
    {
        Message message;
        message.kind = Message::kLeave;
        message.x = message.y = 0.0;
        message.mods = 0;
        post(message);
    }

    BOOL prepare(id<NSDraggingInfo> const info)
    {
        return ! busy && operation(info) == NSDragOperationCopy;
    }

    BOOL perform(id<NSDraggingInfo> const info)
    {
        // a drop inside a nested event loop of one of our own calls is refused
        if (busy)
        {
            exited();
            return NO;
        }

        // the source's mask can change after the last update, with the modifiers held
        if (([info draggingSourceOperationMask] & NSDragOperationCopy) == 0)
        {
            exclusively([this] { session.leave(); });
            return NO;
        }

        taken = false;
        exclusively([this] { session.drop(now(), mods(), now()); });
        return taken ? YES : NO;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // PuglDropHost

    void requestData() override
    {
        // read at the enter already, the list is handed over once the session's call returns
        wanted = true;
    }

    void answer(const bool accept) override
    {
        accepted = accept;
    }

    void finish(const bool wasTaken) override
    {
        taken = wasTaken;
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
    // Handle a message now, or once the call into the session on the stack returns, and say which
    bool post(const Message& message)
    {
        if (busy)
        {
            // positions are coalesced, as the answer AppKit gets meanwhile is the last one anyway
            if (message.kind == Message::kMotion && ! queue.empty() && queue.back().kind == Message::kMotion)
                queue.back() = message;
            else
                queue.push_back(message);
            return false;
        }

        exclusively([this, &message] { handle(message); });
        return true;
    }

    // Do some work that may call the view, then handle what arrived meanwhile, in order. Inside a call already
    // under way the work runs as it is, and the queue is left to that call.
    template <class Work>
    void exclusively(const Work& work)
    {
        if (busy)
            return work();

        busy = true;
        work();

        while (! queue.empty())
        {
            const Message next = queue.front();
            queue.pop_front();
            handle(next);
        }

        busy = false;
    }

    void handle(const Message& message)
    {
        switch (message.kind)
        {
        case Message::kEnter:
        {
            std::vector<std::string> paths(message.paths);
            wanted = false;
            accepted = false;
            session.begin(! paths.empty(), view != nullptr && accepting);
            session.position(message.x, message.y, now(), message.mods, now());

            // the list was read with the enter, so the session gets it at its first position
            if (wanted)
            {
                wanted = false;
                session.dataPaths(paths);
            }
            break;
        }

        case Message::kMotion:
            session.position(message.x, message.y, now(), message.mods, now());
            break;

        case Message::kLeave:
            session.leave();
            break;
        }
    }

    // Copy only, and only while the source allows it: a move-only mask is refused, never made a move
    NSDragOperation operation(id<NSDraggingInfo> const info) const
    {
        return view != nullptr && accepting && session.isActive() && accepted
            && ([info draggingSourceOperationMask] & NSDragOperationCopy) != 0
             ? NSDragOperationCopy
             : NSDragOperationNone;
    }

    void locate(id<NSDraggingInfo> const info, Message& message) const
    {
        // what eventLocation: does to a mouse event
        const NSPoint point = nsPointFromPoints(view, [wrapper convertPoint:[info draggingLocation] fromView:nil]);
        message.x = point.x;
        message.y = point.y;
        message.mods = mods();
    }

    // The keys held now, as the drag carries none of its own
    static PuglMods mods()
    {
        return getModifierFlags([NSEvent modifierFlags]);
    }

    // The clock of NSEvent timestamps, which mouse events carry
    static double now()
    {
        return [[NSProcessInfo processInfo] systemUptime];
    }

    // The files on the pasteboard, as the absolute paths the file system keeps, UTF-8 and in its normal form.
    // File promises are no files yet, and other URLs no files at all, so neither is read.
    static std::vector<std::string> readFiles(NSPasteboard* const pasteboard)
    {
        std::vector<std::string> paths;

        NSArray* const urls = [pasteboard readObjectsForClasses:@[[NSURL class]]
                                                        options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];

        // A file reference URL, which Finder writes, gives the path it resolves to
        for (NSURL* url in urls)
        {
            const char* const path = [url fileSystemRepresentation];

            if (path != nullptr)
                paths.push_back(path);
        }

        return paths;
    }

    DAF_DECLARE_NON_COPYABLE(PuglMacDropTarget)
};

// --------------------------------------------------------------------------------------------------------------------
// hooks declared in mac.h, each called by an attached wrapper

// Keeps the wrapper, and so its target, alive while a call into the target is on the stack
struct PuglMacDropKeep
{
    PuglWrapperView* const wrapper;

    explicit PuglMacDropKeep(PuglWrapperView* const w)
        : wrapper([w retain]) {}

    ~PuglMacDropKeep()
    {
        [wrapper release];
    }

    DAF_DECLARE_NON_COPYABLE(PuglMacDropKeep)
};

static NSDragOperation puglMacDropEntered(PuglWrapperView* const wrapper, id<NSDraggingInfo> const info)
{
    if (wrapper->dropTarget == nullptr)
        return NSDragOperationNone;

    const PuglMacDropKeep keep(wrapper);
    return wrapper->dropTarget->entered(info);
}

static NSDragOperation puglMacDropUpdated(PuglWrapperView* const wrapper, id<NSDraggingInfo> const info)
{
    if (wrapper->dropTarget == nullptr)
        return NSDragOperationNone;

    const PuglMacDropKeep keep(wrapper);
    return wrapper->dropTarget->updated(info);
}

static void puglMacDropExited(PuglWrapperView* const wrapper)
{
    if (wrapper->dropTarget == nullptr)
        return;

    const PuglMacDropKeep keep(wrapper);
    wrapper->dropTarget->exited();
}

static BOOL puglMacDropPrepare(PuglWrapperView* const wrapper, id<NSDraggingInfo> const info)
{
    return wrapper->dropTarget != nullptr && wrapper->dropTarget->prepare(info);
}

static BOOL puglMacDropPerform(PuglWrapperView* const wrapper, id<NSDraggingInfo> const info)
{
    if (wrapper->dropTarget == nullptr)
        return NO;

    const PuglMacDropKeep keep(wrapper);
    return wrapper->dropTarget->perform(info);
}

static void puglMacDropDetach(PuglWrapperView* const wrapper)
{
    if (wrapper->dropTarget != nullptr)
        wrapper->dropTarget->detachView();
}

static void puglMacDropFree(PuglWrapperView* const wrapper)
{
    delete wrapper->dropTarget;
    wrapper->dropTarget = nullptr;
}

// --------------------------------------------------------------------------------------------------------------------
// DGL extension declared in pugl.hpp

PuglStatus puglSetAcceptsFileDrops(PuglView* const view, const bool accept)
{
    PuglWrapperView* const wrapper = view->impl->wrapperView;

    if (! accept)
    {
        if (wrapper != nil && wrapper->dropTarget != nullptr)
        {
            const PuglMacDropKeep keep(wrapper);
            wrapper->dropTarget->setAccepting(false);
        }
        return PUGL_SUCCESS;
    }

    // AppKit could deliver to a view inside a host's window, but drops stay a top-level feature everywhere
    if (view->parent != 0)
        return PUGL_UNSUPPORTED;

    if (wrapper == nil)
        return PUGL_FAILURE;

    if (wrapper->dropTarget == nullptr)
    {
        wrapper->dropTarget = new (std::nothrow) PuglMacDropTarget(wrapper, view);
        if (wrapper->dropTarget == nullptr)
            return PUGL_NO_MEMORY;
    }

    wrapper->dropTarget->setAccepting(true);
    return PUGL_SUCCESS;
}
