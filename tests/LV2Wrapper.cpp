/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2025 Filipe Coelho <falktx@falktx.com>
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

/* The LV2 wrapper compiled into this test, with main() as the host. Covers the wrapper's own time
 * extrapolation between time:Position updates. */

#define DAF_PLUGIN_TARGET_LV2
#define DAF_TEST_NO_DGL

#include "tests.hpp"
#include "plugin-wrappers/WrapperTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

#include <string>
#include <vector>

// --------------------------------------------------------------------------------------------------------------------

static std::vector<std::string> gUris;

static LV2_URID uridMap(LV2_URID_Map_Handle, const char* const uri)
{
    for (size_t i = 0; i < gUris.size(); ++i)
        if (gUris[i] == uri)
            return static_cast<LV2_URID>(i + 1);

    gUris.push_back(uri);
    return static_cast<LV2_URID>(gUris.size());
}

static LV2_URID_Map gMap = { nullptr, uridMap };

// the worker runs scheduled work when the test says so, as a worker thread would between two runs
static std::vector<uint8_t> gScheduledWork;

static LV2_Worker_Status scheduleWork(LV2_Worker_Schedule_Handle, const uint32_t size, const void* const data)
{
    const uint8_t* const bytes = static_cast<const uint8_t*>(data);
    gScheduledWork.assign(bytes, bytes + size);
    return LV2_WORKER_SUCCESS;
}

static constexpr const double kSampleRate = 48000.0;
static constexpr const uint32_t kFrames = 12000; // half a beat at 120 bpm
static constexpr const uint32_t kAtomCapacity = 8192;

// --------------------------------------------------------------------------------------------------------------------

struct Host {
    const LV2_Descriptor* desc;
    LV2_Handle handle;
    LV2_Atom_Forge forge;

    float audioIn[kFrames];
    float audioOut[kFrames];
    float controls[kParamCount];
    alignas(8) uint8_t eventsIn[kAtomCapacity];
    alignas(8) uint8_t eventsOut[kAtomCapacity];

    LV2_Atom_Sequence* eventsInSeq() { return reinterpret_cast<LV2_Atom_Sequence*>(eventsIn); }
    LV2_Atom_Sequence* eventsOutSeq() { return reinterpret_cast<LV2_Atom_Sequence*>(eventsOut); }

    void beginEvents(LV2_Atom_Forge_Frame& frame)
    {
        lv2_atom_forge_set_buffer(&forge, eventsIn, sizeof(eventsIn));
        lv2_atom_forge_sequence_head(&forge, &frame, 0);
    }

    void run()
    {
        LV2_Atom_Sequence* const out = eventsOutSeq();
        out->atom.type = uridMap(nullptr, LV2_ATOM__Chunk);
        out->atom.size = kAtomCapacity - sizeof(LV2_Atom);

        desc->run(handle, kFrames);

        // nothing more to send unless the next step forges something
        LV2_Atom_Forge_Frame frame;
        beginEvents(frame);
        lv2_atom_forge_pop(&forge, &frame);
    }
};

// --------------------------------------------------------------------------------------------------------------------

