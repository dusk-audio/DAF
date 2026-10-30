// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// Receiving file drops, the part the backends share: text/uri-list decoding, the files of a drop, and
// the drag session that turns a protocol's messages into pugl's drop events. See drop-targets.md.
//
// Included once per translation unit, inside the DGL namespace, after pugl.h and the standard headers
// it uses: <cstring>, <string> and <vector>. pugl.cpp includes it ahead of the pugl passes, so that a
// DGL_BACKEND=both build has one copy for both.

#ifndef DGL_FILE_DROP_HPP_INCLUDED
#define DGL_FILE_DROP_HPP_INCLUDED

/// Largest file list a drop may carry, in bytes
#define PUGL_DROP_MAX_PAYLOAD (16U * 1024U * 1024U)

/// Seconds the source has to deliver the file list once asked
#define PUGL_DROP_DATA_TIMEOUT 2.0

/// Seconds a drop that arrived before its file list waits for it
#define PUGL_DROP_HELD_TIMEOUT 5.0

// --------------------------------------------------------------------------------------------------------------------

/**
   The local files of a drop, as absolute UTF-8 paths.
 */
class PuglDropFiles
{
public:
    PuglDropFiles() {}

    uint32_t count() const noexcept
    {
        return static_cast<uint32_t>(pointers.size());
    }

    /** The paths, count() of them, valid until the list changes. */
    const char* const* paths() const noexcept
    {
        return pointers.empty() ? nullptr : pointers.data();
    }

    void clear()
    {
        pointers.clear();
        storage.clear();
    }

    /**
       Replace the list with the local files a text/uri-list names, and return whether there is one.
       @a hostName is this machine's name, which a URI may give as its host; it may be null.
     */
    bool setFromUriList(const char* const data, const size_t size, const char* const hostName)
    {
        clear();

        if (data == nullptr || size == 0)
            return false;

        // some sources end the list with a NUL
        const char* const nul = static_cast<const char*>(std::memchr(data, '\0', size));
        const char* const end = nul != nullptr ? nul : data + size;
        std::string path;

        for (const char* line = data; line < end;)
        {
            const char* const newline = static_cast<const char*>(std::memchr(line, '\n', static_cast<size_t>(end - line)));
            const char* const next = newline != nullptr ? newline + 1 : end;
            const char* stop = newline != nullptr ? newline : end;

            while (line < stop && (*line == ' ' || *line == '\t'))
                ++line;
            while (stop > line && (stop[-1] == '\r' || stop[-1] == ' ' || stop[-1] == '\t'))
                --stop;

            if (line < stop && *line != '#' && decodeFileUri(line, static_cast<size_t>(stop - line), hostName, path))
                storage.push_back(path);

            line = next;
        }

        // after the last push_back, so that the pointers stay valid
        pointers.reserve(storage.size());
        for (size_t i = 0; i < storage.size(); ++i)
            pointers.push_back(storage[i].c_str());

        return ! pointers.empty();
    }

    /**
       Decode one URI into the local path it names, following RFC 8089.
       False for anything but a file URI on this machine whose path is absolute, cleanly escaped and UTF-8.
     */
    static bool decodeFileUri(const char* const uri, const size_t length, const char* const hostName, std::string& path)
    {
        if (length < 5 || ! equalsIgnoringCase(uri, 5, "file:"))
            return false;

        const char* p = uri + 5;
        const char* const end = uri + length;

        // an authority, which must name this machine; "file:/path" has none
        if (end - p >= 2 && p[0] == '/' && p[1] == '/')
        {
            p += 2;
            const char* const slash = static_cast<const char*>(std::memchr(p, '/', static_cast<size_t>(end - p)));
            if (slash == nullptr)
                return false;

            const size_t hostLength = static_cast<size_t>(slash - p);
            if (hostLength != 0
                && ! equalsIgnoringCase(p, hostLength, "localhost")
                && ! (hostName != nullptr && hostName[0] != '\0' && equalsIgnoringCase(p, hostLength, hostName)))
                return false;

            p = slash;
        }

        if (p == end || *p != '/')
            return false;

        path.clear();

        for (; p < end; ++p)
        {
            const unsigned char c = static_cast<unsigned char>(*p);

            // a raw control character is no part of a URI a file manager writes
            if (c < 0x20 || c == 0x7f)
                return false;

            if (c != '%')
            {
                path += static_cast<char>(c);
                continue;
            }

            // a malformed escape rejects the URI rather than guess what it meant
            if (end - p < 3)
                return false;

            const int high = hexValue(p[1]);
            const int low = hexValue(p[2]);
            if (high < 0 || low < 0 || (high == 0 && low == 0))
                return false;

            path += static_cast<char>(static_cast<unsigned char>(high << 4 | low));
            p += 2;
        }

        return isValidUtf8(path);
    }

    /** Whether @a text is well-formed UTF-8: no overlong forms, no surrogates, nothing past U+10FFFF. */
    static bool isValidUtf8(const std::string& text) noexcept
    {
        const unsigned char* s = reinterpret_cast<const unsigned char*>(text.data());
        const unsigned char* const end = s + text.size();

        while (s < end)
        {
            const unsigned char lead = *s;

            if (lead < 0x80)
            {
                ++s;
                continue;
            }

            size_t extra;
            uint32_t codepoint, minimum;

            if ((lead & 0xe0) == 0xc0)
            {
                extra = 1;
                codepoint = lead & 0x1fU;
                minimum = 0x80;
            }
            else if ((lead & 0xf0) == 0xe0)
            {
                extra = 2;
                codepoint = lead & 0x0fU;
                minimum = 0x800;
            }
            else if ((lead & 0xf8) == 0xf0)
            {
                extra = 3;
                codepoint = lead & 0x07U;
                minimum = 0x10000;
            }
            else
            {
                return false;
            }

            if (static_cast<size_t>(end - s) <= extra)
                return false;

            for (size_t i = 1; i <= extra; ++i)
            {
                if ((s[i] & 0xc0) != 0x80)
                    return false;
                codepoint = codepoint << 6 | (s[i] & 0x3fU);
            }

            if (codepoint < minimum || codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
                return false;

            s += extra + 1;
        }

        return true;
    }

private:
    std::vector<std::string> storage;
    std::vector<const char*> pointers;

    static int hexValue(const char c) noexcept
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    // ASCII only, whatever the locale
    static bool equalsIgnoringCase(const char* const text, const size_t length, const char* const lower) noexcept
    {
        size_t i = 0;
        for (; i < length; ++i)
        {
            if (lower[i] == '\0')
                return false;

            char c = text[i];
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c - 'A' + 'a');

            char d = lower[i];
            if (d >= 'A' && d <= 'Z')
                d = static_cast<char>(d - 'A' + 'a');

            if (c != d)
                return false;
        }

        return lower[i] == '\0';
    }
};

// --------------------------------------------------------------------------------------------------------------------

/**
   What a backend does for a PuglDropSession: the protocol's side of a drag.
 */
struct PuglDropHost
{
    virtual ~PuglDropHost() {}

    /** Ask the source for its text/uri-list. The answer comes back as PuglDropSession::data or dataFailed. */
    virtual void requestData() = 0;

    /** Tell the source whether a drop at the latest position would be taken. */
    virtual void answer(bool accept) = 0;

    /** Tell the source how its drop ended, after which the drag is over. */
    virtual void finish(bool taken) = 0;

    /** Hand an event to the view, or refuse when the view has gone. */
    virtual PuglStatus dispatch(const PuglDropEvent& event) = 0;
};

// --------------------------------------------------------------------------------------------------------------------

