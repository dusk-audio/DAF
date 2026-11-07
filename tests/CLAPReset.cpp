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

/* Plugin::reset() through the CLAP wrapper compiled into this test, with main() as the host. clap_plugin::reset is
 * [audio-thread & active], and the plugin (plugin-reset/, DAF_PLUGIN_WANT_RESET) clears its state right there. The
 * wrapper used to ask the host for a restart instead, which CLAP lets a host put off: nothing cleared until the host
 * got round to it, and this one never does.
 *
 * - clap reset calls Plugin::reset() once, while the plugin is active and between process calls. The host is not
 *   asked for a restart, the plugin is neither deactivated nor activated, and the next block is silent.
 * - With a reset designation as well, clap reset still calls Plugin::reset(), and leaves the parameter alone.
 * - A host that resets a deactivated plugin, which CLAP does not allow, does not reach Plugin::reset().
 *
 * CLAPWrapper checks that a plugin without DAF_PLUGIN_WANT_RESET or a reset designation still asks the host for the
 * restart. */

#define DAF_PLUGIN_TARGET_CLAP
#define DAF_TEST_NO_DGL

#include "tests.hpp"
#include "plugin-reset/ResetTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

// --------------------------------------------------------------------------------------------------------------------

static int gRestartRequests = 0;

static const void* CLAP_ABI host_get_extension(const clap_host_t*, const char*) { return nullptr; }
static void CLAP_ABI host_request_restart(const clap_host_t*) { ++gRestartRequests; }
static void CLAP_ABI host_request(const clap_host_t*) {}

static uint32_t CLAP_ABI in_events_size(const clap_input_events_t*) { return 0; }
static const clap_event_header_t* CLAP_ABI in_events_get(const clap_input_events_t*, uint32_t) { return nullptr; }
static bool CLAP_ABI out_events_try_push(const clap_output_events_t*, const clap_event_header_t*) { return true; }

static constexpr const double kSampleRate = 48000.0;
static constexpr const uint32_t kFrames = 64;

struct Instance {
    const clap_plugin_t* plugin;
    int64_t steadyTime;

    // Runs one block whose first sample is `impulse`, the rest silence, and returns its last output sample: what the
    // plugin holds after it. -1 if the block failed.
    float process(const float impulse)
    {
        float input[kFrames] = {};
        float output[kFrames] = {};
        input[0] = impulse;
        float* inputs[1] = { input };
        float* outputs[1] = { output };

        clap_audio_buffer_t audioIn = { inputs, nullptr, 1, 0, 0 };
        clap_audio_buffer_t audioOut = { outputs, nullptr, 1, 0, 0 };
        const clap_input_events_t inEvents = { nullptr, in_events_size, in_events_get };
        const clap_output_events_t outEvents = { nullptr, out_events_try_push };

        clap_process_t process = {};
        process.steady_time = steadyTime;
        process.frames_count = kFrames;
        process.audio_inputs = &audioIn;
        process.audio_outputs = &audioOut;
        process.audio_inputs_count = 1;
        process.audio_outputs_count = 1;
        process.in_events = &inEvents;
        process.out_events = &outEvents;

        if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR)
            return -1.0f;

        steadyTime += kFrames;
        return output[kFrames - 1];
    }
};

