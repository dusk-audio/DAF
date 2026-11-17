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

/* The AU wrapper's MIDI input queue (AUMidiInputQueue), which needs nothing from CoreAudio. It once kept the host's
 * pointer to a sysex until the next render, which a host may free or reuse as soon as MusicDeviceSysEx returns; it
 * now copies the bytes into storage of its own. Covered here: the copy, what a full queue drops (whole messages
 * only), the frame a sysex takes, the swap between what MIDI calls fill and what a run reads, the requests other
 * threads leave (a reset, a block that plays silence), what render does while a MIDI call holds the guard (it does
 * not wait, and what it could not take opens the next run), what a MIDI call on a real-time thread does then (it has
 * the guard next, gives up rather than wait long, and after one has given up the next give up at once until render
 * takes the inbox), and MIDI calls from several threads while another takes and releases, as render does: every
 * message arrives once and whole, or is counted as dropped.
 *
 * Worth running under TSan and ASan. */

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "daf/src/DafPluginAUMidiInput.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

START_NAMESPACE_DAF

// The guard, held as a MIDI call holds it, for as long as a test needs; and how many real-time calls wait for it.
template <class Queue>
struct AUMidiInputQueueTestAccess
{
    static void hold(Queue& queue) { queue.lock(); }
    static void letGo(Queue& queue) { queue.unlock(); }
    static uint32_t realTimeWaiting(Queue& queue) { return queue.fRealTimeWaiting.load(); }
    static void realTimeWaits(Queue& queue, const int calls) { queue.fRealTimeWaiting.fetch_add(calls); }
};

END_NAMESPACE_DAF

USE_NAMESPACE_DAF

