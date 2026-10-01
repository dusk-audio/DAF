/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2026 Filipe Coelho <falktx@falktx.com>
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

#ifndef DAF_PLUGIN_STATE_UPDATES_HPP_INCLUDED
#define DAF_PLUGIN_STATE_UPDATES_HPP_INCLUDED

#include "DafPluginInternal.hpp"

#include <map>
#include <mutex>

#if DAF_PLUGIN_WANT_STATE

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

/**
   State values set by the plugin itself through Plugin::updateStateValue().

   updateStateValue() may be called from any thread except the audio one, while a wrapper's state map, its UI and
   most host notifications belong to the host's main thread. The plugin's own setState() is called right away;
   the new value is queued here, and the wrapper takes the queue on its main thread to update its state map,
   the UI and the host. Later updates of the same key replace earlier ones that were not taken yet.

   Some hosts save and load state off their main thread. There the wrapper only takes the values for its state map,
   with takeForStateMap(): they stay queued for the main thread, which still has to tell the UI, but no longer has
   to mark the host state as modified, since the host has them already (or loaded something over them).
 */
class PluginStateUpdates
{
public:
    typedef std::map<const String, String> Map;

    /**
       Validate and apply a state value to the plugin, then queue it for the main thread.
       Returns false, and leaves the plugin untouched, for an unknown key or a value the plugin rejects.
     */
    bool update(PluginExporter& plugin, const char* const key, const char* const value)
    {
        if (! plugin.wantStateKey(key))
        {
            d_stderr("Failed to find plugin state with key \"%s\"", key);
            return false;
        }

        if (! plugin.validateStateValue(key, value))
            return false;

        plugin.setState(key, value);

        const std::lock_guard<std::mutex> cml(fMutex);
        fFromPlugin[String(key)] = value;
        return true;
    }

    /**
       Take the values that are not in the wrapper's state map yet, from wherever the host saves or loads state.
       They stay queued for takeForMainThread(), but without asking it to mark the host state as modified.
       Returns false if there were none.
     */
    bool takeForStateMap(Map& updates)
    {
        updates.clear();

        const std::lock_guard<std::mutex> cml(fMutex);

        if (fFromPlugin.empty())
            return false;

        for (Map::const_iterator cit=fFromPlugin.begin(), cite=fFromPlugin.end(); cit != cite; ++cit)
            fForUI[cit->first] = cit->second;

        updates.swap(fFromPlugin);
        return true;
    }

    /**
       Forget a queued value: the host or the UI has set the key since, so the plugin has another value now.
       Without this the main thread would later put the older value back into the state map and the UI.
     */
    void supersede(const char* const key)
    {
        const std::lock_guard<std::mutex> cml(fMutex);

        if (fFromPlugin.empty() && fForUI.empty())
            return;

        const String skey(key);
        fFromPlugin.erase(skey);
        fForUI.erase(skey);
    }

    /**
       Take everything queued, host main thread only.
       @a markDirty tells whether some of it has not reached the host through a state save or load yet.
       Returns false if there was nothing.
     */
    bool takeForMainThread(Map& updates, bool& markDirty)
    {
        updates.clear();

        const std::lock_guard<std::mutex> cml(fMutex);

        markDirty = ! fFromPlugin.empty();

        if (fForUI.empty() && fFromPlugin.empty())
            return false;

        updates.swap(fForUI);

        for (Map::const_iterator cit=fFromPlugin.begin(), cite=fFromPlugin.end(); cit != cite; ++cit)
            updates[cit->first] = cit->second;

        fFromPlugin.clear();
        return true;
    }

private:
    std::mutex fMutex;
    Map fFromPlugin; // not in the wrapper's state map yet
    Map fForUI;      // in the state map, but the UI and the host's main thread were not told yet
};

/**
   Whether a state update should reach the UI.
 */
static inline
bool isStateForUI(const PluginExporter& plugin, const char* const key)
{
    uint32_t hints = 0x0;
    return plugin.getStateHints(key, hints) && (hints & kStateIsOnlyForDSP) == 0x0;
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_PLUGIN_WANT_STATE

#endif // DAF_PLUGIN_STATE_UPDATES_HPP_INCLUDED
