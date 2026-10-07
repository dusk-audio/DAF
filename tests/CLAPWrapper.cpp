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

/* The CLAP wrapper compiled into this test, with main() as the host. TimePosition::frame is the
 * transport position: it must follow the transport timeline, not the free-running steady_time.
 *
 * Plugin::updateStateValue() from activate() must ask for a main-thread callback, be in a state saved
 * before that callback, mark the state dirty from the callback unless a save or load got there first,
 * and never overwrite a state loaded after it.
 *
 * Output parameters are reported read-only, and parameter events outside the core event space change
 * nothing, whether they come through process() or clap_plugin_params::flush(). */

#define DAF_PLUGIN_TARGET_CLAP
#define DAF_TEST_NO_DGL

#include "tests.hpp"
#include "plugin-wrappers/WrapperTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

#include <algorithm>
#include <string>

// --------------------------------------------------------------------------------------------------------------------

static int gCallbackRequests = 0;
static int gMarkDirtyCalls = 0;

static void CLAP_ABI host_mark_dirty(const clap_host_t*) { ++gMarkDirtyCalls; }
static const clap_host_state_t gHostState = { host_mark_dirty };

static const void* CLAP_ABI host_get_extension(const clap_host_t*, const char* const id)
{
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &gHostState;
    return nullptr;
}

static void CLAP_ABI host_request(const clap_host_t*) {}
static void CLAP_ABI host_request_callback(const clap_host_t*) { ++gCallbackRequests; }

static int64_t CLAP_ABI ostream_write(const clap_ostream_t* const stream, const void* const buffer, const uint64_t size)
{
    static_cast<std::string*>(stream->ctx)->append(static_cast<const char*>(buffer), size);
    return static_cast<int64_t>(size);
}

struct IStream {
    const std::string* data;
    std::size_t pos;
};

static int64_t CLAP_ABI istream_read(const clap_istream_t* const stream, void* const buffer, const uint64_t size)
{
    IStream* const in = static_cast<IStream*>(stream->ctx);
    const std::size_t count = std::min<std::size_t>(size, in->data->size() - in->pos);
    std::memcpy(buffer, in->data->data() + in->pos, count);
    in->pos += count;
    return static_cast<int64_t>(count);
}

// the saved state, which holds keys and values as null-terminated strings
static std::string saveState(const clap_plugin_t* const plugin, const clap_plugin_state_t* const state)
{
    std::string data;
    const clap_ostream_t stream = { &data, ostream_write };
    if (! state->save(plugin, &stream))
        return std::string();
    return data;
}

static bool loadState(const clap_plugin_t* const plugin, const clap_plugin_state_t* const state, const std::string& data)
{
    IStream in = { &data, 0 };
    const clap_istream_t stream = { &in, istream_read };
    return state->load(plugin, &stream);
}

static bool hasStatus(const std::string& data, const char* const value)
{
    return data.find(std::string(kWrapperTestStatusKey) + '\0' + value + '\0') != std::string::npos;
}

static uint32_t CLAP_ABI in_events_size(const clap_input_events_t*) { return 0; }
static const clap_event_header_t* CLAP_ABI in_events_get(const clap_input_events_t*, uint32_t) { return nullptr; }
// A trigger's return to its default is a value change the host has to see: a host that is told the
// parameter is still pressed may act on it again. Counted, with the value last reported, so the
// trigger tests can assert both that the reset arrived and that it carried the default.
static uint32_t gTriggerReports = 0;
static double gTriggerLastReport = -1.0;

static bool CLAP_ABI out_events_try_push(const clap_output_events_t*, const clap_event_header_t* const event)
{
    if (event->type == CLAP_EVENT_PARAM_VALUE)
    {
        const clap_event_param_value_t* const param = reinterpret_cast<const clap_event_param_value_t*>(event);
        if (param->param_id == kParamTrigger)
        {
            ++gTriggerReports;
            gTriggerLastReport = param->value;
        }
    }
    return true;
}

// an input event list holding a single parameter value event
static uint32_t CLAP_ABI param_event_size(const clap_input_events_t*) { return 1; }
static const clap_event_header_t* CLAP_ABI param_event_get(const clap_input_events_t* const list, const uint32_t index)
{
    return index == 0 ? &static_cast<const clap_event_param_value_t*>(list->ctx)->header : nullptr;
}

