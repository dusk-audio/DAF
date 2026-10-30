// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
// Included after the Win32 backend, inside the DGL namespace: the OLE drop target of a view that accepts
// file drops. See drop-targets.md, whose section on Windows gives the reason for each rule below.

// IDropTarget calls on this thread's stack, and the OleUninitialize calls put off until there are none.
// Releasing the last reference to the apartment inside a call that came in from another process would tear
// the apartment down under that call, which happens when a window goes inside its onFileDrop.
static PUGL_WIN_THREAD_LOCAL unsigned puglWinDropCalls = 0U;
static PUGL_WIN_THREAD_LOCAL unsigned puglWinDropOleOwed = 0U;

// Drop targets in existence, which the tests watch to see one outlive its view
static unsigned puglWinDropTargetsAlive = 0U;

static void puglWinReleaseDropOle()
{
    if (puglWinDropCalls != 0U)
        ++puglWinDropOleOwed;
    else
        OleUninitialize();
}

void puglWinSettleDropOle()
{
    if (puglWinDropCalls != 0U)
        return;

    for (; puglWinDropOleOwed != 0U; --puglWinDropOleOwed)
        OleUninitialize();
}

struct PuglWinDropTarget : IDropTarget, PuglDropHost
{
    // A call that arrived while the target was inside the session, handled once that call returns
    struct Message {
        enum Kind { kEnter, kOver, kLeave, kDrop } kind;
        double x, y, time;               // view coordinates, and seconds as pugl's other Windows events
        PuglMods mods;
        DWORD allowed;                   // the effects the source allows
        bool offers;                     // kEnter: the source has files and allows a copy
        std::vector<std::string> paths;  // kEnter: the files, read as the drag entered
    };

    // Keeps the target alive across a call, since the view may delete itself inside its callback
    struct Call {
        PuglWinDropTarget* const target;

        explicit Call(PuglWinDropTarget* const t) : target(t)
        {
            target->AddRef();
            ++puglWinDropCalls;
        }

        ~Call()
        {
            --puglWinDropCalls;
            target->Release();
        }

        DAF_DECLARE_NON_COPYABLE(Call)
    };

    std::atomic<ULONG> references;
    PuglView* view;  // null once the view has gone; every call then answers none
    const HWND hwnd;
    PuglDropSession session;
    std::deque<Message> queue;
    bool accepting;  // registered with OLE, which holds one OleInitialize of the thread
    bool accepted;   // the session's latest answer
    bool taken;      // how the latest drop ended
    bool busy;

