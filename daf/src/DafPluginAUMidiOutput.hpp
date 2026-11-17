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

#ifndef DAF_PLUGIN_AU_MIDI_OUTPUT_HPP_INCLUDED
#define DAF_PLUGIN_AU_MIDI_OUTPUT_HPP_INCLUDED

#include "../DafUtils.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

/* MIDIPacket and MIDIPacketList must already be declared; on macOS that is <CoreMIDI/MIDIServices.h>, reached through
 * <AudioUnit/AudioUnit.h>. The unit test (tests/AUMidiPacketList.cpp) declares layout-compatible stand-ins, so the
 * layout is exercised on any platform, under each of CoreMIDI's packet rules. */

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

/**
   One render's MIDI output, laid out as the MIDIPacketList that kAudioUnitProperty_MIDIOutputCallback hands the host:
   numPackets, then the packets one after the other, each a header (timeStamp, length) and its bytes.

   A packet goes where the host reads the next one: where NextPacket::next() finds it after the packet before. The
   wrapper's rule is MIDIPacketNext itself, the reader's own: on ARM a packet starts on a 4-byte boundary, on Intel
   right after the last byte of the one before. sizeof(MIDIPacket) is no guide to the stride, as it counts the nominal
   256 data bytes and the struct's tail padding.

   One message per packet. A message that would end past the list is refused, and the list stays as it was. The
   storage is allocated with the object, never on the render thread.
 */
template <class NextPacket>
class AUMidiPacketList
{
public:
    // A packet's header (timeStamp, length) before its bytes, and the list's (numPackets) before its first packet.
    static constexpr const uint32_t kPacketHeaderSize = offsetof(MIDIPacket, data);
    static constexpr const uint32_t kListHeaderSize = offsetof(MIDIPacketList, packet);

    // The bytes a list needs for `count` messages of up to `size` bytes each, at the widest stride (ARM's).
    static constexpr uint32_t sizeFor(const uint32_t count, const uint32_t size) noexcept
    {
        return kListHeaderSize + ((kPacketHeaderSize + size + 3u) & ~3u) * count;
    }

    // At most 65536 bytes, CoreMIDI's largest packet list, so that a packet's length always fits its 16 bits.
    explicit AUMidiPacketList(const uint32_t capacity) noexcept
        : fList(capacity >= kListHeaderSize && capacity <= 65536
                ? static_cast<MIDIPacketList*>(std::calloc(1, capacity)) : nullptr),
          fLast(nullptr),
          fCapacity(fList != nullptr ? capacity : 0) {}

    ~AUMidiPacketList() noexcept
    {
        std::free(fList);
    }

    // The list to hand the host; null when it could not be allocated.
    const MIDIPacketList* list() const noexcept
    {
        return fList;
    }

    bool isEmpty() const noexcept
    {
        return fList == nullptr || fList->numPackets == 0;
    }

    void clear() noexcept
    {
        if (fList != nullptr)
            fList->numPackets = 0;
    }

    bool add(const uint32_t frame, const uint8_t* const data, const uint32_t size) noexcept
    {
        if (fList == nullptr)
            return false;

        MIDIPacket* const packet = fList->numPackets == 0 ? fList->packet : NextPacket::next(fLast);
        const uint64_t start = reinterpret_cast<uintptr_t>(packet) - reinterpret_cast<uintptr_t>(fList);
        if (start + kPacketHeaderSize + size > fCapacity)
            return false;

        // Byte by byte: on Intel a packet is not aligned for its type.
        uint8_t* const bytes = reinterpret_cast<uint8_t*>(packet);
        const decltype(MIDIPacket::timeStamp) timeStamp = frame;
        const decltype(MIDIPacket::length) length = static_cast<decltype(MIDIPacket::length)>(size);
        std::memcpy(bytes + offsetof(MIDIPacket, timeStamp), &timeStamp, sizeof(timeStamp));
        std::memcpy(bytes + offsetof(MIDIPacket, length), &length, sizeof(length));
        std::memcpy(bytes + kPacketHeaderSize, data, size);

        fLast = packet;
        ++fList->numPackets;
        return true;
    }

private:
    MIDIPacketList* const fList;
    MIDIPacket* fLast; // the packet add() wrote last, read only while the list holds one
    const uint32_t fCapacity;

    DAF_DECLARE_NON_COPYABLE(AUMidiPacketList)
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_PLUGIN_AU_MIDI_OUTPUT_HPP_INCLUDED
