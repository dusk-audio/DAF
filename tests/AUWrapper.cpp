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

// Compile the AU adapter into a native host test. Invalid inner dictionaries
// must report failure before touching state; valid optional recalls still work.
#define DAF_PLUGIN_TARGET_AU
#define DAF_TEST_NO_DGL
#include "tests.hpp"
#include "plugin-wrappers/WrapperTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"
USE_NAMESPACE_DAF

namespace
{
CFMutableDictionaryRef copy(CFDictionaryRef root)
{
    return const_cast<CFMutableDictionaryRef>(static_cast<CFDictionaryRef>(
        CFPropertyListCreateDeepCopy(nullptr, root, kCFPropertyListMutableContainersAndLeaves)));
}
CFMutableDictionaryRef data(CFMutableDictionaryRef root)
{
    return const_cast<CFMutableDictionaryRef>(static_cast<CFDictionaryRef>(CFDictionaryGetValue(root, CFSTR("data"))));
}
void states(CFMutableDictionaryRef root, CFTypeRef value, unsigned copies = 1)
{
    CFMutableArrayRef array = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef item = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(item, CFSTR("file"), value);
    for (unsigned i = 0; i < copies; ++i) CFArrayAppendValue(array, item);
    CFDictionarySetValue(data(root), CFSTR("states"), array);
    CFRelease(item); CFRelease(array);
}
void edit(CFMutableDictionaryRef root)
{
    CFMutableArrayRef params = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef item = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    const float value = 9;
    CFNumberRef number = CFNumberCreate(nullptr, kCFNumberFloat32Type, &value);
    CFDictionarySetValue(item, CFSTR("linearlow"), number);
    CFArrayAppendValue(params, item);
    CFDictionarySetValue(data(root), CFSTR("params"), params);
    CFRelease(number); CFRelease(item); CFRelease(params);
}
OSStatus load(AudioUnit unit, CFDictionaryRef root)
{
    CFPropertyListRef object = root;
    return AudioUnitSetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &object, sizeof(object));
}
float value(AudioUnit unit)
{
    float result = -1;
    return AudioUnitGetParameter(unit, kParamLinearLow, kAudioUnitScope_Global, 0, &result) == noErr ? result : -1;
}
}