namespace
{
// A thread is real-time here when it says so. A call on one gives up as in the AU wrapper, after 50 us
// (TestThreads), or after 200 ms (PatientThreads), for tests that tell a call that waits its whole bound from one
// that gives up at once, or need one still waiting when they let the guard go.
thread_local bool tRealTime = false;
struct TestThreads
{
    static bool isRealTime() noexcept { return tRealTime; }
    static uint32_t realTimeWaitMicros() noexcept { return 50; }
};
struct PatientThreads
{
    static bool isRealTime() noexcept { return tRealTime; }
    static uint32_t realTimeWaitMicros() noexcept { return 200000; }
};
typedef AUMidiInputQueue<8, 64, TestThreads> SmallQueue;
typedef AUMidiInputQueue<8, 64, PatientThreads> PatientQueue;

// One MIDI call on a thread of its own, real-time or not; whether it queued its message, and how long it took.
template <class Queue>
bool addOnThread(Queue& queue, const bool realTime, const uint8_t* const data, double& millis)
{
    bool added = false;
    std::thread caller([&] {
        tRealTime = realTime;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        added = queue.add(0, data, 3);
        millis = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    });
    caller.join();
    return added;
}

// take() on a thread of its own, as render, while this thread holds the guard as a MIDI call would. False when it did
// not come back within two seconds: it waited for the guard. The guard is let go either way, so a take() that waits
// still returns, and the test fails rather than hangs.
template <class Queue>
bool takeWhileHeld(Queue& queue, typename Queue::Set*& taken)
{
    AUMidiInputQueueTestAccess<Queue>::hold(queue);
    std::atomic<bool> returned(false);
    std::thread render([&queue, &taken, &returned] {
        taken = queue.take();
        returned.store(true);
    });
    for (int ms = 0; ms < 2000 && ! returned.load(); ++ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool atOnce = returned.load();
    AUMidiInputQueueTestAccess<Queue>::letGo(queue);
    render.join();
    return atOnce;
}

const uint8_t* bytesOf(const MidiEvent& event)
{
    return event.size > MidiEvent::kDataSize ? event.dataExt : event.data;
}

bool holds(const MidiEvent& event, const uint32_t frame, const uint8_t* const bytes, const uint32_t size)
{
    return event.frame == frame && event.size == size && std::memcmp(bytesOf(event), bytes, size) == 0;
}

int testCopiesAndOrder()
{
    SmallQueue queue;
    const uint8_t on[3] = {0x90, 60, 100};
    uint8_t sysex[20];
    for (uint32_t i = 0; i < sizeof(sysex); ++i)
        sysex[i] = static_cast<uint8_t>(i == 0 ? 0xF0 : i + 1 == sizeof(sysex) ? 0xF7 : i);
    const uint8_t shortSysex[4] = {0xF0, 0x7D, 0x01, 0xF7};
    uint8_t original[sizeof(sysex)];
    std::memcpy(original, sysex, sizeof(sysex));

    // Two events before a sysex: it follows the later one, at its frame (30), not the first one's (10).
    DAF_ASSERT_EQUAL(queue.add(10, on, 3) && queue.add(30, on, 3), true, "events must queue");
    DAF_ASSERT_EQUAL(queue.addSysEx(sysex, sizeof(sysex)), true, "a sysex must queue");
    DAF_ASSERT_EQUAL(queue.addSysEx(shortSysex, sizeof(shortSysex)), true, "a sysex of 4 bytes must queue");
    // The host's buffer is its own again: whatever it writes there now must not reach the run.
    std::memset(sysex, 0x42, sizeof(sysex));

    SmallQueue::Set* const set = queue.take();
    DAF_ASSERT_NOT_EQUAL(set, nullptr, "an uncontended take must succeed");
    DAF_ASSERT_EQUAL(set->count, 4u, "the run must get every message");
    DAF_ASSERT_EQUAL(holds(set->events[0], 10, on, 3) && holds(set->events[1], 30, on, 3), true,
                     "events must keep their frames and order");
    DAF_ASSERT_EQUAL(holds(set->events[2], 30, original, sizeof(original)), true,
                     "a sysex must be the bytes it was sent with, at the frame of the event queued just before it");
    DAF_ASSERT_EQUAL(holds(set->events[3], 30, shortSysex, sizeof(shortSysex)) && set->events[3].dataExt == nullptr,
                     true, "a sysex that fits MidiEvent::data must be kept there");
    queue.release(set);

    // A sysex that opens a set takes its start.
    DAF_ASSERT_EQUAL(queue.addSysEx(original, sizeof(original)), true, "a first sysex must queue");
    SmallQueue::Set* const next = queue.take();
    DAF_ASSERT_EQUAL(next != nullptr && next->count == 1 && holds(next->events[0], 0, original, sizeof(original)), true,
                     "a sysex that opens a set must run at the block's start");
    queue.release(next);
    return 0;
}

int testFull()
{
    SmallQueue queue;
    const uint8_t on[3] = {0x90, 60, 100};
    uint8_t sysex[64];
    std::memset(sysex, 0x11, sizeof(sysex));
    sysex[0] = 0xF0;

    // Event slots: 8. The ninth message of any kind is dropped.
    for (uint32_t i = 0; i < 8; ++i)
        DAF_ASSERT_EQUAL(queue.add(i, on, 3), true, "events up to the queue's size must queue");
    DAF_ASSERT_EQUAL(queue.add(8, on, 3) || queue.addSysEx(sysex, 5), false, "a full queue must drop what comes next");
    SmallQueue::Set* set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 8, true, "a full queue must keep what it took");
    queue.release(set);

    // Sysex bytes: 64. They fill exactly; a byte more, alone or after, is dropped whole.
    DAF_ASSERT_EQUAL(queue.addSysEx(sysex, 40) && queue.addSysEx(sysex, 24), true, "sysex filling the storage");
    DAF_ASSERT_EQUAL(queue.addSysEx(sysex, 5), false, "a sysex with no room left must be dropped");
    DAF_ASSERT_EQUAL(queue.addSysEx(sysex, 4), true, "a sysex short enough for MidiEvent::data needs no storage");
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 3 && set->events[0].size == 40 && set->events[1].size == 24
                     && set->events[2].size == 4, true, "the storage must hold exactly what it was filled with");
    queue.release(set);
    DAF_ASSERT_EQUAL(queue.addSysEx(sysex, 65), false, "a sysex longer than the storage must be dropped");
    DAF_ASSERT_EQUAL(queue.addSysEx(sysex, 64), true, "a released set must have its storage back");
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1 && set->events[0].size == 64, true, "the whole storage");
    queue.release(set);

    // Nothing to queue is refused.
    DAF_ASSERT_EQUAL(queue.add(0, on, 0) || queue.add(0, on, 5) || queue.addSysEx(nullptr, 3)
                     || queue.addSysEx(sysex, 0),
                     false, "an empty or oversized event must be refused");
    return 0;
}

// What MIDI calls queue while a run reads its set goes to the other one, for the next run.
int testSwap()
{
    SmallQueue queue;
    const uint8_t a[3] = {0x90, 1, 1}, b[3] = {0x90, 2, 2};
    queue.add(5, a, 3);
    SmallQueue::Set* const first = queue.take();
    DAF_ASSERT_EQUAL(first != nullptr && first->count == 1, true, "the first run gets the first event");
    queue.add(6, b, 3);
    DAF_ASSERT_EQUAL(first->count == 1 && holds(first->events[0], 5, a, 3), true,
                     "an event queued during a run must not reach the set it reads");
    queue.release(first);
    SmallQueue::Set* const second = queue.take();
    DAF_ASSERT_EQUAL(second != nullptr && second != first && second->count == 1 && holds(second->events[0], 6, b, 3),
                     true, "the next run must get the event queued during the last");
    queue.release(second);
    SmallQueue::Set* const third = queue.take();
    DAF_ASSERT_EQUAL(third != nullptr && third->count == 0, true, "a run with nothing queued gets nothing");
    queue.release(third);
    return 0;
}

int testRequests()
{
    SmallQueue queue;
    const uint8_t a[3] = {0x90, 1, 1}, b[3] = {0x90, 2, 2};

    // A reset drops what was queued before it, not what is queued after.
    queue.add(5, a, 3);
    queue.requestReset();
    queue.add(7, b, 3);
    SmallQueue::Set* set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1 && holds(set->events[0], 7, b, 3), true,
                     "a reset must drop the events queued before it only");
    queue.release(set);
    queue.add(5, a, 3);
    queue.requestReset();
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 0, true, "a reset must drop the events a run would take");
    queue.release(set);

    // A block that plays silence: the events queued during it open the next block; the next block's own keep theirs.
    queue.add(60, a, 3);
    queue.openNextBlock();
    queue.add(10, b, 3);
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 2 && holds(set->events[0], 0, a, 3)
                     && holds(set->events[1], 10, b, 3),
                     true, "a silent block's events must open the next block, ahead of its own at their frames");
    queue.release(set);

    // clear() empties both sets, whatever was pending.
    queue.add(1, a, 3);
    set = queue.take();
    queue.add(2, b, 3);
    queue.release(set);
    queue.requestReset();
    queue.clear();
    queue.add(3, b, 3);
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1 && holds(set->events[0], 3, b, 3), true,
                     "clear() must empty both sets and drop pending requests");
    queue.release(set);
    return 0;
}

// Render never waits for the guard: while a MIDI call holds it, take() comes back at once with nothing, and leaves
// what was queued for the next run.
int testRenderNeverWaits()
{
    SmallQueue queue;
    const uint8_t on[3] = {0x90, 60, 100};
    queue.add(7, on, 3);
    SmallQueue::Set* taken = nullptr;
    DAF_ASSERT_EQUAL(takeWhileHeld(queue, taken), true, "take() must not wait for a guard a MIDI call holds");
    DAF_ASSERT_EQUAL(taken, nullptr, "take() must come back with nothing while a MIDI call holds the guard");
    SmallQueue::Set* const set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1 && set->events[0].size == 3, true,
                     "what a missed take() left must reach the next run");
    queue.release(set);
    return 0;
}

