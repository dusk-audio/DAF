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

/* The VST2 wrapper compiled into this test, with main() as the host. VST2 has no trigger parameters,
 * so the wrapper resets a fired trigger itself and must tell the host the value it reset it to.
 *
 * Plugin::updateStateValue() from activate() must be in a chunk saved before any editor idle, and must not
 * overwrite a chunk loaded after it. */

#define DAF_PLUGIN_TARGET_VST2
#define DAF_TEST_NO_DGL

// On Linux the wrapper also exports its entry point under the symbol "main", for old hosts, unless
// built for a web view UI. That would clash with this test's main(), and this plugin has no UI.
#if defined(__linux__) && !defined(DGL_USE_WEB_VIEW)
# define DGL_USE_WEB_VIEW
#endif

#include "tests.hpp"
#include "plugin-wrappers/WrapperTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

#include <string>
#include <vector>

// --------------------------------------------------------------------------------------------------------------------

struct Automation {
    int32_t index;
    float value;
};

static std::vector<Automation> gAutomations;

static intptr_t hostCallback(vst_effect*, const VST_HOST_OPCODE opcode, const int32_t index,
                             int64_t, void*, const float opt)
{
    switch (opcode)
    {
    case VST_HOST_OPCODE_00: // automate
        gAutomations.push_back({ index, opt });
        return 0;
    case VST_HOST_OPCODE_01: // version
        return 2400;
    case VST_HOST_OPCODE_10: // sample rate
        return 48000;
    case VST_HOST_OPCODE_11: // buffer size
        return 64;
    default:
        return 0;
    }
}

// whether the chunk the plugin saves has the status state at this value
static bool chunkHasStatus(vst_effect* const effect, const char* const value)
{
    void* chunk = nullptr;
    const intptr_t size = effect->control(effect, VST_EFFECT_OPCODE_17, 0, 0, &chunk, 0.0f);
    if (size <= 0 || chunk == nullptr)
        return false;

    const std::string data(static_cast<const char*>(chunk), static_cast<std::size_t>(size));
    return data.find(std::string(kWrapperTestStatusKey) + '\0' + value + '\0') != std::string::npos;
}

int main()
{
    USE_NAMESPACE_DAF;

    vst_effect* const effect = const_cast<vst_effect*>(VSTPluginMain(hostCallback));
    DAF_ASSERT_NOT_EQUAL(effect, nullptr, "VSTPluginMain must return an effect");

    effect->control(effect, VST_EFFECT_OPCODE_CREATE, 0, 0, nullptr, 0.0f);
    effect->control(effect, VST_EFFECT_OPCODE_SET_SAMPLE_RATE, 0, 0, nullptr, 48000.0f);
    effect->control(effect, VST_EFFECT_OPCODE_SET_BLOCK_SIZE, 0, 64, nullptr, 0.0f);
    effect->control(effect, VST_EFFECT_OPCODE_SUSPEND, 0, 1, nullptr, 0.0f);

    float input[64] = {};
    float output[64] = {};
    const float* inputs[1] = { input };
    float* outputs[1] = { output };

    // press the trigger, as host automation or a generic editor would
    effect->set_parameter(effect, kParamTrigger, 1.0f);
    gAutomations.clear();

    effect->process_float(effect, inputs, outputs, 64);

    bool reported = false;
    for (const Automation& a : gAutomations)
    {
        if (a.index != kParamTrigger)
            continue;
        reported = true;
        DAF_ASSERT_SAFE_EQUAL(a.value, 0.0f, "a reset trigger must be reported with its default value");
    }
    DAF_ASSERT_EQUAL(reported, true, "a reset trigger must be reported to the host");
    DAF_ASSERT_SAFE_EQUAL(effect->get_parameter(effect, kParamTrigger), 0.0f, "the trigger must read back as reset");

    // nothing left to reset, so the next block must not report it again
    gAutomations.clear();
    effect->process_float(effect, inputs, outputs, 64);

    for (const Automation& a : gAutomations)
        DAF_ASSERT_NOT_EQUAL(a.index, static_cast<int32_t>(kParamTrigger), "an idle trigger must not be reported");

    // the plugin updated its state in activate(); with no editor idle yet, a chunk must still have it
    DAF_ASSERT_EQUAL(gUpdateStateFromConstructor, false, "updateStateValue must fail from the plugin constructor");
    DAF_ASSERT_EQUAL(gUpdateStateFromInitState, false, "updateStateValue must fail from initState");
    DAF_ASSERT_EQUAL(gUpdateStateFromActivate, true, "updateStateValue must succeed from activate");
    DAF_ASSERT_EQUAL(gUpdateStateUnknownKey, false, "updateStateValue must fail for an unknown key");
    DAF_ASSERT_EQUAL(chunkHasStatus(effect, "active-1"), true, "a saved chunk must have the update");

    // an update made before a chunk load must not overwrite the loaded state
    effect->control(effect, VST_EFFECT_OPCODE_SUSPEND, 0, 0, nullptr, 0.0f);
    effect->control(effect, VST_EFFECT_OPCODE_SUSPEND, 0, 1, nullptr, 0.0f);
    static const char loadedChunk[] = "status\0loaded\0";
    effect->control(effect, VST_EFFECT_OPCODE_18, 0, sizeof(loadedChunk), const_cast<char*>(loadedChunk), 0.0f);
    DAF_ASSERT_EQUAL(chunkHasStatus(effect, "loaded"), true, "an update before a load must not overwrite it");

    effect->control(effect, VST_EFFECT_OPCODE_SUSPEND, 0, 0, nullptr, 0.0f);
    effect->control(effect, VST_EFFECT_OPCODE_DESTROY, 0, 0, nullptr, 0.0f);

    d_stdout("VST2 wrapper tests passed");
    return 0;
}
