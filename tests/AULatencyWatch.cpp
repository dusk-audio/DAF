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

// The AU wrapper's latency watch against what a host's listeners do, with main() as the host. The watch tells a
// unit's latency listeners on the main thread; every initialized unit in the process shares it.
//
// - A latency listener that waits for another thread, which initializes another unit, completes. The watch held
//   its process-wide lock while listeners ran, and Initialize needs it: deadlock.
// - A latency listener that resets its unit while another thread changes the unit's sample rate, whose sample-rate
//   listener adds a listener, uninitializes the unit or initializes another, completes. The reconfiguration held
//   the unit's render guard while its listeners ran, the opposite order to the watch: deadlock.
// - A latency that moves on every block, with a listener slower than a block, leaves the main run loop free. The
//   watch walked its units again after every one it told and did not return while the latency kept moving.
// - A unit disposed on another thread while the main thread is in its latency listener waits for the listener to
//   return; a listener that disposes its own unit on the main thread does not wait for itself. A listener that
//   disposes another unit the same tick is about to tell: that unit is not read again.
// - A latency listener removed on another thread while the main thread calls it: the removal waits for the call to
//   end. Removed while an earlier listener runs, it is not called at all (the host may free what it reads).
//
// Run under AddressSanitizer as well (CI): a watch that reads a disposed unit is a use after free there.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "daf/DafPlugin.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <sched.h>
#include <thread>
#include <unistd.h>

START_NAMESPACE_DAF

class WatchPlugin : public Plugin
{
public:
    WatchPlugin() : Plugin(1, 0, 0) {}

protected:
    const char* getLabel() const override { return "LatencyWatchTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    // The latency in samples; -1 moves it on every block, as an automated lookahead does.
    void initParameter(const uint32_t, Parameter& parameter) override
    {
        parameter.hints = kParameterIsAutomatable | kParameterIsInteger;
        parameter.name = "Latency";
        parameter.symbol = "latency";
        parameter.ranges.min = -1.0f;
        parameter.ranges.max = 512.0f;
        parameter.ranges.def = 0.0f;
    }

    float getParameterValue(uint32_t) const override { return fLatency.load(); }
    void setParameterValue(uint32_t, const float value) override { fLatency.store(value); }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        std::memcpy(outputs[0], inputs[0], sizeof(float) * frames);
        const float latency = fLatency.load();
        if (latency < 0.0f)
        {
            fBlock = (fBlock + 1) % 256;
            setLatency(64 + fBlock);
        }
        else
        {
            setLatency(static_cast<uint32_t>(latency));
        }
    }

private:
    std::atomic<float> fLatency{0.0f};
    uint32_t fBlock = 0;
};

Plugin* createPlugin()
{
    return new WatchPlugin();
}

END_NAMESPACE_DAF

#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

// DAF_ASSERT_EQUAL returns from main(); the scenarios below are functions of their own.
#define REQUIRE_EQUAL(v1, v2, msg) \
    do { if ((v1) != (v2)) { d_stderr2("Test condition failed: %s; file:%s line:%i", msg, __FILE__, __LINE__); \
                             std::exit(1); } } while (false)

namespace
{
typedef std::chrono::steady_clock Clock;

AudioComponent gComponent = nullptr;
const char* volatile gScenario = "setup";

void hung(int)
{
    const char head[] = "AULatencyWatch: no progress in 30 s, in: ";
    ssize_t written = write(STDERR_FILENO, head, sizeof(head) - 1);
    written = write(STDERR_FILENO, gScenario, std::strlen(gScenario));
    written = write(STDERR_FILENO, "\n", 1);
    (void)written;
    _exit(1);
}

OSStatus feed(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, const UInt32 frames,
              AudioBufferList* const data)
{
    for (UInt32 ch = 0; ch < data->mNumberBuffers; ++ch)
        std::memset(data->mBuffers[ch].mData, 0, sizeof(float) * frames);
    return noErr;
}

AudioUnit newUnit()
{
    AudioUnit unit = nullptr;
    REQUIRE_EQUAL(AudioComponentInstanceNew(gComponent, &unit), noErr, "test AU must instantiate");
    AURenderCallbackStruct callback = {feed, nullptr};
    REQUIRE_EQUAL(AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0,
                                          &callback, sizeof(callback)), noErr, "the input must connect");
    return unit;
}

OSStatus renderBlock(AudioUnit unit, const UInt32 frames = 16)
{
    float samples[64] = {};
    AudioBufferList output = {1, {{1, static_cast<UInt32>(sizeof(float)) * frames, samples}}};
    AudioTimeStamp timestamp = {};
    timestamp.mFlags = kAudioTimeStampSampleTimeValid;
    AudioUnitRenderActionFlags flags = 0;
    return AudioUnitRender(unit, &flags, &timestamp, 0, frames, &output);
}