// What a take() could not have (a MIDI call held the guard) opens the next run: at its start, in the order queued,
// ahead of the events queued for that run, which keep their frames. Frames never go backwards: a note-off left at 500
// must not follow the next block's note-on at 10.
int testMissedTake()
{
    SmallQueue queue;
    const uint8_t offA[3] = {0x80, 60, 0}, offB[3] = {0x80, 62, 0}, offC[3] = {0x80, 64, 0};
    const uint8_t on[3] = {0x90, 60, 100};
    SmallQueue::Set* missed = nullptr;

    queue.add(100, offA, 3);
    queue.add(300, offB, 3);
    queue.add(500, offC, 3);
    DAF_ASSERT_EQUAL(takeWhileHeld(queue, missed) && missed == nullptr, true, "take() must miss a held guard");
    queue.add(10, on, 3);
    SmallQueue::Set* set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 4 && holds(set->events[0], 0, offA, 3)
                     && holds(set->events[1], 0, offB, 3) && holds(set->events[2], 0, offC, 3)
                     && holds(set->events[3], 10, on, 3), true,
                     "what a missed take() left must open the next run in its order, ahead of that run's events");
    queue.release(set);

    // With nothing queued after the miss, the next take() moves them itself; one that does not miss moves nothing.
    queue.add(200, offA, 3);
    DAF_ASSERT_EQUAL(takeWhileHeld(queue, missed) && missed == nullptr, true, "take() must miss a held guard");
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1 && holds(set->events[0], 0, offA, 3), true,
                     "what a missed take() left must open the next run");
    queue.release(set);
    queue.add(200, offA, 3);
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1 && holds(set->events[0], 200, offA, 3), true,
                     "a take() that does not miss must leave frames as queued");
    queue.release(set);
    return 0;
}

// A MIDI call on a real-time thread waits no longer than realTimeWaitMicros() for a guard another call holds: it
// gives up and drops its message, saying so. One on any other thread waits until the guard is let go.
int testRealTimeCallsDoNotWait()
{
    SmallQueue queue;
    const uint8_t on[3] = {0x90, 60, 100}, off[3] = {0x80, 60, 0};
    AUMidiInputQueueTestAccess<SmallQueue>::hold(queue);
    std::atomic<int> realTimeAdded(-1), otherAdded(-1);
    std::atomic<int64_t> realTimeMicros(0);
    std::thread realTime([&] {
        tRealTime = true;
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        const bool added = queue.add(10, on, 3);
        realTimeMicros.store(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count());
        realTimeAdded.store(added ? 1 : 0);
    });
    std::thread other([&] { otherAdded.store(queue.add(20, off, 3) ? 1 : 0); });
    for (int ms = 0; ms < 2000 && realTimeAdded.load() < 0; ++ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const int realTimeWhileHeld = realTimeAdded.load(), otherWhileHeld = otherAdded.load();
    AUMidiInputQueueTestAccess<SmallQueue>::letGo(queue);
    realTime.join();
    other.join();
    d_stdout("a real-time call gave up after %lld us", static_cast<long long>(realTimeMicros.load()));
    DAF_ASSERT_EQUAL(realTimeWhileHeld, 0, "a call on a real-time thread must give up on a held guard, and say so");
    DAF_ASSERT_EQUAL(realTimeMicros.load() < 500000, true,
                     "a call on a real-time thread must give up within its bound");
    DAF_ASSERT_EQUAL(otherWhileHeld, -1, "a call on another thread must wait for a held guard");
    DAF_ASSERT_EQUAL(otherAdded.load(), 1, "a call on another thread must queue once the guard is let go");
    SmallQueue::Set* const set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1 && holds(set->events[0], 20, off, 3), true,
                     "only the message of the call that waited must be queued");
    queue.release(set);
    return 0;
}

