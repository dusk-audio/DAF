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


// The AU wrapper's MIDI output, with main() as the host reading it the way every host does: the packet list handed
// to kAudioUnitProperty_MIDIOutputCallback, walked with MIDIPacketNext. The wrapper placed each packet 12 bytes plus
// its length after the one before (the padded sizeof(MIDIPacket) less its nominal 256 data bytes), where CoreMIDI
// puts it 10 bytes plus its length after, rounded up to 4 on ARM: the host read every packet after a block's first
// as garbage. Each block here writes a script of messages from run(): every message must reach the host whole, in
// order, at its frame; short and long (sysex) messages of every length class mix; the list takes kMaxMidiEvents
// short messages, refuses what does not fit without spoiling what it holds, and is fresh for the next block.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <cstring>

namespace
{
constexpr uint32_t kBlock = 256;
constexpr uint32_t kCapacity = 4096;
constexpr uint32_t kLongest = 65535;   // a MIDIPacket length is 16 bits

struct Message
{
    uint32_t frame;
    uint32_t size;
    const uint8_t* bytes;
};

// Shared bytes the script's messages point into; a long message reads its bytes from here.
uint8_t gPool[1 << 17];
uint32_t gPoolUsed = 0;

// The block's script, and what writeMidiEvent() answered.
Message gScript[kCapacity];
uint32_t gScriptCount = 0;
uint32_t gAccepted = 0;
bool gRefused = false;

// What reached the host.
struct Received
{
    uint32_t frame;
    uint32_t size;
    uint8_t bytes[kLongest];
};
Received gReceived[4];
uint32_t gReceivedCount = 0;
uint32_t gReceivedShortCount = 0;
uint32_t gOutputCalls = 0, gUnreadablePackets = 0, gReceivedMismatches = 0;
}

START_NAMESPACE_DAF

// Plays its input through and writes the block's script, until the host refuses a message (as the API asks, it then
// writes nothing more until the next run()).
class MidiOutputPlugin : public Plugin
{
public:
    MidiOutputPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "MidiOutputTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void run(const float** const inputs, float** const outputs, const uint32_t frames, const MidiEvent*,
             uint32_t) override
    {
        std::memcpy(outputs[0], inputs[0], sizeof(float) * frames);
        for (uint32_t i = 0; i < gScriptCount; ++i)
        {
            MidiEvent event = {};
            event.frame = gScript[i].frame;
            event.size = gScript[i].size;
            if (event.size > MidiEvent::kDataSize)
                event.dataExt = gScript[i].bytes;
            else
                std::memcpy(event.data, gScript[i].bytes, event.size);
            if (! writeMidiEvent(event))
            {
                gRefused = true;
                return;
            }
            ++gAccepted;
        }
    }
};

Plugin* createPlugin()
{
    return new MidiOutputPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
AudioUnit gUnit = nullptr;

// Compares every message as it arrives against the script (a block can carry more than gReceived keeps).
void receive(const uint32_t frame, const uint8_t* const bytes, const uint32_t size)
{
    const uint32_t index = gReceivedShortCount++;
    if (index >= gAccepted || gScript[index].frame != frame || gScript[index].size != size
        || std::memcmp(gScript[index].bytes, bytes, size) != 0)
    {
        if (gReceivedMismatches++ == 0)
            d_stderr2("message %u arrived as %u bytes from %02X at frame %u; the script has %u bytes from %02X at "
                      "frame %u", index, size, size != 0 ? bytes[0] : 0, frame,
                      index < gScriptCount ? gScript[index].size : 0,
                      index < gScriptCount && gScript[index].size != 0 ? gScript[index].bytes[0] : 0,
                      index < gScriptCount ? gScript[index].frame : 0);
        return;
    }
    if (gReceivedCount < sizeof(gReceived) / sizeof(gReceived[0]))
    {
        gReceived[gReceivedCount].frame = frame;
        gReceived[gReceivedCount].size = size;
        std::memcpy(gReceived[gReceivedCount].bytes, bytes, size);
        ++gReceivedCount;
    }
}

// The list as a host walks it (MIDIPacketNext); a packet that cannot be a message stops the walk. One message per
// packet is what the wrapper writes, but a packet may hold several whole ones (not a sysex), so split them.
OSStatus midiOutput(void*, const AudioTimeStamp*, UInt32, const MIDIPacketList* const list)
{
    ++gOutputCalls;
    const MIDIPacket* packet = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; ++i, packet = MIDIPacketNext(packet))
    {
        if (packet->length == 0 || packet->length > kLongest || packet->timeStamp >= kBlock)
        {
            if (gUnreadablePackets++ == 0)
                d_stderr2("packet %u of %u is unreadable: timeStamp %llu, length %u", i, list->numPackets,
                          static_cast<unsigned long long>(packet->timeStamp), packet->length);
            return noErr;
        }
        if (packet->data[0] == 0xF0)
        {
            receive(static_cast<uint32_t>(packet->timeStamp), packet->data, packet->length);
            continue;
        }
        for (uint32_t at = 0; at < packet->length;)
        {
            const uint8_t status = packet->data[at];
            const uint32_t size = status < 0x80 ? 0 : status < 0xC0 ? 3 : status < 0xE0 ? 2 : status < 0xF0 ? 3
                                : status == 0xF2 ? 3 : status == 0xF1 || status == 0xF3 ? 2 : 1;
            if (size == 0 || at + size > packet->length)
            {
                if (gUnreadablePackets++ == 0)
                    d_stderr2("packet %u of %u holds an incomplete message", i, list->numPackets);
                return noErr;
            }
            receive(static_cast<uint32_t>(packet->timeStamp), packet->data + at, size);
            at += size;
        }
    }
    return noErr;
}

OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
        std::memset(data->mBuffers[ch].mData, 0, sizeof(float) * frames);
    return noErr;
}

void script()
{
    gScriptCount = gPoolUsed = gAccepted = 0;
    gRefused = false;
}

void add(const uint32_t frame, const uint8_t* const bytes, const uint32_t size)
{
    std::memcpy(gPool + gPoolUsed, bytes, size);
    gScript[gScriptCount++] = {frame, size, gPool + gPoolUsed};
    gPoolUsed += size;
}

void addShort(const uint32_t frame, const uint8_t status, const uint8_t data1 = 0, const uint8_t data2 = 0)
{
    const uint8_t bytes[3] = {status, data1, data2};
    add(frame, bytes, status < 0xC0 || (status >= 0xE0 && status < 0xF0) || status == 0xF2 ? 3
                      : status < 0xE0 || status == 0xF1 || status == 0xF3 ? 2 : 1);
}

void addSysEx(const uint32_t frame, const uint32_t size)
{
    uint8_t* const bytes = gPool + gPoolUsed;
    bytes[0] = 0xF0;
    for (uint32_t i = 1; i + 1 < size; ++i)
        bytes[i] = static_cast<uint8_t>((i * 7 + size) & 0x7F);
    bytes[size - 1] = 0xF7;
    gScript[gScriptCount++] = {frame, size, bytes};
    gPoolUsed += size;
}

// Renders one block with the script and checks that the host got exactly what the plugin had accepted.
bool renderBlock(const char* const what)
{
    gReceivedCount = gReceivedShortCount = gOutputCalls = gUnreadablePackets = gReceivedMismatches = 0;
    float samples[kBlock] = {};
    AudioBufferList output = {1, {{1, sizeof(samples), samples}}};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    if (AudioUnitRender(gUnit, &flags, &timestamp, 0, kBlock, &output) != noErr)
    {
        d_stderr2("%s: render failed", what);
        return false;
    }
    if (gUnreadablePackets != 0 || gReceivedMismatches != 0 || gReceivedShortCount != gAccepted
        || gOutputCalls != (gAccepted != 0 ? 1u : 0u))
    {
        d_stderr2("%s: %u of %u accepted messages arrived in %u output calls, %u unreadable packets, %u mismatches",
                  what, gReceivedShortCount, gAccepted, gOutputCalls, gUnreadablePackets, gReceivedMismatches);
        return false;
    }
    return true;
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU MIDI Output Test"), 0x10000,
                                                      factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &gUnit), noErr, "test AU must instantiate");
    UInt32 maxFrames = kBlock;
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                          &maxFrames, sizeof(maxFrames)), noErr, "the block size must set");
    AURenderCallbackStruct input = {feed, nullptr};
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                          &input, sizeof(input)), noErr, "the input must connect");
    AUMIDIOutputCallbackStruct output = {midiOutput, nullptr};
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(gUnit, kAudioUnitProperty_MIDIOutputCallback, kAudioUnitScope_Global, 0,
                                          &output, sizeof(output)), noErr, "the MIDI output callback must set");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(gUnit), noErr, "the unit must initialize");

    // Two messages on one frame: a host read the second as garbage.
    script();
    addShort(0, 0x90, 60, 100);
    addShort(0, 0x80, 60, 0);
    DAF_ASSERT_EQUAL(renderBlock("two notes") && gAccepted == 2, true, "a block's second message must reach the host");

    // Every message length the wrapper places: one, two and three bytes, and sysex of 4 to 40 bytes and longer (kept
    // in MidiEvent::data up to its size, past it read through dataExt), each after a three-byte message, so that
    // every packet start a misplaced packet could take is crossed; frames rise through the block.
    script();
    uint32_t frame = 0;
    addShort(frame++, 0xF8);
    addShort(frame++, 0xC3, 17);
    addShort(frame++, 0xE5, 0x10, 0x40);
    for (uint32_t size = 4; size <= 40; ++size)
    {
        addShort(frame, 0x90, static_cast<uint8_t>(size), 64);
        addSysEx(frame++, size);
    }
    for (const uint32_t size : {255u, 256u, 257u, 301u, 1000u, 1001u})
    {
        addSysEx(frame, size);
        addShort(frame++, 0xD1, static_cast<uint8_t>(size & 0x7F));
    }
    addShort(kBlock - 1, 0xB0, 123, 0);
    DAF_ASSERT_EQUAL(renderBlock("every length") && gAccepted == gScriptCount, true,
                     "messages of every length must reach the host whole, in order, at their frames");

    // A block of short messages: kMaxMidiEvents must fit (as many as a block can receive), and once the list is full
    // the next is refused and what was accepted still reaches the host whole.
    script();
    for (uint32_t i = 0; i < 4 * kMaxMidiEvents; ++i)
        addShort(i * kBlock / (4 * kMaxMidiEvents), 0x90 | (i & 0x0F), static_cast<uint8_t>(i & 0x7F), 1 + i % 127);
    DAF_ASSERT_EQUAL(renderBlock("a full list") && gRefused && gAccepted >= kMaxMidiEvents, true,
                     "a list must take kMaxMidiEvents short messages, refuse the rest, and keep those it took");
    d_stdout("a block's list takes %u three-byte messages", gAccepted);

    // The longest sysex a list takes alone (found by halving): it reaches the host whole, the message after it is
    // refused, and a sysex one byte longer is refused with nothing sent. Placed past the list's end, a refused message
    // would write outside it.
    uint32_t longest = 5, tooLong = kLongest + 1;
    while (tooLong - longest > 1)
    {
        const uint32_t size = longest + (tooLong - longest) / 2;
        script();
        addSysEx(0, size);
        DAF_ASSERT_EQUAL(renderBlock("a sysex alone"), true, "a sysex alone must reach the host whole or be refused");
        (gAccepted == 1 ? longest : tooLong) = size;
    }
    d_stdout("a block's list takes a sysex of up to %u bytes", longest);
    DAF_ASSERT_EQUAL(longest >= kMidiInputSysExBytes, true,
                     "a list must take a sysex as long as all the sysex one render takes in");
    script();
    addSysEx(0, longest);
    addShort(1, 0xF8);
    DAF_ASSERT_EQUAL(renderBlock("the longest sysex") && gAccepted == 1 && gRefused, true,
                     "a sysex filling the list must reach the host whole, and the next message be refused");
    DAF_ASSERT_EQUAL(gReceivedCount == 1 && gReceived[0].size == longest, true, "the longest sysex must arrive whole");
    script();
    addSysEx(0, longest + 1);
    DAF_ASSERT_EQUAL(renderBlock("a sysex too long") && gAccepted == 0 && gRefused, true,
                     "a sysex longer than the list holds must be refused, and nothing sent");

    // The next block starts a fresh list.
    script();
    addShort(3, 0x90, 64, 1);
    addShort(4, 0x80, 64, 0);
    DAF_ASSERT_EQUAL(renderBlock("the next block") && gAccepted == 2, true, "the next block must start a fresh list");

    DAF_ASSERT_EQUAL(AudioUnitUninitialize(gUnit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(gUnit), noErr, "test AU must dispose");
    return 0;
}