    explicit PuglWinDropTarget(PuglView* const v)
        : references(1),
          view(v),
          hwnd(v->impl->hwnd),
          session(*this),
          queue(),
          accepting(false),
          accepted(false),
          taken(false),
          busy(false)
    {
        ++puglWinDropTargetsAlive;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // the view's side

    PuglStatus optIn()
    {
        if (accepting)
            return PUGL_SUCCESS;

        // RegisterDragDrop needs OLE, in a single-threaded apartment
        const HRESULT ole = OleInitialize(nullptr);
        if (ole == RPC_E_CHANGED_MODE)
        {
            d_stderr("DGL: this thread is in COM's multi-threaded apartment, where OLE cannot deliver file drops");
            return PUGL_FAILURE;
        }
        if (FAILED(ole))
            return PUGL_FAILURE;

        if (RegisterDragDrop(hwnd, this) != S_OK)
        {
            puglWinReleaseDropOle();
            return PUGL_FAILURE;
        }

        accepting = true;
        return PUGL_SUCCESS;
    }

    void optOut()
    {
        if (! accepting)
            return;

        // once revoked, OLE holds the target no more, and the view's reference may go with the view in its leave
        AddRef();

        // revoked first, so that the view's leave may opt in again
        accepting = false;
        RevokeDragDrop(hwnd);

        // the view's leave may run a nested event loop: what arrives meanwhile waits, as for any call to it
        exclusively([this] { session.detach(true); });

        puglWinReleaseDropOle();
        Release();
    }

    // The view is going away. A drop being dispatched still gets its answer to the source.
    void orphan()
    {
        if (accepting)
        {
            accepting = false;
            RevokeDragDrop(hwnd);
            puglWinReleaseDropOle();
        }

        session.abandon();
        view = nullptr;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // IUnknown

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** const out) override
    {
        if (out == nullptr)
            return E_POINTER;

        *out = nullptr;

        if (! IsEqualIID(iid, IID_IUnknown) && ! IsEqualIID(iid, IID_IDropTarget))
            return E_NOINTERFACE;

        *out = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++references;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = --references;
        if (count == 0)
            delete this;
        return count;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // IDropTarget

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* const data, const DWORD keys, const POINTL point,
                                        DWORD* const effect) override
    {
        if (effect == nullptr)
            return E_INVALIDARG;

        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;

        if (view == nullptr)
            return S_OK;

        const Call call(this);
        Message message = locate(Message::kEnter, keys, point, allowed);

        // a source that allows no copy is refused: DGL never reports a move
        message.offers = accepting && (allowed & DROPEFFECT_COPY) != 0 && readFiles(data, message.paths);

        *effect = deliver(message);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(const DWORD keys, const POINTL point, DWORD* const effect) override
    {
        if (effect == nullptr)
            return E_INVALIDARG;

        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;

        if (view == nullptr)
            return S_OK;

        const Call call(this);
        Message message = locate(Message::kOver, keys, point, allowed);
        *effect = deliver(message);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
        if (view == nullptr)
            return S_OK;

        const Call call(this);
        Message message = Message();
        message.kind = Message::kLeave;
        deliver(message);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject*, const DWORD keys, const POINTL point, DWORD* const effect) override
    {
        if (effect == nullptr)
            return E_INVALIDARG;

        const DWORD allowed = *effect;
        *effect = DROPEFFECT_NONE;

        if (view == nullptr)
            return S_OK;

        // the list was read as the drag entered, and is not read again
        const Call call(this);
        Message message = locate(Message::kDrop, keys, point, allowed);
        *effect = deliver(message);
        return S_OK;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // PuglDropHost

    void requestData() override
    {
        // the files were read at the enter, and go to the session once position() returns, see handle
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
    // only through Release
    ~PuglWinDropTarget() override
    {
        --puglWinDropTargetsAlive;
    }

    // Handle a call now, or once the call into the session on the stack returns: the view may run a nested
    // event loop from inside its callback, and the session must not be entered twice. A call that has to
    // wait is answered none, since its answer is due before the view can be asked. A drop that waits has been
    // refused to its source by then, so the session gets it as a leave: the view must not take it.
    DWORD deliver(Message& message)
    {
        if (busy)
        {
            if (message.kind == Message::kDrop)
                message.kind = Message::kLeave;
            queue.push_back(std::move(message));
            return DROPEFFECT_NONE;
        }

        DWORD effect = DROPEFFECT_NONE;
        exclusively([this, &message, &effect] { effect = handle(message); });
        return effect;
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
            Message next(std::move(queue.front()));
            queue.pop_front();
            handle(next);
        }

        busy = false;
    }

    DWORD handle(Message& message)
    {
        if (view == nullptr)
            return DROPEFFECT_NONE;

        const double now = puglGetTime(view->world);
        const bool copy = (message.allowed & DROPEFFECT_COPY) != 0;

        switch (message.kind)
        {
        case Message::kEnter:
            accepted = false;
            session.begin(message.offers, accepting);
            session.position(message.x, message.y, message.time, message.mods, now);

            // the data is in hand: the session asked for it inside position(), and gets it now
            if (session.wantsData())
                session.dataPaths(message.paths);
            break;

        case Message::kOver:
            accepted = false;
            session.position(message.x, message.y, message.time, message.mods, now);
            break;

        case Message::kLeave:
            session.leave();
            return DROPEFFECT_NONE;

        case Message::kDrop:
            taken = false;
            session.drop(message.time, message.mods, now);
            return taken && copy ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        }

        // a copy, whatever the source asked for: Shift asks Explorer for a move
        return accepted && copy ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    }

    Message locate(const Message::Kind kind, const DWORD keys, const POINTL point, const DWORD allowed) const
    {
        Message message = Message();
        message.kind = kind;
        message.allowed = allowed;
        message.time = GetMessageTime() / 1e3;

        // client pixels, what WM_MOUSEMOVE reports
        POINT client = { point.x, point.y };
        ScreenToClient(hwnd, &client);
        message.x = client.x;
        message.y = client.y;

        // the drag's own key state, which a drag from another process keeps up to date; the rest as pugl has it
        const uint32_t shift = PUGL_MOD_SHIFT, ctrl = PUGL_MOD_CTRL, alt = PUGL_MOD_ALT;
        message.mods = (getModifiers() & ~(shift | ctrl | alt))
                     | ((keys & MK_SHIFT)   ? shift : 0U)
                     | ((keys & MK_CONTROL) ? ctrl  : 0U)
                     | ((keys & MK_ALT)     ? alt   : 0U);
        return message;
    }

    // The absolute paths of the files a drag carries in CF_HDROP, in UTF-8. False when there is none. Virtual
    // files (Outlook, a zip folder) come as a file group descriptor without CF_HDROP, and are refused so;
    // one offered next to CF_HDROP does not hide the files on disk.
    static bool readFiles(IDataObject* const data, std::vector<std::string>& paths)
    {
        if (data == nullptr)
            return false;

        // asked first, so that a source that does not offer it is never asked to render it
        FORMATETC format = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        if (data->QueryGetData(&format) != S_OK)
            return false;

        STGMEDIUM medium;
        std::memset(&medium, 0, sizeof(medium));
        if (data->GetData(&format, &medium) != S_OK)
            return false;

        if (medium.tymed == TYMED_HGLOBAL && medium.hGlobal != nullptr)
        {
            const HDROP drop = static_cast<HDROP>(medium.hGlobal);
            const UINT count = DragQueryFileW(drop, 0xffffffffU, nullptr, 0);
            std::vector<wchar_t> wide;

            for (UINT i = 0; i < count; ++i)
            {
                // the length first: paths can pass MAX_PATH
                const UINT length = DragQueryFileW(drop, i, nullptr, 0);
                if (length == 0)
                    continue;

                wide.assign(static_cast<size_t>(length) + 1U, L'\0');
                if (DragQueryFileW(drop, i, wide.data(), length + 1U) != length || ! isAbsolute(wide.data(), length))
                    continue;

                // a path with an unpaired surrogate has no UTF-8 form, and is skipped
                const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(length),
                                                     nullptr, 0, nullptr, nullptr);
                if (size <= 0)
                    continue;

                std::string path(static_cast<size_t>(size), '\0');
                if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(), static_cast<int>(length),
                                        &path[0], size, nullptr, nullptr) == size)
                    paths.push_back(path);
            }
        }

        ReleaseStgMedium(&medium);
        return ! paths.empty();
    }

