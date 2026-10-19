/*
 * Dusk Audio Framework (DAF)
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

// The LV2 exporter writes every program to presets.ttl. It loaded the programs one after another
// into a single instance, so a program that does not set every parameter, as a program belonging
// to one of a plugin's modes does not, came out with whatever the programs before it had set:
// Multi-Comp 2's FET preset carried an earlier Bus preset's threshold. Here program 2 sets only
// "b", and must be exported with "a" at its default, in its ports and in its state alike.

#define DAF_PLUGIN_TARGET_LV2
#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "daf/DafPlugin.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

START_NAMESPACE_DAF

// Two parameters, and two programs that each set only their own one.
class PresetExportTestPlugin : public Plugin
{
public:
    PresetExportTestPlugin()
        : Plugin(2, 2, 1) {}

protected:
    const char* getLabel() const override { return "PresetExportTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        parameter.hints = kParameterIsAutomatable;
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 1.0f;
        parameter.ranges.def = 0.0f;
        parameter.name = parameter.symbol = index == 0 ? "a" : "b";
    }

    void initProgramName(const uint32_t index, String& name) override
    {
        name = index == 0 ? "Sets a" : "Sets b";
    }

    void loadProgram(const uint32_t index) override
    {
        fValues[index] = 1.0f;
    }

    void initState(const uint32_t, State& state) override
    {
        state.key = "values";
        state.label = "Values";
        state.hints = kStateIsHostReadable;
    }

    String getState(const char*) const override
    {
        char text[32];
        std::snprintf(text, sizeof(text), "a=%g;b=%g", fValues[0], fValues[1]);
        return String(text);
    }

    void setState(const char*, const char*) override {}

    float getParameterValue(const uint32_t index) const override { return fValues[index]; }
    void setParameterValue(const uint32_t index, const float value) override { fValues[index] = value; }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        std::memcpy(outputs[0], inputs[0], sizeof(float) * frames);
    }

private:
    float fValues[2] = {};
};

Plugin* createPlugin()
{
    return new PresetExportTestPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"

// --------------------------------------------------------------------------------------------------------------------

static std::string readFile(const char* const path)
{
    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

// The text of preset `number`'s block in presets.ttl, up to the next preset.
static std::string presetBlock(const std::string& ttl, const int number)
{
    char subject[32];
    std::snprintf(subject, sizeof(subject), "#preset%03d>", number);
    const size_t start = ttl.find(subject);
    if (start == std::string::npos)
        return std::string();
    const size_t end = ttl.find("#preset", start + std::strlen(subject));
    return ttl.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The pset:value a preset block gives the port `symbol`, as written.
static std::string portValue(const std::string& block, const char* const symbol)
{
    const std::string key = std::string("lv2:symbol \"") + symbol + "\"";
    const size_t at = block.find(key);
    if (at == std::string::npos)
        return "missing";
    const size_t value = block.find("pset:value ", at);
    if (value == std::string::npos)
        return "missing";
    const size_t begin = value + std::strlen("pset:value ");
    return block.substr(begin, block.find(' ', begin) - begin);
}

int main()
{
    char directory[] = "/tmp/daf-lv2-preset-export-XXXXXX";
    DAF_ASSERT_NOT_EQUAL(mkdtemp(directory), nullptr, "a temporary directory");
    char previous[4096];
    DAF_ASSERT_NOT_EQUAL(getcwd(previous, sizeof(previous)), nullptr, "the working directory");
    DAF_ASSERT_EQUAL(chdir(directory), 0, "into the temporary directory");

    lv2_generate_ttl("LV2PresetExport");
    const std::string ttl = readFile("presets.ttl");

    for (const char* const file : { "manifest.ttl", "LV2PresetExport.ttl", "presets.ttl" })
        std::remove(file);
    DAF_ASSERT_EQUAL(chdir(previous), 0, "back to the working directory");
    rmdir(directory);

    const std::string first = presetBlock(ttl, 1), second = presetBlock(ttl, 2);
    DAF_ASSERT_EQUAL(first.empty() || second.empty(), false, "both programs are exported");

    DAF_ASSERT_EQUAL(portValue(first, "a"), std::string("1"), "program 1 sets a");
    DAF_ASSERT_EQUAL(portValue(first, "b"), std::string("0"), "program 1 leaves b at its default");
    DAF_ASSERT_EQUAL(portValue(second, "b"), std::string("1"), "program 2 sets b");
    DAF_ASSERT_EQUAL(portValue(second, "a"), std::string("0"), "program 2 is exported without program 1's a");

    DAF_ASSERT_NOT_EQUAL(first.find("\"a=1;b=0\""), std::string::npos, "program 1's state");
    DAF_ASSERT_NOT_EQUAL(second.find("\"a=0;b=1\""), std::string::npos, "program 2's state, without program 1's a");

    return 0;
}
