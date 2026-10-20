// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
#ifndef DGL_IDLE_CALLBACKS_HPP_INCLUDED
#define DGL_IDLE_CALLBACKS_HPP_INCLUDED

#include "../Base.hpp"
#include <algorithm>
#include <memory>
#include <vector>

START_NAMESPACE_DGL

// UI-thread callback registrations. A snapshot retains the registration, never
// the callback object: removing a callback immediately cancels every outstanding
// snapshot of that registration, including one in a nested native event loop.
class IdleCallbacks
{
    struct Entry
    {
        IdleCallback* callback;
        bool calling;
        explicit Entry(IdleCallback* cb) noexcept : callback(cb), calling(false) {}
    };
    typedef std::shared_ptr<Entry> Registration;
    std::vector<Registration> entries;

    struct CallScope
    {
        Entry& entry;
        explicit CallScope(Entry& e) noexcept : entry(e) { entry.calling = true; }
        ~CallScope() { entry.calling = false; }
    };

public:
    IdleCallbacks() = default;
    ~IdleCallbacks() { clear(); }
    IdleCallbacks(const IdleCallbacks&) = delete;
    IdleCallbacks& operator=(const IdleCallbacks&) = delete;

    void push_back(IdleCallback* callback)
    {
        entries.push_back(std::make_shared<Entry>(callback));
    }

    bool contains(const IdleCallback* callback) const noexcept
    {
        return std::any_of(entries.begin(), entries.end(), [callback](const Registration& e) {
            return e->callback == callback;
        });
    }

    void remove(const IdleCallback* callback) noexcept
    {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [callback](const Registration& e) {
            if (e->callback != callback) return false;
            e->callback = nullptr;
            return true;
        }), entries.end());
    }

    void clear() noexcept
    {
        for (const Registration& e : entries) e->callback = nullptr;
        entries.clear();
    }

    void trigger()
    {
        // Keep all dispatch state local after taking the snapshot: a callback
        // may destroy the containing Application as well as its own Window.
        const std::vector<Registration> pending(entries);
        for (const Registration& e : pending)
        {
            if (e->callback != nullptr && !e->calling)
            {
                CallScope scope(*e);
                e->callback->idleCallback();
            }
        }
    }
};

END_NAMESPACE_DGL
#endif
