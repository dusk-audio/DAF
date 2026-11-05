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

// The AU wrapper's latency notification, with main() as the host. A plugin may move its latency from run()
// (Plugin::setLatency()); hosts learn of it through kAudioUnitProperty_Latency listeners, and Logic keeps its delay
// compensation at the old value until one fires. The wrapper only answered queries, so a host that listens never
// heard of a change. Rendering on a thread of its own, as a host does: the listener must fire on the main thread
// once render has seen the latency move, report the new value when the host reads it, fire again when it moves
// back, and stay quiet while it does not move, and once the unit is uninitialized or disposed, even with a move
// still pending.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <pthread.h>
#include <thread>

START_NAMESPACE_DAF

class LatencyPlugin : public Plugin
{
public:
    LatencyPlugin() : Plugin(1, 0, 0) {}

protected:
    const char* getLabel() const override { return "LatencyTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initParameter(const uint32_t, Parameter& parameter) override
    {
        parameter.hints = kParameterIsAutomatable | kParameterIsInteger;
        parameter.name = "Latency";
        parameter.symbol = "latency";
        parameter.ranges.min = 0.0f;
        parameter.ranges.max = 512.0f;
        parameter.ranges.def = 0.0f;
    }

    float getParameterValue(uint32_t) const override { return fLatency; }
    void setParameterValue(uint32_t, const float value) override { fLatency = value; }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        std::memcpy(outputs[0], inputs[0], sizeof(float) * frames);
        setLatency(static_cast<uint32_t>(fLatency));
    }

private:
    float fLatency = 0.0f;
};

Plugin* createPlugin()
{
    return new LatencyPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
int gNotifications = 0;
bool gNotifiedOffMain = false;

void latencyListener(void* const count, AudioUnit, const AudioUnitPropertyID property, const AudioUnitScope scope,
                     const AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency || scope != kAudioUnitScope_Global)
        return;
    if (pthread_main_np() == 0)
        gNotifiedOffMain = true;
    ++*static_cast<int*>(count);
}

OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
        std::memset(data->mBuffers[ch].mData, 0, sizeof(float) * frames);
    return noErr;
}

// One render on a thread of its own, as a host's audio thread renders.
OSStatus render(AudioUnit unit)
{
    OSStatus status = noErr;
    std::thread thread([unit, &status] {
        float samples[16] = {};
        AudioBufferList output = {1, {{1, sizeof(samples), samples}}};
        AudioTimeStamp timestamp = {};
        timestamp.mFlags = kAudioTimeStampSampleTimeValid;
        AudioUnitRenderActionFlags flags = 0;
        status = AudioUnitRender(unit, &flags, &timestamp, 0, 16, &output);
    });
    thread.join();
    return status;
}

// Runs the main run loop until the listener count passes `count` or a second has gone by.
int spinUntilAbove(const int count)
{
    for (int i = 0; i < 100 && gNotifications <= count; ++i)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    return gNotifications;
}

double latencySeconds(AudioUnit unit)
{
    Float64 seconds = -1.0;
    UInt32 size = sizeof(seconds);
    AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds, &size);
    return seconds;
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU Latency Test"), 0x10000, factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    AudioUnit unit = nullptr;
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &unit), noErr, "test AU must instantiate");

    AURenderCallbackStruct callback = {feed, nullptr};
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                          &callback, sizeof(callback)), noErr, "the input must connect");
    DAF_ASSERT_EQUAL(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_Latency, latencyListener, &gNotifications),
                     noErr, "a latency listener must register");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    Float64 sampleRate = 0.0;
    UInt32 size = sizeof(sampleRate);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0,
                                          &sampleRate, &size), noErr, "the sample rate must read");

    // At its initial latency the unit renders without a notification.
    DAF_ASSERT_EQUAL(render(unit), noErr, "the unit must render");
    DAF_ASSERT_EQUAL(spinUntilAbove(0), 0, "an unchanged latency must not notify");

    // run() moves the latency to 100 samples: one notification, on the main thread, and the host reads 100.
    DAF_ASSERT_EQUAL(AudioUnitSetParameter(unit, 0, kAudioUnitScope_Global, 0, 100.0f, 0), noErr,
                     "the latency parameter must set");
    DAF_ASSERT_EQUAL(render(unit), noErr, "the unit must render at the new latency");
    DAF_ASSERT_EQUAL(spinUntilAbove(0), 1, "a latency moved in run() must notify the host once");
    DAF_ASSERT_EQUAL(gNotifiedOffMain, false, "the latency listener must be called on the main thread");
    DAF_ASSERT_EQUAL(latencySeconds(unit), 100.0 / sampleRate, "the host must read the new latency");

    // Further renders at the same latency stay quiet; moving it back notifies again.
    DAF_ASSERT_EQUAL(render(unit), noErr, "the unit must keep rendering");
    DAF_ASSERT_EQUAL(spinUntilAbove(1), 1, "a latency that stays put must not notify again");
    DAF_ASSERT_EQUAL(AudioUnitSetParameter(unit, 0, kAudioUnitScope_Global, 0, 0.0f, 0), noErr,
                     "the latency parameter must set back");
    DAF_ASSERT_EQUAL(render(unit), noErr, "the unit must render at the old latency");
    DAF_ASSERT_EQUAL(spinUntilAbove(1), 2, "a latency moved back must notify again");
    DAF_ASSERT_EQUAL(latencySeconds(unit), 0.0, "the host must read the latency moved back");

    // A move still pending when the unit is uninitialized is not told: the unit has left the watch.
    DAF_ASSERT_EQUAL(AudioUnitSetParameter(unit, 0, kAudioUnitScope_Global, 0, 200.0f, 0), noErr,
                     "the latency parameter must set before uninitializing");
    DAF_ASSERT_EQUAL(render(unit), noErr, "the unit must render at the pending latency");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(spinUntilAbove(2), 2, "an uninitialized unit must not notify");

    // The same for a disposed unit (a watch that kept it would call into freed memory).
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize again");
    const int beforeDispose = spinUntilAbove(2);   // the re-initialized unit tells the move it was not told
    DAF_ASSERT_EQUAL(AudioUnitSetParameter(unit, 0, kAudioUnitScope_Global, 0, 300.0f, 0), noErr,
                     "the latency parameter must set before disposing");
    DAF_ASSERT_EQUAL(render(unit), noErr, "the unit must render at the latency pending at dispose");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(unit), noErr, "test AU must dispose");
    DAF_ASSERT_EQUAL(spinUntilAbove(beforeDispose), beforeDispose, "a disposed unit must not notify");
    return 0;
}
