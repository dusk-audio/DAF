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

// libAUViewDisposeWatch, see AUViewDisposeWatch.hpp.

#include "AUViewDisposeWatch.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

namespace
{

std::atomic<const void*> gPluginImage(nullptr);
std::mutex gLock;
std::vector<AudioUnit> gDisposedUnits;
std::vector<CFRunLoopTimerRef> gPluginTimers;  // retained here
int gCallsOnDisposedUnits = 0;
const char* gFirstCall = nullptr;
// see auViewWatchBeforeNextPluginTimer: set, and taken and cleared, together under gLock
void (*gBeforeNextTimer)(void*) = nullptr;
void* gBeforeNextTimerArg = nullptr;

bool calledFromPlugin(const void* const returnAddress)
{
    const void* const image = gPluginImage.load();
    Dl_info info;
    return image != nullptr && dladdr(returnAddress, &info) != 0 && info.dli_fbase == image;
}

void noteCall(const char* const function, AudioUnit const unit, const void* const returnAddress)
{
    if (! calledFromPlugin(returnAddress))
        return;
    const std::lock_guard<std::mutex> guard(gLock);
    if (std::find(gDisposedUnits.begin(), gDisposedUnits.end(), unit) == gDisposedUnits.end())
        return;
    if (gCallsOnDisposedUnits++ == 0)
        gFirstCall = function;
}

OSStatus watchedGetProperty(AudioUnit const unit, const AudioUnitPropertyID property, const AudioUnitScope scope,
                            const AudioUnitElement element, void* const data, UInt32* const size)
{
    noteCall("AudioUnitGetProperty", unit, __builtin_return_address(0));
    return AudioUnitGetProperty(unit, property, scope, element, data, size);
}

OSStatus watchedSetProperty(AudioUnit const unit, const AudioUnitPropertyID property, const AudioUnitScope scope,
                            const AudioUnitElement element, const void* const data, const UInt32 size)
{
    noteCall("AudioUnitSetProperty", unit, __builtin_return_address(0));
    return AudioUnitSetProperty(unit, property, scope, element, data, size);
}

OSStatus watchedAddPropertyListener(AudioUnit const unit, const AudioUnitPropertyID property,
                                    const AudioUnitPropertyListenerProc proc, void* const userData)
{
    noteCall("AudioUnitAddPropertyListener", unit, __builtin_return_address(0));
    return AudioUnitAddPropertyListener(unit, property, proc, userData);
}

OSStatus watchedRemovePropertyListener(AudioUnit const unit, const AudioUnitPropertyID property,
                                       const AudioUnitPropertyListenerProc proc, void* const userData)
{
    noteCall("AudioUnitRemovePropertyListenerWithUserData", unit, __builtin_return_address(0));
    return AudioUnitRemovePropertyListenerWithUserData(unit, property, proc, userData);
}

CFRunLoopTimerRef watchedTimerCreate(const CFAllocatorRef allocator, const CFAbsoluteTime fireDate,
                                     const CFTimeInterval interval, const CFOptionFlags flags, const CFIndex order,
                                     const CFRunLoopTimerCallBack callout, CFRunLoopTimerContext* const context)
{
    const bool fromPlugin = calledFromPlugin(__builtin_return_address(0));
    if (fromPlugin)
    {
        void (*callback)(void*) = nullptr;
        void* arg = nullptr;
        {
            const std::lock_guard<std::mutex> guard(gLock);
            std::swap(callback, gBeforeNextTimer);
            std::swap(arg, gBeforeNextTimerArg);
        }
        if (callback != nullptr)  // outside the lock: it may wait for a dispose that notes calls
            callback(arg);
    }
    CFRunLoopTimerRef const timer = CFRunLoopTimerCreate(allocator, fireDate, interval, flags, order, callout, context);
    if (timer != nullptr && fromPlugin)
    {
        CFRetain(timer);
        const std::lock_guard<std::mutex> guard(gLock);
        gPluginTimers.push_back(timer);
    }
    return timer;
}

} // namespace

#define WATCH(replacement, replacee) \
    __attribute__((used)) static struct { const void* replacement; const void* replacee; } \
        watch_##replacee __attribute__((section("__DATA,__interpose"))) = \
        { (const void*)(unsigned long)&replacement, (const void*)(unsigned long)&replacee };

WATCH(watchedGetProperty, AudioUnitGetProperty)
WATCH(watchedSetProperty, AudioUnitSetProperty)
WATCH(watchedAddPropertyListener, AudioUnitAddPropertyListener)
WATCH(watchedRemovePropertyListener, AudioUnitRemovePropertyListenerWithUserData)
WATCH(watchedTimerCreate, CFRunLoopTimerCreate)

extern "C" {

void auViewWatchPluginImage(const void* const header)
{
    gPluginImage.store(header);
}

void auViewWatchDisposed(AudioUnit const unit)
{
    const std::lock_guard<std::mutex> guard(gLock);
    gDisposedUnits.push_back(unit);
}

void auViewWatchCreated(AudioUnit const unit)
{
    const std::lock_guard<std::mutex> guard(gLock);
    gDisposedUnits.erase(std::remove(gDisposedUnits.begin(), gDisposedUnits.end(), unit), gDisposedUnits.end());
}

int auViewWatchCallsOnDisposedUnits(const char** const first)
{
    const std::lock_guard<std::mutex> guard(gLock);
    if (first != nullptr)
        *first = gFirstCall;
    return gCallsOnDisposedUnits;
}

size_t auViewWatchPluginTimers(size_t* const valid)
{
    const std::lock_guard<std::mutex> guard(gLock);
    size_t count = 0;
    for (size_t i = 0; i < gPluginTimers.size(); ++i)
        if (CFRunLoopTimerIsValid(gPluginTimers[i]))
            ++count;
    if (valid != nullptr)
        *valid = count;
    return gPluginTimers.size();
}

void auViewWatchBeforeNextPluginTimer(void (*const callback)(void*), void* const arg)
{
    const std::lock_guard<std::mutex> guard(gLock);
    gBeforeNextTimer = callback;
    gBeforeNextTimerArg = arg;
}

void auViewWatchForgetTimers(void)
{
    const std::lock_guard<std::mutex> guard(gLock);
    for (size_t i = 0; i < gPluginTimers.size(); ++i)
        CFRelease(gPluginTimers[i]);
    gPluginTimers.clear();
}

}
