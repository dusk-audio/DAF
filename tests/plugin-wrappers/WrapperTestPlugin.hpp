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

/* A plugin for testing format wrappers in-process. A *Wrapper test defines DAF_PLUGIN_TARGET_<format>,
 * includes this file and then daf/DafPluginMain.cpp, so the wrapper is compiled into the test and
 * main() can call its entry points directly, as a host would, without loading a binary.
 *
 * It reports what the wrapper handed it through output parameters, so a test reads them back through
 * the format's own API.
 */

#pragma once

#include "daf/DafPlugin.hpp"

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

enum WrapperTestParameters {
    kParamTrigger,      // trigger, default 0
    kParamLogHigh,      // logarithmic 1..65535, default 4096 = 65535^0.75 (HIGH on a log scale, LOW on a linear one)
    kParamLogMiddle,    // logarithmic 20..20000, default 632 ~= sqrt(20*20000) (MIDDLE on a log scale)
    kParamLinearLow,    // linear 0..10, default 3 (LOW)
   #if DAF_PLUGIN_WANT_TIMEPOS
    kParamOutFrame,
    kParamOutBar,
    kParamOutBeat,
    kParamOutTick,
    kParamOutBarStartTick,
   #endif
    kParamCount
};

#if DAF_PLUGIN_WANT_STATE
static constexpr const char* const kWrapperTestStateKey = "file";
#endif

#ifdef DAF_WRAPPER_TEST_UPDATE_STATE
/* A second state, which the plugin sets itself through updateStateValue() on every activate(),
 * to "active-1", "active-2" and so on. */
static constexpr const char* const kWrapperTestStatusKey = "status";

// what updateStateValue() returned where it was last called from
static bool gUpdateStateFromConstructor = true; // too early, must be false
static bool gUpdateStateFromInitState = true;   // too early, must be false
static bool gUpdateStateFromActivate = false;   // must be true
static bool gUpdateStateUnknownKey = true;      // must be false

static constexpr const uint32_t kWrapperTestStateCount = 2;
#elif DAF_PLUGIN_WANT_STATE
static constexpr const uint32_t kWrapperTestStateCount = 1;
#else
static constexpr const uint32_t kWrapperTestStateCount = 0;
#endif

class WrapperTestPlugin : public Plugin
{
public:
    WrapperTestPlugin()
        : Plugin(kParamCount, 0, kWrapperTestStateCount)
    {
        std::memset(fParameters, 0, sizeof(fParameters));
        fParameters[kParamLogHigh] = 4096.0f;
        fParameters[kParamLogMiddle] = 632.0f;
        fParameters[kParamLinearLow] = 3.0f;

       #ifdef DAF_WRAPPER_TEST_UPDATE_STATE
        fActivations = 0;
        gUpdateStateFromConstructor = updateStateValue(kWrapperTestStatusKey, "constructor");
       #endif
    }

protected:
    const char* getLabel() const override { return "WrapperTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        parameter.hints = kParameterIsAutomatable;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 1.0f;
        parameter.ranges.def = 0.0f;

        switch (index)
        {
        case kParamTrigger:
            parameter.hints |= kParameterIsTrigger;
            parameter.name = parameter.symbol = "trigger";
            break;
        case kParamLogHigh:
            parameter.hints |= kParameterIsLogarithmic;
            parameter.name = parameter.symbol = "loghigh";
            parameter.ranges.min = 1.0f;
            parameter.ranges.max = 65535.0f;
            parameter.ranges.def = 4096.0f;
            break;
        case kParamLogMiddle:
            parameter.hints |= kParameterIsLogarithmic;
            parameter.name = parameter.symbol = "logmiddle";
            parameter.ranges.min = 20.0f;
            parameter.ranges.max = 20000.0f;
            parameter.ranges.def = 632.0f;
            break;
        case kParamLinearLow:
            parameter.name = parameter.symbol = "linearlow";
            parameter.ranges.max = 10.0f;
            parameter.ranges.def = 3.0f;
            break;
       #if DAF_PLUGIN_WANT_TIMEPOS
        case kParamOutFrame:
            parameter.hints = kParameterIsOutput;
            parameter.name = parameter.symbol = "frame";
            parameter.ranges.max = 1e9f;
            break;
        case kParamOutBar:
            parameter.hints = kParameterIsOutput;
            parameter.name = parameter.symbol = "bar";
            parameter.ranges.max = 1e6f;
            break;
        case kParamOutBeat:
            parameter.hints = kParameterIsOutput;
            parameter.name = parameter.symbol = "beat";
            parameter.ranges.max = 64.0f;
            break;
        case kParamOutTick:
            parameter.hints = kParameterIsOutput;
            parameter.name = parameter.symbol = "tick";
            parameter.ranges.max = 1e6f;
            break;
        case kParamOutBarStartTick:
            parameter.hints = kParameterIsOutput;
            parameter.name = parameter.symbol = "barstarttick";
            parameter.ranges.max = 1e9f;
            break;
       #endif
        }
    }

   #if DAF_PLUGIN_WANT_STATE
    void initState(const uint32_t index, State& state) override
    {
       #ifdef DAF_WRAPPER_TEST_UPDATE_STATE
        if (index == 1)
        {
            state.key = kWrapperTestStatusKey;
            state.label = "Status";
            state.hints = kStateIsHostReadable;
            gUpdateStateFromInitState = updateStateValue(kWrapperTestStatusKey, "initState");
            return;
        }
       #endif

        state.key = kWrapperTestStateKey;
        state.label = "File";
        state.hints = kStateIsFilenamePath;
        return; (void)index;
    }

    void setState(const char*, const char*) override {}
   #endif

   #ifdef DAF_WRAPPER_TEST_UPDATE_STATE
    void activate() override
    {
        char status[32];
        std::snprintf(status, sizeof(status), "active-%u", ++fActivations);
        gUpdateStateFromActivate = updateStateValue(kWrapperTestStatusKey, status);
        gUpdateStateUnknownKey = updateStateValue("unknown", status);
    }
   #endif

    float getParameterValue(const uint32_t index) const override
    {
        return fParameters[index];
    }

    void setParameterValue(const uint32_t index, const float value) override
    {
        fParameters[index] = value;
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        if (outputs[0] != inputs[0])
            std::memcpy(outputs[0], inputs[0], sizeof(float)*frames);

       #if DAF_PLUGIN_WANT_TIMEPOS
        const TimePosition& timePos(getTimePosition());
        fParameters[kParamOutFrame] = static_cast<float>(timePos.frame);
        fParameters[kParamOutBar] = static_cast<float>(timePos.bbt.bar);
        fParameters[kParamOutBeat] = static_cast<float>(timePos.bbt.beat);
        fParameters[kParamOutTick] = static_cast<float>(timePos.bbt.tick);
        fParameters[kParamOutBarStartTick] = static_cast<float>(timePos.bbt.barStartTick);
       #endif
    }

private:
    float fParameters[kParamCount];
   #ifdef DAF_WRAPPER_TEST_UPDATE_STATE
    uint32_t fActivations;
   #endif

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(WrapperTestPlugin)
};

Plugin* createPlugin()
{
    return new WrapperTestPlugin();
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF
