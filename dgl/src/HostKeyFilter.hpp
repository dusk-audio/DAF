/*
 * DAF - Dusk Audio Framework
 * Copyright (C) 2026 Dusk Audio
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with
 * or without fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef DGL_HOST_KEY_FILTER_HPP_INCLUDED
#define DGL_HOST_KEY_FILTER_HPP_INCLUDED

#include "../Base.hpp"

START_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------

/**
   Drops the native copy of a key the host already offered through the plugin format.

   VST2 and VST3 hosts on Windows hand the editor its keys through the format first
   (effEditKeyDown, IPlugView::onKeyDown). Some, REAPER for one, then dispatch the very same
   WM_KEYDOWN to the focused editor window when the editor declined it, and its WM_CHAR after
   it, so the UI would see that key twice.

   Both deliveries happen while the host processes one message, so they share its message time
   (GetMessageTime): the format call records the time, and a native key event of the same kind
   at the same time is the duplicate. Its character input goes with it, but only when the
   format call already delivered one, so keys the format path gives no character (Enter,
   Backspace, Ctrl+key) keep the one the native path brings.

   A duplicate of a key the UI declined still goes on to the host, as any declined native key
   does: dispatching it to the editor window is how such a host lets the key go, so its
   shortcuts only see the key if the window sends it on.

   Any other native key clears the record, so a stale one never matches a later key.
   Pure logic with no platform calls, the caller supplies the times.
 */
struct HostKeyFilter {
    HostKeyFilter() noexcept
        : offered(false),
          offeredPress(false),
          offeredChar(false),
          offeredUsed(false),
          offeredTime(0),
          dropText(false) {}

    /** A key was offered through the plugin format API at message time @a time.
        @a deliveredChar tells whether that call also delivered a character input. */
    void keyOffered(const bool press, const uint32_t time, const bool deliveredChar) noexcept
    {
        offered      = true;
        offeredPress = press;
        offeredChar  = deliveredChar;
        offeredUsed  = false;
        offeredTime  = time;
        dropText     = false;
    }

    /** Whether the UI used the key last offered, known once the format call has dispatched it. */
    void keyUsed(const bool used) noexcept
    {
        offeredUsed = used;
    }

    /** A native key event arrived at message time @a time. Returns true if it is the copy of the
        offered key and must not reach the UI again; @a declined then tells whether the UI declined
        it, so that it goes on to the host. */
    bool dropNativeKey(const bool press, const uint32_t time, bool& declined) noexcept
    {
        const bool duplicate = offered && offeredPress == press && offeredTime == time;

        declined = duplicate && ! offeredUsed;
        offered  = false;
        dropText = duplicate && press && offeredChar;
        return duplicate;
    }

    /** A native character input arrived. Returns true if it belongs to a dropped key. */
    bool dropNativeText() noexcept
    {
        const bool drop = dropText;
        dropText = false;
        return drop;
    }

private:
    bool offered;
    bool offeredPress;
    bool offeredChar;
    bool offeredUsed;
    uint32_t offeredTime;
    bool dropText;
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL

#endif // DGL_HOST_KEY_FILTER_HPP_INCLUDED
