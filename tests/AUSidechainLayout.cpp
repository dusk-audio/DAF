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

// The AU wrapper's DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO, with main() as the host. On a mono track Logic sets every input
// element to one channel (the sidechain element too), then the output, initializes, and only then connects both
// elements (traced in Logic 2026-10-26). The sidechain element used to refuse one channel, and Logic abandoned the
// unit: no editor, no audio. The unit must take the whole sequence, report ioChanged(2, 1), and hand the plugin its
// main channel, a silent second main port, the mono sidechain on port 2 and silence on port 3. A stereo sidechain on
// a mono main (3, 1) and a mono one on a stereo main (3, 2) run too; a sidechain never runs wider than declared, a
// mono main with a mono sidechain into two outputs (whose 1 + 1 sums to a stereo main's 2) is refused, render runs
// the layout the plugin was told, and the advertised main layouts stay {2, 2} and {1, 1}.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

START_NAMESPACE_DAF

static uint16_t gIoInputs = 0, gIoOutputs = 0;
static float gSeen[4] = {-1, -1, -1, -1};

class SidechainLayoutPlugin : public Plugin
{
public:
    SidechainLayoutPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "SidechainLayoutTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initAudioPort(const bool input, const uint32_t index, AudioPort& port) override
    {
        Plugin::initAudioPort(input, index, port);
        if (input && index >= 2)
        {
            port.hints |= kAudioPortIsSidechain;
            port.groupId = 1;
        }
        else
        {
            port.groupId = 0;
        }
    }

    void initPortGroup(const uint32_t groupId, PortGroup& group) override
    {
        group.name = group.symbol = groupId == 0 ? "main" : "sidechain";
    }

    void ioChanged(const uint16_t numInputs, const uint16_t numOutputs) override
    {
        gIoInputs = numInputs;
        gIoOutputs = numOutputs;
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        for (int port = 0; port < 4; ++port)
            gSeen[port] = inputs[port][0];
        for (uint16_t ch = 0; ch < gIoOutputs; ++ch)
            for (uint32_t i = 0; i < frames; ++i)
                outputs[ch][i] = inputs[0][i];
    }
};

Plugin* createPlugin()
{
    return new SidechainLayoutPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
// The main bus reads 0.25, the sidechain 0.5 (left) and 0.75 (right).
OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, const UInt32 bus, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
    {
        float* const samples = static_cast<float*>(data->mBuffers[ch].mData);
        const float value = bus == 0 ? 0.25f : (ch == 0 ? 0.5f : 0.75f);
        for (UInt32 i = 0; i < frames; ++i)
            samples[i] = value;
    }
    return noErr;
}

OSStatus connect(AudioUnit unit, const UInt32 bus, const bool on)
{
    AURenderCallbackStruct callback = {};
    if (on)
        callback.inputProc = feed;
    return AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, bus,
                                &callback, sizeof(callback));
}

OSStatus width(AudioUnit unit, const AudioUnitScope scope, const UInt32 element, const UInt32 channels)
{
    AudioStreamBasicDescription format = {};
    UInt32 size = sizeof(format);
    const OSStatus got = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, scope, 0, &format, &size);
    if (got != noErr)
        return got;
    format.mChannelsPerFrame = channels;
    return AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, scope, element, &format, sizeof(format));
}

struct Output
{
    UInt32 numberBuffers;
    AudioBuffer buffers[2];
};

OSStatus render(AudioUnit unit, const UInt32 channels, float (&left)[16], float (&right)[16])
{
    Output output = {channels, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    return AudioUnitRender(unit, &flags, &timestamp, 0, 16, reinterpret_cast<AudioBufferList*>(&output));
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU Sidechain Layout Test"), 0x10000,
                                                      factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    AudioUnit unit = nullptr;
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &unit), noErr, "test AU must instantiate");

    AUChannelInfo layouts[4] = {};
    UInt32 size = sizeof(layouts);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(unit, kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global, 0,
                                          layouts, &size), noErr, "channel layouts must read");
    DAF_ASSERT_EQUAL(size, 2 * sizeof(AUChannelInfo), "the sidechain rule must not advertise new main layouts");
    DAF_ASSERT_EQUAL(layouts[0].inChannels == 2 && layouts[0].outChannels == 2
                     && layouts[1].inChannels == 1 && layouts[1].outChannels == 1, true,
                     "advertised main layouts stay stereo and mono");

    // Logic's mono track, in Logic's order.
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 0, 1), noErr, "the main element must take one channel");
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 1, 1), noErr, "the sidechain element must take one channel");
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Output, 0, 1), noErr, "the output must take one channel");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "a Logic mono track must initialize");
    DAF_ASSERT_EQUAL(gIoInputs == 1 && gIoOutputs == 1, true, "before the connections the plugin runs plain mono");
    DAF_ASSERT_EQUAL(connect(unit, 0, true), noErr, "the main element must connect while initialized");
    DAF_ASSERT_EQUAL(connect(unit, 1, true), noErr, "the mono sidechain must connect while initialized");
    DAF_ASSERT_EQUAL(gIoInputs == 2 && gIoOutputs == 1, true, "the plugin must be told 1 + 1 inputs and 1 output");
    float left[16] = {}, right[16] = {};
    DAF_ASSERT_EQUAL(render(unit, 1, left, right), noErr, "the Logic mono track must render");
    DAF_ASSERT_EQUAL(gSeen[0] == 0.25f && gSeen[1] == 0.0f && gSeen[2] == 0.5f && gSeen[3] == 0.0f, true,
                     "ports keep their positions: main, silence, the mono sidechain on port 2, silence");
    DAF_ASSERT_EQUAL(left[15], 0.25f, "the mono output must carry the plugin's output");
    // A main width set while the unit runs that it cannot run (2 + 1 into 1) is not reported, and render keeps
    // running what the plugin was told: the render guard must not depend on the bus state a property call changes.
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 0, 2), noErr,
                     "the main element takes two channels while initialized (the format setter takes it)");
    DAF_ASSERT_EQUAL(gIoInputs == 2 && gIoOutputs == 1, true, "a layout the plugin cannot run must not be reported");
    DAF_ASSERT_EQUAL(render(unit, 1, left, right), noErr, "render must keep running the reported layout");
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 0, 1), noErr, "the main element must narrow again");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the unit must uninitialize");

    // A stereo key on the mono main.
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 1, 2), noErr, "the sidechain element must widen to stereo");
    DAF_ASSERT_EQUAL(gIoInputs == 3 && gIoOutputs == 1, true, "the plugin must be told 1 + 2 inputs and 1 output");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "a mono main keyed by a stereo sidechain must initialize");
    DAF_ASSERT_EQUAL(render(unit, 1, left, right), noErr, "the stereo-keyed mono unit must render");
    DAF_ASSERT_EQUAL(gSeen[1] == 0.0f && gSeen[2] == 0.5f && gSeen[3] == 0.75f, true,
                     "a stereo sidechain stays on ports 2-3 of a mono main");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the stereo-keyed mono unit must uninitialize");
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 1, 3), kAudioUnitErr_FormatNotSupported,
                     "a sidechain never runs wider than declared");

    // Without a sidechain the mono main is a declared layout of its own.
    DAF_ASSERT_EQUAL(connect(unit, 1, false), noErr, "the sidechain element must disconnect");
    DAF_ASSERT_EQUAL(gIoInputs == 1 && gIoOutputs == 1, true, "a plain mono main must be reported as 1 input");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "a plain mono main must initialize");
    DAF_ASSERT_EQUAL(render(unit, 1, left, right), noErr, "the plain mono unit must render");
    DAF_ASSERT_EQUAL(gSeen[2] == 0.0f && gSeen[3] == 0.0f, true, "a disconnected sidechain must read silence");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the plain mono unit must uninitialize");

    // Stereo with a stereo sidechain is the declared { 4, 2 }; with a mono one, 2 + 1.
    DAF_ASSERT_EQUAL(connect(unit, 1, true), noErr, "the sidechain element must reconnect");
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 0, 2), noErr, "the main element must widen to stereo");
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Output, 0, 2), noErr, "the output must widen to stereo");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "stereo with a stereo sidechain must initialize");
    DAF_ASSERT_EQUAL(gIoInputs == 4 && gIoOutputs == 2, true, "stereo with a stereo sidechain must report 4 inputs");
    DAF_ASSERT_EQUAL(render(unit, 2, left, right), noErr, "stereo with a stereo sidechain must render");
    DAF_ASSERT_EQUAL(gSeen[1] == 0.25f && gSeen[2] == 0.5f && gSeen[3] == 0.75f, true,
                     "stereo ports must read the main pair and the sidechain");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the stereo unit must uninitialize");
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 1, 1), noErr, "a stereo main must take a mono sidechain");
    DAF_ASSERT_EQUAL(gIoInputs == 3 && gIoOutputs == 2, true, "stereo with a mono sidechain must report 2 + 1 inputs");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "stereo with a mono sidechain must initialize");
    DAF_ASSERT_EQUAL(render(unit, 2, left, right), noErr, "stereo with a mono sidechain must render");
    DAF_ASSERT_EQUAL(gSeen[2] == 0.5f && gSeen[3] == 0.0f, true, "a mono sidechain reads port 2, port 3 is silent");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the stereo, mono-keyed unit must uninitialize");

    // A mono main with a mono sidechain into two outputs sums to the 2 of the stereo main without one, but is no
    // declared layout: neither reported as a stereo main nor run. (The format setter takes it as an intermediate
    // state while a host sets formats.)
    DAF_ASSERT_EQUAL(width(unit, kAudioUnitScope_Input, 0, 1), noErr, "the main element takes one channel meanwhile");
    DAF_ASSERT_EQUAL(gIoInputs == 3 && gIoOutputs == 2, true, "1 + 1 into 2 must not be reported as a stereo main");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), kAudioUnitErr_FormatNotSupported, "1 + 1 into 2 must not initialize");

    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(unit), noErr, "test AU must dispose");
    return 0;
}
