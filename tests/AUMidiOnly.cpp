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


// A plugin without audio against reconfiguration in the AU wrapper, with main() as the host. Such a plugin once ran
// from its MIDI input (auMIDIEvent), outside render, where render's guard did not keep it out of a reset on another
// thread: run() started inside the plugin's deactivate() and activate(). It now runs from render, as every plugin
// does (see AUMidiOffsets), and render's guard covers it. The host's audio thread sends MIDI and renders the unit,
// with no buffers, without pause while the main thread resets it 500 times; MusicDeviceMIDIEvent must never run the
// plugin, no run() may start inside a reset, the plugin may be activated on the main thread only, and it must keep
// running. A block that met a reconfiguration and was rendered with no buffer list at all, which DoNotCheckRenderArgs
// lets through to a unit without audio and which a block that runs takes, read through the null list and crashed. Such
// a block, rendered from inside a sample rate change, must succeed with nothing to fill: the host's render
// notifications hear of it, its post-render marked silent, and a note-on sent before it opens the next block that runs.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <AudioToolbox/MusicDevice.h>
#include <atomic>
#include <chrono>
#include <pthread.h>
#include <thread>

START_NAMESPACE_DAF

static std::atomic<bool> gReconfiguring(false);
static std::atomic<int> gOverlaps(0), gRuns(0), gReconfigurations(0), gActivatedOffMain(0), gRunsInsideMidiCall(0),
    gEvents(0);
// Set by the audio thread around its MusicDeviceMIDIEvent calls.
static thread_local bool tInsideMidiCall = false;
// Set by main(): deactivate() calls it once, from inside the reconfiguration it is part of.
static void (*gInsideReconfiguration)() = nullptr;
// What the last run() was given: how many events, and the first one's status byte and frame.
static uint32_t gLastCount = 0, gLastFrame = 0;
static uint8_t gLastStatus = 0;

// Holds the reconfiguration open for a while, so a run() that slips into it is caught.
static void dwell()
{
    for (volatile int i = 0; i < 20000; i = i + 1) {}
}

class MidiOnlyPlugin : public Plugin
{
public:
    MidiOnlyPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "MidiOnlyTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void deactivate() override
    {
        gReconfiguring.store(true);
        dwell();
        void (*const hook)() = gInsideReconfiguration;
        gInsideReconfiguration = nullptr;
        if (hook != nullptr)
            hook();
    }

    void activate() override
    {
        if (pthread_main_np() == 0)
            gActivatedOffMain.fetch_add(1);
        dwell();
        gReconfiguring.store(false);
        gReconfigurations.fetch_add(1);
    }

    void run(const float**, float**, uint32_t, const MidiEvent* const events, const uint32_t count) override
    {
        if (gReconfiguring.load())
            gOverlaps.fetch_add(1);
        if (tInsideMidiCall)
            gRunsInsideMidiCall.fetch_add(1);
        gLastCount = count;
        if (count != 0)
        {
            gLastStatus = events[0].data[0];
            gLastFrame = events[0].frame;
        }
        gRuns.fetch_add(1);
        gEvents.fetch_add(static_cast<int>(count));
    }
};