// Once a real-time call has given up, the next real-time calls that find the guard taken give up at once, until
// render next takes the inbox: a render cycle's MIDI calls together wait no longer than one may. With a bound of
// 200 ms, a call that waits takes 200 ms and one that gives up at once next to nothing.
int testRealTimeGiveUpLastsTheCycle()
{
    typedef AUMidiInputQueueTestAccess<PatientQueue> Access;
    PatientQueue queue;
    const uint8_t note[3] = {0x90, 60, 100};
    double first = 0, second = 0, other = 0, afterTake = 0;
    bool otherAdded = false;

    Access::hold(queue);
    const bool firstAdded = addOnThread(queue, true, note, first);
    const bool secondAdded = addOnThread(queue, true, note, second);
    std::thread notRealTime([&] { otherAdded = addOnThread(queue, false, note, other); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    Access::letGo(queue);
    notRealTime.join();
    DAF_ASSERT_EQUAL(! firstAdded && first >= 150, true, "a real-time call must give up after its bound");
    DAF_ASSERT_EQUAL(! secondAdded && second < 100, true, "the next real-time call must give up at once");
    DAF_ASSERT_EQUAL(otherAdded, true, "a call on another thread must still wait, and queue");

    // Render takes the inbox: the next cycle's real-time calls wait again.
    PatientQueue::Set* const set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1, true, "render must take what the waiting call queued");
    queue.release(set);
    Access::hold(queue);
    const bool afterTakeAdded = addOnThread(queue, true, note, afterTake);
    Access::letGo(queue);
    DAF_ASSERT_EQUAL(! afterTakeAdded && afterTake >= 150, true,
                     "after render took the inbox, a real-time call must wait its whole bound again");
    d_stdout("real-time calls: gave up after %.0f ms, the next at once (%.3f ms), after a take after %.0f ms", first,
             second, afterTake);
    return 0;
}

// A MIDI call on a real-time thread has the guard next: while one waits, calls on other threads stand back, even
// from a guard that is free at that moment, so that it does not wait behind them, however many there are. Then the
// real-time call waiting is let go and gets the guard first.
int testOthersStandBack()
{
    typedef AUMidiInputQueueTestAccess<PatientQueue> Access;
    PatientQueue queue;
    const uint8_t realTimeNote[3] = {0x90, 1, 1}, otherNote[3] = {0x90, 2, 2};

    // A real-time call waiting, as the queue counts it: another call finds the guard free, and stands back.
    Access::realTimeWaits(queue, 1);
    std::atomic<bool> otherAdded(false);
    std::thread other([&] { otherAdded.store(queue.add(2, otherNote, 3)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool stoodBack = ! otherAdded.load();
    Access::realTimeWaits(queue, -1);
    other.join();
    DAF_ASSERT_EQUAL(stoodBack, true, "a call on another thread must stand back while a real-time call waits");
    DAF_ASSERT_EQUAL(otherAdded.load(), true, "and queue once none waits");
    PatientQueue::Set* set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 1, true, "the call that stood back must have queued");
    queue.release(set);

    // A real call: the guard held, a real-time call waits, others come and stand back; let go, the real-time call
    // queues first.
    Access::hold(queue);
    std::thread realTime([&] {
        tRealTime = true;
        queue.add(1, realTimeNote, 3);
    });
    for (int ms = 0; ms < 150 && Access::realTimeWaiting(queue) == 0; ++ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const bool realTimeWaits = Access::realTimeWaiting(queue) == 1;
    std::thread others[3];
    for (std::thread& another : others)
        another = std::thread([&] { queue.add(2, otherNote, 3); });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    Access::letGo(queue);
    realTime.join();
    for (std::thread& another : others)
        another.join();
    DAF_ASSERT_EQUAL(realTimeWaits, true, "a call on a real-time thread must wait for a held guard, for a while");
    set = queue.take();
    DAF_ASSERT_EQUAL(set != nullptr && set->count == 4 && holds(set->events[0], 1, realTimeNote, 3), true,
                     "a call on a real-time thread must have the guard ahead of the calls on other threads");
    queue.release(set);
    return 0;
}

// MIDI calls from several threads, the first of them real-time, while one thread takes and releases, as render does.
// Each message carries its thread and sequence number; every one must arrive once, whole and in its thread's order,
// or be counted as dropped, at its frame, or at 0 in the first set taken after a take() that missed.
int testThreads()
{
    typedef AUMidiInputQueue<512, 16384, TestThreads> Queue;
    static Queue queue;
    constexpr uint32_t kThreads = 4;
    constexpr uint32_t kMessages = 20000;
    std::atomic<uint32_t> dropped(0), droppedRealTime(0);
    std::atomic<bool> producing(true);

    std::vector<std::thread> producers;
    for (uint32_t t = 0; t < kThreads; ++t)
    {
        producers.emplace_back([t, &dropped, &droppedRealTime] {
            tRealTime = t == 0;
            uint8_t sysex[40];
            for (uint32_t n = 0; n < kMessages; ++n)
            {
                bool added;
                if (n % 5 == 4)
                {
                    sysex[0] = 0xF0;
                    sysex[1] = static_cast<uint8_t>(t);
                    std::memcpy(sysex + 2, &n, sizeof(n));
                    for (uint32_t i = 6; i < sizeof(sysex) - 1; ++i)
                        sysex[i] = static_cast<uint8_t>((n + i) & 0x7F);
                    sysex[sizeof(sysex) - 1] = 0xF7;
                    added = queue.addSysEx(sysex, sizeof(sysex));
                    std::memset(sysex, 0x42, sizeof(sysex));
                }
                else
                {
                    const uint8_t data[4] = {
                        static_cast<uint8_t>(0x90 | t), static_cast<uint8_t>(n & 0x7F),
                        static_cast<uint8_t>((n >> 7) & 0x7F), static_cast<uint8_t>((n >> 14) & 0x7F)
                    };
                    added = queue.add(n & 0xFF, data, 4);
                }
                if (! added)
                    (tRealTime ? droppedRealTime : dropped).fetch_add(1);
            }
        });
    }

    uint32_t received = 0, misses = 0, bad = 0;
    int64_t last[kThreads];
    for (uint32_t t = 0; t < kThreads; ++t)
        last[t] = -1;
    std::thread consumer([&] {
        bool missed = false;
        for (;;)
        {
            const bool done = ! producing.load();
            Queue::Set* const set = queue.take();
            if (set == nullptr)
            {
                ++misses;
                missed = true;
                continue;
            }
            for (uint32_t i = 0; i < set->count; ++i)
            {
                const MidiEvent& event(set->events[i]);
                const uint8_t* const bytes = bytesOf(event);
                uint32_t t, n;
                if (event.size == 40)
                {
                    t = bytes[1];
                    std::memcpy(&n, bytes + 2, sizeof(n));
                    bool intact = bytes[0] == 0xF0 && bytes[39] == 0xF7;
                    for (uint32_t b = 6; b < 39 && intact; ++b)
                        intact = bytes[b] == static_cast<uint8_t>((n + b) & 0x7F);
                    bad += intact ? 0 : 1;
                }
                else
                {
                    t = bytes[0] & 0x0F;
                    n = bytes[1] | (bytes[2] << 7) | (bytes[3] << 14);
                    bad += event.size == 4 && (event.frame == (n & 0xFF) || (missed && event.frame == 0)) ? 0 : 1;
                }
                if (t >= kThreads || static_cast<int64_t>(n) <= last[t])
                    ++bad;
                else
                    last[t] = n;
                ++received;
            }
            queue.release(set);
            missed = false;
            if (done)
                break;
        }
    });

    for (std::thread& producer : producers)
        producer.join();
    producing.store(false);
    consumer.join();

    d_stdout("threads: %u sent, %u received, dropped %u from the other threads and %u from the real-time one (the "
             "queue full, or a real-time call that gave up), %u takes found the queue held, %u bad",
             kThreads * kMessages, received, dropped.load(), droppedRealTime.load(), misses, bad);
    DAF_ASSERT_EQUAL(bad, 0u, "every message must arrive whole, once, in its thread's order");
    DAF_ASSERT_EQUAL(received + dropped.load() + droppedRealTime.load(), kThreads * kMessages,
                     "every message must arrive or be dropped");
    return 0;
}
}

int main()
{
    if (const int failed = testCopiesAndOrder())
        return failed;
    if (const int failed = testFull())
        return failed;
    if (const int failed = testSwap())
        return failed;
    if (const int failed = testRequests())
        return failed;
    if (const int failed = testRenderNeverWaits())
        return failed;
    if (const int failed = testMissedTake())
        return failed;
    if (const int failed = testRealTimeCallsDoNotWait())
        return failed;
    if (const int failed = testRealTimeGiveUpLastsTheCycle())
        return failed;
    if (const int failed = testOthersStandBack())
        return failed;
    if (const int failed = testThreads())
        return failed;
    d_stdout("AU MIDI input queue tests passed");
    return 0;
}
