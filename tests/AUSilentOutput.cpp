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


// A plugin without audio (an aumi) and the output element its AU gets, with main() as the host. Such a unit had no
// element at all, so a host could not set its sample rate (the plugin kept 44100 Hz at any session rate, and a clock
// it keeps from its frames ran fast or slow by the ratio), and a host rendering it with a buffer list got an error
// on every render, with the plugin never run. Now: one output element, stereo until the host sets another count
// (any, as kAudioUnitProperty_SupportedNumChannels says with {0, -1}); its sample rate, set through SampleRate or
// StreamFormat, is the plugin's, which is activated at it; and render plays silence in whatever buffers the host
// gives (or none, or ones it leaves to the unit), running the plugin once a block either way.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <vector>

static double gActivatedAt = 0.0, gRanAt = 0.0;
static uint32_t gActivations = 0, gRuns = 0, gLastRunFrames = 0;
static bool gGotAudio = false;

START_NAMESPACE_DAF

class SilentOutputPlugin : public Plugin
{
public:
    SilentOutputPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "MidiOnlyTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void activate() override
    {
        ++gActivations;
        gActivatedAt = getSampleRate();
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames, const MidiEvent*,
             uint32_t) override
    {
        ++gRuns;
        gLastRunFrames = frames;
        gRanAt = getSampleRate();
        gGotAudio = gGotAudio || inputs != nullptr || outputs != nullptr;
    }
};

Plugin* createPlugin()
{
    return new SilentOutputPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
constexpr UInt32 kBlock = 128;
AudioUnit gUnit = nullptr;

OSStatus setFormat(const Float64 sampleRate, const UInt32 channels)
{
    AudioStreamBasicDescription format = {};
    UInt32 size = sizeof(format);
    OSStatus status = AudioUnitGetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format,
                                           &size);
    if (status != noErr)
        return status;
    format.mSampleRate = sampleRate;
    format.mChannelsPerFrame = channels;
    return AudioUnitSetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format,
                                sizeof(format));
}

AudioStreamBasicDescription getFormat()
{
    AudioStreamBasicDescription format = {};
    UInt32 size = sizeof(format);
    AudioUnitGetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format, &size);
    return format;
}

// Renders `buffers` buffers of `frames`, filled with 1 beforehand; with `leftToUnit` their mData is null, for the unit
// to provide. True when the render succeeded, ran the plugin once for `frames` and left only silence behind.
OSStatus gStatus = noErr;
bool render(const UInt32 buffers, const UInt32 frames, const bool leftToUnit = false)
{
    std::vector<std::vector<float>> data(buffers, std::vector<float>(frames, 1.0f));
    std::vector<uint8_t> storage(sizeof(AudioBufferList) + sizeof(AudioBuffer) * (buffers + 1));
    AudioBufferList* const list = reinterpret_cast<AudioBufferList*>(storage.data());
    list->mNumberBuffers = buffers;
    for (UInt32 i = 0; i < buffers; ++i)
        list->mBuffers[i] = {1, static_cast<UInt32>(sizeof(float) * frames), leftToUnit ? nullptr : data[i].data()};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    const uint32_t runs = gRuns;
    gStatus = AudioUnitRender(gUnit, &flags, &timestamp, 0, frames, list);
    if (gStatus != noErr)
        return false;
    bool silent = true;
    for (UInt32 i = 0; i < buffers; ++i)
    {
        const float* const samples = static_cast<const float*>(list->mBuffers[i].mData);
        if (samples == nullptr)
            return false;
        for (UInt32 f = 0; f < frames; ++f)
            silent = silent && samples[f] == 0.0f;
        for (UInt32 j = 0; j < i; ++j)
            silent = silent && list->mBuffers[j].mData != list->mBuffers[i].mData;
    }
    return silent && gRuns == runs + 1 && gLastRunFrames == frames;
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU Silent Output Test"), 0x10000,
                                                      factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &gUnit), noErr, "test AU must instantiate");

    // One output element, no input; stereo float until the host says otherwise, any count allowed.
    UInt32 outputs = 0, inputs = 1, size = sizeof(UInt32);
    AudioUnitGetProperty(gUnit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Output, 0, &outputs, &size);
    size = sizeof(UInt32);
    AudioUnitGetProperty(gUnit, kAudioUnitProperty_ElementCount, kAudioUnitScope_Input, 0, &inputs, &size);
    DAF_ASSERT_EQUAL(outputs == 1 && inputs == 0, true, "the unit must have one output element and no input");
    const AudioStreamBasicDescription initial = getFormat();
    DAF_ASSERT_EQUAL(initial.mChannelsPerFrame == 2 && initial.mFormatID == kAudioFormatLinearPCM
                     && initial.mBitsPerChannel == 32 && initial.mSampleRate > 0.0, true,
                     "the output element must start as stereo float");
    AUChannelInfo channelInfo[4] = {};
    size = sizeof(channelInfo);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(gUnit, kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global, 0,
                                          channelInfo, &size), noErr, "the channel layouts must read");
    DAF_ASSERT_EQUAL(size == sizeof(AUChannelInfo) && channelInfo[0].inChannels == 0
                     && channelInfo[0].outChannels == -1, true, "the unit must offer any number of output channels");

    // The sample rate the host sets on the element is the plugin's, and the plugin is activated at it.
    UInt32 maxFrames = kBlock;
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                          &maxFrames, sizeof(maxFrames)), noErr, "the block size must set");
    DAF_ASSERT_EQUAL(setFormat(48000.0, 2), noErr, "the output format must set before initialize");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(gUnit), noErr, "the unit must initialize");
    DAF_ASSERT_EQUAL(gActivatedAt, 48000.0, "the plugin must be activated at the host's rate");
    DAF_ASSERT_EQUAL(render(2, kBlock) && gRanAt == 48000.0, true, "a stereo render must run the plugin at 48 kHz");
    for (const Float64 rate : {96000.0, 44100.0})
    {
        const uint32_t activations = gActivations;
        DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &rate,
                                              sizeof(rate)), noErr, "the sample rate must set while initialized");
        Float64 read = 0.0;
        size = sizeof(read);
        AudioUnitGetProperty(gUnit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &read, &size);
        DAF_ASSERT_EQUAL(read == rate && gActivatedAt == rate && gActivations == activations + 1, true,
                         "a new sample rate must activate the plugin again, at it");
        DAF_ASSERT_EQUAL(render(2, kBlock) && gRanAt == rate, true, "the next render must run the plugin at it");
    }
    DAF_ASSERT_EQUAL(setFormat(88200.0, 2) == noErr && gActivatedAt == 88200.0, true,
                     "a rate set through StreamFormat must reach the plugin too");

    // Render takes any buffer list: none (as Apple's bridge renders a unit with no element), fewer or more than the
    // element's channels, buffers left to the unit, or a block shorter than the largest.
    DAF_ASSERT_EQUAL(render(0, kBlock), true, "a render without buffers must run the plugin");
    DAF_ASSERT_EQUAL(render(1, kBlock), true, "a render with fewer buffers than channels must play silence");
    DAF_ASSERT_EQUAL(render(5, kBlock), true, "a render with more buffers than channels must play silence");
    DAF_ASSERT_EQUAL(render(2, kBlock, true), true, "buffers left to the unit must be provided, silent");
    DAF_ASSERT_EQUAL(render(2, kBlock / 2), true, "a shorter block must run the plugin for its frames");
    DAF_ASSERT_EQUAL(render(3, kBlock, true) || gStatus != kAudio_ParamError, false,
                     "more buffers left to the unit than it has must be refused");
    DAF_ASSERT_EQUAL(render(2, kBlock * 2) || gStatus != kAudioUnitErr_TooManyFramesToProcess, false,
                     "more frames than the largest block must be refused");

    // The element's channel count and the block size are fixed while the unit is initialized (its own buffers are
    // sized then); before, any count from 1 to the limit goes.
    DAF_ASSERT_EQUAL(setFormat(48000.0, 6), kAudioUnitErr_Initialized,
                     "the channel count must not change while initialized");
    maxFrames = kBlock * 2;
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                          &maxFrames, sizeof(maxFrames)), kAudioUnitErr_Initialized,
                     "the block size must not change while initialized");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(gUnit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(setFormat(48000.0, 0), kAudioUnitErr_FormatNotSupported, "no channels at all");
    DAF_ASSERT_EQUAL(setFormat(48000.0, 65), kAudioUnitErr_FormatNotSupported, "more channels than the limit");
    DAF_ASSERT_EQUAL(setFormat(48000.0, 1) == noErr && getFormat().mChannelsPerFrame == 1, true, "a mono element");
    DAF_ASSERT_EQUAL(setFormat(48000.0, 6) == noErr && getFormat().mChannelsPerFrame == 6, true, "a 5.1 element");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(gUnit), noErr, "the unit must initialize with six channels");
    DAF_ASSERT_EQUAL(render(6, kBlock, true), true, "six buffers left to the unit must be provided, silent");
    DAF_ASSERT_EQUAL(render(6, kBlock), true, "six channels must play silence");

    DAF_ASSERT_EQUAL(gGotAudio, false, "the plugin must never be given audio");
    d_stdout("%u runs, %u activations, last at %.0f Hz", gRuns, gActivations, gActivatedAt);
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(gUnit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(gUnit), noErr, "test AU must dispose");
    return 0;
}
