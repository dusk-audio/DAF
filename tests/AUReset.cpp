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

/* Plugin::reset() through the AU wrapper compiled into this test, with main() as the host. The plugin (plugin-reset/,
 * DAF_PLUGIN_WANT_RESET) clears its state on the thread the host resets the unit from, while render leaves it alone.
 * AudioUnitReset used to deactivate and activate the plugin instead, which clears nothing that a plugin's activate()
 * keeps, and this one keeps its tail.
 *
 * - A unit that is not initialized has no active plugin, and its reset does not reach Plugin::reset().
 * - AudioUnitReset calls Plugin::reset() once, while the plugin is active and outside run(). The plugin is neither
 *   deactivated nor activated, the next block is silent, and MIDI queued before the reset is dropped, as it was.
 * - With a reset designation as well, AudioUnitReset still calls Plugin::reset(), and leaves the parameter alone,
 *   which the host can still set.
 * - 200 resets while another thread renders, each made while a block is in flight and lasting longer than render's
 *   pause between blocks: every one calls Plugin::reset(), none while run() runs, no run() starts inside one, and the
 *   blocks that start during one complete without running the plugin (AUIoRender checks that such a block is
 *   silent).
 *
 * AUWrapper checks that a plugin without DAF_PLUGIN_WANT_RESET or a reset designation is still deactivated and
 * activated. */

#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL

#include "tests.hpp"
#include "plugin-reset/ResetTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

#include <AudioToolbox/MusicDevice.h>
#include <chrono>
#include <csignal>
#include <thread>
#include <unistd.h>

USE_NAMESPACE_DAF

namespace
{
// The first sample of the next block the host feeds the unit; the rest is silence.
float gImpulse = 0.0f;

OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
    {
        float* const samples = static_cast<float*>(data->mBuffers[ch].mData);
        std::memset(samples, 0, sizeof(float) * frames);
        samples[0] = gImpulse;
    }
    gImpulse = 0.0f;
    return noErr;
}

AudioUnit create(AudioComponent component)
{
    AudioUnit unit = nullptr;
    if (AudioComponentInstanceNew(component, &unit) != noErr)
        return nullptr;
    AURenderCallbackStruct callback = {feed, nullptr};
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                             &callback, sizeof(callback)) != noErr)
    {
        AudioComponentInstanceDispose(unit);
        return nullptr;
    }
    return unit;
}

OSStatus render(AudioUnit unit, float* const last = nullptr)
{
    float samples[64] = {};
    AudioBufferList output = {1, {{1, sizeof(samples), samples}}};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    const OSStatus status = AudioUnitRender(unit, &flags, &timestamp, 0, 64, &output);
    if (last != nullptr)
        *last = static_cast<const float*>(output.mBuffers[0].mData)[63];
    return status;
}

// Renders a block whose first input sample is `impulse` and returns its last output sample, what the plugin holds
// after it; -1 if the block failed.
float play(AudioUnit unit, const float impulse)
{
    gImpulse = impulse;
    float last = -1.0f;
    return render(unit, &last) == noErr ? last : -1.0f;
}

OSStatus reset(AudioUnit unit)
{
    return AudioUnitReset(unit, kAudioUnitScope_Global, 0);
}

void hung(int)
{
    const char message[] = "AUReset: no progress in 30 s (render stopped running the plugin, or a reset waits)\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(1);
}
}

int main()
{
    std::signal(SIGALRM, hung);
    alarm(30);

    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU Reset Test"), 0x10000, factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");

    AudioUnit unit = create(component);
    DAF_ASSERT_NOT_EQUAL(unit, nullptr, "test AU must instantiate with its input connected");

    DAF_ASSERT_EQUAL(reset(unit), noErr, "a reset of a unit that is not initialized must succeed");
    DAF_ASSERT_EQUAL(gResets.load(), 0u, "a reset of a unit that is not initialized must not reach Plugin::reset()");

    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    DAF_ASSERT_EQUAL(play(unit, 0.25f), 0.25f, "an impulse must start the tail");
    DAF_ASSERT_EQUAL(play(unit, 0.0f), 0.25f, "the tail must ring on in silence");

    // the plugin clears its own tail, and is neither deactivated nor activated
    {
        const uint32_t activations = gActivations.load(), deactivations = gDeactivations.load();
        DAF_ASSERT_EQUAL(reset(unit), noErr, "AudioUnitReset must succeed");
        DAF_ASSERT_EQUAL(gResets.load(), 1u, "AudioUnitReset must call Plugin::reset() once");
        DAF_ASSERT_EQUAL(gResetsWhileActive.load(), 1u, "Plugin::reset() must be called while the plugin is active");
        DAF_ASSERT_EQUAL(gResetsOutsideRun.load(), 1u, "Plugin::reset() must be called outside run()");
        DAF_ASSERT_EQUAL(gActivations.load() == activations && gDeactivations.load() == deactivations, true,
                         "AudioUnitReset must neither deactivate nor activate the plugin");
        DAF_ASSERT_EQUAL(play(unit, 0.0f), 0.0f, "the block after AudioUnitReset must be silent");
        DAF_ASSERT_EQUAL(play(unit, 0.5f), 0.5f, "the plugin must play on after AudioUnitReset");
    }

    // MIDI queued before a reset does not reach the block after it, as without Plugin::reset()
    {
        const uint32_t events = gMidiEvents.load();
        DAF_ASSERT_EQUAL(MusicDeviceMIDIEvent(unit, 0x90, 60, 100, 0), noErr, "a note-on must queue");
        DAF_ASSERT_EQUAL(render(unit), noErr, "a block with a note-on must render");
        DAF_ASSERT_EQUAL(gMidiEvents.load(), events + 1, "a queued note-on must reach the next block");
        DAF_ASSERT_EQUAL(MusicDeviceMIDIEvent(unit, 0x80, 60, 0, 0), noErr, "a note-off must queue");
        DAF_ASSERT_EQUAL(reset(unit), noErr, "AudioUnitReset with MIDI queued must succeed");
        DAF_ASSERT_EQUAL(render(unit), noErr, "the block after AudioUnitReset must render");
        DAF_ASSERT_EQUAL(gMidiEvents.load(), events + 1, "MIDI queued before AudioUnitReset must be dropped");
    }

    // a reset designation as well: Plugin::reset() still clears the tail, and the parameter is left alone
    {
        gResetTestDesignateParameter = true;
        AudioUnit designated = create(component);
        gResetTestDesignateParameter = false;
        DAF_ASSERT_NOT_EQUAL(designated, nullptr, "a unit with a reset designation must instantiate");
        DAF_ASSERT_EQUAL(AudioUnitInitialize(designated), noErr, "a unit with a reset designation must initialize");
        DAF_ASSERT_EQUAL(play(designated, 0.25f), 0.25f, "an impulse must start the tail");

        const uint32_t resets = gResets.load(), presses = gParameterPresses.load();
        const uint32_t activations = gActivations.load(), deactivations = gDeactivations.load();
        DAF_ASSERT_EQUAL(reset(designated), noErr, "AudioUnitReset with a reset designation must succeed");
        DAF_ASSERT_EQUAL(gResets.load(), resets + 1,
                         "with a reset designation, AudioUnitReset must call Plugin::reset()");
        DAF_ASSERT_EQUAL(gParameterPresses.load(), presses, "AudioUnitReset must leave a reset designation alone");
        DAF_ASSERT_EQUAL(gActivations.load() == activations && gDeactivations.load() == deactivations, true,
                         "with a reset designation, AudioUnitReset must neither deactivate nor activate the plugin");
        DAF_ASSERT_EQUAL(play(designated, 0.0f), 0.0f,
                         "with a reset designation, the block after AudioUnitReset must be silent");

        const uint32_t pressesBeforeSet = gParameterPresses.load();
        DAF_ASSERT_EQUAL(AudioUnitSetParameter(designated, 0, kAudioUnitScope_Global, 0, 1.0f, 0), noErr,
                         "the host must be able to set the reset designation");
        DAF_ASSERT_EQUAL(gParameterPresses.load(), pressesBeforeSet + 1,
                         "the reset designation must still reach the plugin when the host sets it");

        DAF_ASSERT_EQUAL(AudioUnitUninitialize(designated), noErr, "a unit with a reset designation must uninitialize");
        DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(designated), noErr, "a unit with a reset designation must dispose");
    }

    // Resets while another thread renders, both ways round: each reset is made while a block is in flight, which it
    // has to wait out, and lasts longer than render's pause between blocks, so that blocks start during it and have
    // to play silence. The pause is what a host's audio thread leaves between periods. Rendering back to back left a
    // waiting reset only the instants between two blocks, and copies of this test run at once under load starved
    // until the watchdog ended them. run() and reset() dwell, so that an overlap either way would be caught.
    {
        constexpr uint32_t kResets = 200;
        constexpr uint32_t kRenderPause = 1000;  // microseconds between two blocks
        gResetTestRunDwell = 200;                // a block in flight for a reset to meet
        gResetTestResetDwell = 3 * kRenderPause; // longer than the pause: blocks start during the reset

        const uint32_t resets = gResets.load(), outsideRun = gResetsOutsideRun.load(), runs = gRuns.load();
        std::atomic<bool> stop(false);
        std::atomic<int> renderErrors(0);
        std::atomic<uint32_t> silentBlocks(0);   // blocks that met a reset: rendered, and did not run the plugin
        std::thread renderer([&] {
            while (! stop.load())
            {
                const uint32_t before = gRuns.load();
                if (render(unit) != noErr)
                    renderErrors.fetch_add(1);
                else if (gRuns.load() == before)
                    silentBlocks.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::microseconds(kRenderPause));
            }
        });
        int resetErrors = 0;
        for (uint32_t i = 0; i < kResets && renderErrors.load() == 0; ++i)
        {
            while (! gResetTestInRun.load() && renderErrors.load() == 0)
                std::this_thread::yield();
            resetErrors += reset(unit) != noErr ? 1 : 0;
        }
        stop.store(true);
        renderer.join();
        gResetTestRunDwell = 0;
        gResetTestResetDwell = 0;

        const uint32_t made = gResets.load() - resets;
        d_stdout("%u resets while rendering, %u runs, %u blocks that met a reset, %u resets inside a run, "
                 "%u runs inside a reset", made, gRuns.load() - runs, silentBlocks.load(),
                 made - (gResetsOutsideRun.load() - outsideRun), gRunsInsideReset.load());
        DAF_ASSERT_EQUAL(renderErrors.load(), 0, "render must not fail while the unit resets");
        DAF_ASSERT_EQUAL(resetErrors, 0, "every reset must succeed while rendering");
        DAF_ASSERT_EQUAL(made, kResets, "every reset while rendering must call Plugin::reset()");
        DAF_ASSERT_EQUAL(gResetsOutsideRun.load() - outsideRun, kResets, "no Plugin::reset() may run inside run()");
        DAF_ASSERT_EQUAL(gRunsInsideReset.load(), 0u, "no run() may start inside Plugin::reset()");
        DAF_ASSERT_EQUAL(silentBlocks.load() != 0, true, "blocks must have started during a reset, and played silence");
    }

    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(unit), noErr, "test AU must dispose");

    d_stdout("AU reset tests passed");
    return 0;
}
