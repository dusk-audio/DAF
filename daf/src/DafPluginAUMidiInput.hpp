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

#ifndef DAF_PLUGIN_AU_MIDI_INPUT_HPP_INCLUDED
#define DAF_PLUGIN_AU_MIDI_INPUT_HPP_INCLUDED

#include "../DafDetails.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

#if defined(_MSC_VER)
# include <intrin.h>
#endif

/* Needs nothing from CoreAudio, so the unit test (tests/AUMidiInputQueue.cpp) builds and runs it on any platform. */

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

// Defined by tests only, to hold the guard as a MIDI call on another thread would.
template <class Queue> struct AUMidiInputQueueTestAccess;

/**
   The MIDI a host hands an AU for its next render (MusicDeviceMIDIEvent, MusicDeviceSysEx), held until that render's
   run() has been given it.

   Two sets of events, each with storage for the bytes of its sysex messages. MIDI calls fill one, the inbox; render
   takes the inbox for its run and hands the calls the other set, which its previous run left empty, so a call never
   touches what a run is reading.

   A call and render meet only at an append or that swap, under a guard. Render only tries for it, at most 32 times in
   a row, and never waits: when calls hold it all that time, render leaves the inbox where it is, and its events open
   the next run, at its start, in their order, ahead of the events queued after them (a call that races with a render
   counts as one for it).

   A call takes the guard at once when it is free; each append holds it for a moment. A call that finds it taken asks
   Threads::isRealTime() whether it is on a real-time thread, as hosts render on, which must not wait for another
   thread for long nor give up its CPU:
   - Such a call has the guard next: calls on other threads stand back while it waits. It waits spinning on its CPU,
     never yielding it, and for at most Threads::realTimeWaitMicros(), past which only a call taken off its CPU in the
     middle of an append, or a flood of calls, holds the guard. Then it gives up, its message dropped and the call
     saying so.
   - Once one has given up, the next ones that find the guard taken give up at once, until render next takes the
     inbox: the MIDI calls of one render cycle together wait about as long as one of them may, and no longer.
   - A call on any other thread waits as long as it takes, yielding its CPU.

   Room, per render: kMaxEvents messages, and kSysExBytes bytes for the sysex messages longer than MidiEvent::kDataSize.
   A sysex is copied in, as its bytes are the host's again once the call returns. A message with no room left is
   dropped whole, never cut short, and the call says so. Room comes back with the next render, except for a sysex
   longer than kSysExBytes, which never finds any. The AU wrapper's queue takes 512 messages and 16384 bytes of sysex
   per render (kMidiInputSysExBytes), and its output list holds all of that (kMidiOutputListSize), so a run can send on
   everything it was given.

   Requests from another thread, a reset and a block that plays silence, are carried out by the next holder of the
   guard before anything else it does.
 */
template <uint32_t kMaxEvents, uint32_t kSysExBytes, class Threads>
class AUMidiInputQueue
{
public:
    struct Set
    {
        uint32_t count;
        uint32_t sysExUsed;
        MidiEvent events[kMaxEvents];
        uint8_t sysEx[kSysExBytes];
    };

    AUMidiInputQueue() noexcept
        : fInbox(0),
          fGuard(false),
          fGuardLineEnd(),
          fRealTimeWaiting(0),
          fRealTimeGaveUp(false),
          fRealTimeLineEnd(),
          fResetRequested(false),
          fOpenRequested(false)
    {
        std::memset(fSets, 0, sizeof(fSets));
    }

    // A message of up to MidiEvent::kDataSize bytes, at `frame` of the next render. Any thread.
    bool add(const uint32_t frame, const uint8_t* const data, const uint32_t size) noexcept
    {
        if (size == 0 || size > MidiEvent::kDataSize)
            return false;
        if (! lock())
            return false;

        Set& inbox(fSets[fInbox]);
        const bool room = inbox.count < kMaxEvents;
        if (room)
        {
            MidiEvent& event(inbox.events[inbox.count++]);
            event.frame = frame;
            event.size = size;
            std::memset(event.data, 0, sizeof(event.data));
            std::memcpy(event.data, data, size);
            event.dataExt = nullptr;
        }
        unlock();
        return room;
    }

    // A sysex, copied. It has no offset of its own: it keeps its place after the events queued before it, at the last
    // one's frame (the block's start when it is the first). Any thread.
    bool addSysEx(const uint8_t* const data, const uint32_t size) noexcept
    {
        if (data == nullptr || size == 0)
            return false;
        if (! lock())
            return false;

        Set& inbox(fSets[fInbox]);
        const bool room = inbox.count < kMaxEvents
                       && (size <= MidiEvent::kDataSize || size <= kSysExBytes - inbox.sysExUsed);
        if (room)
        {
            MidiEvent& event(inbox.events[inbox.count]);
            event.frame = inbox.count != 0 ? inbox.events[inbox.count - 1].frame : 0;
            event.size = size;
            std::memset(event.data, 0, sizeof(event.data));
            if (size <= MidiEvent::kDataSize)
            {
                std::memcpy(event.data, data, size);
                event.dataExt = nullptr;
            }
            else
            {
                uint8_t* const bytes = inbox.sysEx + inbox.sysExUsed;
                std::memcpy(bytes, data, size);
                event.dataExt = bytes;
                inbox.sysExUsed += size;
            }
            ++inbox.count;
        }
        unlock();
        return room;
    }