// One block on a thread of its own, as a host's audio thread renders; the plugin moves its latency to `latency`.
void moveLatency(AudioUnit unit, const float latency)
{
    REQUIRE_EQUAL(AudioUnitSetParameter(unit, 0, kAudioUnitScope_Global, 0, latency, 0), noErr,
                     "the latency parameter must set");
    OSStatus status = noErr;
    std::thread([unit, &status] { status = renderBlock(unit); }).join();
    REQUIRE_EQUAL(status, noErr, "the unit must render");
}

// Runs the main run loop until `done` or `seconds` have gone by.
bool spinUntil(const std::atomic<bool>& done, const double seconds)
{
    const Clock::time_point start = Clock::now();
    while (! done.load() && std::chrono::duration<double>(Clock::now() - start).count() < seconds)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    return done.load();
}

void addLatencyListener(AudioUnit unit, AudioUnitPropertyListenerProc proc)
{
    REQUIRE_EQUAL(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_Latency, proc, nullptr), noErr,
                     "a latency listener must register");
}

void noop(void*, AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement) {}

// -------------------------------------------------------------------------------------------------------------------
// A latency listener that waits for a thread initializing another unit.

AudioUnit gOther = nullptr;
std::atomic<bool> gJoined(false);

void joinInitializing(void*, AudioUnit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency || gJoined.load())
        return;
    std::thread([] { AudioUnitInitialize(gOther); }).join();
    gJoined.store(true);
}

void listenerWaitsForInitialize()
{
    gScenario = "a latency listener waiting for another unit's Initialize";
    AudioUnit unit = newUnit();
    gOther = newUnit();
    addLatencyListener(unit, joinInitializing);
    REQUIRE_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    moveLatency(unit, 100.0f);
    REQUIRE_EQUAL(spinUntil(gJoined, 5.0), true,
                     "a latency listener that waits for another unit's Initialize must complete");
    AudioUnitUninitialize(gOther);
    AudioComponentInstanceDispose(gOther);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}

// -------------------------------------------------------------------------------------------------------------------
// A latency listener that resets its unit while another thread changes the unit's sample rate; that thread's
// sample-rate listener adds a listener, uninitializes the unit or initializes another one.

enum class RateAction { AddListener, Uninitialize, InitializeOther };
RateAction gRateAction = RateAction::AddListener;
std::atomic<bool> gMainInLatencyListener(false), gWorkerInRateListener(false), gLatencyListenerDone(false),
    gWorkerDone(false);

void resetFromLatencyListener(void*, AudioUnit unit, const AudioUnitPropertyID property, AudioUnitScope,
                              AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency || gMainInLatencyListener.load())
        return;
    gMainInLatencyListener.store(true);
    const Clock::time_point start = Clock::now();
    while (! gWorkerInRateListener.load() && std::chrono::duration<double>(Clock::now() - start).count() < 2.0)
        sched_yield();
    AudioUnitReset(unit, kAudioUnitScope_Global, 0);
    gLatencyListenerDone.store(true);
}

void actFromRateListener(void*, AudioUnit unit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property != kAudioUnitProperty_SampleRate || gWorkerInRateListener.load())
        return;
    gWorkerInRateListener.store(true);
    const Clock::time_point start = Clock::now();
    while (! gMainInLatencyListener.load() && std::chrono::duration<double>(Clock::now() - start).count() < 2.0)
        sched_yield();
    usleep(20000);   // the main thread is in AudioUnitReset by now
    switch (gRateAction)
    {
    case RateAction::AddListener:
        AudioUnitAddPropertyListener(unit, kAudioUnitProperty_StreamFormat, noop, nullptr);
        break;
    case RateAction::Uninitialize:
        AudioUnitUninitialize(unit);
        break;
    case RateAction::InitializeOther:
        AudioUnitInitialize(gOther);
        break;
    }
}

void reconfigurationAgainstWatch(const RateAction action, const char* const name)
{
    gScenario = name;
    gRateAction = action;
    gMainInLatencyListener.store(false);
    gWorkerInRateListener.store(false);
    gLatencyListenerDone.store(false);
    gWorkerDone.store(false);
    AudioUnit unit = newUnit();
    gOther = newUnit();
    addLatencyListener(unit, resetFromLatencyListener);
    REQUIRE_EQUAL(AudioUnitAddPropertyListener(unit, kAudioUnitProperty_SampleRate, actFromRateListener, nullptr),
                     noErr, "a sample rate listener must register");
    REQUIRE_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    moveLatency(unit, 100.0f);
    std::thread worker([unit] {
        const Float64 rate = 48000.0;
        AudioUnitSetProperty(unit, kAudioUnitProperty_SampleRate, kAudioUnitScope_Output, 0, &rate, sizeof(rate));
        gWorkerDone.store(true);
    });
    const bool listenerDone = spinUntil(gLatencyListenerDone, 5.0);
    const bool workerDone = spinUntil(gWorkerDone, 5.0);
    worker.join();
    REQUIRE_EQUAL(listenerDone && workerDone, true, name);
    AudioUnitUninitialize(gOther);
    AudioComponentInstanceDispose(gOther);
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}