    // a drive path (C:\) or a UNC one (\\server\share, \\?\C:\)
    static bool isAbsolute(const wchar_t* const path, const UINT length) noexcept
    {
        if (length >= 3 && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/'))
            return (path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z');

        return length >= 3 && (path[0] == L'\\' || path[0] == L'/') && (path[1] == L'\\' || path[1] == L'/');
    }

    DAF_DECLARE_NON_COPYABLE(PuglWinDropTarget)
};

// --------------------------------------------------------------------------------------------------------------------
// hooks, declared in win.h

void puglWinForgetDropTarget(PuglView* const view)
{
    PuglWinDropTarget* const target = view->impl->drop;
    if (target == nullptr)
        return;

    view->impl->drop = nullptr;
    target->orphan();
    target->Release();
}

// --------------------------------------------------------------------------------------------------------------------

PuglStatus puglSetAcceptsFileDrops(PuglView* const view, const bool accept)
{
    PuglInternals* const impl = view->impl;

    if (! accept)
    {
        if (impl->drop != nullptr)
            impl->drop->optOut();
        return PUGL_SUCCESS;
    }

    // DGL addresses drops to windows of their own, as on the other platforms
    if (view->parent != 0)
        return PUGL_UNSUPPORTED;

    if (impl->hwnd == nullptr)
        return PUGL_FAILURE;

    if (impl->drop == nullptr)
    {
        impl->drop = new (std::nothrow) PuglWinDropTarget(view);
        if (impl->drop == nullptr)
            return PUGL_NO_MEMORY;
    }

    return impl->drop->optIn();
}
