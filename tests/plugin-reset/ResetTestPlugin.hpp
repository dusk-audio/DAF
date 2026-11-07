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

/* A plugin with DAF_PLUGIN_WANT_RESET, for testing the host resets of the format wrappers in-process. A *Reset test
 * defines DAF_PLUGIN_TARGET_<format>, includes this file and then daf/DafPluginMain.cpp, as the *Wrapper tests do
 * with WrapperTestPlugin.hpp, and main() acts as the host.
 *
 * Its tail never decays: run() outputs the loudest input it has had since its last reset(), so a reset that does not
 * reach the plugin leaves it ringing. activate() keeps the tail, as a reverb that holds on to its buffers across a
 * deactivation does, so deactivating and activating it clears nothing either. What the wrapper did to it, the plugin
 * counts in the globals below.
 */

#pragma once

#include "daf/DafPlugin.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

// Set while creating an instance to give its parameter the reset designation; otherwise the parameter is a plain
// trigger. A host reset of a plugin that has both must call reset() and leave the parameter alone.
static bool gResetTestDesignateParameter = false;

// Atomic: AUReset resets on one thread while another renders.
static std::atomic<uint32_t> gResets(0);             // reset() calls
static std::atomic<uint32_t> gResetsWhileActive(0);  // reset() calls made while the plugin was active
static std::atomic<uint32_t> gResetsOutsideRun(0);   // reset() calls made while no run() was running
static std::atomic<uint32_t> gRunsInsideReset(0);    // run() calls that started while a reset() was running
static std::atomic<uint32_t> gRuns(0);
static std::atomic<uint32_t> gActivations(0);
static std::atomic<uint32_t> gDeactivations(0);
static std::atomic<uint32_t> gParameterPresses(0);   // values of 0.5 or more given to the parameter
static std::atomic<uint32_t> gMidiEvents(0);         // MIDI events given to run()

// Whether a run() or a reset() is running. Each sets its own flag before it checks the other's, so of two that
// overlap at least one sees it.
static std::atomic<bool> gResetTestInRun(false);
static std::atomic<bool> gResetTestInReset(false);

// Set by a test to hold every run() and reset() open for that many microseconds (0: not at all), so that one starting
// inside the other is caught. A sleep rather than a spin, so that copies of a test running at once do not compete for
// the CPU through it.
static std::atomic<uint32_t> gResetTestRunDwell(0);
static std::atomic<uint32_t> gResetTestResetDwell(0);

static void resetTestDwell(const uint32_t microseconds)
{
    if (microseconds != 0)
        std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

class ResetTestPlugin : public Plugin
{
public:
    ResetTestPlugin()
        : Plugin(1, 0, 0),
          fDesignated(gResetTestDesignateParameter),
          fActive(false),
          fParameter(0.0f),
          fHeld(0.0f) {}

protected:
    const char* getLabel() const override { return "ResetTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initParameter(uint32_t, Parameter& parameter) override
    {
        if (fDesignated)
        {
            parameter.initDesignation(kParameterDesignationReset);
            return;
        }

        parameter.hints = kParameterIsAutomatable | kParameterIsTrigger;
        parameter.name = parameter.symbol = "trigger";
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 1.0f;
        parameter.ranges.def = 0.0f;
    }

    float getParameterValue(uint32_t) const override
    {
        return fParameter;
    }

    // Counts a press and nothing more: the tail stays, so a reset designation pressed in place of reset() shows.
    void setParameterValue(uint32_t, const float value) override
    {
        if (value >= 0.5f)
            ++gParameterPresses;
        fParameter = value;
    }

    void activate() override
    {
        fActive = true;
        ++gActivations;
    }

    void deactivate() override
    {
        fActive = false;
        ++gDeactivations;
    }

    void reset() override
    {
        gResetTestInReset = true;
        ++gResets;
        if (fActive)
            ++gResetsWhileActive;
        if (! gResetTestInRun)
            ++gResetsOutsideRun;
        resetTestDwell(gResetTestResetDwell);
        fHeld = 0.0f;
        gResetTestInReset = false;
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames,
             const MidiEvent*, const uint32_t midiEventCount) override
    {
        gResetTestInRun = true;
        if (gResetTestInReset)
            ++gRunsInsideReset;
        ++gRuns;
        gMidiEvents += midiEventCount;
        resetTestDwell(gResetTestRunDwell);

        for (uint32_t i = 0; i < frames; ++i)
        {
            fHeld = std::max(fHeld, std::fabs(inputs[0][i]));
            outputs[0][i] = fHeld;
        }

        gResetTestInRun = false;
    }

private:
    const bool fDesignated;
    std::atomic<bool> fActive;
    float fParameter;
    float fHeld;

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ResetTestPlugin)
};

Plugin* createPlugin()
{
    return new ResetTestPlugin();
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF
