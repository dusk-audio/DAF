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


// Sysex into the AU wrapper, with main() as a host that frees or reuses its buffer as soon as MusicDeviceSysEx
// returns, as it may. The wrapper kept the host's pointer until the next render, and the plugin then read freed or
// overwritten memory (a heap use after free under AddressSanitizer, garbage otherwise). Built twice, for a plugin
// without audio (AUSysEx, au-midi-only/) and for an effect (AUSysExEffect, au-midi/): the plugin must be given the
// bytes as sent; sysex of exactly the storage a render takes is kept whole, and one that finds no room is dropped
// whole, never cut short, with an error the host sees; a sysex keeps its place after the events queued before it.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <AudioToolbox/MusicDevice.h>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
struct Received
{
    uint32_t frame;
    std::vector<uint8_t> bytes;
};
std::vector<Received> gReceived;
}

START_NAMESPACE_DAF

// Keeps a copy of every event it is given (a test plugin: it allocates, which a real one must not).
class SysExPlugin : public Plugin
{
public:
    SysExPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "SysExTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void run(const float** const inputs, float** const outputs, const uint32_t frames, const MidiEvent* const events,
             const uint32_t count) override
    {
       #if DAF_PLUGIN_NUM_OUTPUTS != 0
        std::memcpy(outputs[0], inputs[0], sizeof(float) * frames);
       #else
        (void)inputs;
        (void)outputs;
        (void)frames;
       #endif
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint8_t* const data = events[i].size > MidiEvent::kDataSize ? events[i].dataExt : events[i].data;
            gReceived.push_back({events[i].frame, std::vector<uint8_t>(data, data + events[i].size)});
        }
    }
};

Plugin* createPlugin()
{
    return new SysExPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
constexpr UInt32 kBlock = 256;
AudioUnit gUnit = nullptr;
UInt32 gChannels = 0;

OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
        std::memset(data->mBuffers[ch].mData, 0, sizeof(float) * frames);
    return noErr;
}

OSStatus render()
{
    std::vector<std::vector<float>> data(gChannels, std::vector<float>(kBlock));
    std::vector<uint8_t> storage(sizeof(AudioBufferList) + sizeof(AudioBuffer) * (gChannels + 1));
    AudioBufferList* const list = reinterpret_cast<AudioBufferList*>(storage.data());
    list->mNumberBuffers = gChannels;
    for (UInt32 i = 0; i < gChannels; ++i)
        list->mBuffers[i] = {1, static_cast<UInt32>(sizeof(float) * kBlock), data[i].data()};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    return AudioUnitRender(gUnit, &flags, &timestamp, 0, kBlock, list);
}

std::vector<uint8_t> sysex(const uint32_t size, const uint8_t seed)
{
    std::vector<uint8_t> bytes(size);
    bytes[0] = 0xF0;
    for (uint32_t i = 1; i + 1 < size; ++i)
        bytes[i] = static_cast<uint8_t>((seed + i * 3) & 0x7F);
    bytes[size - 1] = 0xF7;
    return bytes;
}

// Sends a sysex from a buffer of the host's own, then does to it what the host may once the call has returned: write
// something else there (the buffer stays valid, so a plugin reading it gets the wrong bytes, sanitizer or not), or
// free it (a plugin reading it is a use after free, which AddressSanitizer reports).
enum Afterwards { kReuse, kFree };
std::vector<uint8_t> gReusedBuffer;
OSStatus sendFromHostBuffer(const std::vector<uint8_t>& bytes, const Afterwards afterwards)
{
    if (afterwards == kReuse)
    {
        gReusedBuffer = bytes;
        const OSStatus status = MusicDeviceSysEx(gUnit, gReusedBuffer.data(), static_cast<UInt32>(bytes.size()));
        std::memset(gReusedBuffer.data(), 0x42, gReusedBuffer.size());
        return status;
    }
    uint8_t* const buffer = static_cast<uint8_t*>(std::malloc(bytes.size()));
    std::memcpy(buffer, bytes.data(), bytes.size());
    const OSStatus status = MusicDeviceSysEx(gUnit, buffer, static_cast<UInt32>(bytes.size()));
    std::free(buffer);
    return status;
}

