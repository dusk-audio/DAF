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


// MIDI through a plugin without audio (an aumi) in the AU wrapper, with main() as the host, driven the way Apple's
// AUv3 bridge drives one: each cycle the host hands the unit that cycle's events (MusicDeviceMIDIEvent, at their
// offsets into the next render) and then renders it, with no buffers. The wrapper used to run such a plugin from
// inside MusicDeviceMIDIEvent instead, for as many frames as the event's offset: an event at offset n arrived at frame
// n of an n-frame block, which the plugin drops, so only offset 0 got through; and the plugin's output went to the
// host outside render with no timestamp, which Apple's bridge dereferences. Every event must reach run() in the
// render it is for, at its offset, in the order sent, once, with that render's frame count; what the plugin writes
// must reach the host from that render, stamped with the render's timestamp, at the frames written. A sysex keeps its
// place after the event sent before it. Every status byte the call can carry must arrive as a message of the length
// MIDI gives it, with nothing past that length; the others, and an event a full queue has no room for, must be
// refused with an error. Everything one render takes in must fit what the plugin sends back. And the wrapper must
// tell a real-time thread, which its MIDI calls do not let wait long, from any other, and let such a call wait out a
// guard held briefly but give up on one held long.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <AudioToolbox/MusicDevice.h>
#include <mach/mach_time.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <thread>

