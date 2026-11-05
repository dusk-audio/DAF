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


// MIDI events against a block that plays silence in the AU wrapper (a reconfiguration holds the unit), with main()
// as the host. The block's events are neither run nor dropped: they wait for the next block that runs, which handed
// them to run() with the silent block's frames, ahead of its own. A note-on at frame 60 of the silent block and a
// note-off at frame 10 of the next arrived as [on@60, off@10]: the note-off came first in time, and the note hung.
// They now open the next block: [on@0, off@10]. MIDI that a host sends while the unit renders, from its pre-render
// notification or from its input callback (sample-accurate scheduling), is the running block's own and keeps its
// frame; so does MIDI sent from a silent block's post-render notification, which is already the next block's.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <AudioToolbox/MusicDevice.h>
#include <thread>

START_NAMESPACE_DAF

static uint32_t gFrames[16];
static uint8_t gStatus[16];
static int gRunOf[16];
static uint32_t gCount = 0;
static int gRuns = 0;

// Set by main(): deactivate() calls it once, from inside the reconfiguration it is part of.
static void (*gInsideReconfiguration)() = nullptr;

class MidiPlugin : public Plugin
{
public:
    MidiPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "MidiSilentTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void deactivate() override
    {
        void (*const hook)() = gInsideReconfiguration;
        gInsideReconfiguration = nullptr;
        if (hook != nullptr)
            hook();
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames,
             const MidiEvent* const events, const uint32_t count) override
    {
        std::memcpy(outputs[0], inputs[0], sizeof(float) * frames);
        ++gRuns;
        for (uint32_t i = 0; i < count && gCount < 16; ++i, ++gCount)
        {
            gFrames[gCount] = events[i].frame;
            gStatus[gCount] = events[i].data[0];
            gRunOf[gCount] = gRuns;
        }
    }
};

Plugin* createPlugin()
{
    return new MidiPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
AudioUnit gUnit = nullptr;
OSStatus gSilentStatus = -1;
int gRunsInSilentBlock = -1;

bool gSendFromInput = false, gSendFromPreRender = false, gSendFromSilentPostRender = false;

OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
        std::memset(data->mBuffers[ch].mData, 0, sizeof(float) * frames);
    if (gSendFromInput)
    {
        gSendFromInput = false;
        MusicDeviceMIDIEvent(gUnit, 0x90, 64, 100, 40);
    }
    return noErr;
}

OSStatus preRender(void*, AudioUnitRenderActionFlags* const flags, const AudioTimeStamp*, UInt32, UInt32,
                   AudioBufferList*)
{
    if ((*flags & kAudioUnitRenderAction_PreRender) != 0 && gSendFromPreRender)
    {
        gSendFromPreRender = false;
        MusicDeviceMIDIEvent(gUnit, 0x90, 62, 100, 40);
    }
    // The next block's note, scheduled from a silent block's post-render notification.
    if ((*flags & kAudioUnitRenderAction_PostRender) != 0 && (*flags & kAudioUnitRenderAction_OutputIsSilence) != 0
        && gSendFromSilentPostRender)
    {
        gSendFromSilentPostRender = false;
        MusicDeviceMIDIEvent(gUnit, 0x90, 67, 100, 40);
    }
    return noErr;
}

OSStatus render(AudioUnit unit)
{
    float samples[64] = {};
    AudioBufferList output = {1, {{1, sizeof(samples), samples}}};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    return AudioUnitRender(unit, &flags, &timestamp, 0, 64, &output);
}

// The host's audio thread during a reconfiguration: one block, which plays silence.
void silentBlock()
{
    std::thread([] {
        const int runs = gRuns;
        gSilentStatus = render(gUnit);
        gRunsInSilentBlock = gRuns - runs;
    }).join();
}

// The host's audio thread during the reconfiguration: a note-on at frame 60, then the block, which plays silence.
void silentBlockWithNoteOn()
{
    std::thread([] {
        const int runs = gRuns;
        MusicDeviceMIDIEvent(gUnit, 0x90, 60, 100, 60);
        gSilentStatus = render(gUnit);
        gRunsInSilentBlock = gRuns - runs;
    }).join();
}