    // Render: the inbox for this run, or, without waiting, null when MIDI calls hold the guard throughout. Its events
    // then open the next run, ahead of the events queued after them. The set is the caller's until release(), which
    // empties it.
    Set* take() noexcept
    {
        if (! tryLock())
        {
            fOpenRequested.store(true, std::memory_order_release);
            return nullptr;
        }

        Set* const taken = &fSets[fInbox];
        fInbox ^= 1;
        // a new render cycle: the real-time calls in it may wait again
        if (fRealTimeGaveUp.load(std::memory_order_relaxed))
            fRealTimeGaveUp.store(false, std::memory_order_relaxed);
        unlock();
        return taken;
    }

    // Render, after the run that take() was for. The calls do not touch this set until the next take() hands it back.
    void release(Set* const taken) noexcept
    {
        taken->count = 0;
        taken->sysExUsed = 0;
    }

    // Render, in a block that plays silence: the events queued so far open the next block that runs. Their frames
    // counted from the silent block's start; the next block's own keep theirs.
    void openNextBlock() noexcept
    {
        fOpenRequested.store(true, std::memory_order_release);
        if (tryLock())
            unlock();
    }

    // A reset, on its thread: the next to hold the guard empties the inbox.
    void requestReset() noexcept
    {
        fResetRequested.store(true, std::memory_order_release);
    }

    // When neither render nor a MIDI call can run (Initialize): both sets empty, and nothing left from before.
    void clear() noexcept
    {
        while (! lock())
            std::this_thread::yield();
        fSets[0].count = fSets[0].sysExUsed = 0;
        fSets[1].count = fSets[1].sysExUsed = 0;
        fRealTimeGaveUp.store(false, std::memory_order_relaxed);
        fResetRequested.store(false, std::memory_order_relaxed);
        fOpenRequested.store(false, std::memory_order_relaxed);
        unlock();
    }

private:
    Set fSets[2];
    uint32_t fInbox; // the set the calls fill, under fGuard
    std::atomic<bool> fGuard;
    // Every call reads these, and they rarely change: kept off the guard's cache line (128 bytes on Apple silicon),
    // they do not slow the guard's handover; with them on it, render's takes missed a flood several times as often.
    char fGuardLineEnd[128];
    std::atomic<uint32_t> fRealTimeWaiting; // calls on real-time threads waiting for the guard
    std::atomic<bool> fRealTimeGaveUp;      // one gave up since render last took the inbox
    char fRealTimeLineEnd[128];
    std::atomic<bool> fResetRequested, fOpenRequested;

    // A MIDI call: false only on a real-time thread that gave up (see the class).
    bool lock() noexcept
    {
        if (! takeGuardUnlessRealTimeWaits())
        {
            if (Threads::isRealTime())
                return lockOnRealTimeThread();
            while (! takeGuardUnlessRealTimeWaits())
                std::this_thread::yield();
        }
        carryOutRequests();
        return true;
    }

    bool takeGuardUnlessRealTimeWaits() noexcept
    {
        return fRealTimeWaiting.load(std::memory_order_relaxed) == 0
            && ! fGuard.exchange(true, std::memory_order_acquire);
    }

    // The guard is taken and this call is on a real-time thread.
    bool lockOnRealTimeThread() noexcept
    {
        if (fRealTimeGaveUp.load(std::memory_order_relaxed))
            return false;

        fRealTimeWaiting.fetch_add(1, std::memory_order_relaxed);
        const std::chrono::steady_clock::time_point giveUp = std::chrono::steady_clock::now()
            + std::chrono::microseconds(Threads::realTimeWaitMicros());
        bool locked = false;
        for (uint32_t spins = 1;; ++spins)
        {
            if (! fGuard.load(std::memory_order_relaxed) && ! fGuard.exchange(true, std::memory_order_acquire))
            {
                locked = true;
                break;
            }
            pauseCPU();
            if ((spins & 15) == 0 && std::chrono::steady_clock::now() >= giveUp)
                break;
        }
        fRealTimeWaiting.fetch_sub(1, std::memory_order_relaxed);

        if (! locked)
        {
            fRealTimeGaveUp.store(true, std::memory_order_relaxed);
            return false;
        }
        carryOutRequests();
        return true;
    }

    // Tells the CPU this thread is spinning, without giving the CPU up as std::this_thread::yield() does: on a
    // time-constraint thread the scheduler may take that for milliseconds.
    static void pauseCPU() noexcept
    {
       #if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
       #elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
       #elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_pause();
       #elif defined(_MSC_VER) && defined(_M_ARM64)
        __yield();
       #endif
    }

    // Render: never waits. Up to 32 tries in a row, never yielding (a microsecond at most): calls back to back on
    // another thread let the guard go only for moments, and one try lands in their holds far more often than many do.
    bool tryLock() noexcept
    {
        for (int tries = 1; fGuard.exchange(true, std::memory_order_acquire); ++tries)
            if (tries == 32)
                return false;
        carryOutRequests();
        return true;
    }

    void unlock() noexcept
    {
        fGuard.store(false, std::memory_order_release);
    }

    // The guard's holder, first: what other threads asked for while it could not be had.
    void carryOutRequests() noexcept
    {
        Set& inbox(fSets[fInbox]);
        if (fResetRequested.load(std::memory_order_relaxed)
            && fResetRequested.exchange(false, std::memory_order_acquire))
        {
            inbox.count = 0;
            inbox.sysExUsed = 0;
        }
        if (fOpenRequested.load(std::memory_order_relaxed)
            && fOpenRequested.exchange(false, std::memory_order_acquire))
        {
            for (uint32_t i = 0; i < inbox.count; ++i)
                inbox.events[i].frame = 0;
        }
    }

    template <class Queue> friend struct AUMidiInputQueueTestAccess;

    DAF_DECLARE_NON_COPYABLE(AUMidiInputQueue)
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_PLUGIN_AU_MIDI_INPUT_HPP_INCLUDED