namespace
{
// A message as recorded: its first bytes, and a checksum of all of them for a longer one (a sysex).
struct Message
{
    uint32_t frame;
    uint32_t size;
    uint32_t checksum;
    uint8_t bytes[320];
};
constexpr uint32_t kCapacity = 1024;

uint32_t checksum(const uint8_t* const bytes, const uint32_t size)
{
    uint32_t hash = 2166136261u; // FNV-1a
    for (uint32_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

// What the host is doing.
bool gInsideRender = false;
AudioTimeStamp gRenderTimeStamp = {};

// What run() received, and when it ran; and the events it got with bytes left past their length in MidiEvent::data.
Message gReceived[kCapacity];
uint32_t gReceivedCount = 0;
uint32_t gRuns = 0, gRunsOutsideRender = 0, gLastRunFrames = 0, gEchoesRefused = 0, gUncleared = 0;

// What reached the host's MIDI output callback.
Message gSent[kCapacity];
uint32_t gSentCount = 0;
uint32_t gOutputCallsOutsideRender = 0, gOutputCallsWithoutTheRenderTimeStamp = 0, gUnreadablePackets = 0;

void record(Message* const messages, uint32_t& count, const uint32_t frame, const uint8_t* const bytes,
            const uint32_t size)
{
    if (count == kCapacity || size == 0)
    {
        ++gUnreadablePackets;
        return;
    }
    Message& message(messages[count++]);
    message.frame = frame;
    message.size = size;
    message.checksum = checksum(bytes, size);
    std::memcpy(message.bytes, bytes, std::min<uint32_t>(size, sizeof(message.bytes)));
}
}

START_NAMESPACE_DAF

// Records what it receives and how it is run, and sends every event back out at its frame.
class MidiOnlyEchoPlugin : public Plugin
{
public:
    MidiOnlyEchoPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "MidiOnlyTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void run(const float**, float**, const uint32_t frames, const MidiEvent* const events,
             const uint32_t count) override
    {
        ++gRuns;
        gLastRunFrames = frames;
        if (! gInsideRender)
            ++gRunsOutsideRender;
        for (uint32_t i = 0; i < count; ++i)
        {
            const MidiEvent& event(events[i]);
            record(gReceived, gReceivedCount, event.frame,
                   event.size > MidiEvent::kDataSize ? event.dataExt : event.data, event.size);
            for (uint32_t b = event.size; b < MidiEvent::kDataSize; ++b)
                if (event.data[b] != 0)
                {
                    ++gUncleared;
                    break;
                }
            if (! writeMidiEvent(event))
                ++gEchoesRefused;
        }
    }
};

Plugin* createPlugin()
{
    return new MidiOnlyEchoPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"

START_NAMESPACE_DAF

// The guard of a queue of the wrapper's own type, held as a MIDI call on another thread holds it.
template <class Queue>
struct AUMidiInputQueueTestAccess
{
    static void hold(Queue& queue) { queue.lock(); }
    static void letGo(Queue& queue) { queue.unlock(); }
    static bool realTimeWaits(Queue& queue) { return queue.fRealTimeWaiting.load() != 0; }
};

END_NAMESPACE_DAF

USE_NAMESPACE_DAF

namespace
{
constexpr UInt32 kBlock = 256;
AudioUnit gUnit = nullptr;
Float64 gSampleTime = 4096.0;

// The packets as a host walks them (MIDIPacketNext); a packet that cannot be a message stops the walk.
OSStatus midiOutput(void*, const AudioTimeStamp* const timeStamp, UInt32, const MIDIPacketList* const list)
{
    if (! gInsideRender)
        ++gOutputCallsOutsideRender;
    if (timeStamp == nullptr || timeStamp->mSampleTime != gRenderTimeStamp.mSampleTime)
        ++gOutputCallsWithoutTheRenderTimeStamp;
    const MIDIPacket* packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i, packet = MIDIPacketNext(packet))
    {
        if (packet->length == 0 || packet->timeStamp >= kBlock)
        {
            ++gUnreadablePackets;
            break;
        }
        record(gSent, gSentCount, static_cast<uint32_t>(packet->timeStamp), packet->data, packet->length);
    }
    return noErr;
}

// How a host renders a unit without audio: through its output element, with the buffers the element's format calls for
// (stereo), with buffers left for the unit to provide, or, as Apple's bridge renders a unit with no element, with
// none. Each render the sweeps below make takes the next way in turn.
enum Buffers { kNoBuffers, kStereo, kStereoLeftToUnit, kBufferWays };
uint32_t gRenders = 0;

OSStatus render(const UInt32 frames = kBlock)
{
    const Buffers way = static_cast<Buffers>(gRenders++ % kBufferWays);
    float samples[2][kBlock];
    alignas(AudioBufferList) uint8_t storage[sizeof(AudioBufferList) + sizeof(AudioBuffer)] = {};
    AudioBufferList* const list = reinterpret_cast<AudioBufferList*>(storage);
    list->mNumberBuffers = way == kNoBuffers ? 0 : 2;
    for (UInt32 ch = 0; ch < list->mNumberBuffers; ++ch)
    {
        for (UInt32 i = 0; i < kBlock; ++i)
            samples[ch][i] = 1.0f;
        list->mBuffers[ch].mNumberChannels = 1;
        list->mBuffers[ch].mDataByteSize = static_cast<UInt32>(sizeof(float) * frames);
        list->mBuffers[ch].mData = way == kStereo ? samples[ch] : nullptr;
    }
    gRenderTimeStamp = AudioTimeStamp();
    gRenderTimeStamp.mFlags = kAudioTimeStampSampleTimeValid;
    gRenderTimeStamp.mSampleTime = gSampleTime;
    AudioUnitRenderActionFlags flags = 0;
    gInsideRender = true;
    OSStatus status = AudioUnitRender(gUnit, &flags, &gRenderTimeStamp, 0, frames, list);
    gInsideRender = false;
    gSampleTime += frames;
    // whatever the buffers, they come back silent
    for (UInt32 ch = 0; status == noErr && ch < list->mNumberBuffers; ++ch)
    {
        const float* const data = static_cast<const float*>(list->mBuffers[ch].mData);
        if (data == nullptr)
            status = kAudio_ParamError;
        for (UInt32 i = 0; data != nullptr && i < frames; ++i)
            if (data[i] != 0.0f)
                status = kAudio_ParamError;
    }
    return status;
}

// The messages a block must carry, in order: what run() is given and what the host is sent back.
Message gExpected[kCapacity];
uint32_t gExpectedCount = 0;

// Starts a block's record.
void clear()
{
    gExpectedCount = gReceivedCount = gSentCount = 0;
    gRunsOutsideRender = gEchoesRefused = gUncleared = 0;
    gOutputCallsOutsideRender = gOutputCallsWithoutTheRenderTimeStamp = gUnreadablePackets = 0;
}

OSStatus send(const uint8_t status, const uint8_t data1, const uint8_t data2, const UInt32 offset)
{
    const uint8_t bytes[3] = {status, data1, data2};
    record(gExpected, gExpectedCount, offset, bytes, (status & 0xF0) == 0xC0 || (status & 0xF0) == 0xD0 ? 2 : 3);
    return MusicDeviceMIDIEvent(gUnit, status, data1, data2, offset);
}

OSStatus sendSysEx(const uint8_t* const bytes, const UInt32 size, const UInt32 expectedFrame)
{
    record(gExpected, gExpectedCount, expectedFrame, bytes, size);
    return MusicDeviceSysEx(gUnit, bytes, size);
}

bool same(const Message* const messages, const uint32_t count, const char* const what, const char* const block)
{
    if (count != gExpectedCount)
    {
        d_stderr2("%s: %s %u messages, expected %u", block, what, count, gExpectedCount);
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        const Message& got(messages[i]);
        const Message& want(gExpected[i]);
        if (got.frame != want.frame || got.size != want.size || got.checksum != want.checksum
            || std::memcmp(got.bytes, want.bytes, std::min<uint32_t>(want.size, sizeof(want.bytes))) != 0)
        {
            d_stderr2("%s: %s message %u as %u bytes from %02X at frame %u, expected %u bytes from %02X at "
                      "frame %u", block, what, i, got.size, got.bytes[0], got.frame, want.size, want.bytes[0],
                      want.frame);
            return false;
        }
    }
    return true;
}

// One render's worth of checks: it ran the plugin once, inside render, for the whole block; the plugin got the
// expected events and the host got them back from that render.
bool renderAndCheck(const char* const block)
{
    const uint32_t runsBefore = gRuns;
    if (render() != noErr)
    {
        d_stderr2("%s: render failed", block);
        return false;
    }
    if (gRuns != runsBefore + 1 || gRunsOutsideRender != 0 || gLastRunFrames != kBlock)
    {
        d_stderr2("%s: %u runs in render, %u outside it, last for %u frames", block, gRuns - runsBefore,
                  gRunsOutsideRender, gLastRunFrames);
        return false;
    }
    if (gEchoesRefused != 0 || gUnreadablePackets != 0 || gOutputCallsOutsideRender != 0
        || gOutputCallsWithoutTheRenderTimeStamp != 0 || gUncleared != 0)
    {
        d_stderr2("%s: %u echoes refused, %u unreadable packets, %u output calls outside render, %u without "
                  "the render's timestamp, %u events with bytes past their length", block, gEchoesRefused,
                  gUnreadablePackets, gOutputCallsOutsideRender, gOutputCallsWithoutTheRenderTimeStamp, gUncleared);
        return false;
    }
    return same(gReceived, gReceivedCount, "run() received", block)
        && same(gSent, gSentCount, "the host was sent", block);
}

// The length of the message each status byte starts, as the MIDI 1.0 specification gives it, written out rather than
// worked out the way the wrapper does; 0 where MusicDeviceMIDIEvent carries no message: a data byte, sysex (F0, and
// its end F7, which MusicDeviceSysEx carries) and the undefined F4 and F5.
uint32_t specifiedLength(const uint32_t status)
{
    static const uint8_t kSystem[16] = {
        0, // F0 start of sysex
        2, // F1 MIDI time code quarter frame
        3, // F2 song position pointer
        2, // F3 song select
        0, // F4 undefined
        0, // F5 undefined
        1, // F6 tune request
        0, // F7 end of sysex
        1, // F8 timing clock
        1, // F9 undefined (real time)
        1, // FA start
        1, // FB continue
        1, // FC stop
        1, // FD undefined (real time)
        1, // FE active sensing
        1, // FF system reset
    };
    if (status < 0x80)
        return 0;
    if (status >= 0xF0)
        return kSystem[status - 0xF0];
    return status >= 0xC0 && status <= 0xDF ? 2 : 3;
}

// Makes the calling thread a time-constraint thread, as CoreAudio renders on: a 1 ms period.
bool makeTimeConstraint()
{
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    const double ticksPerMilli = 1000000.0 * timebase.denom / timebase.numer;
    thread_time_constraint_policy_data_t policy;
    policy.period = static_cast<uint32_t>(ticksPerMilli);
    policy.computation = static_cast<uint32_t>(ticksPerMilli / 4);
    policy.constraint = static_cast<uint32_t>(ticksPerMilli);
    policy.preemptible = 1;
    return thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                             reinterpret_cast<thread_policy_t>(&policy), THREAD_TIME_CONSTRAINT_POLICY_COUNT)
        == KERN_SUCCESS;
}

// A MIDI call on a time-constraint thread into a queue of the wrapper's own type, with its thread policy and limit,
// while another time-constraint thread holds the queue's guard for holdMicros more once the call waits for it:
// whether the call queued its message, and how long it took. Then render takes the inbox, a new cycle.
struct HeldCall
{
    bool added;
    double micros;
};

HeldCall callWhileHeld(MidiInputQueue& queue, const double holdMicros)
{
    typedef AUMidiInputQueueTestAccess<MidiInputQueue> Access;
    std::atomic<bool> held(false);
    HeldCall call = {false, 0.0};
    std::thread holder([&] {
        makeTimeConstraint();
        Access::hold(queue);
        held.store(true);
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now()
                                                             + std::chrono::seconds(1);
        while (! Access::realTimeWaits(queue) && std::chrono::steady_clock::now() < deadline) {}
        const auto letGo = std::chrono::steady_clock::now() + std::chrono::duration<double, std::micro>(holdMicros);
        while (std::chrono::steady_clock::now() < letGo) {}
        Access::letGo(queue);
    });
    std::thread caller([&] {
        makeTimeConstraint();
        while (! held.load())
            std::this_thread::yield();
        const uint8_t note[3] = {0x90, 60, 100};
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        call.added = queue.add(0, note, 3);
        call.micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
    });
    holder.join();
    caller.join();
    if (MidiInputQueue::Set* const set = queue.take())
        queue.release(set);
    return call;
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU MIDI Offsets Test"), 0x10000,
                                                      factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &gUnit), noErr, "test AU must instantiate");
    UInt32 maxFrames = kBlock;
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                          &maxFrames, sizeof(maxFrames)), noErr, "the block size must set");
    AUMIDIOutputCallbackStruct output = {midiOutput, nullptr};
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0,
                                          &output, sizeof(output)), noErr, "the MIDI output callback must set");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(gUnit), noErr, "the unit must initialize");

    // Every offset of a block: a note-on and a controller there, and the note-off one frame later.
    bool everyOffset = true;
    for (UInt32 offset = 0; offset < kBlock && everyOffset; ++offset)
    {
        clear();
        const uint32_t runsBefore = gRuns;
        const uint8_t note = static_cast<uint8_t>(offset & 0x7F);
        everyOffset = send(0x90, note, 100, offset) == noErr && send(0xB0, 1, note, offset) == noErr
            && (offset + 1 == kBlock || send(0x80, note, 0, offset + 1) == noErr);
        if (everyOffset && gRuns != runsBefore)
        {
            d_stderr2("offset %u: MusicDeviceMIDIEvent ran the plugin", offset);
            everyOffset = false;
        }
        char block[32];
        std::snprintf(block, sizeof(block), "offset %u", offset);
        everyOffset = everyOffset && renderAndCheck(block);
    }
    DAF_ASSERT_EQUAL(everyOffset, true,
                     "every offset's events must run in their render at their frames, and go back out");

    // A full queue in one block: kMaxMidiEvents events, two on each frame. One more is refused, with an error.
    clear();
    bool fullQueue = true;
    for (uint32_t i = 0; i < kMaxMidiEvents && fullQueue; ++i)
        fullQueue = send(0x90 | (i & 0x0F), static_cast<uint8_t>(i & 0x7F), static_cast<uint8_t>(1 + i % 127),
                         static_cast<UInt32>(i * kBlock / kMaxMidiEvents)) == noErr;
    DAF_ASSERT_EQUAL(MusicDeviceMIDIEvent(gUnit, 0x90, 1, 1, kBlock - 1), kAudioUnitErr_CannotDoInCurrentContext,
                     "an event a full queue has no room for must be refused, as one there is room for later");
    DAF_ASSERT_EQUAL(fullQueue && renderAndCheck("a full queue"), true,
                     "a block of kMaxMidiEvents events must run and go back out whole, in order");

    // Every status byte, with both data bytes set, each at a frame of its own. Two blocks of note-ons first leave three
    // bytes in the slots of both of the queue's sets, so that a shorter message not cleared past its length shows.
    for (int i = 0; i < 2; ++i)
    {
        clear();
        bool filled = true;
        for (uint32_t frame = 0; frame < kBlock && filled; ++frame)
            filled = send(0x90, static_cast<uint8_t>(frame & 0x7F), 0x7F, frame) == noErr;
        DAF_ASSERT_EQUAL(filled && renderAndCheck("note-ons in every slot"), true, "a block of note-ons must run");
    }
    clear();
    bool everyStatus = true;
    for (uint32_t status = 0; status < 256 && everyStatus; ++status)
    {
        const uint8_t bytes[3] = {static_cast<uint8_t>(status), 0x12, 0x34};
        const uint32_t length = specifiedLength(status);
        const OSStatus result = MusicDeviceMIDIEvent(gUnit, status, bytes[1], bytes[2], status);
        if ((result == noErr) != (length != 0))
        {
            d_stderr2("status %02X: MusicDeviceMIDIEvent returned %d for a message of %u bytes", status,
                      static_cast<int>(result), length);
            everyStatus = false;
        }
        if (length != 0)
            record(gExpected, gExpectedCount, status, bytes, length);
    }
    DAF_ASSERT_EQUAL(everyStatus && renderAndCheck("every status byte"), true,
                     "every status byte must arrive as a message of its length, or be refused when it starts none");

    // Blocks without events still run, each for its own frames: the plugin's clock is the host's.
    for (int i = 0; i < 4; ++i)
    {
        clear();
        DAF_ASSERT_EQUAL(renderAndCheck("an empty block"), true, "an empty block must run once, for its frames");
    }

    // A sysex follows the last event sent before it, at its frame. Two blocks with events at frames 0, 20 and 200 leave
    // frame 200 in the third slot of both of the queue's sets; then note-ons at 10 and 30 and a sysex, which must take
    // frame 30: not the first event's 10, nor the slot's old 200, nor the block's start.
    for (int i = 0; i < 2; ++i)
    {
        clear();
        DAF_ASSERT_EQUAL(send(0x90, 60, 100, 0) == noErr && send(0x90, 61, 100, 20) == noErr
                         && send(0x80, 60, 0, 200) == noErr && renderAndCheck("frames 0, 20 and 200"),
                         true, "a block with events at frames 0, 20 and 200 must run");
    }
    static const uint8_t shortSysEx[] = {0xF0, 0x7D, 0x01, 0x02, 0x03, 0x04, 0x05, 0xF7};
    clear();
    DAF_ASSERT_EQUAL(send(0x90, 62, 100, 10) == noErr && send(0x90, 63, 100, 30) == noErr
                     && sendSysEx(shortSysEx, sizeof(shortSysEx), 30) == noErr
                     && renderAndCheck("a sysex after two note-ons"), true,
                     "a sysex must keep the frame of the last event sent before it");

    // A render without frames runs nothing it was sent: a plugin returns early on no frames and would drop them. They
    // wait for the next render, at their frames.
    clear();
    const uint32_t runsBefore = gRuns;
    DAF_ASSERT_EQUAL(send(0x90, 65, 100, 0) == noErr && send(0x80, 65, 0, 5) == noErr && render(0) == noErr, true,
                     "a render without frames must succeed");
    DAF_ASSERT_EQUAL(gRuns == runsBefore + 1 && gLastRunFrames == 0 && gReceivedCount == 0 && gSentCount == 0, true,
                     "a render without frames must run the plugin with no events");
    DAF_ASSERT_EQUAL(renderAndCheck("after a render without frames"), true,
                     "the events sent before a render without frames must reach the next render, at their frames");

    // A sysex that opens a block runs at its start, ahead of the events sent after it; a long one (an odd length, so
    // that a misplaced packet after it shows) goes back out whole.
    static uint8_t longSysEx[301];
    longSysEx[0] = 0xF0;
    for (size_t i = 1; i + 1 < sizeof(longSysEx); ++i)
        longSysEx[i] = static_cast<uint8_t>(i & 0x7F);
    longSysEx[sizeof(longSysEx) - 1] = 0xF7;
    clear();
    DAF_ASSERT_EQUAL(sendSysEx(longSysEx, sizeof(longSysEx), 0) == noErr && send(0x90, 64, 90, 5) == noErr
                     && renderAndCheck("a sysex first"), true,
                     "a sysex that opens a block must run at its start, and go back out whole");

    // Everything one render takes in goes back out, and no echo is refused: kMaxMidiEvents messages carrying
    // kMidiInputSysExBytes of sysex, once as one sysex of all of it and once as sysex of 256 bytes, the rest notes.
    static uint8_t bigSysEx[kMidiInputSysExBytes];
    for (const uint32_t sysExSize : {kMidiInputSysExBytes, 256u})
    {
        clear();
        bool fullSet = true;
        uint32_t messages = 0;
        for (uint32_t used = 0; used < kMidiInputSysExBytes && fullSet; used += sysExSize, ++messages)
        {
            bigSysEx[0] = 0xF0;
            for (uint32_t i = 1; i + 1 < sysExSize; ++i)
                bigSysEx[i] = static_cast<uint8_t>((i * 5 + messages) & 0x7F);
            bigSysEx[sysExSize - 1] = 0xF7;
            fullSet = sendSysEx(bigSysEx, sysExSize, 0) == noErr;
        }
        for (; messages < kMaxMidiEvents && fullSet; ++messages)
            fullSet = send(0x90 | (messages & 0x0F), static_cast<uint8_t>(messages & 0x7F), 1 + messages % 127,
                           messages * kBlock / kMaxMidiEvents) == noErr;
        char block[64];
        std::snprintf(block, sizeof(block), "a full input set with %u-byte sysex", sysExSize);
        DAF_ASSERT_EQUAL(fullSet && renderAndCheck(block), true,
                         "all that one render takes in must go back out, none of it refused");
    }

    // The wrapper's own answer to whether a MIDI call is on a real-time thread: yes on a time-constraint thread, as
    // CoreAudio renders on, and no on any other; asked twice, as the second answer is the one each thread kept.
    bool policySet = false, onTimeConstraint = false, onOrdinary = true;
    std::thread timeConstraint([&] {
        policySet = makeTimeConstraint();
        onTimeConstraint = AUMidiCallThreads::isRealTime() && AUMidiCallThreads::isRealTime();
    });
    timeConstraint.join();
    std::thread ordinary([&] { onOrdinary = AUMidiCallThreads::isRealTime() || AUMidiCallThreads::isRealTime(); });
    ordinary.join();
    DAF_ASSERT_EQUAL(policySet && onTimeConstraint, true, "a time-constraint thread must count as real-time");
    DAF_ASSERT_EQUAL(onOrdinary || AUMidiCallThreads::isRealTime() || AUMidiCallThreads::isRealTime(), false,
                     "an ordinary thread must not");

    // And the wrapper's own limit for such a call, through a queue of its own type: a guard held 15 us past the moment
    // a call on a time-constraint thread starts waiting is waited out, and the message queued; one held 2 ms is given
    // up on within the limit, and the message dropped. Of three tries at the short hold one is enough, as the
    // scheduler can stretch a hold; with no wait at all, none would queue.
    static MidiInputQueue queue;
    HeldCall brief[3];
    bool waitedOut = false;
    for (HeldCall& call : brief)
    {
        call = callWhileHeld(queue, 15.0);
        waitedOut = waitedOut || call.added;
    }
    const HeldCall held = callWhileHeld(queue, 2000.0);
    d_stdout("a time-constraint thread's call: a guard held 15 us %s (%.1f, %.1f, %.1f us), one held 2 ms %s after "
             "%.1f us", waitedOut ? "waited out" : "NOT waited out", brief[0].micros, brief[1].micros, brief[2].micros,
             held.added ? "NOT given up on" : "given up on", held.micros);
    DAF_ASSERT_EQUAL(waitedOut, true, "a call on a time-constraint thread must wait out a guard held briefly");
    DAF_ASSERT_EQUAL(! held.added && held.micros < 1000.0, true,
                     "a call on a time-constraint thread must give up on a guard held long, within its limit");

    d_stdout("%u offsets, a full queue of %u, empty blocks, sysex order and a full input set sent back: every event "
             "ran in its render at its frame", kBlock, kMaxMidiEvents);
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(gUnit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(gUnit), noErr, "test AU must dispose");
    return 0;
}
