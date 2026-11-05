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

// Reconfigurations against render in the AU wrapper, with main() as the host. Connecting or disconnecting the
// sidechain of an initialized unit (DAF_PLUGIN_EXTRA_IO), changing its sample rate and resetting it deactivate and
// activate the plugin on the host's thread, and a host does those while render runs (choosing a sidechain source
// during playback). Nothing kept the two apart, so run() could see a plugin half way through a reconfiguration.
//
// - A render thread renders without pause while the main thread connects and disconnects the sidechain 2000 times
//   through a render callback and 500 times through a connection to another unit, changes the sample rate 500 times
//   through each of SampleRate and StreamFormat and resets the unit 500 times; the plugin counts every run() that
//   starts inside a reconfiguration. None may, and render must keep running.
// - A block that meets a reconfiguration is silent, in the host's buffers and in the unit's own when the host asks
//   for them, and more frames than the unit's buffers hold is an error, not a write past them.
// - A host property listener that sets the output format from inside the notification of an input format change,
//   on the thread making it, must not wait for itself (it hung). Listeners now hear of a reconfiguration once it is
//   over, so a listener that waits for another thread, which resets the unit (a host handing the change to its
//   engine thread), completes as well: inside the reconfiguration the two waited for each other.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <atomic>
#include <csignal>
#include <thread>
#include <unistd.h>

START_NAMESPACE_DAF

static std::atomic<bool> gReconfiguring(false);
static std::atomic<int> gOverlaps(0), gRuns(0), gReconfigurations(0);

// Set by the silent-block check: deactivate() renders from inside the reconfiguration it is part of.
static void (*gInsideReconfiguration)() = nullptr;

// Holds the reconfiguration open for a while, so a run() that slips into it is caught.
static void dwell()
{
    for (volatile int i = 0; i < 20000; i = i + 1) {}
}

class IoRenderPlugin : public Plugin
{
public:
    IoRenderPlugin() : Plugin(0, 0, 0) {}

protected:
    const char* getLabel() const override { return "IoRenderTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initAudioPort(const bool input, const uint32_t index, AudioPort& port) override
    {
        Plugin::initAudioPort(input, index, port);
        if (input && index >= 2)
        {
            port.hints |= kAudioPortIsSidechain;
            port.groupId = 1;
        }
        else
        {
            port.groupId = 0;
        }
    }

    void initPortGroup(const uint32_t groupId, PortGroup& group) override
    {
        group.name = group.symbol = groupId == 0 ? "main" : "sidechain";
    }

    void deactivate() override
    {
        gReconfiguring.store(true);
        dwell();
        if (gInsideReconfiguration != nullptr)
            gInsideReconfiguration();
    }

    void ioChanged(uint16_t, uint16_t) override
    {
        dwell();
    }

    void activate() override
    {
        dwell();
        gReconfiguring.store(false);
        gReconfigurations.fetch_add(1);
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        if (gReconfiguring.load())
            gOverlaps.fetch_add(1);
        gRuns.fetch_add(1);
        for (uint32_t i = 0; i < frames; ++i)
            outputs[0][i] = outputs[1][i] = inputs[0][i] + 1.0f;
    }
};

Plugin* createPlugin()
{
    return new IoRenderPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
AudioUnit gUnit = nullptr;

OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
        std::memset(data->mBuffers[ch].mData, 0, sizeof(float) * frames);
    return noErr;
}

OSStatus connect(AudioUnit unit, const UInt32 bus, const bool on)
{
    AURenderCallbackStruct callback = {};
    if (on)
        callback.inputProc = feed;
    return AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, bus,
                                &callback, sizeof(callback));
}

