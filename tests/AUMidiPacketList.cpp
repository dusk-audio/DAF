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

/* The AU wrapper's MIDI output list (AUMidiPacketList), under both of CoreMIDI's packet rules on whatever machine runs
 * this: on ARM a packet starts on a 4-byte boundary, on Intel right after the one before. The wrapper places packets
 * with MIDIPacketNext, which knows only the rule of the machine it was built for, so the AU tests (AUMidiOutput) check
 * the native rule and this one checks the list under both, with readers of its own that walk the bytes as the
 * header documents them. The list once put each packet 12 bytes plus its length after the one before (the padded
 * sizeof(MIDIPacket) less its 256 nominal data bytes), where both rules put it 10 bytes plus its length after.
 *
 * Worth running under ASan as well: the list is filled to its last byte here. */

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>

// layout-compatible stand-ins for the CoreMIDI declarations (MIDIServices.h packs both to 4 bytes)
#pragma pack(push, 4)
struct MIDIPacket {
    uint64_t timeStamp;
    uint16_t length;
    uint8_t data[256];
};

struct MIDIPacketList {
    uint32_t numPackets;
    MIDIPacket packet[1];
};
#pragma pack(pop)

static_assert(offsetof(MIDIPacket, length) == 8 && offsetof(MIDIPacket, data) == 10, "a packet's header is 10 bytes");
static_assert(offsetof(MIDIPacketList, packet) == 4, "a list's header is 4 bytes");

#include "daf/src/DafPluginAUMidiOutput.hpp"

USE_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

namespace
{
// CoreMIDI's two rules for where the next packet starts, as MIDIServices.h writes MIDIPacketNext.
uint16_t lengthOf(const MIDIPacket* const packet)
{
    uint16_t length;
    std::memcpy(&length, reinterpret_cast<const uint8_t*>(packet) + offsetof(MIDIPacket, length), sizeof(length));
    return length;
}

struct IntelRule
{
    static MIDIPacket* next(const MIDIPacket* const packet) noexcept
    {
        return reinterpret_cast<MIDIPacket*>(reinterpret_cast<uintptr_t>(packet) + 10 + lengthOf(packet));
    }
};

struct ArmRule
{
    static MIDIPacket* next(const MIDIPacket* const packet) noexcept
    {
        return reinterpret_cast<MIDIPacket*>((reinterpret_cast<uintptr_t>(packet) + 10 + lengthOf(packet) + 3)
                                             & ~static_cast<uintptr_t>(3));
    }
};

struct Message
{
    uint32_t frame;
    uint32_t size;
    const uint8_t* bytes;
};

uint8_t gBytes[1 << 17];
uint32_t gBytesUsed = 0;
Message gMessages[2048];
uint32_t gCount = 0;

void start()
{
    gBytesUsed = gCount = 0;
}

void message(const uint32_t frame, const uint32_t size, const uint8_t first)
{
    uint8_t* const bytes = gBytes + gBytesUsed;
    bytes[0] = first;
    for (uint32_t i = 1; i < size; ++i)
        bytes[i] = static_cast<uint8_t>((i * 7 + size) & 0x7F);
    if (first == 0xF0 && size > 1)
        bytes[size - 1] = 0xF7;
    gMessages[gCount++] = {frame, size, bytes};
    gBytesUsed += size;
}

// Writes the messages into the list until it refuses one; returns how many it took.
template <class Rule>
uint32_t write(AUMidiPacketList<Rule>& list)
{
    list.clear();
    for (uint32_t i = 0; i < gCount; ++i)
        if (! list.add(gMessages[i].frame, gMessages[i].bytes, gMessages[i].size))
            return i;
    return gCount;
}

// Walks the list the way a host on the rule's machine does, from the bytes alone, and checks that it holds exactly the
// first `count` messages, in order, at their frames, without reading past `capacity` bytes.
template <class Rule>
bool read(const MIDIPacketList* const list, const uint32_t capacity, const uint32_t count, const char* const what)
{
    const uint8_t* const base = reinterpret_cast<const uint8_t*>(list);
    uint32_t numPackets;
    std::memcpy(&numPackets, base, sizeof(numPackets));
    if (numPackets != count)
    {
        d_stderr2("%s: %u packets, expected %u", what, numPackets, count);
        return false;
    }
    const MIDIPacket* packet = list->packet;
    for (uint32_t i = 0; i < count; ++i, packet = Rule::next(packet))
    {
        const uintptr_t start = reinterpret_cast<uintptr_t>(packet) - reinterpret_cast<uintptr_t>(base);
        uint64_t timeStamp;
        uint16_t length;
        if (start + 10 > capacity)
        {
            d_stderr2("%s: packet %u starts at byte %u, past the list's %u", what, i, static_cast<unsigned>(start),
                      capacity);
            return false;
        }
        std::memcpy(&timeStamp, base + start, sizeof(timeStamp));
        std::memcpy(&length, base + start + 8, sizeof(length));
        if (start + 10 + length > capacity || timeStamp != gMessages[i].frame || length != gMessages[i].size
            || std::memcmp(base + start + 10, gMessages[i].bytes, length) != 0)
        {
            d_stderr2("%s: packet %u at byte %u reads as %u bytes at frame %llu; message %u is %u bytes at frame %u",
                      what, i, static_cast<unsigned>(start), length, static_cast<unsigned long long>(timeStamp), i,
                      gMessages[i].size, gMessages[i].frame);
            return false;
        }
    }
    return true;
}

// The list's own constants against the layout the header documents.
static_assert(AUMidiPacketList<IntelRule>::kPacketHeaderSize == 10, "packet header");
static_assert(AUMidiPacketList<IntelRule>::kListHeaderSize == 4, "list header");
static_assert(AUMidiPacketList<IntelRule>::sizeFor(512, 4) == 4 + 16 * 512, "512 four-byte messages at ARM's stride");

constexpr uint32_t kListSize = AUMidiPacketList<IntelRule>::sizeFor(512, 4);

template <class Rule>
int testRule(const char* const rule)
{
    AUMidiPacketList<Rule> list(kListSize);
    DAF_ASSERT_NOT_EQUAL(list.list(), nullptr, "the list must be allocated");
    DAF_ASSERT_EQUAL(list.isEmpty(), true, "a new list must be empty");

    // Two messages on one frame: a host read the second as garbage.
    start();
    message(0, 3, 0x90);
    message(0, 3, 0x80);
    DAF_ASSERT_EQUAL(write(list) == 2 && read<Rule>(list.list(), kListSize, 2, rule), true,
                     "a block's second message must be where the host reads it");

    // Every length from 1 to 40 bytes, then long ones, each after a three-byte message, so that every packet start a
    // misplaced packet could take is crossed under either rule; frames rise.
    start();
    uint32_t frame = 0;
    for (uint32_t size = 1; size <= 40; ++size, ++frame)
    {
        message(frame, 3, 0x90);
        message(frame, size, size == 1 ? 0xF8 : size == 2 ? 0xC0 : size == 3 ? 0xB0 : 0xF0);
    }
    for (const uint32_t size : {255u, 256u, 257u, 301u, 1000u, 1001u})
    {
        message(frame, size, 0xF0);
        message(frame++, 2, 0xD1);
    }
    DAF_ASSERT_EQUAL(write(list) == gCount && read<Rule>(list.list(), kListSize, gCount, rule), true,
                     "messages of every length must be where the host reads them");

    // As many three-byte messages as the list takes: at least 512 (as many as a block can receive), the rest refused,
    // and what was taken still reads whole.
    start();
    for (uint32_t i = 0; i < 1024; ++i)
        message(i / 4, 3, static_cast<uint8_t>(0x90 | (i & 0x0F)));
    const uint32_t taken = write(list);
    DAF_ASSERT_EQUAL(taken >= 512 && taken < gCount && read<Rule>(list.list(), kListSize, taken, rule), true,
                     "the list must take at least 512 three-byte messages, refuse the next, and keep the rest whole");
    d_stdout("%s rule: a list of %u bytes takes %u three-byte messages", rule, kListSize, taken);

    // The longest message the list takes alone ends on its last byte; one byte more is refused, and so is anything
    // after a list that full. A bound off by one either way writes past the list or wastes its last byte.
    const uint32_t longest = kListSize - AUMidiPacketList<Rule>::kListHeaderSize
                           - AUMidiPacketList<Rule>::kPacketHeaderSize;
    start();
    message(0, longest, 0xF0);
    message(1, 1, 0xF8);
    DAF_ASSERT_EQUAL(write(list) == 1 && read<Rule>(list.list(), kListSize, 1, rule), true,
                     "a message ending on the list's last byte must be taken whole, and the next refused");
    start();
    message(0, longest + 1, 0xF0);
    DAF_ASSERT_EQUAL(write(list) == 0 && list.isEmpty(), true, "a message one byte too long must be refused");

    // A refused message leaves the list as it was.
    start();
    message(0, 3, 0x90);
    message(0, kListSize, 0xF0);
    DAF_ASSERT_EQUAL(write(list) == 1 && read<Rule>(list.list(), kListSize, 1, rule), true,
                     "a refused message must leave the list as it was");

    // clear() starts the next block's list.
    list.clear();
    DAF_ASSERT_EQUAL(list.isEmpty(), true, "a cleared list must be empty");
    return 0;
}

// The rules differ wherever a packet does not end on a 4-byte boundary: a list written under one must not read under
// the other. Without this, a reader that ignored its rule would pass both cases above.
int testRulesDiffer()
{
    AUMidiPacketList<ArmRule> arm(kListSize);
    AUMidiPacketList<IntelRule> intel(kListSize);
    start();
    message(0, 3, 0x90);
    message(0, 3, 0x80);
    write(arm);
    write(intel);
    // these print the mismatch they detect
    DAF_ASSERT_EQUAL(read<IntelRule>(arm.list(), kListSize, 2, "ARM list read as Intel (a mismatch is expected)"),
                     false,
                     "a list written under ARM's rule must not read under Intel's");
    DAF_ASSERT_EQUAL(read<ArmRule>(intel.list(), kListSize, 2, "Intel list read as ARM (a mismatch is expected)"),
                     false,
                     "a list written under Intel's rule must not read under ARM's");
    return 0;
}

// A list that cannot hold its header, or more than CoreMIDI's largest list, is refused outright.
int testCapacity()
{
    AUMidiPacketList<ArmRule> tooSmall(3);
    AUMidiPacketList<ArmRule> tooBig(65537);
    AUMidiPacketList<ArmRule> largest(65536);
    const uint8_t note[3] = {0x90, 60, 100};
    DAF_ASSERT_EQUAL(tooSmall.list() == nullptr && ! tooSmall.add(0, note, 3), true,
                     "a list without room for its header");
    DAF_ASSERT_EQUAL(tooBig.list() == nullptr && ! tooBig.add(0, note, 3), true,
                     "a list larger than CoreMIDI's largest");
    DAF_ASSERT_EQUAL(largest.list() != nullptr && largest.add(0, note, 3), true, "CoreMIDI's largest list");
    return 0;
}
}

int main()
{
    if (const int failed = testRule<ArmRule>("ARM"))
        return failed;
    if (const int failed = testRule<IntelRule>("Intel"))
        return failed;
    if (const int failed = testRulesDiffer())
        return failed;
    if (const int failed = testCapacity())
        return failed;
    d_stdout("AU MIDI packet list: both packet rules pass");
    return 0;
}