static clap_event_param_value_t makeParamEvent(const uint16_t spaceId, const clap_id paramId, const double value)
{
    clap_event_param_value_t event = {};
    event.header.size = sizeof(event);
    event.header.space_id = spaceId;
    event.header.type = CLAP_EVENT_PARAM_VALUE;
    event.param_id = paramId;
    event.note_id = event.port_index = event.channel = event.key = -1;
    event.value = value;
    return event;
}

// any namespace other than CLAP_CORE_EVENT_SPACE_ID, the one clap-validator uses
static constexpr const uint16_t kForeignSpaceId = 0xb33f;

static constexpr const double kSampleRate = 48000.0;
static constexpr const uint32_t kFrames = 64;

struct Host {
    const clap_plugin_t* plugin;
    const clap_plugin_params_t* params;
    int64_t steadyTime;

    // runs one block with the given transport and returns the frame the plugin was given
    double process(const clap_event_transport_t& transport, const clap_input_events_t* const events = nullptr)
    {
        float input[kFrames] = {};
        float output[kFrames] = {};
        float* inputs[1] = { input };
        float* outputs[1] = { output };

        clap_audio_buffer_t audioIn = { inputs, nullptr, 1, 0, 0 };
        clap_audio_buffer_t audioOut = { outputs, nullptr, 1, 0, 0 };
        const clap_input_events_t inEvents = { nullptr, in_events_size, in_events_get };
        const clap_output_events_t outEvents = { nullptr, out_events_try_push };

        clap_process_t process = {};
        process.steady_time = steadyTime;
        process.frames_count = kFrames;
        process.transport = &transport;
        process.audio_inputs = &audioIn;
        process.audio_outputs = &audioOut;
        process.audio_inputs_count = 1;
        process.audio_outputs_count = 1;
        process.in_events = events != nullptr ? events : &inEvents;
        process.out_events = &outEvents;

        plugin->process(plugin, &process);
        steadyTime += kFrames;

        double frame = -1.0;
        params->get_value(plugin, kParamOutFrame, &frame);
        return frame;
    }

    double getValue(const clap_id paramId) const
    {
        double value = -1.0;
        params->get_value(plugin, paramId, &value);
        return value;
    }

    void flush(const clap_event_param_value_t& event) const
    {
        const clap_input_events_t in = { const_cast<clap_event_param_value_t*>(&event), param_event_size, param_event_get };
        const clap_output_events_t out = { nullptr, out_events_try_push };
        params->flush(plugin, &in, &out);
    }
};

static clap_event_transport_t makeTransport(const uint32_t flags)
{
    clap_event_transport_t transport = {};
    transport.header.size = sizeof(transport);
    transport.header.type = CLAP_EVENT_TRANSPORT;
    transport.flags = flags;
    transport.tempo = 120.0;
    transport.tsig_num = 4;
    transport.tsig_denom = 4;
    return transport;
}

