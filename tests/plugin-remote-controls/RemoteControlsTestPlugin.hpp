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

/* A plugin with DAF_PLUGIN_WANT_REMOTE_CONTROLS, for testing the CLAP wrapper's remote controls in-process.
 * CLAPRemoteControls defines DAF_PLUGIN_TARGET_CLAP, includes this file and then daf/DafPluginMain.cpp, and main()
 * acts as the host.
 *
 * Five input parameters (0 to 4) and one output parameter (5), laid out on three pages:
 *   page 0  "Main"  / "Performance"  2, 0, -, 4, 1, -, -, 3    (order and empty controls kept)
 *   page 1  "Main"  / "Checks"       5, 99, 0                  (an output and a missing index, both emptied)
 *   page 2  "Other" / ""             3                         (an unnamed page)
 * initRemoteControlPage() returns false from page 3 on, and counts every call it gets per index.
 */

#pragma once

#include "daf/DafPlugin.hpp"

#include <atomic>

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

static constexpr const uint32_t kRemoteControlsTestParameters = 6;   // 0..4 inputs, 5 an output
static constexpr const uint32_t kRemoteControlsTestPages = 3;
static constexpr const uint32_t kRemoteControlsTestCountedIndexes = 8;

// initRemoteControlPage() calls per page index, and calls for an index past those counted.
static std::atomic<uint32_t> gPageInits[kRemoteControlsTestCountedIndexes];
static std::atomic<uint32_t> gPageInitsPast(0);

class RemoteControlsTestPlugin : public Plugin
{
public:
    RemoteControlsTestPlugin()
        : Plugin(kRemoteControlsTestParameters, 0, 0)
    {
        for (uint32_t i = 0; i < kRemoteControlsTestParameters; ++i)
            fValues[i] = 0.0f;
    }

protected:
    const char* getLabel() const override { return "RemoteControlsTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        static const char* const kSymbols[kRemoteControlsTestParameters] = { "p0", "p1", "p2", "p3", "p4", "meter" };

        parameter.hints = index == 5 ? kParameterIsOutput : kParameterIsAutomatable;
        parameter.name = parameter.symbol = kSymbols[index];
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 1.0f;
        parameter.ranges.def = 0.0f;
    }

    bool initRemoteControlPage(const uint32_t index, RemoteControlPage& page) override
    {
        if (index < kRemoteControlsTestCountedIndexes)
            ++gPageInits[index];
        else
            ++gPageInitsPast;

        switch (index)
        {
        case 0:
            page.section = "Main";
            page.name = "Performance";
            page.parameters[0] = 2;
            page.parameters[1] = 0;
            page.parameters[3] = 4;
            page.parameters[4] = 1;
            page.parameters[7] = 3;
            return true;
        case 1:
            page.section = "Main";
            page.name = "Checks";
            page.parameters[0] = 5;    // an output parameter
            page.parameters[1] = 99;   // past the last parameter
            page.parameters[2] = 0;
            return true;
        case 2:
            page.section = "Other";
            page.parameters[0] = 3;
            return true;
        default:
            return false;
        }
    }

    float getParameterValue(const uint32_t index) const override
    {
        return index < kRemoteControlsTestParameters ? fValues[index] : 0.0f;
    }

    void setParameterValue(const uint32_t index, const float value) override
    {
        if (index < kRemoteControlsTestParameters)
            fValues[index] = value;
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        for (uint32_t i = 0; i < frames; ++i)
            outputs[0][i] = inputs[0][i];
    }

private:
    float fValues[kRemoteControlsTestParameters];

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(RemoteControlsTestPlugin)
};

Plugin* createPlugin()
{
    return new RemoteControlsTestPlugin();
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF
