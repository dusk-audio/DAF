/*
 * Dusk Audio Framework (DAF)
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

#ifndef DAF_PLUGIN_AU_UI_LINK_HPP_INCLUDED
#define DAF_PLUGIN_AU_UI_LINK_HPP_INCLUDED

#include "../DafUtils.hpp"

#include <atomic>
#include <mutex>
#include <vector>

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

/**
   The tie between an AU instance and the UIs made for it, shared by the two halves of the AU wrapper.

   A UI lives as long as its Cocoa view, and the view lives as long as anybody holds it: the host, for as long as it
   likes, and AppKit, which can keep a view alive after the host has let go of it (one that was its window's first
   responder, for one).
   The plugin lives until the host disposes the instance. A UI that outlives its instance has an idle timer still
   running, a direct access pointer to the deleted plugin and the handle of the disposed unit, and a UI that reads its
   plugin on every idle tick, as metering UIs do, reads freed memory from the next tick on.

   So the instance detaches every UI through here, from its destructor and before its plugin is deleted: the UI stops
   its idle timer, its instance pointer turns nullptr and it calls the unit no more. A UI that goes first takes itself
   off. Both happen under the lock, which a UI also holds for each idle tick and for its setup on the unit: from its
   attach to its idle timer and property listeners, and then for its request for the plugin's current values. The lock
   is recursive, so that a tick can see its own instance disposed.

   That guarantees this much. A unit disposed on the main thread is disposed between the UI's callbacks, none of which
   reads the plugin afterwards. A unit disposed on another thread waits for an idle tick in progress to end, and for a
   new UI's setup on the unit, whose timer it then stops; the UI reads nullptr from the next callback on. An instance
   destroyed before a new UI attaches, during its plugin UI's construction for one, leaves that UI detached from the
   start: the view fetches its link before it reads anything else of the instance, and attaches only once the plugin
   UI is built. One whose destruction AudioToolbox holds back past the attach detaches the UI as usual. Calls a tick,
   the setup or the values request makes into the unit do not wait for the dispose either: AudioToolbox waits for a
   call in flight before it closes the unit, and answers new ones with an error at once.

   It does not guarantee more. A unit disposed on another thread does not wait for the plugin UI's construction, which
   comes before the attach, nor for a paint, an input event, a host resize or the UI's destruction in progress, so any
   of those can still read the plugin as it goes; waiting for them would deadlock a host whose own AppKit callbacks,
   which that UI code can reach (a resize reaches the host's frame observers), take a lock the host holds across the
   dispose. The idle tick waited for can reach host code the same way when the plugin's uiIdle(), or an idle callback
   the tick runs (one added with no timer), resizes the UI or otherwise calls into AppKit, and with such a host that
   wait is a deadlock too. So can every parameterChanged(), programLoaded() and stateChanged(): they run inside a call
   into the unit (the host's own value and preset changes, or the values request a new view makes, which reaches every
   view of the unit), and a dispose waits for that call. Plugin UI code run from threads, timers or blocks of its own
   is not covered at all, and the property listeners a UI registers are called on the thread of whatever changed the
   property.

   The instance creates the link and hands each UI a reference of its own through the private 'DPFu' property. The link
   is freed when the last of them lets go.
 */
class AUUILink
{
public:
    typedef void (*DetachFunc)(void* ptr);

    AUUILink() noexcept
        : fRefs(1),
          fLock(),
          fClients(),
          fUnitAlive(true) {}

    void retain() noexcept
    {
        fRefs.fetch_add(1, std::memory_order_relaxed);
    }

    void release() noexcept
    {
        if (fRefs.fetch_sub(1, std::memory_order_acq_rel) == 1)
            delete this;
    }

    // held by a UI for each idle tick, see above
    std::recursive_mutex& lock() noexcept
    {
        return fLock;
    }

    // UI side: `detach(ptr)` is called once, under the lock and on the thread disposing the instance, unless the UI
    // takes itself off first. False when the instance is already gone, the UI is then never called.
    bool attach(const DetachFunc detach, void* const ptr)
    {
        const std::lock_guard<std::recursive_mutex> guard(fLock);
        if (! fUnitAlive)
            return false;
        const Client client = { detach, ptr };
        fClients.push_back(client);
        return true;
    }

    // UI side, going away: from here on its detach function is not called, and a call in progress has ended
    void remove(void* const ptr)
    {
        const std::lock_guard<std::recursive_mutex> guard(fLock);
        for (std::vector<Client>::iterator it = fClients.begin(); it != fClients.end(); ++it)
        {
            if (it->ptr == ptr)
            {
                fClients.erase(it);
                break;
            }
        }
    }

    // instance side, from its destructor: detaches every UI attached, and none attaches afterwards
    void unitDisposed()
    {
        const std::lock_guard<std::recursive_mutex> guard(fLock);
        fUnitAlive = false;

        // taken out first: a detach function may call back in
        std::vector<Client> clients;
        clients.swap(fClients);
        for (size_t i = 0; i < clients.size(); ++i)
            clients[i].detach(clients[i].ptr);
    }

private:
    struct Client {
        DetachFunc detach;
        void* ptr;
    };

    std::atomic<int> fRefs;
    std::recursive_mutex fLock;
    std::vector<Client> fClients;
    bool fUnitAlive;

    ~AUUILink() noexcept {}

    DAF_DECLARE_NON_COPYABLE(AUUILink)
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_PLUGIN_AU_UI_LINK_HPP_INCLUDED