AudioStreamBasicDescription format(const double sampleRate, const UInt32 channels)
{
    AudioStreamBasicDescription desc = {};
    desc.mSampleRate = sampleRate;
    desc.mFormatID = kAudioFormatLinearPCM;
    desc.mFormatFlags = static_cast<AudioFormatFlags>(kAudioFormatFlagsNativeFloatPacked)
        | static_cast<AudioFormatFlags>(kAudioFormatFlagIsNonInterleaved);
    desc.mBytesPerPacket = desc.mBytesPerFrame = sizeof(float);
    desc.mFramesPerPacket = 1;
    desc.mChannelsPerFrame = channels;
    desc.mBitsPerChannel = 32;
    return desc;
}

OSStatus setStreamRate(AudioUnit unit, const Float64 sampleRate)
{
    // The output first: the plugin changes rate once the main input matches it.
    const AudioStreamBasicDescription desc = format(sampleRate, 2);
    OSStatus status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0,
                                           &desc, sizeof(desc));
    if (status == noErr)
        status = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                      &desc, sizeof(desc));
    return status;
}

OSStatus connectUnit(AudioUnit unit, AudioUnit source)
{
    AudioUnitConnection connection = {source, 0, 1};
    return AudioUnitSetProperty(unit, kAudioUnitProperty_MakeConnection, kAudioUnitScope_Input, 1,
                                &connection, sizeof(connection));
}

OSStatus setSampleRate(AudioUnit unit, const Float64 sampleRate)
{
    // The output first: the plugin changes rate once the main input matches it.
    OSStatus status = AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0,
                                           &sampleRate, sizeof(sampleRate));
    if (status == noErr)
        status = AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0,
                                      &sampleRate, sizeof(sampleRate));
    return status;
}

struct Output
{
    UInt32 numberBuffers;
    AudioBuffer buffers[2];
};

OSStatus render(AudioUnit unit, Output& output, const UInt32 frames)
{
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    return AudioUnitRender(unit, &flags, &timestamp, 0, frames, reinterpret_cast<AudioBufferList*>(&output));
}

// Runs `change` `count` times on this thread while another renders without pause; returns its failures.
template <class Change>
int whileRendering(AudioUnit unit, const int count, Change&& change, int& renderErrors)
{
    std::atomic<bool> stop(false);
    std::atomic<int> errors(0);
    std::thread renderer([&] {
        float left[64] = {}, right[64] = {};
        Output output = {2, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
        while (! stop.load())
        {
            output.buffers[0].mData = left;
            output.buffers[1].mData = right;
            if (render(unit, output, 64) != noErr)
                errors.fetch_add(1);
        }
    });
    int failures = 0;
    for (int i = 0; i < count; ++i)
        failures += change(i) != noErr ? 1 : 0;
    stop.store(true);
    renderer.join();
    renderErrors = errors.load();
    return failures;
}

// What a render made from inside a reconfiguration returned and wrote.
struct SilentBlock
{
    OSStatus hostBuffers = -1, ownBuffers = -1, oversized = -1, sameThread = -1;
    bool hostSilent = false, ownSilent = false, ownProvided = false, ran = true;
    UInt32 maxFrames = 0;
} gSilent;

void renderInsideReconfiguration()
{
    gInsideReconfiguration = nullptr;   // once
    const int runs = gRuns.load();
    // From another thread, as the host's audio thread would: the reconfiguration holds the unit.
    std::thread([] {
        float left[64], right[64];
        for (int i = 0; i < 64; ++i)
            left[i] = right[i] = 0.5f;
        Output host = {2, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
        gSilent.hostBuffers = render(gUnit, host, 64);
        gSilent.hostSilent = true;
        for (int i = 0; i < 64; ++i)
            gSilent.hostSilent = gSilent.hostSilent && left[i] == 0.0f && right[i] == 0.0f;

        Output own = {2, {{1, sizeof(float) * 64, nullptr}, {1, sizeof(float) * 64, nullptr}}};
        gSilent.ownBuffers = render(gUnit, own, 64);
        gSilent.ownProvided = own.buffers[0].mData != nullptr && own.buffers[1].mData != nullptr;
        gSilent.ownSilent = gSilent.ownProvided;
        for (int i = 0; gSilent.ownProvided && i < 64; ++i)
            gSilent.ownSilent = gSilent.ownSilent && static_cast<const float*>(own.buffers[0].mData)[i] == 0.0f
                && static_cast<const float*>(own.buffers[1].mData)[i] == 0.0f;

        const UInt32 frames = gSilent.maxFrames + 1, bytes = static_cast<UInt32>(sizeof(float)) * frames;
        Output oversized = {2, {{1, bytes, nullptr}, {1, bytes, nullptr}}};
        gSilent.oversized = render(gUnit, oversized, frames);
    }).join();
    // On this thread, which holds the reconfiguration: silence too, and no wait for itself.
    float left[64] = {}, right[64] = {};
    Output output = {2, {{1, sizeof(left), left}, {1, sizeof(right), right}}};
    gSilent.sameThread = render(gUnit, output, 64);
    gSilent.ran = gRuns.load() != runs;
}

// A host that keeps the output format in step with the input's, from the input's sample rate notification.
int gFollowed = 0;
void followInputRate(void*, AudioUnit unit, const AudioUnitPropertyID property, const AudioUnitScope scope,
                     const AudioUnitElement)
{
    if (property != kAudioUnitProperty_SampleRate || scope != kAudioUnitScope_Input)
        return;
    Float64 sampleRate = 0.0;
    UInt32 size = sizeof(sampleRate);
    AudioUnitGetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Input, 0, &sampleRate, &size);
    const AudioStreamBasicDescription output = format(sampleRate, 2);
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &output,
                             sizeof(output)) == noErr)
        ++gFollowed;
}