Plugin* createPlugin()
{
    return new MidiOnlyPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
AudioUnit gUnit = nullptr;

// A render with no buffer list at all, which DoNotCheckRenderArgs lets through to a unit without audio.
OSStatus renderWithoutBufferList()
{
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = kAudioUnitRenderAction_DoNotCheckRenderArgs;
    AudioBufferList* none = nullptr; // a variable, as the header marks the list non-null
    return AudioUnitRender(gUnit, &flags, &timestamp, 0, 64, none);
}

// The host's render notifications.
std::atomic<int> gPreRenders(0), gSilentPostRenders(0);
OSStatus countRenders(void*, AudioUnitRenderActionFlags* const flags, const AudioTimeStamp*, UInt32, UInt32,
                      AudioBufferList*)
{
    if ((*flags & kAudioUnitRenderAction_PreRender) != 0)
        gPreRenders.fetch_add(1);
    if ((*flags & kAudioUnitRenderAction_PostRender) != 0 && (*flags & kAudioUnitRenderAction_OutputIsSilence) != 0)
        gSilentPostRenders.fetch_add(1);
    return noErr;
}

// What the block rendered inside the reconfiguration returned, ran and told the render notifications.
OSStatus gSilentStatus = -1;
int gRunsInSilentBlock = -1, gPreRendersInSilentBlock = -1, gSilentPostRendersInSilentBlock = -1;

// The host's audio thread during the reconfiguration: a note-on at frame 60, then the block, with no buffer list.
void silentBlockWithoutBufferList()
{
    std::thread([] {
        const int runs = gRuns.load(), preRenders = gPreRenders.load(), silentPostRenders = gSilentPostRenders.load();
        MusicDeviceMIDIEvent(gUnit, 0x90, 60, 100, 60);
        gSilentStatus = renderWithoutBufferList();
        gRunsInSilentBlock = gRuns.load() - runs;
        gPreRendersInSilentBlock = gPreRenders.load() - preRenders;
        gSilentPostRendersInSilentBlock = gSilentPostRenders.load() - silentPostRenders;
    }).join();
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU MIDI-only Test"), 0x10000, factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    AudioUnit unit = nullptr;
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &unit), noErr, "test AU must instantiate");
    UInt32 maxFrames = 64;
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                          &maxFrames, sizeof(maxFrames)), noErr, "the block size must set");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");

    // A block inside a reconfiguration, a new sample rate, rendered with no buffer list; then the next, the same way.
    gUnit = unit;
    DAF_ASSERT_EQUAL(AudioUnitAddRenderNotify(unit, countRenders, nullptr), noErr,
                     "a render notification must register");
    Float64 sampleRate = 0.0;
    UInt32 size = sizeof(sampleRate);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sampleRate,
                                          &size), noErr, "the sample rate must read");
    sampleRate = sampleRate == 48000.0 ? 96000.0 : 48000.0;
    gInsideReconfiguration = silentBlockWithoutBufferList;
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &sampleRate,
                                          sizeof(sampleRate)), noErr, "the sample rate must change");
    DAF_ASSERT_EQUAL(gInsideReconfiguration == nullptr, true, "the sample rate change must reconfigure the plugin");
    DAF_ASSERT_EQUAL(gSilentStatus, noErr, "a block without a buffer list inside the reconfiguration must render");
    DAF_ASSERT_EQUAL(gRunsInSilentBlock, 0, "the block inside the reconfiguration must not run the plugin");
    DAF_ASSERT_EQUAL(gPreRendersInSilentBlock == 1 && gSilentPostRendersInSilentBlock == 1, true,
                     "the render notifications must hear of the block, its post-render marked silent");
    const int runsBefore = gRuns.load();
    DAF_ASSERT_EQUAL(renderWithoutBufferList(), noErr, "the next block without a buffer list must render");
    DAF_ASSERT_EQUAL(gRuns.load() == runsBefore + 1 && gLastCount == 1, true,
                     "the next block must run the plugin once, with one event");
    DAF_ASSERT_EQUAL((gLastStatus & 0xF0) == 0x90 && gLastFrame == 0, true,
                     "the note-on sent before the silent block must open the next block that runs");
    DAF_ASSERT_EQUAL(AudioUnitRemoveRenderNotify(unit, countRenders, nullptr), noErr,
                     "the render notification must unregister");

    // The host's audio thread: each cycle, the cycle's MIDI and then the render, which has no buffers to fill.
    std::atomic<bool> stop(false);
    std::atomic<int> renderErrors(0);
    std::thread audio([unit, &stop, &renderErrors] {
        AudioBufferList none = {0, {{0, 0, nullptr}}};
        AudioTimeStamp timestamp = {};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        while (! stop.load())
        {
            tInsideMidiCall = true;
            MusicDeviceMIDIEvent(unit, 0xB0, 1, 64, 0);
            MusicDeviceMIDIEvent(unit, 0xB0, 1, 65, 63);
            tInsideMidiCall = false;
            AudioUnitRenderActionFlags flags = 0;
            if (AudioUnitRender(unit, &flags, &timestamp, 0, 64, &none) != noErr)
                renderErrors.fetch_add(1);
            timestamp.mSampleTime += 64;
        }
    });
    // Whether the plugin runs another `count` times within a generous deadline.
    const auto runsAgain = [](const int count) {
        const int target = gRuns.load() + count;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (gRuns.load() < target && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return gRuns.load() >= target;
    };
    const bool ranBefore = runsAgain(100);
    int resetErrors = 0;
    for (int i = 0; i < 500; ++i)
        resetErrors += AudioUnitReset(unit, kAudioUnitScope_Global, 0) != noErr ? 1 : 0;
    const bool ranAfter = runsAgain(100);
    stop.store(true);
    audio.join();

    d_stdout("%d runs with %d events, %d of them inside a MIDI call, %d resets, %d runs inside one, %d activations off "
             "the main thread", gRuns.load(), gEvents.load(), gRunsInsideMidiCall.load(), gReconfigurations.load(),
             gOverlaps.load(), gActivatedOffMain.load());
    DAF_ASSERT_EQUAL(resetErrors, 0, "every reset must succeed while the unit renders");
    DAF_ASSERT_EQUAL(renderErrors.load(), 0, "every render must succeed while the unit is reset");
    DAF_ASSERT_EQUAL(gReconfigurations.load() >= 500, true, "every reset must reconfigure the plugin");
    DAF_ASSERT_EQUAL(gRunsInsideMidiCall.load(), 0, "MusicDeviceMIDIEvent must not run the plugin");
    DAF_ASSERT_EQUAL(ranBefore, true, "the plugin must run from the host's renders");
    DAF_ASSERT_EQUAL(ranAfter, true, "the plugin must keep running from the host's renders after the resets");
    DAF_ASSERT_EQUAL(gEvents.load() > 100, true, "the plugin must receive its MIDI");
    DAF_ASSERT_EQUAL(gOverlaps.load(), 0, "no run() may start inside a reset");
    DAF_ASSERT_EQUAL(gActivatedOffMain.load(), 0, "the plugin may be activated by the resetting thread only");

    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(unit), noErr, "test AU must dispose");
    return 0;
}
