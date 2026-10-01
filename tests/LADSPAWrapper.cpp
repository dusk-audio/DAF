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

/* The LADSPA wrapper compiled into this test, with main() as the host. Checks the default hints it
 * gives control ports, which a host turns back into values on the port's own scale. */

#define DAF_PLUGIN_TARGET_LADSPA
#define DAF_TEST_NO_DGL

#include "tests.hpp"
#include "plugin-wrappers/WrapperTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

// --------------------------------------------------------------------------------------------------------------------

static int portForParameter(const uint32_t index)
{
    return DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS + static_cast<int>(index);
}

// the value a host derives from a port's range hints, as ladspa.h specifies it
static double hostDefault(const LADSPA_PortRangeHint& hint)
{
    const LADSPA_PortRangeHintDescriptor desc = hint.HintDescriptor;
    const double lower = hint.LowerBound;
    const double upper = hint.UpperBound;
    const bool logarithmic = LADSPA_IS_HINT_LOGARITHMIC(desc);

    if (LADSPA_IS_HINT_DEFAULT_LOW(desc))
        return logarithmic ? std::exp(std::log(lower) * 0.75 + std::log(upper) * 0.25) : lower * 0.75 + upper * 0.25;
    if (LADSPA_IS_HINT_DEFAULT_MIDDLE(desc))
        return logarithmic ? std::exp(std::log(lower) * 0.5 + std::log(upper) * 0.5) : lower * 0.5 + upper * 0.5;
    if (LADSPA_IS_HINT_DEFAULT_HIGH(desc))
        return logarithmic ? std::exp(std::log(lower) * 0.25 + std::log(upper) * 0.75) : lower * 0.25 + upper * 0.75;
    return -1.0;
}

int main()
{
    USE_NAMESPACE_DAF;

    const LADSPA_Descriptor* const desc = ladspa_descriptor(0);
    DAF_ASSERT_NOT_EQUAL(desc, nullptr, "ladspa_descriptor must return the plugin");

    const LADSPA_PortRangeHint* const hints = desc->PortRangeHints;
    LADSPA_PortRangeHintDescriptor d;

    // 1..65535, default 4096: on the logarithmic scale the host uses, 4096 is exactly HIGH
    d = hints[portForParameter(kParamLogHigh)].HintDescriptor;
    DAF_ASSERT_NOT_EQUAL(LADSPA_IS_HINT_LOGARITHMIC(d), 0, "loghigh must be a logarithmic port");
    DAF_ASSERT_NOT_EQUAL(LADSPA_IS_HINT_DEFAULT_HIGH(d), 0, "loghigh default must be HIGH on a log scale");
    DAF_ASSERT_EQUAL(std::round(hostDefault(hints[portForParameter(kParamLogHigh)])), 4096.0,
                     "a host must compute 4096 for loghigh");

    // 20..20000, default 632: the geometric middle, which a linear choice would call LOW
    d = hints[portForParameter(kParamLogMiddle)].HintDescriptor;
    DAF_ASSERT_NOT_EQUAL(LADSPA_IS_HINT_DEFAULT_MIDDLE(d), 0, "logmiddle default must be MIDDLE on a log scale");
    DAF_ASSERT_EQUAL(std::round(hostDefault(hints[portForParameter(kParamLogMiddle)])), 632.0,
                     "a host must compute about 632 for logmiddle");

    // 0..10, default 3: nearest of the linear points 2.5, 5 and 7.5 is LOW
    d = hints[portForParameter(kParamLinearLow)].HintDescriptor;
    DAF_ASSERT_EQUAL(LADSPA_IS_HINT_LOGARITHMIC(d), 0, "linearlow must not be a logarithmic port");
    DAF_ASSERT_NOT_EQUAL(LADSPA_IS_HINT_DEFAULT_LOW(d), 0, "linearlow default must be LOW");

    d_stdout("LADSPA wrapper tests passed");
    return 0;
}
