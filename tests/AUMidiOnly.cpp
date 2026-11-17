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
// running.
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
    }

    void activate() override
    {
        if (pthread_main_np() == 0)
            gActivatedOffMain.fetch_add(1);
        dwell();
        gReconfiguring.store(false);
        gReconfigurations.fetch_add(1);
    }

    void run(const float**, float**, uint32_t, const MidiEvent*, const uint32_t count) override
    {
        if (gReconfiguring.load())
            gOverlaps.fetch_add(1);
        if (tInsideMidiCall)
            gRunsInsideMidiCall.fetch_add(1);
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