static int testBarExtrapolation(Host& host)
{
    // 4/4 at 120 bpm, a host that sends the position once and then only on change:
    // bar 1 (0-based 0), half way through its 4th beat
    LV2_Atom_Forge& forge(host.forge);
    LV2_Atom_Forge_Frame seqFrame, objFrame;
    host.beginEvents(seqFrame);
    lv2_atom_forge_frame_time(&forge, 0);
    lv2_atom_forge_object(&forge, &objFrame, 0, uridMap(nullptr, LV2_TIME__Position));
    lv2_atom_forge_key(&forge, uridMap(nullptr, LV2_TIME__speed));
    lv2_atom_forge_float(&forge, 1.0f);
    lv2_atom_forge_key(&forge, uridMap(nullptr, LV2_TIME__bar));
    lv2_atom_forge_long(&forge, 0);
    lv2_atom_forge_key(&forge, uridMap(nullptr, LV2_TIME__barBeat));
    lv2_atom_forge_float(&forge, 3.5f);
    lv2_atom_forge_key(&forge, uridMap(nullptr, LV2_TIME__beatUnit));
    lv2_atom_forge_int(&forge, 4);
    lv2_atom_forge_key(&forge, uridMap(nullptr, LV2_TIME__beatsPerBar));
    lv2_atom_forge_float(&forge, 4.0f);
    lv2_atom_forge_key(&forge, uridMap(nullptr, LV2_TIME__beatsPerMinute));
    lv2_atom_forge_float(&forge, 120.0f);
    lv2_atom_forge_key(&forge, uridMap(nullptr, LV2_TIME__frame));
    lv2_atom_forge_long(&forge, 0);
    lv2_atom_forge_pop(&forge, &objFrame);
    lv2_atom_forge_pop(&forge, &seqFrame);

    // the bar must change on the same block as the beat wraps, never before it
    static const struct { float bar, beat; } expected[] = {
        { 1, 4 }, { 2, 1 }, { 2, 1 }, { 2, 2 }, { 2, 2 }, { 2, 3 }, { 2, 3 }, { 2, 4 },
        { 2, 4 }, { 3, 1 }, { 3, 1 }, { 3, 2 },
    };

    for (size_t i = 0; i < sizeof(expected)/sizeof(expected[0]); ++i)
    {
        host.run();

        if (d_isNotEqual(host.controls[kParamOutBar], expected[i].bar) ||
            d_isNotEqual(host.controls[kParamOutBeat], expected[i].beat))
        {
            d_stderr2("block %u: got bar %g beat %g, expected bar %g beat %g", static_cast<unsigned>(i),
                      host.controls[kParamOutBar], host.controls[kParamOutBeat], expected[i].bar, expected[i].beat);
            DAF_ASSERT_EQUAL(true, false, "bar and beat must advance together");
        }

        // 1920 ticks per beat by default, 4 beats per bar
        DAF_ASSERT_SAFE_EQUAL(host.controls[kParamOutBarStartTick], (expected[i].bar - 1) * 4 * 1920,
                              "barStartTick must match the bar");
    }

    return 0;
}

int main()
{
    USE_NAMESPACE_DAF;

    static Host host;
    std::memset(&host, 0, sizeof(host));
    lv2_atom_forge_init(&host.forge, &gMap);

    host.desc = lv2_descriptor(0);
    DAF_ASSERT_NOT_EQUAL(host.desc, nullptr, "lv2_descriptor must return the plugin");

    const int32_t blockLength = kFrames;
    const LV2_Options_Option options[] = {
        { LV2_OPTIONS_INSTANCE, 0, uridMap(nullptr, LV2_BUF_SIZE__maxBlockLength),
          sizeof(int32_t), uridMap(nullptr, LV2_ATOM__Int), &blockLength },
        { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr },
    };
    LV2_Worker_Schedule schedule = { nullptr, scheduleWork };

    const LV2_Feature mapFeature = { LV2_URID__map, &gMap };
    const LV2_Feature optionsFeature = { LV2_OPTIONS__options, const_cast<LV2_Options_Option*>(options) };
    const LV2_Feature workerFeature = { LV2_WORKER__schedule, &schedule };
    const LV2_Feature* const features[] = { &mapFeature, &optionsFeature, &workerFeature, nullptr };

    host.handle = host.desc->instantiate(host.desc, kSampleRate, "", features);
    DAF_ASSERT_NOT_EQUAL(host.handle, nullptr, "instantiate must succeed");

    // audio in, audio out, events in, events out, then one port per parameter
    uint32_t port = 0;
    host.desc->connect_port(host.handle, port++, host.audioIn);
    host.desc->connect_port(host.handle, port++, host.audioOut);
    host.desc->connect_port(host.handle, port++, host.eventsIn);
    host.desc->connect_port(host.handle, port++, host.eventsOut);
    for (uint32_t i = 0; i < kParamCount; ++i)
        host.desc->connect_port(host.handle, port++, &host.controls[i]);

    host.controls[kParamLogHigh] = 4096.0f;
    host.controls[kParamLogMiddle] = 632.0f;
    host.controls[kParamLinearLow] = 3.0f;

    host.desc->activate(host.handle);

    if (const int ret = testBarExtrapolation(host))
        return ret;

    host.desc->deactivate(host.handle);
    host.desc->cleanup(host.handle);

    d_stdout("LV2 wrapper tests passed");
    return 0;
}