/**
   One drag over a view, from the protocol's first message to its last.

   The backend reports what the protocol says, and the session decides what the view hears and what the
   source is told. The view hears of a drag once there is both a position and a list of usable files, and
   the drag ends for it with exactly one leave or drop.

   The backend must not call into the session while it is inside one of its calls (it queues what
   arrives meanwhile), with one exception: detach and abandon, which a view going away or no longer
   accepting drops can cause from inside any dispatch. Once a drop is being taken or refused, the drag
   stays with the session until its end has told the source, whatever the view does meanwhile.
 */
class PuglDropSession
{
public:
    explicit PuglDropSession(PuglDropHost& h) noexcept
        : host(h),
          state(kIdle),
          dispatching(false),
          finishing(false)
    {
        reset();
    }

    /** Whether a drag is in progress, at the protocol level. */
    bool isActive() const noexcept
    {
        return state != kIdle;
    }

    /** Whether a transfer of the file list is still of use. */
    bool wantsData() const noexcept
    {
        return state == kAttached && requested && ! ready && ! failed;
    }

    /** Whether the session is inside a call to the view. */
    bool isDispatching() const noexcept
    {
        return dispatching;
    }

    /** When a deadline falls due, or 0 for none. */
    double getDeadline() const noexcept
    {
        return deadline;
    }

    /**
       A drag entered the view. @a offersFiles tells whether the source offers a text/uri-list, and an
       action this target takes. A detached session refuses the drag without telling the view.
     */
    void begin(const bool offersFiles, const bool attached)
    {
        if (state != kIdle)
            leave();

        reset();
        state = attached ? kAttached : kDetached;
        offers = offersFiles;
    }

    /** The pointer moved to x, y in view coordinates. The source gets its answer now, or with the data. */
    void position(const double x, const double y, const double time, const PuglMods mods, const double now)
    {
        if (state == kIdle)
            return;

        posX = x;
        posY = y;
        posTime = time;
        posMods = mods;
        positioned = true;
        owed = true;

        if (state != kAttached || ! offers || failed)
            return settle(false);

        if (! requested)
        {
            requested = true;
            deadline = now + PUGL_DROP_DATA_TIMEOUT;
            host.requestData();
            return;
        }

        if (ready)
            consult();
    }

    /** The file list arrived. */
    void data(const char* const bytes, const size_t size, const char* const hostName)
    {
        if (! wantsData())
            return;

        if (! files.setFromUriList(bytes, size, hostName))
            return dataFailed();

        ready = true;
        deadline = 0.0;

        if (held)
            return performDrop();

        if (owed)
            consult();
    }

    /** The file list could not be had, or would be too large. */
    void dataFailed()
    {
        if (! wantsData())
            return;

        failed = true;
        deadline = 0.0;

        if (held)
            return end(false);

        settle(false);
    }

    /** The drag left the view, or ended without a drop. */
    void leave()
    {
        if (state == kIdle)
            return;

        const bool notify = state == kAttached && entered && ! dispatching;
        const bool wasHeld = held;

        if (notify)
            call(PUGL_DRAG_LEAVE);

        reset();

        if (wasHeld)
            host.finish(false);
    }

    /** The files were dropped at the latest position. */
    void drop(const double time, const PuglMods mods, const double now)
    {
        if (state == kIdle)
            return;

        // a drop supersedes the answer the latest position may still be waiting for
        owed = false;
        posTime = time;
        posMods = mods;

        if (state != kAttached || ! offers || failed || ! positioned)
            return refuseDrop();

        if (ready)
            return performDrop();

        held = true;
        deadline = now + PUGL_DROP_HELD_TIMEOUT;
    }

    /** Let deadlines fall due. */
    void tick(const double now)
    {
        if (state == kIdle || deadline <= 0.0 || now < deadline)
            return;

        deadline = 0.0;

        if (held)
            return end(false);

        dataFailed();
    }