int main()
{
    USE_NAMESPACE_DAF;

    const clap_host_t host = {
        CLAP_VERSION, nullptr, "DAF tests", "DAF", "", "1.0",
        host_get_extension, host_request_restart, host_request, host_request
    };

    DAF_ASSERT_EQUAL(clap_entry.init(""), true, "clap_entry.init must succeed");

    const clap_plugin_factory_t* const factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    DAF_ASSERT_NOT_EQUAL(factory, nullptr, "the plugin factory must be available");

    // the plugin clears its own tail, and the host is asked for nothing
    {
        Instance instance = { factory->create_plugin(factory, &host, DAF_PLUGIN_CLAP_ID), 0 };
        const clap_plugin_t* const plugin = instance.plugin;
        DAF_ASSERT_NOT_EQUAL(plugin, nullptr, "create_plugin must succeed");
        DAF_ASSERT_EQUAL(plugin->init(plugin), true, "plugin init must succeed");
        DAF_ASSERT_EQUAL(plugin->activate(plugin, kSampleRate, 1, kFrames), true, "activate must succeed");
        DAF_ASSERT_EQUAL(plugin->start_processing(plugin), true, "start_processing must succeed");

        DAF_ASSERT_EQUAL(instance.process(0.25f), 0.25f, "an impulse must start the tail");
        DAF_ASSERT_EQUAL(instance.process(0.0f), 0.25f, "the tail must ring on in silence");

        const int restarts = gRestartRequests;
        const uint32_t activations = gActivations.load(), deactivations = gDeactivations.load();
        plugin->reset(plugin);
        DAF_ASSERT_EQUAL(gResets.load(), 1u, "clap reset must call Plugin::reset() once");
        DAF_ASSERT_EQUAL(gResetsWhileActive.load(), 1u, "Plugin::reset() must be called while the plugin is active");
        DAF_ASSERT_EQUAL(gResetsOutsideRun.load(), 1u, "Plugin::reset() must be called between process calls");
        DAF_ASSERT_EQUAL(gRestartRequests, restarts, "clap reset must not ask the host for a restart");
        DAF_ASSERT_EQUAL(gActivations.load() == activations && gDeactivations.load() == deactivations, true,
                         "clap reset must neither deactivate nor activate the plugin");
        DAF_ASSERT_EQUAL(instance.process(0.0f), 0.0f, "the block after clap reset must be silent");
        DAF_ASSERT_EQUAL(instance.process(0.5f), 0.5f, "the plugin must play on after clap reset");

        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);

        const uint32_t resets = gResets.load();
        plugin->reset(plugin);
        DAF_ASSERT_EQUAL(gResets.load(), resets, "a reset of a deactivated plugin must not reach Plugin::reset()");

        plugin->destroy(plugin);
    }

    // a reset designation as well: Plugin::reset() still clears the tail, and the parameter is left alone
    {
        gResetTestDesignateParameter = true;
        Instance instance = { factory->create_plugin(factory, &host, DAF_PLUGIN_CLAP_ID), 0 };
        gResetTestDesignateParameter = false;
        const clap_plugin_t* const plugin = instance.plugin;
        DAF_ASSERT_NOT_EQUAL(plugin, nullptr, "create_plugin with a reset designation must succeed");
        DAF_ASSERT_EQUAL(plugin->init(plugin), true, "plugin init with a reset designation must succeed");

        const clap_plugin_params_t* const params =
            static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        DAF_ASSERT_NOT_EQUAL(params, nullptr, "the params extension must be available");
        clap_param_info_t info = {};
        DAF_ASSERT_EQUAL(params->get_info(plugin, 0, &info) && std::strcmp(info.module, "daf_reset") == 0, true,
                         "the parameter must carry the reset designation");

        DAF_ASSERT_EQUAL(plugin->activate(plugin, kSampleRate, 1, kFrames), true, "activate must succeed");
        DAF_ASSERT_EQUAL(plugin->start_processing(plugin), true, "start_processing must succeed");
        DAF_ASSERT_EQUAL(instance.process(0.25f), 0.25f, "an impulse must start the tail");

        const uint32_t resets = gResets.load(), presses = gParameterPresses.load();
        const int restarts = gRestartRequests;
        plugin->reset(plugin);
        DAF_ASSERT_EQUAL(gResets.load(), resets + 1, "with a reset designation, clap reset must call Plugin::reset()");
        DAF_ASSERT_EQUAL(gParameterPresses.load(), presses, "clap reset must leave a reset designation alone");
        DAF_ASSERT_EQUAL(gRestartRequests, restarts, "with a reset designation, clap reset must not ask for a restart");
        DAF_ASSERT_EQUAL(instance.process(0.0f), 0.0f, "with a reset designation, the block after clap reset must be silent");

        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }

    clap_entry.deinit();

    d_stdout("CLAP reset tests passed");
    return 0;
}