// -------------------------------------------------------------------------------------------------------------------
// A latency that moves on every block, with a listener slower than a block: the main run loop stays free.

int gStarveNotifications = 0;
Clock::time_point gLastBeat;
double gLongestGapMs = 0.0;

void slowListener(void*, AudioUnit unit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency)
        return;
    // A host reads the latency it is told of (on the main thread, while render may be moving it).
    Float64 seconds = 0.0;
    UInt32 size = sizeof(seconds);
    AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0, &seconds, &size);
    ++gStarveNotifications;
    std::this_thread::sleep_for(std::chrono::microseconds(1500));
}

void heartbeat(CFRunLoopTimerRef, void*)
{
    const Clock::time_point now = Clock::now();
    const double gap = std::chrono::duration<double, std::milli>(now - gLastBeat).count();
    if (gap > gLongestGapMs)
        gLongestGapMs = gap;
    gLastBeat = now;
}

void movingLatencyLeavesTheRunLoopFree()
{
    gScenario = "a latency that moves on every block";
    AudioUnit unit = newUnit();
    addLatencyListener(unit, slowListener);
    REQUIRE_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    REQUIRE_EQUAL(AudioUnitSetParameter(unit, 0, kAudioUnitScope_Global, 0, -1.0f, 0), noErr,
                     "the latency parameter must set");
    CFRunLoopTimerRef beat = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.005, 0.005,
                                                  0, 0, heartbeat, nullptr);
    CFRunLoopAddTimer(CFRunLoopGetMain(), beat, kCFRunLoopCommonModes);
    gLastBeat = Clock::now();
    std::atomic<bool> stop(false);
    std::thread audio([unit, &stop] {
        while (! stop.load())
        {
            renderBlock(unit, 64);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    const Clock::time_point start = Clock::now();
    while (std::chrono::duration<double>(Clock::now() - start).count() < 1.0)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, false);
    stop.store(true);
    audio.join();
    CFRunLoopTimerInvalidate(beat);
    CFRelease(beat);
    d_stdout("moving latency: %d notifications, longest main run loop stall %.0f ms", gStarveNotifications,
             gLongestGapMs);
    REQUIRE_EQUAL(gStarveNotifications > 0, true, "a moving latency must notify");
    REQUIRE_EQUAL(gLongestGapMs < 500.0, true, "a latency that moves on every block must not hold the main thread");
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}

// -------------------------------------------------------------------------------------------------------------------
// Disposal against a latency listener that is running.

std::atomic<bool> gInListener(false), gListenerReturned(false), gDisposed(false), gDisposeSawReturn(false);

void sleepyListener(void*, AudioUnit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency || gInListener.load())
        return;
    gInListener.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gListenerReturned.store(true);
}

void disposeWaitsForTheListener()
{
    gScenario = "a unit disposed on another thread while its latency listener runs";
    AudioUnit unit = newUnit();
    addLatencyListener(unit, sleepyListener);
    REQUIRE_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    moveLatency(unit, 100.0f);
    std::thread disposer([unit] {
        while (! gInListener.load())
            sched_yield();
        AudioComponentInstanceDispose(unit);
        gDisposeSawReturn.store(gListenerReturned.load());
        gDisposed.store(true);
    });
    const bool disposed = spinUntil(gDisposed, 5.0);
    disposer.join();
    REQUIRE_EQUAL(disposed, true, "a unit disposed while its latency listener runs must dispose");
    REQUIRE_EQUAL(gDisposeSawReturn.load(), true,
                     "a unit disposed on another thread must wait for its running latency listener");
}

AudioUnit gSecond = nullptr;
std::atomic<bool> gSecondDisposed(false);
std::atomic<int> gSecondCallsAfterDispose(0);

void disposeSecond(void*, AudioUnit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency || gSecondDisposed.load())
        return;
    AudioComponentInstanceDispose(gSecond);
    gSecondDisposed.store(true);
}

void secondListener(void*, AudioUnit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property == kAudioUnitProperty_Latency && gSecondDisposed.load())
        gSecondCallsAfterDispose.fetch_add(1);
}