    /**
       The view stopped accepting drops: the rest of the drag is refused. A view that had the enter gets a
       leave if @a notify, unless this happens inside a call to it.
     */
    void detach(const bool notify)
    {
        if (state != kAttached)
            return;

        const bool tell = notify && entered && ! dispatching;
        state = kDetached;
        deadline = 0.0;

        if (held && ! finishing)
            return end(false);

        settle(false);

        if (tell)
            call(PUGL_DRAG_LEAVE);
    }

    /** The view is going away: refuse what is owed and forget the drag. A drop being finished still finishes. */
    void abandon()
    {
        detach(false);

        if (! finishing && state != kIdle)
            reset();
    }

private:
    enum State {
        kIdle,     // no drag
        kAttached, // a drag the view takes part in
        kDetached  // a drag the view has left: refused to the end
    };

    PuglDropHost& host;
    PuglDropFiles files;
    State state;
    bool offers;     // the source has something this target can take
    bool requested;  // the file list was asked for
    bool ready;      // the file list is in, with a usable file
    bool failed;     // the file list will not come, or has nothing usable
    bool positioned; // a position has been seen
    bool owed;       // the latest position waits for its answer
    bool entered;    // the view had the enter
    bool accepted;   // what the view last answered
    bool held;       // a drop arrived before its file list
    bool dispatching;
    bool finishing;  // a drop is being taken or refused, and the source is yet to be told
    double deadline;
    double posX, posY, posTime;
    PuglMods posMods;

    void reset()
    {
        state = kIdle;
        offers = requested = ready = failed = positioned = owed = entered = accepted = held = false;
        deadline = 0.0;
        posX = posY = posTime = 0.0;
        posMods = 0;
        files.clear();
    }

    void settle(const bool accept)
    {
        if (! owed)
            return;

        owed = false;
        host.answer(accept);
    }

    PuglStatus call(const PuglEventType type)
    {
        PuglDropEvent event = {};
        event.type = type;
        event.time = posTime;
        event.x = posX;
        event.y = posY;
        event.mods = posMods;

        if (type != PUGL_DRAG_LEAVE)
        {
            event.numFiles = files.count();
            event.files = files.paths();
        }

        dispatching = true;
        const PuglStatus status = host.dispatch(event);
        dispatching = false;
        return status;
    }

    // ask the view about the latest position, and tell the source
    void consult()
    {
        const PuglStatus status = call(entered ? PUGL_DRAG_MOTION : PUGL_DRAG_ENTER);

        // detached during the call, which answered the source for it
        if (state != kAttached)
            return;

        entered = true;
        accepted = status == PUGL_SUCCESS;
        settle(accepted);
    }

    // the files are in and a drop is due
    void performDrop()
    {
        finishing = true;
        held = false;
        deadline = 0.0;

        // a drop that came before its data: the view hears of the drag only now
        if (state == kAttached && ! entered)
        {
            const PuglStatus status = call(PUGL_DRAG_ENTER);

            if (state == kAttached)
            {
                entered = true;
                accepted = status == PUGL_SUCCESS;
            }
        }

        bool taken = false;

        // the view's answer stands even if it went away while answering
        if (state == kAttached && accepted)
            taken = call(PUGL_DROP) == PUGL_SUCCESS;
        else
            refuseInView();

        finishing = false;
        end(taken);
    }

    void end(const bool taken)
    {
        reset();
        host.finish(taken);
    }

    // a drop where the view did not accept one, from a source that dropped anyway
    void refuseDrop()
    {
        finishing = true;
        refuseInView();
        finishing = false;
        end(false);
    }

    // the view that had the enter hears the drag leave
    void refuseInView()
    {
        if (state == kAttached && entered && ! dispatching)
            call(PUGL_DRAG_LEAVE);
    }

    DAF_DECLARE_NON_COPYABLE(PuglDropSession)
};

#endif // DGL_FILE_DROP_HPP_INCLUDED