// The host's render notifications: a silent block is announced as any other, its post-render marked silent.
std::atomic<int> gPreRenders(0), gSilentPostRenders(0);
OSStatus countRenders(void*, AudioUnitRenderActionFlags* const flags, const AudioTimeStamp*, UInt32, UInt32,
                      AudioBufferList*)
{
    if ((*flags & kAudioUnitRenderAction_PreRender) != 0)
        gPreRenders.fetch_add(1);
    if ((*flags & kAudioUnitRenderAction_PostRender) != 0 && (*flags & kAudioUnitRenderAction_OutputIsSilence) != 0)
        gSilentPostRenders.fetch_add(1);
    return noErr;
}

// A host that hands a sample rate change to its engine thread and waits for it there; the engine resets the unit.
bool gHandedOver = false;
void handOverToEngine(void*, AudioUnit unit, const AudioUnitPropertyID property, const AudioUnitScope scope,
                      const AudioUnitElement)
{
    if (property != kAudioUnitProperty_SampleRate || scope != kAudioUnitScope_Input || gHandedOver)
        return;
    std::thread([unit] { AudioUnitReset(unit, kAudioUnitScope_Global, 0); }).join();
    gHandedOver = true;
}

void hung(int)
{
    const char message[] = "AUIoRender: a reconfiguration waited for itself (no progress in 30 s)\n";
    ssize_t written = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)written;
    _exit(1);
}
}

int main()
{
    std::signal(SIGALRM, hung);
    alarm(30);

    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU I/O Render Test"), 0x10000,
                                                      factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    AudioUnit unit = nullptr;
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &unit), noErr, "test AU must instantiate");
    gUnit = unit;

    // A listener that sets a format from inside another's notification, on the thread making that change.
    DAF_ASSERT_EQUAL(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_SampleRate, followInputRate, nullptr),
                     noErr, "a sample rate listener must register");
    const AudioStreamBasicDescription input96k = format(96000.0, 2);
    DAF_ASSERT_EQUAL(AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0,
                                          &input96k, sizeof(input96k)), noErr, "the input format must set");
    DAF_ASSERT_EQUAL(gFollowed > 0, true, "a listener must set the output format from the input's notification");
    DAF_ASSERT_EQUAL(AudioUnitRemovePropertyListenerWithUserData(unit, kAudioUnitProperty_SampleRate,
                                                                 followInputRate, nullptr), noErr,
                     "the sample rate listener must unregister");

    DAF_ASSERT_EQUAL(connect(unit, 0, true), noErr, "the main element must connect");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    UInt32 size = sizeof(gSilent.maxFrames);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                                          &gSilent.maxFrames, &size), noErr, "the maximum frames must read");

    // Render while the sidechain comes and goes, the sample rate changes and the unit resets.
    int renderErrors = 0;
    const int connectErrors = whileRendering(unit, 2000, [unit](const int i) {
        return connect(unit, 1, i % 2 == 0);
    }, renderErrors);
    DAF_ASSERT_EQUAL(connectErrors, 0, "every sidechain connect and disconnect must succeed while rendering");
    DAF_ASSERT_EQUAL(renderErrors, 0, "render must not fail while the sidechain changes");
    const int afterConnects = gReconfigurations.load();
    DAF_ASSERT_EQUAL(afterConnects >= 2000, true, "every connect and disconnect must reconfigure the plugin");

    const int rateErrors = whileRendering(unit, 500, [unit](const int i) {
        return setSampleRate(unit, i % 2 == 0 ? 48000.0 : 96000.0);
    }, renderErrors);
    DAF_ASSERT_EQUAL(rateErrors, 0, "every sample rate change must succeed while rendering");
    DAF_ASSERT_EQUAL(renderErrors, 0, "render must not fail while the sample rate changes");
    const int afterSampleRates = gReconfigurations.load();
    DAF_ASSERT_EQUAL(afterSampleRates - afterConnects >= 500, true,
                     "every sample rate change must reconfigure the plugin");

    const int formatErrors = whileRendering(unit, 500, [unit](const int i) {
        return setStreamRate(unit, i % 2 == 0 ? 44100.0 : 96000.0);
    }, renderErrors);
    DAF_ASSERT_EQUAL(formatErrors, 0, "every stream format change must succeed while rendering");
    DAF_ASSERT_EQUAL(renderErrors, 0, "render must not fail while the stream format changes");
    const int afterFormats = gReconfigurations.load();
    DAF_ASSERT_EQUAL(afterFormats - afterSampleRates >= 500, true,
                     "every stream format change must reconfigure the plugin");

    // A second unit feeds the sidechain through a connection (MakeConnection) instead of a render callback.
    AudioUnit source = nullptr;
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &source), noErr, "the source unit must instantiate");
    DAF_ASSERT_EQUAL(connect(source, 0, true), noErr, "the source unit's input must connect");
    DAF_ASSERT_EQUAL(AudioUnitInitialize(source), noErr, "the source unit must initialize");
    const int beforeConnections = gReconfigurations.load();
    const int connectionErrors = whileRendering(unit, 500, [unit, source](const int i) {
        return connectUnit(unit, i % 2 == 0 ? source : nullptr);
    }, renderErrors);
    DAF_ASSERT_EQUAL(connectionErrors, 0, "every unit connection and disconnection must succeed while rendering");
    DAF_ASSERT_EQUAL(renderErrors, 0, "render must not fail while a unit connection changes");
    DAF_ASSERT_EQUAL(gReconfigurations.load() - beforeConnections >= 500, true,
                     "every unit connection and disconnection must reconfigure the plugin");
    const int afterRates = gReconfigurations.load();

    const int resetErrors = whileRendering(unit, 500, [unit](int) {
        return AudioUnitReset(unit, kAudioUnitScope_Global, 0);
    }, renderErrors);
    DAF_ASSERT_EQUAL(resetErrors, 0, "every reset must succeed while rendering");
    DAF_ASSERT_EQUAL(renderErrors, 0, "render must not fail while the unit resets");
    DAF_ASSERT_EQUAL(gReconfigurations.load() - afterRates >= 500, true, "every reset must reconfigure the plugin");

    d_stdout("%d runs, %d reconfigurations, %d runs inside one", gRuns.load(), gReconfigurations.load(),
             gOverlaps.load());
    DAF_ASSERT_EQUAL(gRuns.load() > 100, true, "render must keep running while the unit is reconfigured");
    DAF_ASSERT_EQUAL(gOverlaps.load(), 0, "no run() may start inside a reconfiguration");

    // A sample-rate listener that waits for an engine thread resetting the unit.
    DAF_ASSERT_EQUAL(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_SampleRate, handOverToEngine, nullptr),
                     noErr, "the hand-over listener must register");
    DAF_ASSERT_EQUAL(setSampleRate(unit, 88200.0), noErr, "the sample rate must change with a hand-over listener");
    DAF_ASSERT_EQUAL(gHandedOver, true, "a listener waiting for an engine thread's reset must complete");
    DAF_ASSERT_EQUAL(AudioUnitRemovePropertyListenerWithUserData(unit, kAudioUnitProperty_SampleRate,
                                                                 handOverToEngine, nullptr), noErr,
                     "the hand-over listener must unregister");

    // The unit's own buffers hold the plugin's output (its input plus one) before a silent block must clear them.
    {
        Output own = {2, {{1, sizeof(float) * 64, nullptr}, {1, sizeof(float) * 64, nullptr}}};
        DAF_ASSERT_EQUAL(render(unit, own, 64), noErr, "a block into the unit's own buffers must render");
        DAF_ASSERT_EQUAL(own.buffers[0].mData != nullptr
                             && static_cast<const float*>(own.buffers[0].mData)[0] == 1.0f, true,
                         "the plugin must write the unit's own buffers before the silent block");
    }

    // The blocks rendered from inside a reconfiguration (a reset: deactivate() renders). Three render (the oversized
    // one is refused), and the host's render notifications hear of each.
    DAF_ASSERT_EQUAL(AudioUnitAddRenderNotify(unit, countRenders, nullptr), noErr,
                     "a render notification must register");
    gInsideReconfiguration = renderInsideReconfiguration;
    DAF_ASSERT_EQUAL(AudioUnitReset(unit, kAudioUnitScope_Global, 0), noErr, "the reset must succeed");
    DAF_ASSERT_EQUAL(gInsideReconfiguration == nullptr, true, "the reset must render from inside itself");
    DAF_ASSERT_EQUAL(gSilent.hostBuffers, noErr, "a block in a reconfiguration must render");
    DAF_ASSERT_EQUAL(gSilent.hostSilent, true, "a block in a reconfiguration must be silent");
    DAF_ASSERT_EQUAL(gSilent.ownBuffers, noErr, "a block in a reconfiguration must render into the unit's buffers");
    DAF_ASSERT_EQUAL(gSilent.ownProvided, true, "a block in a reconfiguration must provide the buffers asked for");
    DAF_ASSERT_EQUAL(gSilent.ownSilent, true, "the unit's buffers must be silent in a reconfiguration");
    DAF_ASSERT_EQUAL(gSilent.oversized, kAudioUnitErr_TooManyFramesToProcess,
                     "more frames than the unit's buffers hold must be refused in a reconfiguration");
    DAF_ASSERT_EQUAL(gSilent.sameThread, noErr, "a block on the reconfiguring thread must render");
    DAF_ASSERT_EQUAL(gSilent.ran, false, "no block in a reconfiguration may run the plugin");
    DAF_ASSERT_EQUAL(gPreRenders.load(), 3, "each silent block must reach the pre-render notification");
    DAF_ASSERT_EQUAL(gSilentPostRenders.load(), 3, "each silent block must reach the post-render notification, silent");
    DAF_ASSERT_EQUAL(AudioUnitRemoveRenderNotify(unit, countRenders, nullptr), noErr,
                     "the render notification must unregister");

    DAF_ASSERT_EQUAL(AudioUnitUninitialize(unit), noErr, "the unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(unit), noErr, "test AU must dispose");
    DAF_ASSERT_EQUAL(AudioUnitUninitialize(source), noErr, "the source unit must uninitialize");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(source), noErr, "the source unit must dispose");
    return 0;
}