int main()
{
    USE_NAMESPACE_DAF;

    const clap_host_t host = {
        CLAP_VERSION, nullptr, "DAF tests", "DAF", "", "1.0",
        host_get_extension, host_request, host_request, host_request_callback
    };

    DAF_ASSERT_EQUAL(clap_entry.init(""), true, "clap_entry.init must succeed");

    const clap_plugin_factory_t* const factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    DAF_ASSERT_NOT_EQUAL(factory, nullptr, "the plugin factory must be available");

    Host h;
    h.plugin = factory->create_plugin(factory, &host, DAF_PLUGIN_CLAP_ID);
    DAF_ASSERT_NOT_EQUAL(h.plugin, nullptr, "create_plugin must succeed");
    DAF_ASSERT_EQUAL(h.plugin->init(h.plugin), true, "plugin init must succeed");

    h.params = static_cast<const clap_plugin_params_t*>(h.plugin->get_extension(h.plugin, CLAP_EXT_PARAMS));
    DAF_ASSERT_NOT_EQUAL(h.params, nullptr, "the params extension must be available");

    // output parameters are read-only, inputs are not
    DAF_ASSERT_EQUAL(h.params->count(h.plugin), static_cast<uint32_t>(kParamCount), "every parameter must be exposed");
    for (uint32_t i = 0; i < kParamCount; ++i)
    {
        clap_param_info_t info = {};
        DAF_ASSERT_EQUAL(h.params->get_info(h.plugin, i, &info), true, "get_info must succeed");
        DAF_ASSERT_EQUAL((info.flags & CLAP_PARAM_IS_READONLY) != 0, i >= kParamOutFrame,
                         "exactly the output parameters must be read-only");
    }

    const clap_plugin_state_t* const state =
        static_cast<const clap_plugin_state_t*>(h.plugin->get_extension(h.plugin, CLAP_EXT_STATE));
    DAF_ASSERT_NOT_EQUAL(state, nullptr, "the state extension must be available");

    DAF_ASSERT_EQUAL(gUpdateStateFromConstructor, false, "updateStateValue must fail from the plugin constructor");
    DAF_ASSERT_EQUAL(gUpdateStateFromInitState, false, "updateStateValue must fail from initState");

    // the plugin updates its state in activate(), the wrapper asks for the main thread to finish
    DAF_ASSERT_EQUAL(h.plugin->activate(h.plugin, kSampleRate, 1, kFrames), true, "activate must succeed");
    DAF_ASSERT_EQUAL(gUpdateStateFromActivate, true, "updateStateValue must succeed from activate");
    DAF_ASSERT_EQUAL(gUpdateStateUnknownKey, false, "updateStateValue must fail for an unknown key");
    DAF_ASSERT_NOT_EQUAL(gCallbackRequests, 0, "updateStateValue must request a main-thread callback");

    // a save before that callback already has the value, and leaves nothing to mark dirty
    DAF_ASSERT_EQUAL(hasStatus(saveState(h.plugin, state), "active-1"), true, "a saved state must have the update");
    h.plugin->on_main_thread(h.plugin);
    DAF_ASSERT_EQUAL(gMarkDirtyCalls, 0, "an update the host saved already must not mark the state dirty");

    DAF_ASSERT_EQUAL(h.plugin->start_processing(h.plugin), true, "start_processing must succeed");

    // --- triggers ---------------------------------------------------------------------------------
    //
    // A trigger is an action, not a state. The press is delivered to the plugin: to setParameterValue,
    // where a plugin that acts as it arrives consumes it, and in the parameter's own value, where a
    // plugin that reads it on its next run consumes it. Then the value returns to its default and the
    // host is told, so that a host does not take the parameter for still pressed. Both entry points
    // must leave the same observable state -- that is what clap-validator's flush-versus-process
    // comparison and its state-reproducibility tests check, because the values they compare are read
    // back through get_value and a trigger is never part of a saved state.
    {
        const clap_event_transport_t stopped = makeTransport(0);

        // through process(): the run in the same call presents the press
        clap_event_param_value_t press = makeParamEvent(CLAP_CORE_EVENT_SPACE_ID, kParamTrigger, 1.0);
        const clap_input_events_t pressList = { &press, param_event_size, param_event_get };
        h.process(stopped, &pressList);
        DAF_ASSERT_EQUAL(gTriggerSetterPresses, 1, "a process press must reach setParameterValue");
        DAF_ASSERT_EQUAL(gTriggerRunPresses, 1, "a process press must reach the run in the same call");
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamTrigger), 0.0,
                              "the host must read a trigger at its default after the run");
        DAF_ASSERT_EQUAL(gTriggerReports, 1, "the reset to the default must be reported to the host");
        DAF_ASSERT_SAFE_EQUAL(gTriggerLastReport, 0.0, "the reported reset must carry the trigger's default");

        // through flush(): there is no run in this call to present the press to, so it waits for the
        // next one. The press reached the setter immediately -- that part is the plugin's to act on --
        // and the host still reads the default, because a pulse is not a value the host can hold.
        h.flush(makeParamEvent(CLAP_CORE_EVENT_SPACE_ID, kParamTrigger, 1.0));
        DAF_ASSERT_EQUAL(gTriggerSetterPresses, 2, "a flush press must reach setParameterValue");
        DAF_ASSERT_EQUAL(gTriggerRunPresses, 1, "flush has no run to present the press to");
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamTrigger), 0.0, "flush must leave the trigger at its default");

        h.process(stopped);
        DAF_ASSERT_EQUAL(gTriggerRunPresses, 2, "the run after a flush press must see it");
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamTrigger), 0.0, "the run after a flush press must leave the default");
        DAF_ASSERT_EQUAL(gTriggerReports, 2, "the flush press's reset must be reported exactly once");
        DAF_ASSERT_SAFE_EQUAL(gTriggerLastReport, 0.0, "the flush press's reset must carry the default");
    }

    // steady_time has been counting for a while, and has nothing to do with the song position
    h.steadyTime = 1000000;

    const uint32_t timelines = CLAP_TRANSPORT_HAS_TEMPO|CLAP_TRANSPORT_HAS_BEATS_TIMELINE|
                               CLAP_TRANSPORT_HAS_SECONDS_TIMELINE|CLAP_TRANSPORT_HAS_TIME_SIGNATURE;

    // playing at 1 s
    clap_event_transport_t transport = makeTransport(timelines|CLAP_TRANSPORT_IS_PLAYING);
    transport.song_pos_seconds = 1 * CLAP_SECTIME_FACTOR;
    transport.song_pos_beats = 2 * CLAP_BEATTIME_FACTOR;
    DAF_ASSERT_SAFE_EQUAL(h.process(transport), 48000.0, "frame must follow song_pos_seconds");

    // stopped there: steady_time keeps going, the frame must not
    transport.flags = timelines;
    DAF_ASSERT_SAFE_EQUAL(h.process(transport), 48000.0, "frame must hold while the transport is stopped");
    DAF_ASSERT_SAFE_EQUAL(h.process(transport), 48000.0, "frame must hold while the transport is stopped");

    // located to 2.5 s
    transport.song_pos_seconds = 5 * CLAP_SECTIME_FACTOR / 2;
    transport.song_pos_beats = 5 * CLAP_BEATTIME_FACTOR;
    DAF_ASSERT_SAFE_EQUAL(h.process(transport), 120000.0, "frame must jump on a locate");

    // only a beats timeline: 4 beats at 120 bpm is 2 s
    transport = makeTransport(CLAP_TRANSPORT_HAS_TEMPO|CLAP_TRANSPORT_HAS_BEATS_TIMELINE|CLAP_TRANSPORT_IS_PLAYING);
    transport.song_pos_beats = 4 * CLAP_BEATTIME_FACTOR;
    DAF_ASSERT_SAFE_EQUAL(h.process(transport), 96000.0, "frame must be derived from beats and tempo");

    // no timeline at all
    transport = makeTransport(CLAP_TRANSPORT_IS_PLAYING);
    DAF_ASSERT_SAFE_EQUAL(h.process(transport), 0.0, "frame must be 0 without a timeline");

    // a parameter event in a foreign namespace through process() changes nothing, a core one does
    {
        clap_event_param_value_t event = makeParamEvent(kForeignSpaceId, kParamLinearLow, 7.0);
        const clap_input_events_t in = { &event, param_event_size, param_event_get };
        h.process(transport, &in);
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamLinearLow), 3.0, "process must ignore events outside the core space");

        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        h.process(transport, &in);
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamLinearLow), 7.0, "process must apply core parameter events");
    }

    // nor a core one for an output parameter. run() rewrites the outputs every block, so their value
    // cannot show it; the plugin counts any write that reached it instead
    {
        const uint32_t writesBefore = gOutputParameterWrites;
        clap_event_param_value_t event = makeParamEvent(CLAP_CORE_EVENT_SPACE_ID, kParamOutFrame, 5.0);
        const clap_input_events_t in = { &event, param_event_size, param_event_get };
        h.process(transport, &in);
        DAF_ASSERT_EQUAL(gOutputParameterWrites, writesBefore, "process must ignore events for output parameters");
    }

    h.plugin->stop_processing(h.plugin);
    h.plugin->deactivate(h.plugin);

    // the same through clap_plugin_params::flush(), here on the main thread
    h.flush(makeParamEvent(kForeignSpaceId, kParamLinearLow, 3.0));
    DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamLinearLow), 7.0, "flush must ignore events outside the core space");
    h.flush(makeParamEvent(CLAP_CORE_EVENT_SPACE_ID, kParamLinearLow, 3.0));
    DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamLinearLow), 3.0, "flush must apply core parameter events");

    // and the host cannot set an output parameter at all
    {
        const uint32_t writesBefore = gOutputParameterWrites;
        h.flush(makeParamEvent(CLAP_CORE_EVENT_SPACE_ID, kParamOutFrame, 5.0));
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamOutFrame), 0.0, "flush must ignore events for output parameters");
        DAF_ASSERT_EQUAL(gOutputParameterWrites, writesBefore, "flush must not set an output parameter");
    }

    // the main-thread callback marks the state dirty, and a save has the value
    DAF_ASSERT_EQUAL(h.plugin->activate(h.plugin, kSampleRate, 1, kFrames), true, "activate must succeed");
    h.plugin->on_main_thread(h.plugin);
    DAF_ASSERT_EQUAL(gMarkDirtyCalls, 1, "an update must mark the state dirty from the main-thread callback");
    const std::string saved = saveState(h.plugin, state);
    DAF_ASSERT_EQUAL(hasStatus(saved, "active-2"), true, "a saved state must have the update");
    h.plugin->deactivate(h.plugin);

    // an update made before a load must not overwrite the loaded state, nor mark it dirty
    DAF_ASSERT_EQUAL(h.plugin->activate(h.plugin, kSampleRate, 1, kFrames), true, "activate must succeed");
    DAF_ASSERT_EQUAL(loadState(h.plugin, state, saved), true, "state load must succeed");
    h.plugin->on_main_thread(h.plugin);
    DAF_ASSERT_EQUAL(gMarkDirtyCalls, 1, "an update before a load must not mark the state dirty");
    DAF_ASSERT_EQUAL(hasStatus(saveState(h.plugin, state), "active-2"), true,
                     "an update before a load must not overwrite the loaded state");
    h.plugin->deactivate(h.plugin);

    // The same press while the plugin is deactivated: flush has no run to present it to, and the host
    // still reads the default. A plugin that acts in its setter has already acted; one that reads the
    // value on its next run gets it there, because the pulse stays pending until a run can consume it.
    // Nothing about the two calls differs from the active case.
    {
        const uint32_t setterPresses = gTriggerSetterPresses;
        const uint32_t runPresses = gTriggerRunPresses;
        const uint32_t reports = gTriggerReports;

        h.flush(makeParamEvent(CLAP_CORE_EVENT_SPACE_ID, kParamTrigger, 1.0));
        DAF_ASSERT_EQUAL(gTriggerSetterPresses, setterPresses + 1,
                         "a flush press while deactivated must reach setParameterValue");
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamTrigger), 0.0,
                              "a flush press while deactivated must leave the trigger at its default");

        DAF_ASSERT_EQUAL(h.plugin->activate(h.plugin, kSampleRate, 1, kFrames), true, "activate must succeed");
        DAF_ASSERT_EQUAL(h.plugin->start_processing(h.plugin), true, "start_processing must succeed");
        h.process(makeTransport(0));
        DAF_ASSERT_EQUAL(gTriggerRunPresses, runPresses + 1,
                         "the run after a deactivated flush press must see it");
        DAF_ASSERT_SAFE_EQUAL(h.getValue(kParamTrigger), 0.0,
                              "the run after a deactivated flush press must leave the default");
        DAF_ASSERT_EQUAL(gTriggerReports, reports + 1,
                         "the deactivated flush press's reset must be reported exactly once");
        DAF_ASSERT_SAFE_EQUAL(gTriggerLastReport, 0.0,
                              "the deactivated flush press's reset must carry the default");
        h.plugin->stop_processing(h.plugin);
        h.plugin->deactivate(h.plugin);
    }

    h.plugin->destroy(h.plugin);
    clap_entry.deinit();

    d_stdout("CLAP wrapper tests passed");
    return 0;
}