void listenerDisposesAnotherUnitMidPass()
{
    gScenario = "a latency listener disposing a unit the same tick is about to tell";
    AudioUnit unit = newUnit();
    gSecond = newUnit();
    addLatencyListener(unit, disposeSecond);
    addLatencyListener(gSecond, secondListener);
    REQUIRE_EQUAL(AudioUnitInitialize(unit), noErr, "the first unit must initialize");
    REQUIRE_EQUAL(AudioUnitInitialize(gSecond), noErr, "the second unit must initialize");
    moveLatency(unit, 100.0f);
    moveLatency(gSecond, 100.0f);
    REQUIRE_EQUAL(spinUntil(gSecondDisposed, 5.0), true, "the first unit's listener must dispose the second");
    std::atomic<bool> never(false);
    spinUntil(never, 0.2);
    REQUIRE_EQUAL(gSecondCallsAfterDispose.load(), 0, "a unit disposed in the tick must not be told afterwards");
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}

std::atomic<bool> gSlowInCall(false), gSlowReturned(false), gRemoved(false), gRemovalSawReturn(false);
std::atomic<int> gLaterCalls(0);

void slowLatencyListener(void*, AudioUnit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency || gSlowInCall.load())
        return;
    gSlowInCall.store(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    gSlowReturned.store(true);
}

void laterLatencyListener(void*, AudioUnit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property == kAudioUnitProperty_Latency)
        gLaterCalls.fetch_add(1);
}

// `removeTheRunningOne`: remove the listener the main thread is in; else the one registered after it.
void removalAgainstARunningListener(const bool removeTheRunningOne)
{
    gScenario = removeTheRunningOne ? "removing the latency listener the main thread is calling"
                                    : "removing a latency listener while an earlier one runs";
    gSlowInCall.store(false);
    gSlowReturned.store(false);
    gRemoved.store(false);
    gRemovalSawReturn.store(false);
    gLaterCalls.store(0);
    AudioUnit unit = newUnit();
    addLatencyListener(unit, slowLatencyListener);
    addLatencyListener(unit, laterLatencyListener);
    REQUIRE_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    moveLatency(unit, 100.0f);
    std::thread remover([unit, removeTheRunningOne] {
        while (! gSlowInCall.load())
            sched_yield();
        AudioUnitRemovePropertyListenerWithUserData(unit, kAudioUnitProperty_Latency,
                                                    removeTheRunningOne ? slowLatencyListener : laterLatencyListener,
                                                    nullptr);
        gRemovalSawReturn.store(gSlowReturned.load());
        gRemoved.store(true);
    });
    const bool removed = spinUntil(gRemoved, 5.0);
    remover.join();
    std::atomic<bool> never(false);
    spinUntil(never, 0.2);
    REQUIRE_EQUAL(removed, true, "a latency listener must be removable while one runs");
    if (removeTheRunningOne)
        REQUIRE_EQUAL(gRemovalSawReturn.load(), true,
                      "removing the latency listener being called must wait for the call to end");
    else
        REQUIRE_EQUAL(gLaterCalls.load(), 0, "a latency listener removed before its turn must not be called");
    AudioUnitUninitialize(unit);
    AudioComponentInstanceDispose(unit);
}

std::atomic<bool> gSelfDisposed(false);

void disposingListener(void*, AudioUnit unit, const AudioUnitPropertyID property, AudioUnitScope, AudioUnitElement)
{
    if (property != kAudioUnitProperty_Latency || gSelfDisposed.load())
        return;
    AudioComponentInstanceDispose(unit);
    gSelfDisposed.store(true);
}

void listenerDisposesItsUnit()
{
    gScenario = "a latency listener disposing its own unit";
    AudioUnit unit = newUnit();
    addLatencyListener(unit, disposingListener);
    REQUIRE_EQUAL(AudioUnitInitialize(unit), noErr, "the unit must initialize");
    moveLatency(unit, 100.0f);
    REQUIRE_EQUAL(spinUntil(gSelfDisposed, 5.0), true,
                     "a latency listener that disposes its own unit must not wait for itself");
    // The watch reads nothing of the disposed unit on its next ticks (AddressSanitizer: no use after free).
    std::atomic<bool> never(false);
    spinUntil(never, 0.2);
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
    gComponent = AudioComponentRegister(&description, CFSTR("DAF AU Latency Watch Test"), 0x10000, factory);
    DAF_ASSERT_NOT_EQUAL(gComponent, nullptr, "test AU must register");

    listenerWaitsForInitialize();
    reconfigurationAgainstWatch(RateAction::AddListener,
                                "a reset in a latency listener against a sample-rate listener that adds a listener");
    reconfigurationAgainstWatch(RateAction::Uninitialize,
                                "a reset in a latency listener against a sample-rate listener that uninitializes");
    reconfigurationAgainstWatch(RateAction::InitializeOther,
                                "a reset in a latency listener against a sample-rate listener that initializes "
                                "another unit");
    movingLatencyLeavesTheRunLoopFree();
    disposeWaitsForTheListener();
    listenerDisposesItsUnit();
    listenerDisposesAnotherUnitMidPass();
    removalAgainstARunningListener(true);
    removalAgainstARunningListener(false);
    return 0;
}
