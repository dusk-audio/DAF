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


// A plugin without audio against reconfiguration in the AU wrapper, with main() as the host. Such a plugin runs from
// its MIDI input (auMIDIEvent), outside render, so render's guard did not keep it out of a reset on another thread:
// run() started inside the plugin's deactivate() and activate(), and on a plugin left inactive, which the wrapper
// then activated from the MIDI thread. A MIDI thread sends events without pause while the main thread resets the unit
// 500 times; no run() may start inside a reset, the plugin may be activated on the main thread only, and it must keep
// running.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <AudioToolbox/MusicDevice.h>
#include <atomic>
#include <pthread.h>
#include <thread>

START_NAMESPACE_DAF

static std::atomic<bool> gReconfiguring(false);
static std::atomic<int> gOverlaps(0), gRuns(0), gReconfigurations(0), gActivatedOffMain(0);

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

    void run(const float**, float**, uint32_t, const MidiEvent*, uint32_t) override
    {
        if (gReconfiguring.load())
            gOverlaps.fetch_add(1);
        gRuns.fetch_add(1);
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
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");

    std::atomic<bool> stop(false);
    std::thread midi([unit, &stop] {
        while (! stop.load())
            MusicDeviceMIDIEvent(unit, 0xB0, 1, 64, 0);
    });
    int resetErrors = 0;
    for (int i = 0; i < 500; ++i)
        resetErrors += AudioUnitReset(unit, kAudioUnitScope_Global, 0) != noErr ? 1 : 0;
    stop.store(true);
    midi.join();

    d_stdout("%d runs from MIDI, %d resets, %d runs inside one, %d activations off the main thread", gRuns.load(),
             gReconfigurations.load(), gOverlaps.load(), gActivatedOffMain.load());
    DAF_ASSERT_EQUAL(resetErrors, 0, "every reset must succeed while MIDI arrives");
    DAF_ASSERT_EQUAL(gReconfigurations.load() >= 500, true, "every reset must reconfigure the plugin");
    DAF_ASSERT_EQUAL(gRuns.load() > 100, true, "the plugin must keep running from its MIDI input");
    DAF_ASSERT_EQUAL(gOverlaps.load(), 0, "no run() may start inside a reset");
    DAF_ASSERT_EQUAL(gActivatedOffMain.load(), 0, "the plugin may be activated by the resetting thread only");

    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(unit), noErr, "test AU must dispose");
    return 0;
}