int main()
{
    AudioComponentDescription description = {kType, kSubType, kManufacturer, 0, 0};
    const auto factory = [](const AudioComponentDescription* desc) -> AudioComponentPlugInInterface* {
        return static_cast<AudioComponentPlugInInterface*>(PluginAUFactory(desc));
    };
    AudioComponent component = AudioComponentRegister(&description, CFSTR("DAF AU State Test"), 0x10000, factory);
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "test AU must register");
    gRequireWrapperTestState = true;
    AudioUnit unit = nullptr;
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &unit), noErr, "required-state AU must instantiate");
    gRequireWrapperTestState = false;
    CFPropertyListRef saved = nullptr;
    UInt32 size = sizeof(saved);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &saved, &size), noErr,
                     "AU must save a complete class-info dictionary");
    DAF_ASSERT_NOT_EQUAL(saved, nullptr, "saved AU state must exist");
    for (unsigned scenario = 0; scenario < 8; ++scenario)
    {
        DAF_ASSERT_EQUAL(AudioUnitSetParameter(unit, kParamLinearLow, kAudioUnitScope_Global, 0, 3, 0), noErr, "seed control must apply");
        CFMutableDictionaryRef invalid = copy(static_cast<CFDictionaryRef>(saved));
        edit(invalid);
        switch (scenario)
        {
        case 0: CFDictionaryRemoveValue(invalid, CFSTR("data")); break;
        case 1: CFDictionarySetValue(invalid, CFSTR("data"), CFSTR("invalid")); break;
        case 2: CFDictionaryRemoveValue(data(invalid), CFSTR("states")); break;
        case 3: CFDictionarySetValue(data(invalid), CFSTR("states"), CFSTR("invalid")); break;
        case 4: states(invalid, kCFBooleanTrue); break;
        case 5: states(invalid, CFSTR(""), 2); break;
        case 6:
        {
            states(invalid, CFSTR(""));
            const UInt8 bytes[] = {'f', 'i', 'l', 'e', 0, 'j', 'u', 'n', 'k'};
            CFStringRef key = CFStringCreateWithBytes(nullptr, bytes, sizeof(bytes), kCFStringEncodingUTF8, false);
            CFArrayRef array = static_cast<CFArrayRef>(CFDictionaryGetValue(data(invalid), CFSTR("states")));
            CFMutableDictionaryRef item = const_cast<CFMutableDictionaryRef>(static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(array, 0)));
            CFDictionaryRemoveAllValues(item); CFDictionarySetValue(item, key, CFSTR(""));
            CFRelease(key);
            break;
        }
        case 7:
        {
            const UInt8 bytes[] = {'a', 0, 'b'};
            CFStringRef text = CFStringCreateWithBytes(nullptr, bytes, sizeof(bytes), kCFStringEncodingUTF8, false);
            states(invalid, text); CFRelease(text);
            break;
        }
        }
        const OSStatus status = load(unit, invalid);
        CFRelease(invalid);
        DAF_ASSERT_EQUAL(status, kAudioUnitErr_InvalidPropertyValue, "malformed AU state must report failure to the host");
        DAF_ASSERT_EQUAL(value(unit), 3, "rejected AU state must not apply earlier parameter changes");
    }
    CFMutableDictionaryRef valid = copy(static_cast<CFDictionaryRef>(saved));
    edit(valid); states(valid, CFSTR(""));
    DAF_ASSERT_EQUAL(load(unit, valid), noErr, "one valid required state must accept");
    DAF_ASSERT_EQUAL(value(unit), 9, "accepted AU recall must still apply its parameter changes");
    CFRelease(valid);
    valid = copy(static_cast<CFDictionaryRef>(saved));
    states(valid, CFSTR("Caf\u00e9 \U0001f305"));
    DAF_ASSERT_EQUAL(load(unit, valid), noErr, "valid Unicode state must accept without truncation");
    CFRelease(valid);
    CFPropertyListRef unicode = nullptr; size = sizeof(unicode);
    DAF_ASSERT_EQUAL(AudioUnitGetProperty(unit, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0, &unicode, &size), noErr,
                     "Unicode state must save");
    CFDictionaryRef unicodeData = static_cast<CFDictionaryRef>(CFDictionaryGetValue(static_cast<CFDictionaryRef>(unicode), CFSTR("data")));
    CFArrayRef unicodeStates = static_cast<CFArrayRef>(CFDictionaryGetValue(unicodeData, CFSTR("states")));
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(unicodeStates); ++i)
    {
        CFDictionaryRef item = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(unicodeStates, i));
        const void* text = CFDictionaryGetValue(item, CFSTR("file"));
        if (text != nullptr) found = CFEqual(text, CFSTR("Caf\u00e9 \U0001f305"));
    }
    CFRelease(unicode);
    DAF_ASSERT_EQUAL(found, true, "non-ASCII and surrogate-pair state characters must round trip exactly");
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(unit), noErr, "required-state AU must dispose");

    AudioUnit optional = nullptr;
    DAF_ASSERT_EQUAL(AudioComponentInstanceNew(component, &optional), noErr, "optional-state AU must instantiate");
    valid = copy(static_cast<CFDictionaryRef>(saved));
    edit(valid); CFDictionaryRemoveValue(data(valid), CFSTR("states"));
    DAF_ASSERT_EQUAL(load(optional, valid), noErr, "omitted optional state must retain its existing restore policy");
    DAF_ASSERT_EQUAL(value(optional), 9, "optional-state parameter-only recall must still apply");
    CFRelease(valid); CFRelease(saved);
    DAF_ASSERT_EQUAL(AudioComponentInstanceDispose(optional), noErr, "optional-state AU must dispose");
    d_stdout("AU wrapper state tests passed");
    return 0;
}
