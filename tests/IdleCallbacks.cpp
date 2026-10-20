// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "dgl/src/IdleCallbacks.hpp"
#include <functional>
#include <memory>
#include <vector>

struct Action : DGL_NAMESPACE::IdleCallback
{
    std::function<void()> action;
    void idleCallback() override { action(); }
};

int main()
{
    using DGL_NAMESPACE::IdleCallbacks;
    unsigned checks = 0;
#define CHECK(value, expected, message) ++checks; DAF_ASSERT_EQUAL(value, expected, message)
    {
        IdleCallbacks callbacks;
        callbacks.trigger();
        Action a, b; std::vector<int> order;
        a.action = [&] { order.push_back(1); };
        b.action = [&] { order.push_back(2); };
        callbacks.push_back(&a); callbacks.push_back(&b);
        CHECK(callbacks.contains(&a), true, "registration is findable");
        callbacks.trigger(); callbacks.trigger();
        CHECK(order, (std::vector<int>{1, 2, 1, 2}), "ordinary callbacks retain registration order");
        callbacks.remove(&a); callbacks.remove(&a);
        CHECK(callbacks.contains(&a), false, "removal is idempotent");
        CHECK(callbacks.contains(&b), true, "removing one preserves its neighbour");
        callbacks.trigger();
        CHECK(order.back(), 2, "remaining callback still runs");
    }
    {
        IdleCallbacks callbacks;
        std::unique_ptr<Action> self(new Action);
        Action survivor; unsigned calls = 0, selfCalls = 0;
        self->action = [&] { ++selfCalls; callbacks.remove(self.get()); self.reset(); };
        survivor.action = [&] { ++calls; };
        callbacks.push_back(self.get()); callbacks.push_back(&survivor);
        callbacks.trigger(); callbacks.trigger();
        CHECK(selfCalls, 1u, "a callback can unregister and delete itself");
        CHECK(calls, 2u, "self-deletion preserves subsequent dispatch");
    }
    {
        IdleCallbacks callbacks;
        Action first, survivor;
        std::unique_ptr<Action> victim(new Action);
        unsigned deadCalls = 0, liveCalls = 0;
        victim->action = [&] { ++deadCalls; };
        first.action = [&] { callbacks.remove(victim.get()); victim.reset(); };
        survivor.action = [&] { ++liveCalls; };
        callbacks.push_back(&first); callbacks.push_back(victim.get()); callbacks.push_back(&survivor);
        callbacks.trigger();
        CHECK(deadCalls, 0u, "a removed and destroyed future callback is never called");
        CHECK(liveCalls, 1u, "removing another callback preserves its neighbours");
    }
    {
        IdleCallbacks callbacks;
        Action first, replaced; bool changed = false; unsigned calls = 0;
        first.action = [&] {
            if (changed) return;
            changed = true;
            callbacks.remove(&replaced); callbacks.push_back(&replaced);
        };
        replaced.action = [&] { ++calls; };
        callbacks.push_back(&first); callbacks.push_back(&replaced);
        callbacks.trigger();
        CHECK(calls, 0u, "re-registration at the identical address cannot revive a canceled snapshot");
        callbacks.trigger();
        CHECK(calls, 1u, "the new registration starts on the next dispatch");
    }
    {
        IdleCallbacks callbacks;
        Action first, added; bool registered = false; unsigned calls = 0;
        first.action = [&] { if (!registered) { registered = true; callbacks.push_back(&added); } };
        added.action = [&] { ++calls; };
        callbacks.push_back(&first);
        callbacks.trigger();
        CHECK(calls, 0u, "new callbacks wait for the next dispatch");
        callbacks.trigger();
        CHECK(calls, 1u, "new callbacks remain registered afterwards");
    }
    {
        IdleCallbacks callbacks;
        Action first, last; unsigned calls = 0;
        first.action = [&] { callbacks.clear(); };
        last.action = [&] { ++calls; };
        callbacks.push_back(&first); callbacks.push_back(&last);
        callbacks.trigger(); callbacks.trigger();
        CHECK(calls, 0u, "clearing during dispatch cancels every pending callback");
        CHECK(callbacks.contains(&last), false, "clear removes registrations");
    }
    {
        std::unique_ptr<IdleCallbacks> callbacks(new IdleCallbacks);
        Action first, last; unsigned calls = 0;
        first.action = [&] { callbacks.reset(); };
        last.action = [&] { ++calls; };
        callbacks->push_back(&first); callbacks->push_back(&last);
        callbacks->trigger();
        CHECK(calls, 0u, "destroying the registry during dispatch cancels pending calls");
        CHECK(callbacks.get(), nullptr, "the owner really was destroyed");
    }
    {
        IdleCallbacks callbacks;
        Action recursive; unsigned calls = 0;
        recursive.action = [&] { ++calls; callbacks.trigger(); };
        callbacks.push_back(&recursive);
        callbacks.trigger(); callbacks.trigger();
        CHECK(calls, 2u, "a nested event loop does not recursively re-enter the active registration");
    }
    {
        IdleCallbacks callbacks;
        Action same; unsigned calls = 0;
        same.action = [&] { ++calls; };
        callbacks.push_back(&same); callbacks.push_back(&same);
        callbacks.trigger();
        CHECK(calls, 2u, "explicit duplicate registrations retain their prior behavior");
        callbacks.remove(&same); callbacks.trigger();
        CHECK(calls, 2u, "removal cancels every duplicate registration");
    }
    d_stdout("Idle callbacks: %u checks passed", checks);
    return 0;
}