OSStatus setSampleRate(AudioUnit unit, const Float64 sampleRate)
{
    // The output first: the plugin changes rate (deactivate(), activate()) once the input matches it.
    OSStatus status = AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0,
                                           &sampleRate, sizeof(sampleRate));
    if (status == noErr)
        status = AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0,
                                      &sampleRate, sizeof(sampleRate));
    return status;
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU MIDI Silent Block Test"), 0x10000,
                                                      factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &gUnit), noErr, "test AU must instantiate");
    AURenderCallbackStruct callback = {feed, nullptr};
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                          &callback, sizeof(callback)), noErr, "the input must connect");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(gUnit), noErr, "the unit must initialize");
    DAF_ASSERT_EQUAL(render(gUnit), noErr, "a first block must render");

    gInsideReconfiguration = silentBlockWithNoteOn;
    Float64 sampleRate = 0.0;
    UInt32 size = sizeof(sampleRate);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(gUnit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0,
                                          &sampleRate, &size), noErr, "the sample rate must read");
    DAF_ASSERT_EQUAL(setSampleRate(gUnit, sampleRate == 48000.0 ? 96000.0 : 48000.0), noErr,
                     "the sample rate must change");
    DAF_ASSERT_EQUAL(gInsideReconfiguration == nullptr, true, "the sample rate change must reconfigure the plugin");
    DAF_ASSERT_EQUAL(gSilentStatus, noErr, "the block inside the reconfiguration must render");
    DAF_ASSERT_EQUAL(gRunsInSilentBlock, 0, "the block inside the reconfiguration must not run the plugin");

    // The next block: a note-off at frame 10.
    const uint32_t before = gCount;
    MusicDeviceMIDIEvent(gUnit, 0x80, 60, 0, 10);
    DAF_ASSERT_EQUAL(render(gUnit), noErr, "the next block must render");
    d_stdout("the next block's run() received:");
    for (uint32_t i = before; i < gCount; ++i)
        d_stdout("  run() %d: %s at frame %u", gRunOf[i], (gStatus[i] & 0xF0) == 0x90 ? "note-on" : "note-off",
                 gFrames[i]);
    DAF_ASSERT_EQUAL(gCount - before, 2u, "the next block must run the silent block's event and its own");
    DAF_ASSERT_EQUAL(gRunOf[before] == gRunOf[before + 1], true, "both events must reach one run()");
    DAF_ASSERT_EQUAL((gStatus[before] & 0xF0) == 0x90 && gFrames[before] == 0, true,
                     "the silent block's note-on must open the next block");
    DAF_ASSERT_EQUAL((gStatus[before + 1] & 0xF0) == 0x80 && gFrames[before + 1] == 10, true,
                     "the next block's note-off must keep its frame, after the note-on");

    // MIDI sent while the unit renders: from its pre-render notification, then from its input callback.
    DAF_ASSERT_EQUAL(AudioUnitAddRenderNotify(gUnit, preRender, nullptr), noErr, "a render notification must register");
    uint32_t sent = gCount;
    gSendFromPreRender = true;
    DAF_ASSERT_EQUAL(render(gUnit), noErr, "a block with MIDI from its pre-render notification must render");
    DAF_ASSERT_EQUAL(gCount - sent == 1 && gFrames[sent] == 40, true,
                     "MIDI sent from the pre-render notification must keep its frame");
    sent = gCount;
    gSendFromInput = true;
    DAF_ASSERT_EQUAL(render(gUnit), noErr, "a block with MIDI from its input callback must render");
    DAF_ASSERT_EQUAL(gCount - sent == 1 && gFrames[sent] == 40, true,
                     "MIDI sent from the input callback must keep its frame");
    // A silent block whose post-render notification schedules the next block's note at frame 40.
    gSendFromSilentPostRender = true;
    gInsideReconfiguration = silentBlock;
    DAF_ASSERT_EQUAL(setSampleRate(gUnit, sampleRate == 48000.0 ? 48000.0 : 96000.0), noErr,
                     "the sample rate must change back");
    DAF_ASSERT_EQUAL(gInsideReconfiguration == nullptr && gSilentStatus == noErr && gRunsInSilentBlock == 0, true,
                     "the second reconfiguration must render a silent block");
    DAF_ASSERT_EQUAL(gSendFromSilentPostRender, false, "the silent block's post-render notification must be told");
    sent = gCount;
    DAF_ASSERT_EQUAL(render(gUnit), noErr, "the block after the silent one must render");
    DAF_ASSERT_EQUAL(gCount - sent == 1 && gFrames[sent] == 40, true,
                     "MIDI sent from a silent block's post-render notification must keep its frame");
    DAF_ASSERT_EQUAL(AudioUnitRemoveRenderNotify(gUnit, preRender, nullptr), noErr,
                     "the render notification must unregister");

    DAF_ASSERT_EQUAL(AudioUnitUninitialize(gUnit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(gUnit), noErr, "test AU must dispose");
    return 0;
}