bool receivedOnly(const std::vector<std::vector<uint8_t>>& wanted, const char* const what)
{
    bool same = gReceived.size() == wanted.size();
    for (size_t i = 0; same && i < wanted.size(); ++i)
        same = gReceived[i].bytes == wanted[i];
    if (! same)
    {
        d_stderr2("%s: the plugin got %zu message(s), expected %zu", what, gReceived.size(), wanted.size());
        for (size_t i = 0; i < gReceived.size() && i < 4; ++i)
            d_stderr2("  message %zu: %zu bytes from %02X, last %02X", i, gReceived[i].bytes.size(),
                      gReceived[i].bytes.empty() ? 0 : gReceived[i].bytes[0],
                      gReceived[i].bytes.empty() ? 0 : gReceived[i].bytes.back());
    }
    return same;
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU SysEx Test"), 0x10000, factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &gUnit), noErr, "test AU must instantiate");
    UInt32 maxFrames = kBlock;
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                          &maxFrames, sizeof(maxFrames)), noErr, "the block size must set");
   #if DAF_PLUGIN_NUM_INPUTS != 0
    AURenderCallbackStruct input = {feed, nullptr};
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                          &input, sizeof(input)), noErr, "the input must connect");
   #else
    (void)feed;
   #endif
    AudioStreamBasicDescription format = {};
    UInt32 size = sizeof(format);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &format,
                                          &size), noErr, "the output format must read");
    gChannels = format.mChannelsPerFrame;
    DAF_ASSERT_EQUAL(AudioUnitInitialize(gUnit), noErr, "the unit must initialize");

    // A host that reuses its buffer, then one that frees it, for a sysex short enough for MidiEvent::data and for
    // ones read through dataExt (5, 16 and 300 bytes, the lengths a reviewer saw corrupted).
    for (const uint32_t length : {4u, 5u, 16u, 300u})
    {
        for (const Afterwards afterwards : {kReuse, kFree})
        {
            gReceived.clear();
            const std::vector<uint8_t> bytes = sysex(length, static_cast<uint8_t>(length));
            DAF_ASSERT_EQUAL(sendFromHostBuffer(bytes, afterwards), noErr, "the sysex must be taken");
            DAF_ASSERT_EQUAL(render(), noErr, "the block must render");
            DAF_ASSERT_EQUAL(receivedOnly({bytes}, afterwards == kReuse ? "reused buffer" : "freed buffer"), true,
                             "the plugin must get the sysex as sent, whatever the host did with its buffer after");
        }
    }

    // Two events before a sysex: it follows the later one, at its frame.
    gReceived.clear();
    const std::vector<uint8_t> after = sysex(12, 7);
    DAF_ASSERT_EQUAL(MusicDeviceMIDIEvent(gUnit, 0x90, 60, 100, 10) == noErr
                     && MusicDeviceMIDIEvent(gUnit, 0x90, 64, 100, 30) == noErr
                     && sendFromHostBuffer(after, kFree) == noErr && render() == noErr, true, "a block must render");
    DAF_ASSERT_EQUAL(gReceived.size() == 3 && gReceived[2].frame == 30 && gReceived[2].bytes == after, true,
                     "a sysex must follow the last event queued before it, at its frame");

    // The storage a render takes holds kMidiInputSysExBytes of sysex: a sysex that size is kept whole, and in the
    // same block a sysex that finds no room is dropped whole, refused as one there is room for later; a longer one is
    // dropped at once, refused as a length the unit never takes; the next block's storage is empty again.
    gReceived.clear();
    const std::vector<uint8_t> whole = sysex(kMidiInputSysExBytes, 1), more = sysex(5, 2);
    DAF_ASSERT_EQUAL(sendFromHostBuffer(whole, kFree), noErr, "a sysex filling the storage must be taken");
    DAF_ASSERT_EQUAL(sendFromHostBuffer(more, kFree), kAudioUnitErr_CannotDoInCurrentContext,
                     "a sysex finding the storage full must be refused, as one there is room for later");
    DAF_ASSERT_EQUAL(render(), noErr, "a block with a full sysex storage must render");
    DAF_ASSERT_EQUAL(receivedOnly({whole}, "full storage"), true,
                     "a sysex filling the storage must be kept whole and the next dropped whole");
    gReceived.clear();
    DAF_ASSERT_EQUAL(sendFromHostBuffer(sysex(kMidiInputSysExBytes + 1, 3), kFree), kAudio_ParamError,
                     "a sysex longer than the storage must be refused, as a length the unit never takes");
    DAF_ASSERT_EQUAL(sendFromHostBuffer(more, kFree) == noErr && render() == noErr, true, "a block must render");
    DAF_ASSERT_EQUAL(receivedOnly({more}, "too long"), true,
                     "a sysex longer than the storage must be dropped, leaving room for the next");

    // A sysex that finds every event slot taken is refused too, as one there is room for later, short or long.
    gReceived.clear();
    bool slotsFilled = true;
    for (uint32_t i = 0; i < kMaxMidiEvents && slotsFilled; ++i)
        slotsFilled = MusicDeviceMIDIEvent(gUnit, 0xB0, 1, i & 0x7F, 0) == noErr;
    DAF_ASSERT_EQUAL(slotsFilled, true, "events up to the queue's size must be taken");
    const uint8_t shortSysEx[4] = {0xF0, 0x7D, 0x01, 0xF7};
    DAF_ASSERT_EQUAL(MusicDeviceSysEx(gUnit, shortSysEx, sizeof(shortSysEx)), kAudioUnitErr_CannotDoInCurrentContext,
                     "a short sysex finding no event slot must be refused");
    DAF_ASSERT_EQUAL(sendFromHostBuffer(more, kFree), kAudioUnitErr_CannotDoInCurrentContext,
                     "a long sysex finding no event slot must be refused");
    DAF_ASSERT_EQUAL(render() == noErr && gReceived.size() == kMaxMidiEvents, true,
                     "a block with every event slot taken must run them all and nothing more");
    gReceived.clear();
    DAF_ASSERT_EQUAL(sendFromHostBuffer(whole, kReuse) == noErr && render() == noErr, true, "a block must render");
    DAF_ASSERT_EQUAL(receivedOnly({whole}, "next block"), true, "the next block must have the whole storage again");

    // No bytes at all is refused.
    const uint8_t one = 0xF0;
    DAF_ASSERT_EQUAL(MusicDeviceSysEx(gUnit, &one, 0) != noErr, true, "a sysex with no bytes must be refused");

    d_stdout("sysex reached the plugin as sent, from host buffers reused and freed");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(gUnit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(gUnit), noErr, "test AU must dispose");
    return 0;
}
