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

// TODO
// - g_nextBundlePath vs d_nextBundlePath cleanup
// - scale points to kAudioUnitParameterFlag_ValuesHaveStrings
// - report latency changes

#include "DafPluginInternal.hpp"
#include "../DafPluginUtils.hpp"

#if DAF_PLUGIN_WANT_MIDI_INPUT
# include "../extra/RingBuffer.hpp"
#endif

#include <AudioUnit/AudioUnit.h>
#include <AudioToolbox/AudioUnitUtilities.h>

#include "DafPluginAUBufferList.hpp"

#include <map>
#include <vector>

#ifndef DAF_PLUGIN_BRAND_ID
# error DAF_PLUGIN_BRAND_ID undefined!
#endif

#ifndef DAF_PLUGIN_UNIQUE_ID
# error DAF_PLUGIN_UNIQUE_ID undefined!
#endif

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

#ifndef __MAC_12_3
enum {
    kAudioUnitProperty_MIDIOutputBufferSizeHint = 66,
};
#endif

// --------------------------------------------------------------------------------------------------------------------

static const char* AudioUnitPropertyID2Str(const AudioUnitPropertyID prop) noexcept
{
    switch (prop)
    {
    #define PROP(s) case s: return #s;
    PROP(kAudioUnitProperty_ClassInfo)
    PROP(kAudioUnitProperty_MakeConnection)
    PROP(kAudioUnitProperty_SampleRate)
    PROP(kAudioUnitProperty_ParameterList)
    PROP(kAudioUnitProperty_ParameterInfo)
   #if !TARGET_OS_IPHONE
    PROP(kAudioUnitProperty_FastDispatch)
   #endif
    PROP(kAudioUnitProperty_CPULoad)
    PROP(kAudioUnitProperty_StreamFormat)
    PROP(kAudioUnitProperty_ElementCount)
    PROP(kAudioUnitProperty_Latency)
    PROP(kAudioUnitProperty_SupportedNumChannels)
    PROP(kAudioUnitProperty_MaximumFramesPerSlice)
    PROP(kAudioUnitProperty_ParameterValueStrings)
    PROP(kAudioUnitProperty_AudioChannelLayout)
    PROP(kAudioUnitProperty_TailTime)
    PROP(kAudioUnitProperty_BypassEffect)
    PROP(kAudioUnitProperty_LastRenderError)
    PROP(kAudioUnitProperty_SetRenderCallback)
    PROP(kAudioUnitProperty_FactoryPresets)
    PROP(kAudioUnitProperty_RenderQuality)
    PROP(kAudioUnitProperty_HostCallbacks)
    PROP(kAudioUnitProperty_InPlaceProcessing)
    PROP(kAudioUnitProperty_ElementName)
    PROP(kAudioUnitProperty_SupportedChannelLayoutTags)
    PROP(kAudioUnitProperty_PresentPreset)
    PROP(kAudioUnitProperty_DependentParameters)
    PROP(kAudioUnitProperty_InputSamplesInOutput)
    PROP(kAudioUnitProperty_ShouldAllocateBuffer)
    PROP(kAudioUnitProperty_FrequencyResponse)
    PROP(kAudioUnitProperty_ParameterHistoryInfo)
    PROP(kAudioUnitProperty_NickName)
    PROP(kAudioUnitProperty_OfflineRender)
    PROP(kAudioUnitProperty_ParameterIDName)
    PROP(kAudioUnitProperty_ParameterStringFromValue)
    PROP(kAudioUnitProperty_ParameterClumpName)
    PROP(kAudioUnitProperty_ParameterValueFromString)
    PROP(kAudioUnitProperty_PresentationLatency)
    PROP(kAudioUnitProperty_ClassInfoFromDocument)
    PROP(kAudioUnitProperty_RequestViewController)
    PROP(kAudioUnitProperty_ParametersForOverview)
    PROP(kAudioUnitProperty_SupportsMPE)
    PROP(kAudioUnitProperty_RenderContextObserver)
    PROP(kAudioUnitProperty_LastRenderSampleTime)
    PROP(kAudioUnitProperty_LoadedOutOfProcess)
   #if !TARGET_OS_IPHONE
    PROP(kAudioUnitProperty_SetExternalBuffer)
    PROP(kAudioUnitProperty_GetUIComponentList)
    PROP(kAudioUnitProperty_CocoaUI)
    PROP(kAudioUnitProperty_IconLocation)
    PROP(kAudioUnitProperty_AUHostIdentifier)
   #endif
    PROP(kAudioUnitProperty_MIDIOutputCallbackInfo)
    PROP(kAudioUnitProperty_MIDIOutputCallback)
    PROP(kAudioUnitProperty_MIDIOutputEventListCallback)
    PROP(kAudioUnitProperty_AudioUnitMIDIProtocol)
    PROP(kAudioUnitProperty_HostMIDIProtocol)
    PROP(kAudioUnitProperty_MIDIOutputBufferSizeHint)
    PROP(kMusicDeviceProperty_DualSchedulingMode)
    #undef PROP
    // DAF specific properties
    #define PROPX(s) (s[0] << 24) | (s[1] << 16) | (s[2] << 8) | (s[3] << 0)
    #define PROP(s) case PROPX(#s): return #s;
    PROP(DPFi)
    PROP(DPFe)
    PROP(DPFp)
    PROP(DPFn)
    PROP(DPFo)
    PROP(DPFl)
    PROP(DPFs)
    PROP(DPFa)
    #undef PROP
    #undef PROPX
    }
    return "[unknown]";
}

static const char* AudioUnitScope2Str(const AudioUnitScope scope) noexcept
{
    switch (scope)
    {
    #define SCOPE(s) case s: return #s;
    SCOPE(kAudioUnitScope_Global)
    SCOPE(kAudioUnitScope_Input)
    SCOPE(kAudioUnitScope_Output)
    SCOPE(kAudioUnitScope_Group)
    SCOPE(kAudioUnitScope_Part)
    SCOPE(kAudioUnitScope_Note)
    SCOPE(kAudioUnitScope_Layer)
    SCOPE(kAudioUnitScope_LayerItem)
    #undef SCOPE
    }
    return "[unknown]";
}

static const char* AudioUnitSelector2Str(const SInt16 selector) noexcept
{
    switch (selector)
    {
    #define SEL(s) case s: return #s;
    SEL(kAudioUnitInitializeSelect)
    SEL(kAudioUnitUninitializeSelect)
    SEL(kAudioUnitGetPropertyInfoSelect)
    SEL(kAudioUnitGetPropertySelect)
    SEL(kAudioUnitSetPropertySelect)
    SEL(kAudioUnitAddPropertyListenerSelect)
    SEL(kAudioUnitRemovePropertyListenerSelect)
    SEL(kAudioUnitRemovePropertyListenerWithUserDataSelect)
    SEL(kAudioUnitAddRenderNotifySelect)
    SEL(kAudioUnitRemoveRenderNotifySelect)
    SEL(kAudioUnitGetParameterSelect)
    SEL(kAudioUnitSetParameterSelect)
    SEL(kAudioUnitScheduleParametersSelect)
    SEL(kAudioUnitRenderSelect)
    SEL(kAudioUnitResetSelect)
    SEL(kAudioUnitComplexRenderSelect)
    SEL(kAudioUnitProcessSelect)
    SEL(kAudioUnitProcessMultipleSelect)
    SEL(kMusicDeviceMIDIEventSelect)
    SEL(kMusicDeviceSysExSelect)
    SEL(kMusicDevicePrepareInstrumentSelect)
    SEL(kMusicDeviceReleaseInstrumentSelect)
    SEL(kMusicDeviceStartNoteSelect)
    SEL(kMusicDeviceStopNoteSelect)
    SEL(kMusicDeviceMIDIEventListSelect)
    SEL(kAudioOutputUnitStartSelect)
    SEL(kAudioOutputUnitStopSelect)
    #undef SEL
    }
    return "[unknown]";
}

#if 0
static OSStatus FastDispatchGetParameter(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, Float32*);
static OSStatus FastDispatchSetParameter(void*, AudioUnitParameterID, AudioUnitScope, AudioUnitElement, Float32, UInt32);
static OSStatus FastDispatchRender(void*, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32, AudioBufferList*);
#endif

// --------------------------------------------------------------------------------------------------------------------

static constexpr const uint32_t kType = d_cconst(STRINGIFY(DAF_PLUGIN_AU_TYPE));
static constexpr const uint32_t kSubType = d_cconst(STRINGIFY(DAF_PLUGIN_UNIQUE_ID));
static constexpr const uint32_t kManufacturer = d_cconst(STRINGIFY(DAF_PLUGIN_BRAND_ID));

static constexpr const uint32_t kWantedAudioFormat = 0
                                                   | kAudioFormatFlagsNativeFloatPacked
                                                   | kAudioFormatFlagIsNonInterleaved;


static constexpr const AUChannelInfo kChannelInfo[] = {
    { DAF_PLUGIN_NUM_INPUTS, DAF_PLUGIN_NUM_OUTPUTS },
   #ifdef DAF_PLUGIN_EXTRA_IO
    DAF_PLUGIN_EXTRA_IO
   #endif
};

#ifdef DAF_PLUGIN_EXTRA_IO
#if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS == 0
#error DAF_PLUGIN_EXTRA_IO defined but no IO available
#endif

static inline
bool isOutputNumChannelsValid(const uint16_t numChannels)
{
    for (uint16_t i = 0; i < ARRAY_SIZE(kChannelInfo); ++i)
    {
        if (kChannelInfo[i].outChannels == numChannels)
            return true;
    }
    return false;
}

static inline
bool isNumChannelsComboValid(const uint16_t numInputs, const uint16_t numOutputs)
{
    for (uint16_t i = 0; i < ARRAY_SIZE(kChannelInfo); ++i)
    {
        if (kChannelInfo[i].inChannels == numInputs && kChannelInfo[i].outChannels == numOutputs)
            return true;
    }
    return false;
}
#endif

// --------------------------------------------------------------------------------------------------------------------

struct PropertyListener {
    AudioUnitPropertyID prop;
    AudioUnitPropertyListenerProc proc;
    void* userData;
};

struct RenderListener {
    AURenderCallback proc;
    void* userData;
};

#if DAF_PLUGIN_NUM_INPUTS != 0
struct AUInputBus {
    uint32_t groupId;
    uint16_t declaredChannels;
    uint16_t channels;
    bool grouped;
    bool sidechain;
    AudioUnitConnection connection;
    AURenderCallbackStruct renderCallback;
    Float64 sampleRate;
    AUBufferList bufferList;

    AUInputBus() noexcept
        : groupId(kPortGroupNone),
          declaredChannels(0),
          channels(0),
          grouped(false),
          sidechain(false),
          connection(),
          renderCallback(),
          sampleRate(0.0),
          bufferList()
    {
        std::memset(&connection, 0, sizeof(connection));
        std::memset(&renderCallback, 0, sizeof(renderCallback));
    }

    bool hasSource() const noexcept
    {
        return connection.sourceAudioUnit != nullptr || renderCallback.inputProc != nullptr;
    }
};
#endif

typedef std::vector<PropertyListener> PropertyListeners;
typedef std::vector<RenderListener> RenderListeners;

#if DAF_PLUGIN_WANT_MIDI_OUTPUT
// useful definitions
static constexpr const uint32_t kMIDIPacketNonDataSize = sizeof(MIDIPacket)
                                                      #if __cplusplus >= 201103L
                                                       - sizeof(MIDIPacket::data);
                                                      #else
                                                       - sizeof(static_cast<MIDIPacket*>(0)->data);
                                                      #endif
static constexpr const uint32_t kMIDIPacketListNonDataSize = sizeof(MIDIPacketList)
                                                          #if __cplusplus >= 201103L
                                                           - sizeof(MIDIPacketList::packet);
                                                          #else
                                                           - sizeof(static_cast<MIDIPacketList*>(0)->packet);
                                                          #endif

// size of data used for midi events
static constexpr const uint32_t kMIDIPacketListMaxDataSize = kMIDIPacketNonDataSize * kMaxMidiEvents
                                                           + sizeof(Byte) * MidiEvent::kDataSize * kMaxMidiEvents;

// size of midi list + data
static constexpr const uint32_t kMIDIPacketListSize = kMIDIPacketListNonDataSize + kMIDIPacketListMaxDataSize;
#endif

// --------------------------------------------------------------------------------------------------------------------

#if ! DAF_PLUGIN_WANT_MIDI_OUTPUT
static constexpr const writeMidiFunc writeMidiCallback = nullptr;
#endif
#if ! DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
static constexpr const requestParameterValueChangeFunc requestParameterValueChangeCallback = nullptr;
#endif
#if ! DAF_PLUGIN_WANT_STATE
static constexpr const updateStateValueFunc updateStateValueCallback = nullptr;
#endif
#if DAF_PLUGIN_WANT_TIMEPOS
static constexpr const double kDefaultTicksPerBeat = 1920.0;
#endif

typedef std::map<const String, String> StringMap;

// --------------------------------------------------------------------------------------------------------------------

class PluginAU
{
public:
    PluginAU(const AudioUnit component)
        : fPlugin(this, writeMidiCallback, requestParameterValueChangeCallback, updateStateValueCallback),
          fComponent(component),
          fLastRenderError(noErr),
          fPropertyListeners(),
          fRenderListeners(),
        #if DAF_PLUGIN_NUM_INPUTS != 0
          fInputBusCount(0),
         #ifdef DAF_PLUGIN_EXTRA_IO
          fNumInputs(DAF_PLUGIN_NUM_INPUTS),
         #endif
        #endif
        #if DAF_PLUGIN_NUM_OUTPUTS != 0
          fSampleRateForOutput(d_nextSampleRate),
         #ifdef DAF_PLUGIN_EXTRA_IO
          fNumOutputs(DAF_PLUGIN_NUM_OUTPUTS),
         #endif
        #endif
         #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
          fAudioBufferList(),
         #endif
          fChannelInfoCount(0),
          fUsingRenderListeners(false),
          fParameterCount(fPlugin.getParameterCount()),
          fLastParameterValues(nullptr),
          fBypassParameterIndex(UINT32_MAX),
          fResetParameterIndex(UINT32_MAX)
       #if DAF_PLUGIN_WANT_MIDI_INPUT
        , fMidiEventCount(0)
       #endif
       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        , fMidiOutputDataOffset(0)
       #endif
       #if DAF_PLUGIN_WANT_PROGRAMS
        , fCurrentProgram(-1)
        , fLastFactoryProgram(0)
        , fProgramCount(fPlugin.getProgramCount())
        , fFactoryPresetsData(nullptr)
       #endif
       #if DAF_PLUGIN_WANT_STATE
        , fStateCount(fPlugin.getStateCount())
       #endif
    {
        if (fParameterCount != 0)
        {
            fLastParameterValues = new float[fParameterCount];
            std::memset(fLastParameterValues, 0, sizeof(float) * fParameterCount);

            for (uint32_t i=0; i<fParameterCount; ++i)
            {
                fLastParameterValues[i] = fPlugin.getParameterValue(i);

                switch (fPlugin.getParameterDesignation(i))
                {
                case kParameterDesignationNull:
                    break;
                case kParameterDesignationBypass:
                    fBypassParameterIndex = i;
                    break;
                case kParameterDesignationReset:
                    fResetParameterIndex = i;
                    break;
                }
            }
        }

       #if DAF_PLUGIN_NUM_INPUTS != 0
        configureInputBuses();
       #else
        configureChannelInfo();
       #endif

       #if DAF_PLUGIN_WANT_MIDI_INPUT
        std::memset(&fMidiEvents, 0, sizeof(fMidiEvents));
       #endif

       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        if ((fMidiOutputPackets = static_cast<MIDIPacketList*>(std::malloc(kMIDIPacketListSize))) != nullptr)
            std::memset(fMidiOutputPackets, 0, kMIDIPacketListSize);

        std::memset(&fMidiOutput, 0, sizeof(fMidiOutput));
       #endif

       #if DAF_PLUGIN_WANT_PROGRAMS
        if (fProgramCount != 0)
        {
            fFactoryPresetsData = new AUPreset[fProgramCount];
            std::memset(fFactoryPresetsData, 0, sizeof(AUPreset) * fProgramCount);

            for (uint32_t i=0; i<fProgramCount; ++i)
            {
                fFactoryPresetsData[i].presetNumber = i;

                if (const CFStringRef nameRef = CFStringCreateWithCString(nullptr,
                                                                          fPlugin.getProgramName(i),
                                                                          kCFStringEncodingUTF8))
                    fFactoryPresetsData[i].presetName = nameRef;
                else
                    fFactoryPresetsData[i].presetName = CFSTR("");
            }
        }
        else
        {
            fFactoryPresetsData = new AUPreset[1];
            std::memset(fFactoryPresetsData, 0, sizeof(AUPreset));

            fFactoryPresetsData->presetNumber = 0;
            fFactoryPresetsData->presetName = CFSTR("Default");
        }
       #endif

        fUserPresetData.presetNumber = -1;
        fUserPresetData.presetName = CFSTR("");

       #if DAF_PLUGIN_WANT_STATE
        for (uint32_t i=0; i<fStateCount; ++i)
        {
            const String& dkey(fPlugin.getStateKey(i));
            fStateMap[dkey] = fPlugin.getStateDefaultValue(i);
        }
       #endif

       #if DAF_PLUGIN_WANT_TIMEPOS
        std::memset(&fHostCallbackInfo, 0, sizeof(fHostCallbackInfo));
        fTimePosition.bbt.ticksPerBeat = kDefaultTicksPerBeat;
       #endif
    }

    ~PluginAU()
    {
        delete[] fLastParameterValues;
        CFRelease(fUserPresetData.presetName);

       #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
        reallocAudioBufferList(false);
       #endif

       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        std::free(fMidiOutputPackets);
       #endif

       #if DAF_PLUGIN_WANT_PROGRAMS
        for (uint32_t i=0; i<fProgramCount; ++i)
            CFRelease(fFactoryPresetsData[i].presetName);
        delete[] fFactoryPresetsData;
       #endif
    }

    OSStatus auInitialize()
    {
       #ifdef DAF_PLUGIN_EXTRA_IO
        if (! updatePluginAudioPortIO())
            return kAudioUnitErr_FormatNotSupported;
       #endif

       #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
        if (! reallocAudioBufferList(true))
            return kAudio_ParamError;
       #endif

       #if DAF_PLUGIN_WANT_MIDI_INPUT
        fMidiEventCount = 0;
       #endif
       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        fMidiOutputDataOffset = 0;
        fMidiOutputPackets->numPackets = 0;
       #endif
       #if DAF_PLUGIN_WANT_TIMEPOS
        fTimePosition.clear();
        fTimePosition.bbt.ticksPerBeat = kDefaultTicksPerBeat;
       #endif

        fPlugin.activate();
        return noErr;
    }

    OSStatus auUninitialize()
    {
        fPlugin.deactivateIfNeeded();

       #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
        reallocAudioBufferList(false);
       #endif
        return noErr;
    }

    OSStatus auGetPropertyInfo(const AudioUnitPropertyID inProp,
                               const AudioUnitScope inScope,
                               const AudioUnitElement inElement,
                               UInt32& outDataSize,
                               Boolean& outWritable)
    {
        switch (inProp)
        {
        case kAudioUnitProperty_ClassInfo:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(CFPropertyListRef);
            outWritable = true;
            return noErr;

        case kAudioUnitProperty_MakeConnection:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input, inScope, kAudioUnitErr_InvalidScope);
           #if DAF_PLUGIN_NUM_INPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(AudioUnitConnection);
            outWritable = true;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_SampleRate:
           #if DAF_PLUGIN_NUM_INPUTS != 0
            if (inScope == kAudioUnitScope_Input)
            {
                DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
                outDataSize = sizeof(Float64);
                outWritable = true;
                return noErr;
            }
           #endif
           #if DAF_PLUGIN_NUM_OUTPUTS != 0
            if (inScope == kAudioUnitScope_Output)
            {
                DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
                outDataSize = sizeof(Float64);
                outWritable = true;
                return noErr;
            }
           #endif
            return kAudioUnitErr_InvalidScope;

        case kAudioUnitProperty_ParameterList:
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = inScope == kAudioUnitScope_Global ? sizeof(AudioUnitParameterID) * fParameterCount : 0;
            outWritable = false;
            return noErr;

        case kAudioUnitProperty_ParameterInfo:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fParameterCount, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(AudioUnitParameterInfo);
            outWritable = false;
            return noErr;

        case kAudioUnitProperty_ParameterStringFromValue:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(AudioUnitParameterStringFromValue);
            outWritable = false;
            return noErr;

        case kAudioUnitProperty_ParameterValueFromString:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(AudioUnitParameterValueFromString);
            outWritable = false;
            return noErr;

       #if 0
        case kAudioUnitProperty_FastDispatch:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            outDataSize = sizeof(void*);
            outWritable = false;
            return noErr;
       #endif

        case kAudioUnitProperty_StreamFormat:
           #if DAF_PLUGIN_NUM_INPUTS != 0
            if (inScope == kAudioUnitScope_Input)
            {
                DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
                outDataSize = sizeof(AudioStreamBasicDescription);
                outWritable = true;
                return noErr;
            }
           #endif
           #if DAF_PLUGIN_NUM_OUTPUTS != 0
            if (inScope == kAudioUnitScope_Output)
            {
                DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
                outDataSize = sizeof(AudioStreamBasicDescription);
                outWritable = true;
                return noErr;
            }
           #endif
            return kAudioUnitErr_InvalidScope;

        case kAudioUnitProperty_ElementCount:
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(UInt32);
            outWritable = false;
            return noErr;

        case kAudioUnitProperty_Latency:
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_LATENCY
            if (inScope == kAudioUnitScope_Global)
            {
                outDataSize = sizeof(Float64);
                outWritable = false;
                return noErr;
            }
           #endif
            return kAudioUnitErr_InvalidProperty;

        case kAudioUnitProperty_TailTime:
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_TAIL
            if (inScope == kAudioUnitScope_Global)
            {
                outDataSize = sizeof(Float64);
                outWritable = false;
                return noErr;
            }
           #endif
            return kAudioUnitErr_InvalidProperty;

        case kAudioUnitProperty_SupportedNumChannels:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(AUChannelInfo) * fChannelInfoCount;
            outWritable = false;
            return noErr;

        case kAudioUnitProperty_MaximumFramesPerSlice:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(UInt32);
            outWritable = true;
            return noErr;

        case kAudioUnitProperty_BypassEffect:
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            if (inScope == kAudioUnitScope_Global && fBypassParameterIndex != UINT32_MAX)
            {
                outDataSize = sizeof(UInt32);
                outWritable = true;
                return noErr;
            }
            return kAudioUnitErr_InvalidProperty;

        case kAudioUnitProperty_LastRenderError:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(OSStatus);
            outWritable = false;
            return noErr;

        case kAudioUnitProperty_SetRenderCallback:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input, inScope, kAudioUnitErr_InvalidScope);
           #if DAF_PLUGIN_NUM_INPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(AURenderCallbackStruct);
            outWritable = true;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_FactoryPresets:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_PROGRAMS
            outDataSize = sizeof(CFArrayRef);
            outWritable = false;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_HostCallbacks:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_TIMEPOS
            outDataSize = sizeof(HostCallbackInfo);
            outWritable = true;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_InPlaceProcessing:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
            // constant for this unit, which never processes in place; see the getter
            outDataSize = sizeof(UInt32);
            outWritable = false;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_PresentPreset:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(AUPreset);
            outWritable = true;
            return noErr;

        case kAudioUnitProperty_SupportsMPE:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_MIDI_AS_MPE
            outDataSize = sizeof(UInt32);
            outWritable = false;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_CocoaUI:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_HAS_UI
            outDataSize = sizeof(AudioUnitCocoaViewInfo);
            outWritable = false;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_MIDIOutputCallbackInfo:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_MIDI_OUTPUT
            outDataSize = sizeof(CFArrayRef);
            outWritable = false;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_MIDIOutputCallback:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_MIDI_OUTPUT
            outDataSize = sizeof(AUMIDIOutputCallbackStruct);
            outWritable = true;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case kAudioUnitProperty_AudioUnitMIDIProtocol:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            // FIXME implement the event list stuff
           #if 0 && (DAF_PLUGIN_WANT_MIDI_INPUT || DAF_PLUGIN_WANT_MIDI_OUTPUT)
            outDataSize = sizeof(SInt32);
            outWritable = false;
            return noErr;
           #else
            return kAudioUnitErr_InvalidProperty;
           #endif

        case 'DPFi':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(uint16_t);
            outWritable = true;
            return noErr;

        case 'DPFe':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fParameterCount, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(uint8_t);
            outWritable = true;
            return noErr;

        case 'DPFp':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fParameterCount, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(float);
            outWritable = true;
            return noErr;

       #if DAF_PLUGIN_WANT_MIDI_INPUT
        case 'DPFn':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(uint8_t) * 3;
            outWritable = true;
            return noErr;
       #endif

       #if DAF_PLUGIN_WANT_PROGRAMS
        case 'DPFo':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(uint32_t);
            outWritable = true;
            return noErr;
       #endif

       #if DAF_PLUGIN_WANT_STATE
        case 'DPFl':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(CFArrayRef);
            outWritable = true;
            return noErr;

        case 'DPFs':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fStateCount, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(CFStringRef);
            outWritable = true;
            return noErr;
       #endif

       #if DAF_PLUGIN_HAS_UI && DAF_PLUGIN_WANT_DIRECT_ACCESS
        case 'DPFa':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            outDataSize = sizeof(void*);
            outWritable = true;
            return noErr;
       #endif

        // unwanted properties
        case kAudioUnitProperty_CPULoad:
        case kAudioUnitProperty_RenderContextObserver:
        case kAudioUnitProperty_AudioChannelLayout:
        case kAudioUnitProperty_SupportedChannelLayoutTags:
        case kMusicDeviceProperty_DualSchedulingMode:
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            return kAudioUnitErr_InvalidProperty;
        }

        d_stdout("TODO GetPropertyInfo(%d:%x:%s, %d:%s, %d, ...)",
                 inProp, inProp, AudioUnitPropertyID2Str(inProp), inScope, AudioUnitScope2Str(inScope), inElement);
        return kAudioUnitErr_InvalidProperty;
    }

    OSStatus auGetProperty(const AudioUnitPropertyID inProp,
                           const AudioUnitScope inScope,
                           const AudioUnitElement inElement,
                           void* const outData)
    {
        switch (inProp)
        {
        case kAudioUnitProperty_ClassInfo:
            *static_cast<CFPropertyListRef*>(outData) = retrieveClassInfo();
            return noErr;

        case kAudioUnitProperty_SampleRate:
           #if DAF_PLUGIN_NUM_INPUTS != 0
            if (inScope == kAudioUnitScope_Input)
            {
                if (inElement >= fInputBusCount)
                    return kAudioUnitErr_InvalidElement;
                *static_cast<Float64*>(outData) = fInputBuses[inElement].sampleRate;
            }
            else
           #endif
           #if DAF_PLUGIN_NUM_OUTPUTS != 0
            if (inScope == kAudioUnitScope_Output)
            {
                *static_cast<Float64*>(outData) = fSampleRateForOutput;
            }
            else
           #endif
            {
                return kAudioUnitErr_InvalidScope;
            }
            return noErr;

        case kAudioUnitProperty_ParameterList:
            {
                AudioUnitParameterID* const paramList = static_cast<AudioUnitParameterID*>(outData);

                for (uint32_t i=0; i<fParameterCount; ++i)
                    paramList[i] = i;
            }
            return noErr;

        case kAudioUnitProperty_ParameterInfo:
            {
                AudioUnitParameterInfo* const info = static_cast<AudioUnitParameterInfo*>(outData);
                std::memset(info, 0, sizeof(*info));

                const ParameterRanges& ranges(fPlugin.getParameterRanges(inElement));

                info->flags = kAudioUnitParameterFlag_IsHighResolution
                            | kAudioUnitParameterFlag_IsReadable
                            | kAudioUnitParameterFlag_HasCFNameString;

                if (fPlugin.getParameterDesignation(inElement) == kParameterDesignationBypass)
                {
                    info->flags |= kAudioUnitParameterFlag_IsWritable;
                    info->unit = kAudioUnitParameterUnit_Boolean;

                    d_strncpy(info->name, "Bypass", sizeof(info->name));
                    info->cfNameString = CFSTR("Bypass");
                }
                else
                {
                    const uint32_t hints = fPlugin.getParameterHints(inElement);

                    info->flags |= kAudioUnitParameterFlag_CFNameRelease;

                    if (hints & kParameterIsOutput)
                    {
                        info->flags |= kAudioUnitParameterFlag_MeterReadOnly;
                    }
                    else
                    {
                        info->flags |= kAudioUnitParameterFlag_IsWritable;

                        if ((hints & kParameterIsAutomatable) == 0x0)
                            info->flags |= kAudioUnitParameterFlag_NonRealTime;
                    }

                    if (hints & kParameterIsBoolean)
                        info->unit = kAudioUnitParameterUnit_Boolean;
                    else if (hints & kParameterIsInteger)
                        info->unit = kAudioUnitParameterUnit_Indexed;
                    else
                        info->unit = kAudioUnitParameterUnit_Generic;

                    const String& name(fPlugin.getParameterName(inElement));
                    d_strncpy(info->name, name, sizeof(info->name));
                    info->cfNameString = CFStringCreateWithCString(nullptr, name, kCFStringEncodingUTF8);
                }

                if (fPlugin.hasCustomParameterText(inElement))
                    info->flags |= kAudioUnitParameterFlag_ValuesHaveStrings;

                info->minValue = ranges.min;
                info->maxValue = ranges.max;
                info->defaultValue = ranges.def;
            }
            return noErr;

        case kAudioUnitProperty_ParameterStringFromValue:
            {
                AudioUnitParameterStringFromValue* const request = static_cast<AudioUnitParameterStringFromValue*>(outData);
                if (request == nullptr || request->inParamID >= fParameterCount)
                    return kAudio_ParamError;
                if (!fPlugin.hasCustomParameterText(request->inParamID))
                    return kAudioUnitErr_InvalidPropertyValue;

                char text[256];
                const float value = request->inValue != nullptr
                                  ? *request->inValue : fPlugin.getParameterValue(request->inParamID);
                if (!fPlugin.getParameterValueText(request->inParamID, value, text, sizeof(text)))
                    return kAudioUnitErr_InvalidPropertyValue;
                request->outString = CFStringCreateWithCString(nullptr, text, kCFStringEncodingUTF8);
                if (request->outString == nullptr)
                    return kAudio_ParamError;
                return noErr;
            }

        case kAudioUnitProperty_ParameterValueFromString:
            {
                AudioUnitParameterValueFromString* const request = static_cast<AudioUnitParameterValueFromString*>(outData);
                if (request == nullptr || request->inParamID >= fParameterCount || request->inString == nullptr)
                    return kAudio_ParamError;
                if (!fPlugin.hasCustomParameterText(request->inParamID))
                    return kAudioUnitErr_InvalidPropertyValue;

                char text[256];
                if (!CFStringGetCString(request->inString, text, sizeof(text), kCFStringEncodingUTF8))
                    return kAudioUnitErr_InvalidPropertyValue;
                float value;
                if (!fPlugin.getParameterValueFromText(request->inParamID, text, value))
                    return kAudioUnitErr_InvalidPropertyValue;
                request->outValue = value;
                return noErr;
            }

       #if 0
        case kAudioUnitProperty_FastDispatch:
            switch (inElement)
            {
            case kAudioUnitGetParameterSelect:
                *static_cast<AudioUnitGetParameterProc*>(outData) = FastDispatchGetParameter;
                return noErr;
            case kAudioUnitSetParameterSelect:
                *static_cast<AudioUnitSetParameterProc*>(outData) = FastDispatchSetParameter;
                return noErr;
            case kAudioUnitRenderSelect:
                *static_cast<AudioUnitRenderProc*>(outData) = FastDispatchRender;
                return noErr;
            }
            d_stdout("WIP FastDispatch(%d:%x:%s)", inElement, inElement, AudioUnitPropertyID2Str(inElement));
            return kAudioUnitErr_InvalidElement;
       #endif

        case kAudioUnitProperty_StreamFormat:
            {
                AudioStreamBasicDescription* const desc = static_cast<AudioStreamBasicDescription*>(outData);
                std::memset(desc, 0, sizeof(*desc));

               #if DAF_PLUGIN_NUM_INPUTS != 0
                if (inScope == kAudioUnitScope_Input)
                {
                    if (inElement >= fInputBusCount)
                        return kAudioUnitErr_InvalidElement;
                    desc->mChannelsPerFrame = fInputBuses[inElement].channels;
                    desc->mSampleRate = fInputBuses[inElement].sampleRate;
                }
                else
               #endif
               #if DAF_PLUGIN_NUM_OUTPUTS != 0
                if (inScope == kAudioUnitScope_Output)
                {
                    if (inElement != 0)
                        return kAudioUnitErr_InvalidElement;
                   #ifdef DAF_PLUGIN_EXTRA_IO
                    desc->mChannelsPerFrame = fNumOutputs;
                    #else
                    desc->mChannelsPerFrame = DAF_PLUGIN_NUM_OUTPUTS;
                   #endif
                    desc->mSampleRate = fSampleRateForOutput;
                }
                else
               #endif
                {
                    return kAudioUnitErr_InvalidScope;
                }

                desc->mFormatID         = kAudioFormatLinearPCM;
                desc->mFormatFlags      = kWantedAudioFormat;
                desc->mBitsPerChannel   = 32;
                desc->mBytesPerFrame    = sizeof(float);
                desc->mBytesPerPacket   = sizeof(float);
                desc->mFramesPerPacket  = 1;
            }
            return noErr;

        case kAudioUnitProperty_ElementCount:
            switch (inScope)
            {
            case kAudioUnitScope_Global:
                *static_cast<UInt32*>(outData) = 1;
                break;
            case kAudioUnitScope_Input:
               #if DAF_PLUGIN_NUM_INPUTS != 0
                *static_cast<UInt32*>(outData) = fInputBusCount;
               #else
                *static_cast<UInt32*>(outData) = 0;
               #endif
                break;
            case kAudioUnitScope_Output:
                *static_cast<UInt32*>(outData) = DAF_PLUGIN_NUM_OUTPUTS != 0 ? 1 : 0;
                break;
            default:
                *static_cast<UInt32*>(outData) = 0;
                break;
            }
            return noErr;

       #if DAF_PLUGIN_WANT_LATENCY
        case kAudioUnitProperty_Latency:
            *static_cast<Float64*>(outData) = static_cast<double>(fPlugin.getLatency()) / fPlugin.getSampleRate();
            return noErr;
       #endif

       #if DAF_PLUGIN_WANT_TAIL
        case kAudioUnitProperty_TailTime:
        {
            // Seconds. AU has no "infinite" sentinel, so an endless tail is reported
            // as one hour: long enough that no bounce stops early, finite enough
            // that a host doing arithmetic on it stays sane.
            const uint32_t tail = fPlugin.getTail();
            *static_cast<Float64*>(outData) = tail == Plugin::kTailInfinite
                                            ? 3600.0
                                            : static_cast<double>(tail) / fPlugin.getSampleRate();
            return noErr;
        }
       #endif

        case kAudioUnitProperty_SupportedNumChannels:
            std::memcpy(outData, fChannelInfo, sizeof(AUChannelInfo) * fChannelInfoCount);
            return noErr;

        case kAudioUnitProperty_MaximumFramesPerSlice:
            *static_cast<UInt32*>(outData) = fPlugin.getBufferSize();
            return noErr;

        case kAudioUnitProperty_LastRenderError:
            *static_cast<OSStatus*>(outData) = fLastRenderError;
            fLastRenderError = noErr;
            return noErr;

        case kAudioUnitProperty_BypassEffect:
            *static_cast<OSStatus*>(outData) = fPlugin.getParameterValue(fBypassParameterIndex) > 0.5f ? 1 : 0;
            return noErr;

       #if DAF_PLUGIN_NUM_INPUTS != 0
        case kAudioUnitProperty_SetRenderCallback:
            if (inElement >= fInputBusCount)
                return kAudioUnitErr_InvalidElement;
            std::memcpy(outData, &fInputBuses[inElement].renderCallback, sizeof(AURenderCallbackStruct));
            return noErr;
       #endif

       #if DAF_PLUGIN_WANT_PROGRAMS
        case kAudioUnitProperty_FactoryPresets:
            if (const CFMutableArrayRef presetsRef = CFArrayCreateMutable(nullptr, fProgramCount, nullptr))
            {
                for (uint32_t i=0; i<fProgramCount; ++i)
                    CFArrayAppendValue(presetsRef, &fFactoryPresetsData[i]);

                *static_cast<CFArrayRef*>(outData) = presetsRef;
                return noErr;
            }
            return kAudio_ParamError;
       #endif

       #if DAF_PLUGIN_WANT_TIMEPOS
        case kAudioUnitProperty_HostCallbacks:
            std::memcpy(outData, &fHostCallbackInfo, sizeof(HostCallbackInfo));
            return noErr;
       #endif

       #if DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
        case kAudioUnitProperty_InPlaceProcessing:
            /* This unit does not process in place. Every input bus, the main one included, is
             * pulled into a buffer list of its own in auRender, so the buffers the plugin reads
             * are not the buffers the host handed us to write into. Claiming otherwise was what
             * had auval reporting that the unit says it processes in place while its input and
             * output buffers differ.
             *
             * Reporting 0 costs the host one buffer copy it could otherwise have skipped and is
             * always safe; matching the claim instead would mean pulling the main bus straight
             * into ioData, which changes what run() is handed and is not a property change. */
            *static_cast<UInt32*>(outData) = 0;
            return noErr;
       #endif

        case kAudioUnitProperty_PresentPreset:
           #if DAF_PLUGIN_WANT_PROGRAMS
            if (fCurrentProgram >= 0)
            {
                std::memcpy(outData, &fFactoryPresetsData[fCurrentProgram], sizeof(AUPreset));
            }
            else
           #endif
            {
                std::memcpy(outData, &fUserPresetData, sizeof(AUPreset));
            }
            if (static_cast<AUPreset*>(outData)->presetName != nullptr)
                CFRetain(static_cast<AUPreset*>(outData)->presetName);
            return noErr;

       #if DAF_PLUGIN_WANT_MIDI_AS_MPE
        case kAudioUnitProperty_SupportsMPE:
            *static_cast<UInt32*>(outData) = 1;
            return noErr;
       #endif

       #if DAF_PLUGIN_HAS_UI
        case kAudioUnitProperty_CocoaUI:
            {
                AudioUnitCocoaViewInfo* const info = static_cast<AudioUnitCocoaViewInfo*>(outData);
                std::memset(info, 0, sizeof(*info));

                NSString* const bundlePathString = [[NSString alloc]
                    initWithBytes:d_nextBundlePath
                           length:strlen(d_nextBundlePath)
                         encoding:NSUTF8StringEncoding];

                info->mCocoaAUViewBundleLocation = static_cast<CFURLRef>([[NSURL fileURLWithPath: bundlePathString] retain]);

                #define MACRO_STR3(a, b, c) a "_" b "_" c
                #define MACRO_STR2(a, b, c) MACRO_STR3(#a, #b, #c)
                #define MACRO_STR(a, b, c) MACRO_STR2(a, b, c)

                info->mCocoaAUViewClass[0] = CFSTR("CocoaAUView_" MACRO_STR(DAF_PLUGIN_AU_TYPE,
                                                                            DAF_PLUGIN_UNIQUE_ID,
                                                                            DAF_PLUGIN_BRAND_ID));

                #undef MACRO_STR
                #undef MACRO_STR2
                #undef MACRO_STR3

                [bundlePathString release];
            }
            return noErr;
       #endif

       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        case kAudioUnitProperty_MIDIOutputCallbackInfo:
            {
                CFStringRef refs[1] = { CFSTR("MIDI Output") };
                *static_cast<CFArrayRef*>(outData) = CFArrayCreate(nullptr,
                                                                   reinterpret_cast<const void**>(refs),
                                                                   1,
                                                                   &kCFTypeArrayCallBacks);
            }
            return noErr;
       #endif

       #if DAF_PLUGIN_WANT_MIDI_INPUT || DAF_PLUGIN_WANT_MIDI_OUTPUT
        /* FIXME implement the event list stuff
        case kAudioUnitProperty_AudioUnitMIDIProtocol:
            *static_cast<SInt32*>(outData) = kMIDIProtocol_1_0;
            return noErr;
        */
       #endif

        case 'DPFp':
            *static_cast<float*>(outData) = fPlugin.getParameterValue(inElement);
            return noErr;

       #if DAF_PLUGIN_WANT_PROGRAMS
        case 'DPFo':
            *static_cast<uint32_t*>(outData) = fLastFactoryProgram;
            return noErr;
       #endif

       #if DAF_PLUGIN_WANT_STATE
        case 'DPFl':
            if (const CFMutableArrayRef keysRef = CFArrayCreateMutable(nullptr,
                                                                       fStateCount,
                                                                       &kCFTypeArrayCallBacks))
            {
                for (uint32_t i=0; i<fStateCount; ++i)
                {
                    const CFStringRef keyRef = CFStringCreateWithCString(nullptr,
                                                                         fPlugin.getStateKey(i),
                                                                         kCFStringEncodingASCII);
                    DAF_SAFE_ASSERT_CONTINUE(keyRef != nullptr);

                    CFArrayAppendValue(keysRef, keyRef);
                    CFRelease(keyRef);
                }

                *static_cast<CFArrayRef*>(outData) = keysRef;
                return noErr;
            }
            return kAudio_ParamError;

        case 'DPFs':
            {
                const String& key(fPlugin.getStateKey(inElement));
               #if DAF_PLUGIN_WANT_FULL_STATE
                fStateMap[key] = fPlugin.getStateValue(key);
               #endif

                *static_cast<CFStringRef*>(outData) = CFStringCreateWithCString(nullptr,
                                                                                fStateMap[key],
                                                                                kCFStringEncodingUTF8);
            }
            return noErr;
       #endif

       #if DAF_PLUGIN_HAS_UI && DAF_PLUGIN_WANT_DIRECT_ACCESS
        case 'DPFa':
            *static_cast<void**>(outData) = fPlugin.getInstancePointer();
            return noErr;
       #endif
        }

        d_stdout("TODO GetProperty(%d:%x:%s, %d:%s, %d, ...)",
                 inProp, inProp, AudioUnitPropertyID2Str(inProp), inScope, AudioUnitScope2Str(inScope), inElement);
        return kAudioUnitErr_InvalidProperty;
    }

    OSStatus auSetProperty(const AudioUnitPropertyID inProp,
                           const AudioUnitScope inScope,
                           const AudioUnitElement inElement,
                           const void* const inData,
                           const UInt32 inDataSize)
    {
        switch (inProp)
        {
        case kAudioUnitProperty_ClassInfo:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(CFPropertyListRef), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
                const CFPropertyListRef propList = *static_cast<const CFPropertyListRef*>(inData);
                DAF_SAFE_ASSERT_RETURN(CFGetTypeID(propList) == CFDictionaryGetTypeID(), kAudioUnitErr_InvalidPropertyValue);

                return restoreClassInfo(static_cast<CFDictionaryRef>(propList))
                     ? OSStatus(noErr) : OSStatus(kAudioUnitErr_InvalidPropertyValue);
            }

        case kAudioUnitProperty_MakeConnection:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(AudioUnitConnection), inDataSize, kAudioUnitErr_InvalidPropertyValue);
           #if DAF_PLUGIN_NUM_INPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
            {
                const AudioUnitConnection conn = *static_cast<const AudioUnitConnection*>(inData);
                AUInputBus& bus(fInputBuses[inElement]);
               #ifdef DAF_PLUGIN_EXTRA_IO
                const AudioUnitConnection previous = bus.connection;
               #endif

                if (conn.sourceAudioUnit == nullptr)
                {
                    std::memset(&bus.connection, 0, sizeof(bus.connection));
                   #ifdef DAF_PLUGIN_EXTRA_IO
                    if (! updatePluginAudioPortIO())
                    {
                        bus.connection = previous;
                        return kAudioUnitErr_FormatNotSupported;
                    }
                   #endif
                    return noErr;
                }

                AudioStreamBasicDescription desc;
                std::memset(&desc, 0, sizeof(desc));
                UInt32 dataSize = sizeof(AudioStreamBasicDescription);
                if (AudioUnitGetProperty(conn.sourceAudioUnit,
                                         kAudioUnitProperty_StreamFormat,
                                         kAudioUnitScope_Output,
                                         conn.sourceOutputNumber, &desc, &dataSize) != noErr)
                    return kAudioUnitErr_InvalidPropertyValue;

                DAF_SAFE_ASSERT_INT_RETURN(desc.mFormatID == kAudioFormatLinearPCM,
                                               desc.mFormatID, kAudioUnitErr_FormatNotSupported);
                DAF_SAFE_ASSERT_INT_RETURN(desc.mBitsPerChannel == 32,
                                               desc.mBitsPerChannel, kAudioUnitErr_FormatNotSupported);
                DAF_SAFE_ASSERT_INT_RETURN(desc.mBytesPerFrame == sizeof(float),
                                               desc.mBytesPerFrame, kAudioUnitErr_FormatNotSupported);
                DAF_SAFE_ASSERT_INT_RETURN(desc.mBytesPerPacket == sizeof(float),
                                               desc.mBytesPerPacket, kAudioUnitErr_FormatNotSupported);
                DAF_SAFE_ASSERT_INT_RETURN(desc.mFramesPerPacket == 1,
                                               desc.mFramesPerPacket, kAudioUnitErr_FormatNotSupported);
                DAF_SAFE_ASSERT_INT_RETURN(desc.mFormatFlags == kWantedAudioFormat,
                                               desc.mFormatFlags, kAudioUnitErr_FormatNotSupported);
                DAF_SAFE_ASSERT_UINT_RETURN(desc.mChannelsPerFrame == bus.channels,
                                            desc.mChannelsPerFrame, kAudioUnitErr_FormatNotSupported);

                bus.connection = conn;
               #ifdef DAF_PLUGIN_EXTRA_IO
                if (! updatePluginAudioPortIO())
                {
                    bus.connection = previous;
                    return kAudioUnitErr_FormatNotSupported;
                }
               #endif
            }
            return noErr;
           #else
            return kAudioUnitErr_PropertyNotInUse;
           #endif

        case kAudioUnitProperty_SampleRate:
           #if DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input || inScope == kAudioUnitScope_Output, inScope, kAudioUnitErr_InvalidScope);
           #elif DAF_PLUGIN_NUM_OUTPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Output, inScope, kAudioUnitErr_InvalidScope);
           #else
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input, inScope, kAudioUnitErr_InvalidScope);
           #endif
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(Float64), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
               #if DAF_PLUGIN_NUM_INPUTS != 0 || DAF_PLUGIN_NUM_OUTPUTS != 0
                const Float64 sampleRate = *static_cast<const Float64*>(inData);
               #endif

               #if DAF_PLUGIN_NUM_INPUTS != 0
                if (inScope == kAudioUnitScope_Input)
                {
                    DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
                    AUInputBus& bus(fInputBuses[inElement]);
                    if (d_isNotEqual(bus.sampleRate, sampleRate))
                    {
                        bus.sampleRate = sampleRate;
                        if (inElement == 0)
                            d_nextSampleRate = sampleRate;

                       #if DAF_PLUGIN_NUM_OUTPUTS != 0
                        if (inElement == 0 && d_isEqual(fSampleRateForOutput, sampleRate))
                       #endif
                        {
                            fPlugin.setSampleRate(sampleRate, true);
                        }

                        notifyPropertyListeners(inProp, inScope, inElement);
                    }
                    return noErr;
                }
               #endif

               #if DAF_PLUGIN_NUM_OUTPUTS != 0
                if (inScope == kAudioUnitScope_Output)
                {
                    DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
                    if (d_isNotEqual(fSampleRateForOutput, sampleRate))
                    {
                        fSampleRateForOutput = sampleRate;
                        d_nextSampleRate = sampleRate;

                       #if DAF_PLUGIN_NUM_INPUTS != 0
                        if (d_isEqual(fInputBuses[0].sampleRate, sampleRate))
                       #endif
                        {
                            fPlugin.setSampleRate(sampleRate, true);
                        }

                        notifyPropertyListeners(inProp, inScope, inElement);
                    }
                    return noErr;
                }
               #endif
            }
            return kAudioUnitErr_PropertyNotInUse;

        case kAudioUnitProperty_StreamFormat:
           #if DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input || inScope == kAudioUnitScope_Output, inScope, kAudioUnitErr_InvalidScope);
           #elif DAF_PLUGIN_NUM_OUTPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Output, inScope, kAudioUnitErr_InvalidScope);
           #else
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input, inScope, kAudioUnitErr_InvalidScope);
           #endif
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(AudioStreamBasicDescription), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
                const AudioStreamBasicDescription* const desc = static_cast<const AudioStreamBasicDescription*>(inData);

                if (desc->mFormatID != kAudioFormatLinearPCM)
                    return kAudioUnitErr_FormatNotSupported;
                if (desc->mBitsPerChannel != 32)
                    return kAudioUnitErr_FormatNotSupported;
                if (desc->mBytesPerFrame != sizeof(float))
                    return kAudioUnitErr_FormatNotSupported;
                if (desc->mBytesPerPacket != sizeof(float))
                    return kAudioUnitErr_FormatNotSupported;
                if (desc->mFramesPerPacket != 1)
                    return kAudioUnitErr_FormatNotSupported;
                if (desc->mFormatFlags != kWantedAudioFormat)
                    return kAudioUnitErr_FormatNotSupported;

               #ifndef DAF_PLUGIN_EXTRA_IO
                if (desc->mChannelsPerFrame != (inScope == kAudioUnitScope_Input ? DAF_PLUGIN_NUM_INPUTS
                                                                                 : DAF_PLUGIN_NUM_OUTPUTS))
                    return kAudioUnitErr_FormatNotSupported;
               #endif

               #if DAF_PLUGIN_NUM_INPUTS != 0
                if (inScope == kAudioUnitScope_Input)
                {
                    DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
                    AUInputBus& bus(fInputBuses[inElement]);
                    bool changed = false;

                    if (! isInputBusChannelCountValid(inElement, desc->mChannelsPerFrame))
                        return kAudioUnitErr_FormatNotSupported;

                    if (bus.channels != desc->mChannelsPerFrame)
                    {
                        changed = true;
                        bus.channels = desc->mChannelsPerFrame;

                       #ifdef DAF_PLUGIN_EXTRA_IO
                        updatePluginAudioPortIO();
                       #endif
                    }

                    if (d_isNotEqual(bus.sampleRate, desc->mSampleRate))
                    {
                        changed = true;
                        bus.sampleRate = desc->mSampleRate;

                       #if DAF_PLUGIN_NUM_OUTPUTS != 0
                        if (inElement == 0 && d_isEqual(fSampleRateForOutput, desc->mSampleRate))
                       #endif
                        {
                            fPlugin.setSampleRate(desc->mSampleRate, true);
                        }

                        notifyPropertyListeners(kAudioUnitProperty_SampleRate, inScope, inElement);
                    }

                    if (changed)
                        notifyPropertyListeners(inProp, inScope, inElement);

                    return noErr;
                }
               #endif

               #if DAF_PLUGIN_NUM_OUTPUTS != 0
                if (inScope == kAudioUnitScope_Output)
                {
                    DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
                    bool changed = false;

                   #ifdef DAF_PLUGIN_EXTRA_IO
                    if (! isOutputNumChannelsValid(desc->mChannelsPerFrame))
                        return kAudioUnitErr_FormatNotSupported;

                    if (fNumOutputs != desc->mChannelsPerFrame)
                    {
                        /* The shared buffer list is sized from the output count in auInitialize
                         * and is not resized again while the unit is initialized, so a wider
                         * output element would have auRender index past it. Uninitialize first,
                         * as the API requires for a channel-count change. */
                        if (fPlugin.isActive())
                            return kAudioUnitErr_Initialized;

                        changed = true;
                        fNumOutputs = desc->mChannelsPerFrame;
                        updatePluginAudioPortIO();
                    }
                   #endif

                    if (d_isNotEqual(fSampleRateForOutput, desc->mSampleRate))
                    {
                        changed = true;
                        fSampleRateForOutput = desc->mSampleRate;

                       #if DAF_PLUGIN_NUM_INPUTS != 0
                        if (d_isEqual(fInputBuses[0].sampleRate, desc->mSampleRate))
                       #endif
                        {
                            fPlugin.setSampleRate(desc->mSampleRate, true);
                        }

                        notifyPropertyListeners(kAudioUnitProperty_SampleRate, inScope, inElement);
                    }

                    if (changed)
                        notifyPropertyListeners(inProp, inScope, inElement);

                    return noErr;
                }
               #endif
            }
            return kAudioUnitErr_PropertyNotInUse;

        case kAudioUnitProperty_MaximumFramesPerSlice:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(UInt32), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
                const UInt32 bufferSize = *static_cast<const UInt32*>(inData);

               #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
                /* The buffer lists are sized from this in auInitialize and are not resized again
                 * while the unit is initialized, so accepting a bigger value here would leave the
                 * render path writing past them. Uninitialize first, as the API requires. */
                if (fPlugin.isActive() && bufferSize != fPlugin.getBufferSize())
                    return kAudioUnitErr_Initialized;
               #endif

                if (fPlugin.setBufferSize(bufferSize, true))
                    notifyPropertyListeners(inProp, inScope, inElement);
            }
            return noErr;

        case kAudioUnitProperty_BypassEffect:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(UInt32), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            DAF_SAFE_ASSERT_RETURN(fBypassParameterIndex != UINT32_MAX, kAudioUnitErr_PropertyNotInUse);
            {
                const bool bypass = *static_cast<const UInt32*>(inData) != 0;

                if ((fLastParameterValues[fBypassParameterIndex] > 0.5f) != bypass)
                {
                    const float value = bypass ? 1.f : 0.f;
                    fLastParameterValues[fBypassParameterIndex] = value;
                    fPlugin.setParameterValue(fBypassParameterIndex, value);
                    notifyPropertyListeners(inProp, inScope, inElement);
                }
            }
            return noErr;

        case kAudioUnitProperty_SetRenderCallback:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Input, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(AURenderCallbackStruct), inDataSize, kAudioUnitErr_InvalidPropertyValue);
           #if DAF_PLUGIN_NUM_INPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fInputBusCount, inElement, kAudioUnitErr_InvalidElement);
            {
                AUInputBus& bus(fInputBuses[inElement]);
               #ifdef DAF_PLUGIN_EXTRA_IO
                const AURenderCallbackStruct previous = bus.renderCallback;
               #endif
                std::memcpy(&bus.renderCallback, inData, sizeof(AURenderCallbackStruct));
               #ifdef DAF_PLUGIN_EXTRA_IO
                if (! updatePluginAudioPortIO())
                {
                    bus.renderCallback = previous;
                    return kAudioUnitErr_FormatNotSupported;
                }
               #endif
            }
            return noErr;
           #else
            return kAudioUnitErr_PropertyNotInUse;
           #endif

        case kAudioUnitProperty_HostCallbacks:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_WANT_TIMEPOS
            {
                const UInt32 usableDataSize = std::min(inDataSize, static_cast<UInt32>(sizeof(HostCallbackInfo)));

                // the host is allowed to supply less than the full struct, everything past it is unset;
                // compare the whole thing so that shrinking the size also counts as a change
                HostCallbackInfo hostCallbackInfo;
                std::memset(&hostCallbackInfo, 0, sizeof(hostCallbackInfo));
                std::memcpy(&hostCallbackInfo, inData, usableDataSize);

                if (std::memcmp(&fHostCallbackInfo, &hostCallbackInfo, sizeof(HostCallbackInfo)) != 0)
                {
                    std::memcpy(&fHostCallbackInfo, &hostCallbackInfo, sizeof(HostCallbackInfo));
                    notifyPropertyListeners(inProp, inScope, inElement);
                }
            }
            return noErr;
           #else
            return kAudioUnitErr_PropertyNotInUse;
           #endif

        case kAudioUnitProperty_InPlaceProcessing:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(UInt32), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            /* Accepted and ignored: the unit never processes in place, so whichever way the host
             * sets this it still gets its output buffers written and nothing else touched. */
            return noErr;

        case kAudioUnitProperty_PresentPreset:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(AUPreset), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
                const int32_t presetNumber = static_cast<const AUPreset*>(inData)->presetNumber;

               #if DAF_PLUGIN_WANT_PROGRAMS
                // a negative number is a user preset, a positive one must match an advertised factory preset;
                // reject anything else before touching state, fCurrentProgram indexes fFactoryPresetsData
                DAF_SAFE_ASSERT_INT_RETURN(presetNumber < static_cast<int32_t>(fProgramCount),
                                               presetNumber, kAudioUnitErr_InvalidPropertyValue);

                if (presetNumber >= 0)
                {
                    if (fCurrentProgram != presetNumber)
                    {
                        fCurrentProgram = presetNumber;
                        fLastFactoryProgram = presetNumber;
                        fPlugin.loadProgram(fLastFactoryProgram);
                        for (uint32_t i=0; i<fParameterCount; ++i)
                            fLastParameterValues[i] = fPlugin.getParameterValue(i);
                        notifyPropertyListeners('DPFo', kAudioUnitScope_Global, 0);
                    }
                }
                else
                {
                    fCurrentProgram = presetNumber;
                    const CFStringRef presetName =
                        static_cast<const AUPreset*>(inData)->presetName;
                    if (presetName != nullptr)
                        CFRetain(presetName);
                    CFRelease(fUserPresetData.presetName);
                    fUserPresetData.presetNumber = presetNumber;
                    fUserPresetData.presetName =
                        presetName != nullptr ? presetName : CFSTR("");
                }
               #else
                DAF_SAFE_ASSERT_INT_RETURN(presetNumber < 0, presetNumber, kAudioUnitErr_InvalidPropertyValue);
                const CFStringRef presetName =
                    static_cast<const AUPreset*>(inData)->presetName;
                if (presetName != nullptr)
                    CFRetain(presetName);
                CFRelease(fUserPresetData.presetName);
                fUserPresetData.presetNumber = presetNumber;
                fUserPresetData.presetName =
                    presetName != nullptr ? presetName : CFSTR("");
               #endif
            }
            return noErr;

        case kAudioUnitProperty_MIDIOutputCallback:
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(AUMIDIOutputCallbackStruct), inDataSize, kAudioUnitErr_InvalidPropertyValue);
           #if DAF_PLUGIN_WANT_MIDI_OUTPUT
            std::memcpy(&fMidiOutput, inData, sizeof(AUMIDIOutputCallbackStruct));
            return noErr;
           #else
            return kAudioUnitErr_PropertyNotInUse;
           #endif

        case 'DPFi':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(uint16_t), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
                const uint16_t magic = *static_cast<const uint16_t*>(inData);

                if (magic != 1337)
                    return noErr;

               #if DAF_PLUGIN_WANT_PROGRAMS
                notifyPropertyListeners('DPFo', kAudioUnitScope_Global, 0);
               #endif

               #if DAF_PLUGIN_WANT_STATE
                for (uint32_t i=0; i<fStateCount; ++i)
                    notifyPropertyListeners('DPFs', kAudioUnitScope_Global, i);
               #endif

                for (uint32_t i=0; i<fParameterCount; ++i)
                    notifyPropertyListeners('DPFp', kAudioUnitScope_Global, i);
            }
            return noErr;

        case 'DPFe':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fParameterCount, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(uint8_t), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
                const uint8_t flag = *static_cast<const uint8_t*>(inData);

                if (flag != 1 && flag != 2)
                    return noErr;

                AudioUnitEvent event;
                std::memset(&event, 0, sizeof(event));

                event.mEventType                        = flag == 1 ? kAudioUnitEvent_BeginParameterChangeGesture
                                                                    : kAudioUnitEvent_EndParameterChangeGesture;
                event.mArgument.mParameter.mAudioUnit   = fComponent;
                event.mArgument.mParameter.mParameterID = inElement;
                event.mArgument.mParameter.mScope       = kAudioUnitScope_Global;
                AUEventListenerNotify(NULL, NULL, &event);
            }
            return noErr;

        case 'DPFp':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement < fParameterCount, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(float), inDataSize, kAudioUnitErr_InvalidPropertyValue);
            {
                const float value = *static_cast<const float*>(inData);
                DAF_SAFE_ASSERT_RETURN(std::isfinite(value), kAudioUnitErr_InvalidParameterValue);

                if (d_isEqual(fLastParameterValues[inElement], value))
                    return noErr;

                fLastParameterValues[inElement] = value;
                fPlugin.setParameterValue(inElement, value);

                AudioUnitEvent event;
                std::memset(&event, 0, sizeof(event));

                event.mEventType                        = kAudioUnitEvent_ParameterValueChange;
                event.mArgument.mParameter.mAudioUnit   = fComponent;
                event.mArgument.mParameter.mParameterID = inElement;
                event.mArgument.mParameter.mScope       = kAudioUnitScope_Global;
                AUEventListenerNotify(NULL, NULL, &event);

                if (fBypassParameterIndex == inElement)
                    notifyPropertyListeners(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
            }
            return noErr;

        case 'DPFn':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(uint8_t) * 3, inDataSize, kAudioUnitErr_InvalidPropertyValue);
           #if DAF_PLUGIN_WANT_MIDI_INPUT
            {
                const uint8_t* const midiData = static_cast<const uint8_t*>(inData);

                if (midiData[0] == 0)
                    return noErr;

                fNotesRingBuffer.writeCustomData(midiData, 3);
                fNotesRingBuffer.commitWrite();
            }
            return noErr;
           #else
            return kAudioUnitErr_PropertyNotInUse;
           #endif

        case 'DPFs':
            DAF_SAFE_ASSERT_UINT_RETURN(inScope == kAudioUnitScope_Global, inScope, kAudioUnitErr_InvalidScope);
            DAF_SAFE_ASSERT_UINT_RETURN(inDataSize == sizeof(CFDictionaryRef), inDataSize, kAudioUnitErr_InvalidPropertyValue);
           #if DAF_PLUGIN_WANT_STATE
            DAF_SAFE_ASSERT_UINT_RETURN(inElement == 0, inElement, kAudioUnitErr_InvalidElement);
            {
                const CFDictionaryRef dictRef = *static_cast<const CFDictionaryRef*>(inData);
                DAF_SAFE_ASSERT_RETURN(CFGetTypeID(dictRef) == CFDictionaryGetTypeID(),
                                           kAudioUnitErr_InvalidPropertyValue);
                DAF_SAFE_ASSERT_RETURN(CFDictionaryGetCount(dictRef) == 1, kAudioUnitErr_InvalidPropertyValue);

                CFStringRef keyRef = nullptr;
                CFStringRef valueRef = nullptr;
                CFDictionaryGetKeysAndValues(dictRef,
                                             reinterpret_cast<const void**>(&keyRef),
                                             reinterpret_cast<const void**>(&valueRef));
                DAF_SAFE_ASSERT_RETURN(keyRef != nullptr && CFGetTypeID(keyRef) == CFStringGetTypeID(),
                                           kAudioUnitErr_InvalidPropertyValue);
                DAF_SAFE_ASSERT_RETURN(valueRef != nullptr && CFGetTypeID(valueRef) == CFStringGetTypeID(),
                                           kAudioUnitErr_InvalidPropertyValue);

                const CFIndex keyRefLen = CFStringGetLength(keyRef);
                char* key = static_cast<char*>(std::malloc(keyRefLen + 1));
                DAF_SAFE_ASSERT_RETURN(CFStringGetCString(keyRef, key, keyRefLen + 1, kCFStringEncodingASCII),
                                           kAudioUnitErr_InvalidPropertyValue);

                const CFIndex valueRefLen = CFStringGetLength(valueRef);
                char* value = static_cast<char*>(std::malloc(valueRefLen + 1));
                DAF_SAFE_ASSERT_RETURN(CFStringGetCString(valueRef, value, valueRefLen + 1, kCFStringEncodingUTF8),
                                           kAudioUnitErr_InvalidPropertyValue);

                if (!fPlugin.validateStateValue(key, value))
                {
                    std::free(key);
                    std::free(value);
                    return kAudioUnitErr_InvalidPropertyValue;
                }
                const String dkey(key);

                // save this key as needed
                if (fPlugin.wantStateKey(dkey))
                    fStateMap[dkey] = value;

                fPlugin.setState(dkey, value);
                if (fPlugin.isParameterSnapshotState(dkey))
                    syncParameterSnapshot();

                std::free(key);
                std::free(value);
            }
            return noErr;
           #else
            return kAudioUnitErr_PropertyNotInUse;
           #endif

        // unwanted properties
        case kMusicDeviceProperty_DualSchedulingMode:
            return kAudioUnitErr_PropertyNotInUse;
        }

        d_stdout("TODO SetProperty(%d:%x:%s, %d:%s, %d, %p, %u)",
                 inProp, inProp, AudioUnitPropertyID2Str(inProp), inScope, AudioUnitScope2Str(inScope), inElement, inData, inDataSize);
        return kAudioUnitErr_InvalidProperty;
    }

    OSStatus auAddPropertyListener(const AudioUnitPropertyID prop,
                                   const AudioUnitPropertyListenerProc proc,
                                   void* const userData)
    {
        const PropertyListener pl = {
            prop, proc, userData
        };

        fPropertyListeners.push_back(pl);
        return noErr;
    }

    OSStatus auRemovePropertyListener(const AudioUnitPropertyID prop, const AudioUnitPropertyListenerProc proc)
    {
        for (PropertyListeners::iterator it = fPropertyListeners.begin(); it != fPropertyListeners.end(); ++it)
        {
            const PropertyListener& pl(*it);

            if (pl.prop == prop && pl.proc == proc)
            {
                fPropertyListeners.erase(it);
                return auRemovePropertyListener(prop, proc);
            }
        }

        return noErr;
    }

    OSStatus auRemovePropertyListenerWithUserData(const AudioUnitPropertyID prop,
                                                  const AudioUnitPropertyListenerProc proc,
                                                  void* const userData)
    {
        for (PropertyListeners::iterator it = fPropertyListeners.begin(); it != fPropertyListeners.end(); ++it)
        {
            const PropertyListener& pl(*it);

            if (pl.prop == prop && pl.proc == proc && pl.userData == userData)
            {
                fPropertyListeners.erase(it);
                return auRemovePropertyListenerWithUserData(prop, proc, userData);
            }
        }

        return noErr;
    }

    OSStatus auAddRenderNotify(const AURenderCallback proc, void* const userData)
    {
        fUsingRenderListeners = true;

        const RenderListener rl = {
            proc, userData
        };

        fRenderListeners.push_back(rl);
        return noErr;
    }

    OSStatus auRemoveRenderNotify(const AURenderCallback proc, void* const userData)
    {
        for (RenderListeners::iterator it = fRenderListeners.begin(); it != fRenderListeners.end(); ++it)
        {
            const RenderListener& rl(*it);

            if (rl.proc == proc && rl.userData == userData)
            {
                fRenderListeners.erase(it);
                return auRemoveRenderNotify(proc, userData);
            }
        }

        if (fRenderListeners.empty())
            fUsingRenderListeners = false;

        return noErr;
    }

    OSStatus auGetParameter(const AudioUnitParameterID param,
                            const AudioUnitScope scope,
                            const AudioUnitElement elem,
                            AudioUnitParameterValue* const value)
    {
        DAF_SAFE_ASSERT_UINT_RETURN(scope == kAudioUnitScope_Global, scope, kAudioUnitErr_InvalidScope);
        DAF_SAFE_ASSERT_UINT_RETURN(param < fParameterCount, param, kAudioUnitErr_InvalidElement);
        DAF_SAFE_ASSERT_UINT_RETURN(elem == 0, elem, kAudioUnitErr_InvalidElement);
        DAF_SAFE_ASSERT_RETURN(value != nullptr, kAudio_ParamError);

        *value = fPlugin.getParameterValue(param);
        return noErr;
    }

    OSStatus auSetParameter(const AudioUnitParameterID param,
                            const AudioUnitScope scope,
                            const AudioUnitElement elem,
                            const AudioUnitParameterValue value,
                            UInt32 /* bufferOffset */)
    {
        DAF_SAFE_ASSERT_UINT_RETURN(scope == kAudioUnitScope_Global, scope, kAudioUnitErr_InvalidScope);
        DAF_SAFE_ASSERT_UINT_RETURN(param < fParameterCount, param, kAudioUnitErr_InvalidElement);
        DAF_SAFE_ASSERT_UINT_RETURN(elem == 0, elem, kAudioUnitErr_InvalidElement);
        DAF_SAFE_ASSERT_RETURN(std::isfinite(value), kAudioUnitErr_InvalidParameterValue);

        if (d_isNotEqual(fLastParameterValues[param], value))
        {
            fLastParameterValues[param] = value;
            fPlugin.setParameterValue(param, value);

            // TODO flag param only, notify listeners later on bg thread (sem_post etc)
            notifyPropertyListeners('DPFp', kAudioUnitScope_Global, param);

            if (fBypassParameterIndex == elem)
                notifyPropertyListeners(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
        }

        return noErr;
    }

    OSStatus auScheduleParameters(const AudioUnitParameterEvent* const events, const UInt32 numEvents)
    {
        for (UInt32 i=0; i<numEvents; ++i)
        {
            const AudioUnitParameterEvent& event(events[i]);

            if (event.eventType == kParameterEvent_Immediate)
            {
                auSetParameter(event.parameter,
                               event.scope,
                               event.element,
                               event.eventValues.immediate.value,
                               event.eventValues.immediate.bufferOffset);
            }
            else
            {
                // TODO?
                d_stdout("WIP ScheduleParameters(%p, %u)", events, numEvents);
            }
        }
        return noErr;
    }

    OSStatus auReset(const AudioUnitScope scope, const AudioUnitElement elem)
    {
        DAF_SAFE_ASSERT_UINT_RETURN(scope == kAudioUnitScope_Global || scope == kAudioUnitScope_Input || scope == kAudioUnitScope_Output, scope, kAudioUnitErr_InvalidScope);
       #if DAF_PLUGIN_NUM_INPUTS != 0
        if (scope == kAudioUnitScope_Input)
        {
            DAF_SAFE_ASSERT_UINT_RETURN(elem < fInputBusCount, elem, kAudioUnitErr_InvalidElement);
        }
        else
       #endif
        {
            DAF_SAFE_ASSERT_UINT_RETURN(elem == 0, elem, kAudioUnitErr_InvalidElement);
        }

        if (fResetParameterIndex != UINT32_MAX)
        {
            fPlugin.setParameterValue(fResetParameterIndex, 1.f);
            fPlugin.setParameterValue(fResetParameterIndex, 0.f);
        }
        else if (fPlugin.isActive())
        {
            fPlugin.deactivate();
            fPlugin.activate();
        }

       #if DAF_PLUGIN_WANT_MIDI_INPUT
        fMidiEventCount = 0;
       #endif
       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        fMidiOutputDataOffset = 0;
        fMidiOutputPackets->numPackets = 0;
       #endif
       #if DAF_PLUGIN_WANT_TIMEPOS
        fTimePosition.clear();
        fTimePosition.bbt.ticksPerBeat = kDefaultTicksPerBeat;
       #endif
        return noErr;
    }

    OSStatus auRender(const AudioUnitRenderActionFlags actionFlags,
                      const AudioTimeStamp* const inTimeStamp,
                      const UInt32 inBusNumber,
                      const UInt32 inFramesToProcess,
                      AudioBufferList* const ioData)
    {
       #if defined(DAF_PLUGIN_EXTRA_IO) && DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
        // Do not use a safe-assert macro here: this can persist and must not log from the audio thread.
        if (! isNumChannelsComboValid(fNumInputs, fNumOutputs))
            return kAudio_ParamError;
       #endif

        if ((actionFlags & kAudioUnitRenderAction_DoNotCheckRenderArgs) == 0x0)
        {
            DAF_SAFE_ASSERT_RETURN(fPlugin.isActive(), kAudio_ParamError);
            DAF_SAFE_ASSERT_UINT_RETURN(inBusNumber == 0, inBusNumber, kAudioUnitErr_InvalidElement);
           #if DAF_PLUGIN_NUM_OUTPUTS != 0
            // The host renders the output element, so the buffer list it hands
            // us is sized for the outputs. For plugins with more inputs than
            // outputs (e.g. a sidechain pair on a stereo effect) this is
            // smaller than fAudioBufferList, which is sized max(in, out).
           #ifdef DAF_PLUGIN_EXTRA_IO
            DAF_SAFE_ASSERT_UINT_RETURN(ioData->mNumberBuffers == fNumOutputs,
                                            ioData->mNumberBuffers, kAudio_ParamError);
           #else
            DAF_SAFE_ASSERT_UINT_RETURN(ioData->mNumberBuffers == DAF_PLUGIN_NUM_OUTPUTS,
                                            ioData->mNumberBuffers, kAudio_ParamError);
           #endif
           #elif DAF_PLUGIN_NUM_INPUTS != 0
            DAF_SAFE_ASSERT_UINT_RETURN(ioData->mNumberBuffers
                                            == static_cast<UInt32>(fAudioBufferList.getNumBuffers()),
                                            ioData->mNumberBuffers, kAudio_ParamError);
           #else
            DAF_SAFE_ASSERT_UINT_RETURN(ioData->mNumberBuffers == 0, ioData->mNumberBuffers, kAudio_ParamError);
           #endif

            if (inFramesToProcess > fPlugin.getBufferSize())
            {
                setLastRenderError(kAudioUnitErr_TooManyFramesToProcess);
                return kAudioUnitErr_TooManyFramesToProcess;
            }

           #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
            for (uint16_t i = 0; i < ioData->mNumberBuffers; ++i)
            {
                if (ioData->mBuffers[i].mDataByteSize != sizeof(float) * inFramesToProcess)
                {
                    setLastRenderError(kAudio_ParamError);
                    return kAudio_ParamError;
                }
            }
           #endif
        }

      #if DAF_PLUGIN_NUM_INPUTS != 0
        const float* inputs[DAF_PLUGIN_NUM_INPUTS] = {};
      #else
        constexpr const float** inputs = nullptr;
      #endif

      #if DAF_PLUGIN_NUM_OUTPUTS != 0
       #ifdef DAF_PLUGIN_EXTRA_IO
        const uint32_t numOutputs = fNumOutputs;
       #else
        constexpr const uint32_t numOutputs = DAF_PLUGIN_NUM_OUTPUTS;
       #endif
        float* outputs[numOutputs];
      #else
        constexpr float** outputs = nullptr;
      #endif

       #if DAF_PLUGIN_NUM_OUTPUTS != 0
        for (uint16_t i = 0; i < numOutputs; ++i)
        {
            if (ioData->mBuffers[i].mData == nullptr)
            {
                ioData->mBuffers[i].mData = fAudioBufferList.getChannelBuffer(i);

                if (ioData->mBuffers[i].mData == nullptr)
                {
                    setLastRenderError(kAudio_ParamError);
                    return kAudio_ParamError;
                }

                std::memset(ioData->mBuffers[i].mData, 0, sizeof(float) * inFramesToProcess);
            }
            outputs[i] = static_cast<float*>(ioData->mBuffers[i].mData);
        }
       #endif

       #if DAF_PLUGIN_NUM_INPUTS != 0
        for (uint16_t busIndex = 0; busIndex < fInputBusCount; ++busIndex)
        {
            AUInputBus& bus(fInputBuses[busIndex]);

            /* Point the list back at our own buffers before every pull. The unit or callback we
             * pulled from last time is allowed to have swapped in buffers of its own, and those are
             * neither ours to write into nor guaranteed to still exist. */
            AudioBufferList* const busData = bus.bufferList.prepare(bus.channels, inFramesToProcess);

            if (busData == nullptr)
            {
                setLastRenderError(kAudio_ParamError);
                return kAudio_ParamError;
            }

            OSStatus err = noErr;
            if (bus.channels != 0 && bus.connection.sourceAudioUnit != nullptr)
            {
                AudioUnitRenderActionFlags ioActionFlags = 0;
                err = AudioUnitRender(bus.connection.sourceAudioUnit,
                                      &ioActionFlags,
                                      inTimeStamp,
                                      bus.connection.sourceOutputNumber,
                                      inFramesToProcess,
                                      busData);
            }
            else if (bus.channels != 0 && bus.renderCallback.inputProc != nullptr)
            {
                AudioUnitRenderActionFlags ioActionFlags = 0;
                err = bus.renderCallback.inputProc(bus.renderCallback.inputProcRefCon,
                                                   &ioActionFlags,
                                                   inTimeStamp,
                                                   busIndex,
                                                   inFramesToProcess,
                                                   busData);
            }

            if (err != noErr)
            {
                setLastRenderError(err);
                return err;
            }

            if (bus.hasSource())
            {
                if (busData->mNumberBuffers != bus.channels)
                {
                    setLastRenderError(kAudio_ParamError);
                    return kAudio_ParamError;
                }
                for (uint16_t channel = 0; channel < bus.channels; ++channel)
                {
                    if (busData->mBuffers[channel].mData == nullptr)
                    {
                        setLastRenderError(kAudio_ParamError);
                        return kAudio_ParamError;
                    }
                }
            }

            for (uint16_t portIndex = 0; portIndex < DAF_PLUGIN_NUM_INPUTS; ++portIndex)
            {
                if (fInputPortBuses[portIndex] != busIndex)
                    continue;

                const uint16_t channel = fInputPortChannels[portIndex];
                if (! bus.hasSource() && busIndex == 0 && channel < ioData->mNumberBuffers)
                {
                    inputs[portIndex] = static_cast<const float*>(ioData->mBuffers[channel].mData);
                    continue;
                }

                if (! bus.hasSource() || channel >= bus.channels)
                    std::memset(busData->mBuffers[channel].mData, 0,
                                sizeof(float) * inFramesToProcess);
                inputs[portIndex] = static_cast<const float*>(busData->mBuffers[channel].mData);
            }
        }
       #endif

        if (fUsingRenderListeners)
        {
            AudioUnitRenderActionFlags ioActionFlags = actionFlags | kAudioUnitRenderAction_PreRender;
            notifyRenderListeners(&ioActionFlags, inTimeStamp, inBusNumber, inFramesToProcess, ioData);
        }

        run(inputs, outputs, inFramesToProcess, inTimeStamp);

        if (fUsingRenderListeners)
        {
            AudioUnitRenderActionFlags ioActionFlags = actionFlags | kAudioUnitRenderAction_PostRender;
            notifyRenderListeners(&ioActionFlags, inTimeStamp, inBusNumber, inFramesToProcess, ioData);
        }

        return noErr;
    }

   #if DAF_PLUGIN_WANT_MIDI_INPUT
    OSStatus auMIDIEvent(const UInt32 inStatus,
                         const UInt32 inData1,
                         const UInt32 inData2,
                         const UInt32 inOffsetSampleFrame)
    {
        if (fMidiEventCount >= kMaxMidiEvents)
            return noErr;

        MidiEvent& midiEvent(fMidiEvents[fMidiEventCount++]);
        midiEvent.frame   = inOffsetSampleFrame;
        midiEvent.data[0] = inStatus;
        midiEvent.data[1] = inData1;
        midiEvent.data[2] = inData2;

        switch (inStatus & 0xF0)
        {
        case 0x80:
        case 0x90:
        case 0xA0:
        case 0xB0:
        case 0xE0:
            midiEvent.size = 3;
            break;
        case 0xC0:
        case 0xD0:
            midiEvent.size = 2;
            break;
        case 0xF0:
            switch (inStatus & 0x0F)
            {
            case 0x0:
            case 0x4:
            case 0x5:
            case 0x7:
            case 0x9:
            case 0xD:
                // unsupported
                return kAudioUnitErr_InvalidPropertyValue;
            case 0x1:
            case 0x2:
            case 0x3:
            case 0xE:
                midiEvent.size = 3;
                break;
            case 0x6:
            case 0x8:
            case 0xA:
            case 0xB:
            case 0xC:
            case 0xF:
                midiEvent.size = 1;
                break;
            }
            break;
        default:
            // invalid
            d_debug("auMIDIEvent received invalid event %u %u %u %u @ %u",
                    inStatus, inData1, inData2, inOffsetSampleFrame, fMidiEventCount);
            return kAudioUnitErr_InvalidPropertyValue;
        }

       #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS == 0
        // handle case of plugin having no working audio, simulate audio-side processing
        run(nullptr, nullptr, std::max(1u, inOffsetSampleFrame), nullptr);
       #endif

        return noErr;
    }

    OSStatus auSysEx(const UInt8* const inData, const UInt32 inLength)
    {
        if (fMidiEventCount >= kMaxMidiEvents)
            return noErr;

        MidiEvent& midiEvent(fMidiEvents[fMidiEventCount++]);
        midiEvent.frame = fMidiEventCount != 1 ? fMidiEvents[fMidiEventCount - 1].frame : 0;
        midiEvent.size  = inLength;

        // FIXME who owns inData ??
        if (inLength > MidiEvent::kDataSize)
        {
            std::memset(midiEvent.data, 0, MidiEvent::kDataSize);
            midiEvent.dataExt = inData;
        }
        else
        {
            std::memcpy(midiEvent.data, inData, inLength);
        }

       #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS == 0
        // handle case of plugin having no working audio, simulate audio-side processing
        run(nullptr, nullptr, 1, nullptr);
       #endif

        return noErr;
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------

private:
    PluginExporter fPlugin;

    // AU component
    const AudioUnit fComponent;

    // AUv2 related fields
    OSStatus fLastRenderError;
    PropertyListeners fPropertyListeners;
    RenderListeners fRenderListeners;
  #if DAF_PLUGIN_NUM_INPUTS != 0
    AUInputBus fInputBuses[DAF_PLUGIN_NUM_INPUTS];
    uint16_t fInputPortBuses[DAF_PLUGIN_NUM_INPUTS];
    uint16_t fInputPortChannels[DAF_PLUGIN_NUM_INPUTS];
    uint16_t fInputBusCount;
   #ifdef DAF_PLUGIN_EXTRA_IO
    uint32_t fNumInputs;
   #endif
  #endif
  #if DAF_PLUGIN_NUM_OUTPUTS != 0
    Float64 fSampleRateForOutput;
   #ifdef DAF_PLUGIN_EXTRA_IO
    uint32_t fNumOutputs;
   #endif
  #endif
   #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
    AUBufferList fAudioBufferList;
   #endif
    AUChannelInfo fChannelInfo[ARRAY_SIZE(kChannelInfo)];
    uint16_t fChannelInfoCount;
    bool fUsingRenderListeners;

    // Caching
    const uint32_t fParameterCount;
    float* fLastParameterValues;
    uint32_t fBypassParameterIndex;
    uint32_t fResetParameterIndex;

   #if DAF_PLUGIN_WANT_MIDI_INPUT
    uint32_t fMidiEventCount;
    MidiEvent fMidiEvents[kMaxMidiEvents];
    SmallStackRingBuffer fNotesRingBuffer;
   #endif

   #if DAF_PLUGIN_WANT_MIDI_OUTPUT
    uint32_t fMidiOutputDataOffset;
    MIDIPacketList* fMidiOutputPackets;
    AUMIDIOutputCallbackStruct fMidiOutput;
   #endif

   #if DAF_PLUGIN_WANT_PROGRAMS
    int32_t fCurrentProgram;
    uint32_t fLastFactoryProgram;
    uint32_t fProgramCount;
    AUPreset* fFactoryPresetsData;
   #endif
    AUPreset fUserPresetData;

   #if DAF_PLUGIN_WANT_STATE
    const uint32_t fStateCount;
    StringMap fStateMap;
   #endif

   #if DAF_PLUGIN_WANT_TIMEPOS
    HostCallbackInfo fHostCallbackInfo;
    TimePosition fTimePosition;
   #endif

    // ----------------------------------------------------------------------------------------------------------------

#if DAF_PLUGIN_NUM_INPUTS != 0
    void configureInputBuses()
    {
        for (uint16_t portIndex = 0; portIndex < DAF_PLUGIN_NUM_INPUTS; ++portIndex)
        {
            const AudioPortWithBusId& port(fPlugin.getAudioPort(true, portIndex));
            const bool grouped = port.groupId != kPortGroupNone;
            const bool sidechain = (port.hints & kAudioPortIsSidechain) != 0x0;
            uint16_t busIndex = fInputBusCount;

            for (uint16_t i = 0; i < fInputBusCount; ++i)
            {
                const AUInputBus& bus(fInputBuses[i]);
                if (grouped ? (bus.grouped && bus.groupId == port.groupId)
                            : (! bus.grouped && bus.sidechain == sidechain))
                {
                    busIndex = i;
                    break;
                }
            }

            if (busIndex == fInputBusCount)
            {
                AUInputBus& bus(fInputBuses[fInputBusCount++]);
                bus.groupId = port.groupId;
                bus.grouped = grouped;
                bus.sidechain = sidechain;
                bus.sampleRate = d_nextSampleRate;
            }

            AUInputBus& bus(fInputBuses[busIndex]);
            DAF_SAFE_ASSERT(bus.sidechain == sidechain);
            fInputPortBuses[portIndex] = busIndex;
            fInputPortChannels[portIndex] = bus.declaredChannels++;
            bus.channels = bus.declaredChannels;
        }

        configureChannelInfo();
    }

    uint16_t getInputBusChannelsForTotal(const uint16_t busIndex, const int16_t total) const noexcept
    {
        if (total <= 0)
            return 0;

        uint16_t channels = 0;
        const uint16_t portCount = std::min<uint16_t>(static_cast<uint16_t>(total),
                                                       DAF_PLUGIN_NUM_INPUTS);
        for (uint16_t portIndex = 0; portIndex < portCount; ++portIndex)
            if (fInputPortBuses[portIndex] == busIndex)
                ++channels;
        return channels;
    }

    bool isInputBusChannelCountValid(const uint16_t busIndex, const uint16_t channels) const noexcept
    {
        for (uint16_t i = 0; i < ARRAY_SIZE(kChannelInfo); ++i)
            if (getInputBusChannelsForTotal(busIndex, kChannelInfo[i].inChannels) == channels)
                return true;
        return false;
    }

    uint16_t getActiveInputCount() const noexcept
    {
        uint16_t channels = fInputBuses[0].channels;
        for (uint16_t busIndex = 1; busIndex < fInputBusCount; ++busIndex)
            if (fInputBuses[busIndex].hasSource())
                channels += fInputBuses[busIndex].channels;
        return channels;
    }
#endif

    void configureChannelInfo()
    {
        for (uint16_t i = 0; i < ARRAY_SIZE(kChannelInfo); ++i)
        {
            AUChannelInfo info = kChannelInfo[i];
           #if DAF_PLUGIN_NUM_INPUTS != 0
            info.inChannels = static_cast<SInt16>(getInputBusChannelsForTotal(0, info.inChannels));
           #endif

            bool duplicate = false;
            for (uint16_t j = 0; j < fChannelInfoCount; ++j)
            {
                if (fChannelInfo[j].inChannels == info.inChannels
                    && fChannelInfo[j].outChannels == info.outChannels)
                {
                    duplicate = true;
                    break;
                }
            }

            if (! duplicate)
                fChannelInfo[fChannelInfoCount++] = info;
        }
    }

#ifdef DAF_PLUGIN_EXTRA_IO
    bool updatePluginAudioPortIO()
    {
       #if DAF_PLUGIN_NUM_INPUTS != 0
        const uint16_t numInputs = getActiveInputCount();
       #else
        constexpr const uint16_t numInputs = 0;
       #endif
       #if DAF_PLUGIN_NUM_OUTPUTS != 0
        const uint16_t numOutputs = fNumOutputs;
       #else
        constexpr const uint16_t numOutputs = 0;
       #endif

       #if DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
        if (! isNumChannelsComboValid(numInputs, numOutputs))
            return false;
       #endif

       #if DAF_PLUGIN_NUM_INPUTS != 0
        fNumInputs = numInputs;
       #endif
        fPlugin.setAudioPortIO(numInputs, numOutputs);
        return true;
    }
#endif

    // ----------------------------------------------------------------------------------------------------------------

    void notifyPropertyListeners(const AudioUnitPropertyID prop, const AudioUnitScope scope, const AudioUnitElement elem)
    {
        for (PropertyListeners::iterator it = fPropertyListeners.begin(); it != fPropertyListeners.end(); ++it)
        {
            const PropertyListener& pl(*it);

            if (pl.prop == prop)
                pl.proc(pl.userData, fComponent, prop, scope, elem);
        }
    }

    void notifyRenderListeners(AudioUnitRenderActionFlags* const ioActionFlags,
                               const AudioTimeStamp* const inTimeStamp,
                               const UInt32 inBusNumber,
                               const UInt32 inNumberFrames,
                               AudioBufferList* const ioData)
    {
        for (RenderListeners::iterator it = fRenderListeners.begin(); it != fRenderListeners.end(); ++it)
        {
            const RenderListener& rl(*it);

            rl.proc(rl.userData, ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, ioData);
        }
    }

    // ----------------------------------------------------------------------------------------------------------------

    void run(const float** inputs, float** outputs, const uint32_t frames, const AudioTimeStamp* const inTimeStamp)
    {
       #if DAF_PLUGIN_WANT_MIDI_INPUT
        if (fMidiEventCount != kMaxMidiEvents && fNotesRingBuffer.isDataAvailableForReading())
        {
            uint8_t midiData[3];
            const uint32_t frame = fMidiEventCount != 0 ? fMidiEvents[fMidiEventCount - 1].frame : 0;

            while (fNotesRingBuffer.isDataAvailableForReading())
            {
                if (! fNotesRingBuffer.readCustomData(midiData, 3))
                    break;

                MidiEvent& midiEvent(fMidiEvents[fMidiEventCount++]);
                midiEvent.frame = frame;
                midiEvent.size  = 3;
                std::memcpy(midiEvent.data, midiData, 3);

                if (fMidiEventCount == kMaxMidiEvents)
                    break;
            }
        }
       #endif

       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        fMidiOutputDataOffset = 0;
        fMidiOutputPackets->numPackets = 0;
       #endif

       #if DAF_PLUGIN_WANT_TIMEPOS
        fTimePosition.bpmValid = false;
        if (fHostCallbackInfo.beatAndTempoProc != nullptr ||
            fHostCallbackInfo.musicalTimeLocationProc != nullptr ||
            fHostCallbackInfo.transportStateProc != nullptr)
        {
            // reused values so we can check null and return value together
            Boolean b1 = false;
            Float32 f1 = 4.f; // initial value for beats per bar
            Float64 g1 = 0.0;
            Float64 g2 = 0.0;
            UInt32 u1 = 0;

            if (fHostCallbackInfo.musicalTimeLocationProc != nullptr
                && fHostCallbackInfo.musicalTimeLocationProc(fHostCallbackInfo.hostUserData,
                                                             nullptr, &f1, &u1, nullptr) == noErr)
            {
                fTimePosition.bbt.beatsPerBar = f1;
                fTimePosition.bbt.beatType    = u1;
            }
            else
            {
                fTimePosition.bbt.beatsPerBar = 4.f;
                fTimePosition.bbt.beatType    = 4.f;
            }

            if (fHostCallbackInfo.beatAndTempoProc != nullptr
                && fHostCallbackInfo.beatAndTempoProc(fHostCallbackInfo.hostUserData, &g1, &g2) == noErr)
            {
                const double beat = static_cast<int32_t>(g1);

                fTimePosition.bbt.valid = true;
                fTimePosition.bbt.bar   = static_cast<int32_t>(beat / f1) + 1;
                fTimePosition.bbt.beat  = static_cast<int32_t>(std::fmod(beat, f1)) + 1;
                fTimePosition.bbt.tick  = std::fmod(g1, 1.0) * 1920.0;
                fTimePosition.bbt.beatsPerMinute = g2;
                fTimePosition.bpmValid = g2 > 0.0;
            }
            else
            {
                fTimePosition.bbt.valid = false;
                fTimePosition.bpmValid = false;
                fTimePosition.bbt.bar   = 1;
                fTimePosition.bbt.beat  = 1;
                fTimePosition.bbt.tick  = 0.0;
                fTimePosition.bbt.beatsPerMinute = 120.0;
            }

            if (fHostCallbackInfo.transportStateProc != nullptr
                && fHostCallbackInfo.transportStateProc(fHostCallbackInfo.hostUserData,
                                                        &b1, nullptr, &g1, nullptr, nullptr, nullptr) == noErr)
            {
                fTimePosition.playing = b1;
                fTimePosition.frame = static_cast<int64_t>(g1);
            }
            else
            {
                fTimePosition.playing = false;
                fTimePosition.frame = 0;
            }

            fTimePosition.bbt.barStartTick = kDefaultTicksPerBeat *
                                             fTimePosition.bbt.beatsPerBar *
                                             (fTimePosition.bbt.bar - 1);

            fPlugin.setTimePosition(fTimePosition);
        }
       #endif

       #if DAF_PLUGIN_WANT_MIDI_INPUT
        fPlugin.run(inputs, outputs, frames, fMidiEvents, fMidiEventCount);
        fMidiEventCount = 0;
       #else
        fPlugin.run(inputs, outputs, frames);
       #endif

       #if DAF_PLUGIN_WANT_MIDI_OUTPUT
        if (fMidiOutputPackets != nullptr &&
            fMidiOutputPackets->numPackets != 0 &&
            fMidiOutput.midiOutputCallback != nullptr)
        {
            fMidiOutput.midiOutputCallback(fMidiOutput.userData, inTimeStamp, 0, fMidiOutputPackets);
        }
       #else
        // unused
        (void)inTimeStamp;
       #endif

        float value;
        AudioUnitEvent event;
        std::memset(&event, 0, sizeof(event));
        event.mEventType                      = kAudioUnitEvent_ParameterValueChange;
        event.mArgument.mParameter.mAudioUnit = fComponent;
        event.mArgument.mParameter.mScope     = kAudioUnitScope_Global;

        for (uint32_t i=0; i<fParameterCount; ++i)
        {
            if (fPlugin.isParameterOutputOrTrigger(i))
            {
                value = fPlugin.getParameterValue(i);

                if (d_isEqual(fLastParameterValues[i], value))
                    continue;

                fLastParameterValues[i] = value;

                // TODO flag param only, notify listeners later on bg thread (sem_post etc)
                event.mArgument.mParameter.mParameterID = i;
                AUEventListenerNotify(NULL, NULL, &event);
                notifyPropertyListeners('DPFp', kAudioUnitScope_Global, i);
            }
        }
    }

    void setLastRenderError(const OSStatus err)
    {
        if (fLastRenderError != noErr)
            return;

        fLastRenderError = err;
        notifyPropertyListeners(kAudioUnitProperty_LastRenderError, kAudioUnitScope_Global, 0);
    }

    // ----------------------------------------------------------------------------------------------------------------

   #if DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
    /* Release, and optionally allocate again, every buffer list this unit owns.
     *
     * Each AUBufferList frees itself by the shape it was allocated with, so this stays correct
     * across a bus reconfiguration that happened while the lists existed, and across a render that
     * left a bus list pointing at buffers the connected unit owns.
     */
    bool reallocAudioBufferList(const bool alloc)
    {
        fAudioBufferList.deallocate();

       #if DAF_PLUGIN_NUM_INPUTS != 0
        for (uint16_t busIndex = 0; busIndex < fInputBusCount; ++busIndex)
            fInputBuses[busIndex].bufferList.deallocate();
       #endif

        if (! alloc)
            return true;

       #if DAF_PLUGIN_NUM_OUTPUTS != 0
        #ifdef DAF_PLUGIN_EXTRA_IO
        const uint16_t numBuffers = fNumOutputs;
        #else
        constexpr const uint16_t numBuffers = DAF_PLUGIN_NUM_OUTPUTS;
        #endif
       #else
        const uint16_t numBuffers = fInputBuses[0].declaredChannels;
       #endif
        const uint32_t bufferSize = fPlugin.getBufferSize();

        if (! fAudioBufferList.allocate(numBuffers, bufferSize))
            return false;

       #if DAF_PLUGIN_NUM_INPUTS != 0
        for (uint16_t busIndex = 0; busIndex < fInputBusCount; ++busIndex)
        {
            AUInputBus& bus(fInputBuses[busIndex]);
            if (! bus.bufferList.allocate(bus.declaredChannels, bufferSize))
            {
                reallocAudioBufferList(false);
                return false;
            }
        }
       #endif

        return true;
    }
   #endif

    // ----------------------------------------------------------------------------------------------------------------

    CFMutableDictionaryRef retrieveClassInfo()
    {
        CFMutableDictionaryRef clsInfo = CFDictionaryCreateMutable(nullptr,
                                                                   0,
                                                                   &kCFTypeDictionaryKeyCallBacks,
                                                                   &kCFTypeDictionaryValueCallBacks);
        SInt32 value;

        value = 0;
        if (const CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value))
        {
            CFDictionarySetValue(clsInfo, CFSTR(kAUPresetVersionKey), num);
            CFRelease(num);
        }

        value = kType;
        if (const CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value))
        {
            CFDictionarySetValue(clsInfo, CFSTR(kAUPresetTypeKey), num);
            CFRelease(num);
        }

        value = kSubType;
        if (const CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value))
        {
            CFDictionarySetValue(clsInfo, CFSTR(kAUPresetSubtypeKey), num);
            CFRelease(num);
        }

        value = kManufacturer;
        if (const CFNumberRef num = CFNumberCreate(nullptr, kCFNumberSInt32Type, &value))
        {
            CFDictionarySetValue(clsInfo, CFSTR(kAUPresetManufacturerKey), num);
            CFRelease(num);
        }

       #if DAF_PLUGIN_WANT_PROGRAMS
        if (fCurrentProgram >= 0)
        {
            CFDictionarySetValue(clsInfo, CFSTR(kAUPresetNameKey), fFactoryPresetsData[fCurrentProgram].presetName);
        }
        else
       #endif
        {
            CFDictionarySetValue(clsInfo, CFSTR(kAUPresetNameKey), fUserPresetData.presetName);
        }

        if (const CFMutableDictionaryRef data = CFDictionaryCreateMutable(nullptr,
                                                                          0,
                                                                          &kCFTypeDictionaryKeyCallBacks,
                                                                          &kCFTypeDictionaryValueCallBacks))
        {
           #if DAF_PLUGIN_WANT_PROGRAMS
            const SInt32 program = fCurrentProgram;
            if (const CFNumberRef programRef = CFNumberCreate(nullptr, kCFNumberSInt32Type, &program))
            {
               CFDictionarySetValue(data, CFSTR("program"), programRef);
               CFRelease(programRef);
            }
           #endif

           #if DAF_PLUGIN_WANT_FULL_STATE
            // Update current state
            for (StringMap::const_iterator cit=fStateMap.begin(), cite=fStateMap.end(); cit != cite; ++cit)
            {
                const String& key(cit->first);
                fStateMap[key] = fPlugin.getStateValue(key);
            }
           #endif

           #if DAF_PLUGIN_WANT_STATE
            if (const CFMutableArrayRef statesRef = CFArrayCreateMutable(nullptr,
                                                                         fStateCount,
                                                                         &kCFTypeArrayCallBacks))
            {
                for (StringMap::const_iterator cit=fStateMap.begin(), cite=fStateMap.end(); cit != cite; ++cit)
                {
                    const String& key(cit->first);
                    const String& value(cit->second);

                   #if DAF_PLUGIN_WANT_DIRECT_ACCESS && ! DAF_PLUGIN_HAS_UI
                    bool wantStateKey = true;

                    for (uint32_t i=0; i<fStateCount; ++i)
                    {
                        if (fPlugin.getStateKey(i) == key)
                        {
                            if (fPlugin.getStateHints(i) & kStateIsOnlyForUI)
                                wantStateKey = false;

                            break;
                        }
                    }

                    if (! wantStateKey)
                        continue;
                   #endif

                    CFStringRef keyRef = CFStringCreateWithCString(nullptr, key, kCFStringEncodingASCII);
                    CFStringRef valueRef = CFStringCreateWithCString(nullptr, value, kCFStringEncodingUTF8);

                    if (const CFDictionaryRef dictRef = CFDictionaryCreate(nullptr,
                                                                           reinterpret_cast<const void**>(&keyRef),
                                                                           reinterpret_cast<const void**>(&valueRef),
                                                                           1,
                                                                           &kCFTypeDictionaryKeyCallBacks,
                                                                           &kCFTypeDictionaryValueCallBacks))
                    {
                        CFArrayAppendValue(statesRef, dictRef);
                        CFRelease(dictRef);
                    }

                    CFRelease(keyRef);
                    CFRelease(valueRef);
                }

                CFDictionarySetValue(data, CFSTR("states"), statesRef);
                CFRelease(statesRef);
            }
           #endif

            if (const CFMutableArrayRef paramsRef = CFArrayCreateMutable(nullptr,
                                                                         fParameterCount,
                                                                         &kCFTypeArrayCallBacks))
            {
                for (uint32_t i=0; i<fParameterCount; ++i)
                {
                    if (fPlugin.isParameterOutputOrTrigger(i))
                        continue;

                    const float value = fPlugin.getParameterValue(i);

                    CFStringRef keyRef = CFStringCreateWithCString(nullptr,
                                                                   fPlugin.getParameterSymbol(i),
                                                                   kCFStringEncodingASCII);
                    CFNumberRef valueRef = CFNumberCreate(nullptr, kCFNumberFloat32Type, &value);

                    if (CFDictionaryRef dictRef = CFDictionaryCreate(nullptr,
                                                                     reinterpret_cast<const void**>(&keyRef),
                                                                     reinterpret_cast<const void**>(&valueRef),
                                                                     1,
                                                                     &kCFTypeDictionaryKeyCallBacks,
                                                                     &kCFTypeDictionaryValueCallBacks))
                    {
                        CFArrayAppendValue(paramsRef, dictRef);
                        CFRelease(dictRef);
                    }

                    CFRelease(keyRef);
                    CFRelease(valueRef);
                }

                CFDictionarySetValue(data, CFSTR("params"), paramsRef);
                CFRelease(paramsRef);
            }

            CFDictionarySetValue(clsInfo, CFSTR(kAUPresetDataKey), data);
            CFRelease(data);
        }

        return clsInfo;
    }

    bool restoreClassInfo(const CFDictionaryRef clsInfo)
    {
        CFDictionaryRef data = nullptr;
        DAF_SAFE_ASSERT_RETURN(CFDictionaryGetValueIfPresent(clsInfo,
                                                                 CFSTR(kAUPresetDataKey),
                                                                 reinterpret_cast<const void**>(&data)), false);
        DAF_SAFE_ASSERT_RETURN(CFGetTypeID(data) == CFDictionaryGetTypeID(), false);

        bool parameterSnapshot = false;
       #if DAF_PLUGIN_WANT_STATE
        StringMap pendingStates;
        CFArrayRef statesRef = nullptr;
        if (CFDictionaryGetValueIfPresent(data, CFSTR("states"), reinterpret_cast<const void**>(&statesRef)))
        {
            if (statesRef == nullptr || CFGetTypeID(statesRef) != CFArrayGetTypeID()) return false;
            const auto readString = [](CFStringRef ref, CFStringEncoding encoding, String& out) {
                if (ref == nullptr || CFGetTypeID(ref) != CFStringGetTypeID()) return false;
                const CFIndex length = CFStringGetLength(ref);
                const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, encoding);
                if (capacity < 0) return false;
                std::vector<char> bytes(static_cast<size_t>(capacity) + 1u);
                CFIndex used = 0;
                const CFIndex converted = CFStringGetBytes(ref, CFRangeMake(0, length), encoding, 0, false,
                                                         reinterpret_cast<UInt8*>(bytes.data()), capacity, &used);
                // DAF's state API uses C strings. An embedded NUL cannot be
                // represented: silently truncating it can turn a different key
                // or malformed value into a valid snapshot.
                if (converted != length || used < 0 || used > capacity
                    || std::memchr(bytes.data(), 0, static_cast<size_t>(used)) != nullptr) return false;
                bytes[static_cast<size_t>(used)] = '\0';
                out = bytes.data();
                return true;
            };
            for (CFIndex i = 0; i < CFArrayGetCount(statesRef); ++i)
            {
                const CFDictionaryRef state = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(statesRef, i));
                if (state == nullptr || CFGetTypeID(state) != CFDictionaryGetTypeID()
                    || CFDictionaryGetCount(state) != 1) return false;
                CFStringRef keyRef = nullptr, valueRef = nullptr;
                CFDictionaryGetKeysAndValues(state, reinterpret_cast<const void**>(&keyRef),
                                            reinterpret_cast<const void**>(&valueRef));
                String key, value;
                if (!readString(keyRef, kCFStringEncodingASCII, key)
                    || !readString(valueRef, kCFStringEncodingUTF8, value)) return false;
                if (!fPlugin.wantStateKey(key)) continue;
                if (pendingStates.find(key) != pendingStates.end()
                    || !fPlugin.validateStateValue(key, value)) return false;
                pendingStates[key] = value;
                parameterSnapshot = parameterSnapshot || fPlugin.isParameterSnapshotState(key);
            }
        }
        if (!fPlugin.hasRequiredStates([&](const char* key) {
            return pendingStates.count(String(key));
        })) return false;
       #endif

       #if DAF_PLUGIN_WANT_PROGRAMS
        CFNumberRef programRef = nullptr;
        if (!parameterSnapshot && CFDictionaryGetValueIfPresent(data, CFSTR("program"), reinterpret_cast<const void**>(&programRef))
            && CFGetTypeID(programRef) == CFNumberGetTypeID())
        {
            SInt32 program = -1;
            // ignore a program that this build does not have, restored data is not trusted input and
            // fCurrentProgram indexes fFactoryPresetsData; negative means user preset, which is fine
            if (CFNumberGetValue(programRef, kCFNumberSInt32Type, &program)
                && program < static_cast<SInt32>(fProgramCount))
            {
                fCurrentProgram = program;

                if (program >= 0)
                {
                    fLastFactoryProgram = program;
                    fPlugin.loadProgram(fLastFactoryProgram);
                    for (uint32_t i=0; i<fParameterCount; ++i)
                        fLastParameterValues[i] = fPlugin.getParameterValue(i);
                    notifyPropertyListeners('DPFo', kAudioUnitScope_Global, 0);
                }

                notifyPropertyListeners(kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0);
            }
        }
       #endif

       #if DAF_PLUGIN_WANT_STATE
        for (const auto& item : pendingStates)
        {
            fStateMap[item.first] = item.second;
            fPlugin.setState(item.first, item.second);
            for (uint32_t j = 0; j < fStateCount; ++j)
                if (fPlugin.getStateKey(j) == item.first)
                {
                    if ((fPlugin.getStateHints(j) & kStateIsOnlyForDSP) == 0x0)
                        notifyPropertyListeners('DPFs', kAudioUnitScope_Global, j);
                    break;
                }
        }
        if (parameterSnapshot) syncParameterSnapshot();
       #endif

        CFArrayRef paramsRef = nullptr;
        if (!parameterSnapshot && CFDictionaryGetValueIfPresent(data, CFSTR("params"), reinterpret_cast<const void**>(&paramsRef))
            && CFGetTypeID(paramsRef) == CFArrayGetTypeID())
        {
            const CFIndex numParams = CFArrayGetCount(paramsRef);
            char* symbol = nullptr;
            CFIndex symbolLen = -1;

            for (CFIndex i=0; i<numParams; ++i)
            {
                const CFDictionaryRef param = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(paramsRef, i));
                DAF_SAFE_ASSERT_BREAK(CFGetTypeID(param) == CFDictionaryGetTypeID());
                DAF_SAFE_ASSERT_BREAK(CFDictionaryGetCount(param) == 1);

                CFStringRef keyRef = nullptr;
                CFNumberRef valueRef = nullptr;
                CFDictionaryGetKeysAndValues(param,
                                             reinterpret_cast<const void**>(&keyRef),
                                             reinterpret_cast<const void**>(&valueRef));
                DAF_SAFE_ASSERT_BREAK(keyRef != nullptr && CFGetTypeID(keyRef) == CFStringGetTypeID());
                DAF_SAFE_ASSERT_BREAK(valueRef != nullptr && CFGetTypeID(valueRef) == CFNumberGetTypeID());

                float value = 0.f;
                DAF_SAFE_ASSERT_BREAK(CFNumberGetValue(valueRef, kCFNumberFloat32Type, &value));

                const CFIndex keyRefLen = CFStringGetLength(keyRef);
                if (symbolLen < keyRefLen)
                {
                    symbolLen = keyRefLen;
                    symbol = static_cast<char*>(std::realloc(symbol, symbolLen + 1));
                }
                DAF_SAFE_ASSERT_BREAK(CFStringGetCString(keyRef, symbol, symbolLen + 1, kCFStringEncodingASCII));

                for (uint32_t j=0; j<fParameterCount; ++j)
                {
                    if (fPlugin.isParameterOutputOrTrigger(j))
                        continue;
                    if (fPlugin.getParameterSymbol(j) != symbol)
                        continue;

                    fLastParameterValues[j] = value;
                    fPlugin.setParameterValue(j, value);
                    notifyPropertyListeners('DPFp', kAudioUnitScope_Global, j);

                    if (fBypassParameterIndex == j)
                        notifyPropertyListeners(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);

                    break;
                }
            }

            std::free(symbol);
        }
        return true;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // DAF callbacks

   #if DAF_PLUGIN_WANT_MIDI_OUTPUT
    bool writeMidi(const MidiEvent& midiEvent)
    {
        DAF_CUSTOM_SAFE_ASSERT_ONCE_RETURN("MIDI output unsupported", fMidiOutput.midiOutputCallback != nullptr, false);
        DAF_CUSTOM_SAFE_ASSERT_ONCE_RETURN("Out of memory", fMidiOutputPackets != nullptr, false);

        if (fMidiOutputDataOffset + kMIDIPacketNonDataSize + midiEvent.size >= kMIDIPacketListMaxDataSize)
            return false;

        const uint8_t* const midiData = midiEvent.size > MidiEvent::kDataSize ? midiEvent.dataExt : midiEvent.data;
        MIDIPacket* const packet = reinterpret_cast<MIDIPacket*>(
             reinterpret_cast<uint8_t*>(fMidiOutputPackets->packet) + fMidiOutputDataOffset);

        packet->timeStamp = midiEvent.frame;
        packet->length = midiEvent.size;
        std::memcpy(packet->data, midiData, midiEvent.size);

        ++fMidiOutputPackets->numPackets;
        fMidiOutputDataOffset += kMIDIPacketNonDataSize + midiEvent.size;
        return true;
    }

    static bool writeMidiCallback(void* const ptr, const MidiEvent& midiEvent)
    {
        return static_cast<PluginAU*>(ptr)->writeMidi(midiEvent);
    }
   #endif

   #if DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
    bool requestParameterValueChange(const uint32_t index, const float value)
    {
        AudioUnitEvent event;
        std::memset(&event, 0, sizeof(event));
        event.mEventType                        = kAudioUnitEvent_ParameterValueChange;
        event.mArgument.mParameter.mAudioUnit   = fComponent;
        event.mArgument.mParameter.mParameterID = index;
        event.mArgument.mParameter.mScope       = kAudioUnitScope_Global;

        fLastParameterValues[index] = value;
        AUEventListenerNotify(NULL, NULL, &event);
        notifyPropertyListeners('DPFp', kAudioUnitScope_Global, index);
        return true;
    }

    static bool requestParameterValueChangeCallback(void* const ptr, const uint32_t index, const float value)
    {
        return static_cast<PluginAU*>(ptr)->requestParameterValueChange(index, value);
    }
   #endif

   #if DAF_PLUGIN_WANT_STATE
    void syncParameterSnapshot()
    {
       #if DAF_PLUGIN_WANT_PROGRAMS
        const int32_t program = fPlugin.getCurrentProgram();
        if (program >= 0 && static_cast<uint32_t>(program) < fPlugin.getProgramCount())
        {
            fCurrentProgram = program;
            notifyPropertyListeners(kAudioUnitProperty_PresentPreset, kAudioUnitScope_Global, 0);
        }
       #endif
        for (uint32_t i=0; i<fParameterCount; ++i)
        {
            if (fPlugin.isParameterOutputOrTrigger(i))
                continue;

            const float value = fPlugin.getParameterValue(i);
            fLastParameterValues[i] = value;

            AudioUnitEvent event;
            std::memset(&event, 0, sizeof(event));
            event.mEventType                        = kAudioUnitEvent_ParameterValueChange;
            event.mArgument.mParameter.mAudioUnit   = fComponent;
            event.mArgument.mParameter.mParameterID = i;
            event.mArgument.mParameter.mScope       = kAudioUnitScope_Global;
            AUEventListenerNotify(NULL, NULL, &event);
            notifyPropertyListeners('DPFp', kAudioUnitScope_Global, i);

            if (fBypassParameterIndex == i)
                notifyPropertyListeners(kAudioUnitProperty_BypassEffect, kAudioUnitScope_Global, 0);
        }
    }

    bool updateState(const char* const key, const char* const newValue)
    {
        fPlugin.setState(key, newValue);

        for (uint32_t i=0; i<fStateCount; ++i)
        {
            if (fPlugin.getStateKey(i) == key)
            {
                const String dkey(key);
                fStateMap[dkey] = newValue;

                if ((fPlugin.getStateHints(i) & kStateIsOnlyForDSP) == 0x0)
                    notifyPropertyListeners('DPFs', kAudioUnitScope_Global, i);

                return true;
            }
        }

        d_stderr("Failed to find plugin state with key \"%s\"", key);
        return false;
    }

    static bool updateStateValueCallback(void* const ptr, const char* const key, const char* const newValue)
    {
        return static_cast<PluginAU*>(ptr)->updateState(key, newValue);
    }
   #endif

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginAU)
};

// --------------------------------------------------------------------------------------------------------------------

struct AudioComponentPlugInInstance {
    AudioComponentPlugInInterface acpi;
    PluginAU* plugin;

    AudioComponentPlugInInstance() noexcept
        : acpi(),
          plugin(nullptr)
    {
        std::memset(&acpi, 0, sizeof(acpi));
        acpi.Open = Open;
        acpi.Close = Close;
        acpi.Lookup = Lookup;
        acpi.reserved = nullptr;
    }

    ~AudioComponentPlugInInstance()
    {
        delete plugin;
    }

    static OSStatus Open(void* const self, const AudioUnit component)
    {
        d_debug("AudioComponentPlugInInstance::Open(%p)", self);

        static_cast<AudioComponentPlugInInstance*>(self)->plugin = new PluginAU(component);
        return noErr;
    }

    static OSStatus Close(void* const self)
    {
        d_debug("AudioComponentPlugInInstance::Close(%p)", self);

        delete static_cast<AudioComponentPlugInInstance*>(self);
        return noErr;
    }

    static AudioComponentMethod Lookup(const SInt16 selector)
    {
        d_debug("AudioComponentPlugInInstance::Lookup(%3d:%s)", selector, AudioUnitSelector2Str(selector));

        switch (selector)
        {
        case kAudioUnitInitializeSelect:
            return reinterpret_cast<AudioComponentMethod>(Initialize);
        case kAudioUnitUninitializeSelect:
            return reinterpret_cast<AudioComponentMethod>(Uninitialize);
        case kAudioUnitGetPropertyInfoSelect:
            return reinterpret_cast<AudioComponentMethod>(GetPropertyInfo);
        case kAudioUnitGetPropertySelect:
            return reinterpret_cast<AudioComponentMethod>(GetProperty);
        case kAudioUnitSetPropertySelect:
            return reinterpret_cast<AudioComponentMethod>(SetProperty);
        case kAudioUnitAddPropertyListenerSelect:
            return reinterpret_cast<AudioComponentMethod>(AddPropertyListener);
        case kAudioUnitRemovePropertyListenerSelect:
            return reinterpret_cast<AudioComponentMethod>(RemovePropertyListener);
        case kAudioUnitRemovePropertyListenerWithUserDataSelect:
            return reinterpret_cast<AudioComponentMethod>(RemovePropertyListenerWithUserData);
        case kAudioUnitAddRenderNotifySelect:
            return reinterpret_cast<AudioComponentMethod>(AddRenderNotify);
        case kAudioUnitRemoveRenderNotifySelect:
            return reinterpret_cast<AudioComponentMethod>(RemoveRenderNotify);
        case kAudioUnitGetParameterSelect:
            return reinterpret_cast<AudioComponentMethod>(GetParameter);
        case kAudioUnitSetParameterSelect:
            return reinterpret_cast<AudioComponentMethod>(SetParameter);
        case kAudioUnitScheduleParametersSelect:
            return reinterpret_cast<AudioComponentMethod>(ScheduleParameters);
        case kAudioUnitRenderSelect:
            return reinterpret_cast<AudioComponentMethod>(Render);
        /*
        case kAudioUnitComplexRenderSelect:
            return reinterpret_cast<AudioComponentMethod>(ComplexRender);
        */
        case kAudioUnitResetSelect:
            return reinterpret_cast<AudioComponentMethod>(Reset);
        /*
        case kAudioUnitProcessSelect:
            return reinterpret_cast<AudioComponentMethod>(Process);
        case kAudioUnitProcessMultipleSelect:
            return reinterpret_cast<AudioComponentMethod>(ProcessMultiple);
        */
       #if DAF_PLUGIN_WANT_MIDI_INPUT
        case kMusicDeviceMIDIEventSelect:
            return reinterpret_cast<AudioComponentMethod>(MIDIEvent);
        case kMusicDeviceSysExSelect:
            return reinterpret_cast<AudioComponentMethod>(SysEx);
       #else
        case kMusicDeviceMIDIEventSelect:
        case kMusicDeviceSysExSelect:
            return nullptr;
       #endif
        }

        d_stdout("TODO Lookup(%3d:%s)", selector, AudioUnitSelector2Str(selector));
        return nullptr;
    }

    static OSStatus Initialize(AudioComponentPlugInInstance* const self)
    {
        d_debug("AudioComponentPlugInInstance::Initialize(%p)", self);
        return self->plugin->auInitialize();
    }

    static OSStatus Uninitialize(AudioComponentPlugInInstance* const self)
    {
        d_debug("AudioComponentPlugInInstance::Uninitialize(%p)", self);
        return self->plugin->auUninitialize();
    }

    static OSStatus GetPropertyInfo(AudioComponentPlugInInstance* const self,
                                    const AudioUnitPropertyID inProp,
                                    const AudioUnitScope inScope,
                                    const AudioUnitElement inElement,
                                    UInt32* const outDataSize,
                                    Boolean* const outWritable)
    {
        d_debug("AudioComponentPlugInInstance::GetPropertyInfo(%p, %d:%x:%s, %d:%s, %d, ...)",
                self, inProp, inProp, AudioUnitPropertyID2Str(inProp), inScope, AudioUnitScope2Str(inScope), inElement);

        UInt32 dataSize = 0;
        Boolean writable = false;
        const OSStatus res = self->plugin->auGetPropertyInfo(inProp, inScope, inElement, dataSize, writable);

        if (outDataSize != nullptr)
            *outDataSize = dataSize;

        if (outWritable != nullptr)
            *outWritable = writable;

        return res;
    }

    static OSStatus GetProperty(AudioComponentPlugInInstance* const self,
                                const AudioUnitPropertyID inProp,
                                const AudioUnitScope inScope,
                                const AudioUnitElement inElement,
                                void* const outData,
                                UInt32* const ioDataSize)
    {
       #ifdef DEBUG
        switch (inProp) {
        case kAudioUnitProperty_PresentPreset:
            break;
        default:
            d_debug("AudioComponentPlugInInstance::GetProperty(%p, %d:%x:%s, %d:%s, %d, ...)",
                    self, inProp, inProp, AudioUnitPropertyID2Str(inProp), inScope, AudioUnitScope2Str(inScope), inElement);
            break;
        }
       #endif
        DAF_SAFE_ASSERT_RETURN(ioDataSize != nullptr, kAudio_ParamError);

        Boolean writable;
        UInt32 outDataSize = 0;
        OSStatus res;

        if (outData == nullptr)
        {
            res = self->plugin->auGetPropertyInfo(inProp, inScope, inElement, outDataSize, writable);
            *ioDataSize = outDataSize;
            return res;
        }

        const UInt32 inDataSize = *ioDataSize;
        if (inDataSize == 0)
            return kAudio_ParamError;

        res = self->plugin->auGetPropertyInfo(inProp, inScope, inElement, outDataSize, writable);

        if (res != noErr)
            return res;

        void* outBuffer;
        uint8_t* tmpBuffer;
        if (inDataSize < outDataSize)
        {
            tmpBuffer = new uint8_t[outDataSize];
            outBuffer = tmpBuffer;
        }
        else
        {
            tmpBuffer = nullptr;
            outBuffer = outData;
        }

        res = self->plugin->auGetProperty(inProp, inScope, inElement, outBuffer);

        if (res != noErr)
        {
            *ioDataSize = 0;
            return res;
        }

        if (tmpBuffer != nullptr)
        {
            memcpy(outData, tmpBuffer, inDataSize);
            delete[] tmpBuffer;
        }
        else
        {
            *ioDataSize = outDataSize;
        }

        return noErr;
    }

    static OSStatus SetProperty(AudioComponentPlugInInstance* const self,
                                const AudioUnitPropertyID inProp,
                                const AudioUnitScope inScope,
                                const AudioUnitElement inElement,
                                const void* const inData,
                                const UInt32 inDataSize)
    {
        d_debug("AudioComponentPlugInInstance::SetProperty(%p, %d:%x:%s, %d:%s, %d, %p, %u)",
                self, inProp, inProp, AudioUnitPropertyID2Str(inProp), inScope, AudioUnitScope2Str(inScope), inElement, inData, inDataSize);
        return self->plugin->auSetProperty(inProp, inScope, inElement, inData, inDataSize);
    }

    static OSStatus AddPropertyListener(AudioComponentPlugInInstance* const self,
                                        const AudioUnitPropertyID prop,
                                        const AudioUnitPropertyListenerProc proc,
                                        void* const userData)
    {
        d_debug("AudioComponentPlugInInstance::AddPropertyListener(%p, %d:%x:%s, %p, %p)",
                self, prop, prop, AudioUnitPropertyID2Str(prop), proc, userData);
        return self->plugin->auAddPropertyListener(prop, proc, userData);
    }

    static OSStatus RemovePropertyListener(AudioComponentPlugInInstance* const self,
                                           const AudioUnitPropertyID prop,
                                           const AudioUnitPropertyListenerProc proc)
    {
        d_debug("AudioComponentPlugInInstance::RemovePropertyListener(%p, %d:%x:%s, %p)",
                self, prop, prop, AudioUnitPropertyID2Str(prop), proc);
        return self->plugin->auRemovePropertyListener(prop, proc);
    }

    static OSStatus RemovePropertyListenerWithUserData(AudioComponentPlugInInstance* const self,
                                                       const AudioUnitPropertyID prop,
                                                       const AudioUnitPropertyListenerProc proc,
                                                       void* const userData)
    {
        d_debug("AudioComponentPlugInInstance::RemovePropertyListenerWithUserData(%p, %d:%x:%s, %p, %p)",
                self, prop, prop, AudioUnitPropertyID2Str(prop), proc, userData);
        return self->plugin->auRemovePropertyListenerWithUserData(prop, proc, userData);
    }

    static OSStatus AddRenderNotify(AudioComponentPlugInInstance* const self,
                                    const AURenderCallback proc,
                                    void* const userData)
    {
        d_debug("AudioComponentPlugInInstance::AddRenderNotify(%p, %p, %p)", self, proc, userData);
        return self->plugin->auAddRenderNotify(proc, userData);
    }

    static OSStatus RemoveRenderNotify(AudioComponentPlugInInstance* const self,
                                       const AURenderCallback proc,
                                       void* const userData)
    {
        d_debug("AudioComponentPlugInInstance::RemoveRenderNotify(%p, %p, %p)", self, proc, userData);
        return self->plugin->auRemoveRenderNotify(proc, userData);
    }

    static OSStatus GetParameter(AudioComponentPlugInInstance* const self,
                                 const AudioUnitParameterID param,
                                 const AudioUnitScope scope,
                                 const AudioUnitElement elem,
                                 AudioUnitParameterValue* const value)
    {
        d_debug("AudioComponentPlugInInstance::GetParameter(%p, %d, %d:%s, %d, %p)",
                self, param, scope, AudioUnitScope2Str(scope), elem, value);
        return self->plugin->auGetParameter(param, scope, elem, value);
    }

    static OSStatus SetParameter(AudioComponentPlugInInstance* const self,
                                 const AudioUnitParameterID param,
                                 const AudioUnitScope scope,
                                 const AudioUnitElement elem,
                                 const AudioUnitParameterValue value,
                                 const UInt32 bufferOffset)
    {
        d_debug("AudioComponentPlugInInstance::SetParameter(%p, %d %d:%s, %d, %f, %u)",
                self, param, scope, AudioUnitScope2Str(scope), elem, value, bufferOffset);
        return self->plugin->auSetParameter(param, scope, elem, value, bufferOffset);
    }

    static OSStatus ScheduleParameters(AudioComponentPlugInInstance* const self,
                                       const AudioUnitParameterEvent* const events,
                                       const UInt32 numEvents)
    {
        d_debug("AudioComponentPlugInInstance::ScheduleParameters(%p, %p, %u)", self, events, numEvents);
        return self->plugin->auScheduleParameters(events, numEvents);
    }

    static OSStatus Reset(AudioComponentPlugInInstance* const self,
                          const AudioUnitScope scope,
                          const AudioUnitElement elem)
    {
        d_debug("AudioComponentPlugInInstance::Reset(%p, %d:%s, %d)", self, scope, AudioUnitScope2Str(scope), elem);
        return self->plugin->auReset(scope, elem);
    }

    static OSStatus Render(AudioComponentPlugInInstance* const self,
                           AudioUnitRenderActionFlags* ioActionFlags,
                           const AudioTimeStamp* const inTimeStamp,
                           const UInt32 inOutputBusNumber,
                           const UInt32 inNumberFrames,
                           AudioBufferList* const ioData)
    {
        const AudioUnitRenderActionFlags actionFlags = ioActionFlags != nullptr ? *ioActionFlags : 0;

        if ((actionFlags & kAudioUnitRenderAction_DoNotCheckRenderArgs) == 0x0)
        {
            DAF_SAFE_ASSERT_RETURN(inTimeStamp != nullptr, kAudio_ParamError);
            DAF_SAFE_ASSERT_RETURN(ioData != nullptr, kAudio_ParamError);
        }

        return self->plugin->auRender(actionFlags, inTimeStamp, inOutputBusNumber, inNumberFrames, ioData);
    }

   #if DAF_PLUGIN_WANT_MIDI_INPUT
    static OSStatus MIDIEvent(AudioComponentPlugInInstance* const self,
                              const UInt32 inStatus,
                              const UInt32 inData1,
                              const UInt32 inData2,
                              const UInt32 inOffsetSampleFrame)
    {
        return self->plugin->auMIDIEvent(inStatus, inData1, inData2, inOffsetSampleFrame);
    }

    static OSStatus SysEx(AudioComponentPlugInInstance* const self, const UInt8* const inData, const UInt32 inLength)
    {
        return self->plugin->auSysEx(inData, inLength);
    }
   #endif

    DAF_DECLARE_NON_COPYABLE(AudioComponentPlugInInstance)
};

#if 0
static OSStatus FastDispatchGetParameter(void* const self,
                                         const AudioUnitParameterID param,
                                         const AudioUnitScope scope,
                                         const AudioUnitElement elem,
                                         Float32* const value)
{
    d_debug("FastDispatchGetParameter(%p, %d, %d:%s, %d, %p)",
            self, param, scope, AudioUnitScope2Str(scope), elem, value);
    return static_cast<AudioComponentPlugInInstance*>(self)->plugin->auGetParameter(param, scope, elem, value);
}

static OSStatus FastDispatchSetParameter(void* const self,
                                         const AudioUnitParameterID param,
                                         const AudioUnitScope scope,
                                         const AudioUnitElement elem,
                                         const Float32 value,
                                         const UInt32 bufferOffset)
{
    d_debug("FastDispatchSetParameter(%p, %d %d:%s, %d, %f, %u)",
            self, param, scope, AudioUnitScope2Str(scope), elem, value, bufferOffset);
    return static_cast<AudioComponentPlugInInstance*>(self)->plugin->auSetParameter(param, scope, elem, value, bufferOffset);
}

static OSStatus FastDispatchRender(void* const self,
                                   AudioUnitRenderActionFlags* const ioActionFlags,
                                   const AudioTimeStamp* const inTimeStamp,
                                   const UInt32 inBusNumber,
                                   const UInt32 inNumberFrames,
                                   AudioBufferList* const ioData)
{
    DAF_SAFE_ASSERT_RETURN(inTimeStamp != nullptr, kAudio_ParamError);
    DAF_SAFE_ASSERT_RETURN(ioData != nullptr, kAudio_ParamError);

    return static_cast<AudioComponentPlugInInstance*>(self)->plugin->auRender(ioActionFlags, inTimeStamp, inBusNumber, inNumberFrames, *ioData);
}
#endif

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

DAF_PLUGIN_EXPORT
void* PluginAUFactory(const AudioComponentDescription* const desc)
{
    USE_NAMESPACE_DAF

    DAF_SAFE_ASSERT_UINT2_RETURN(desc->componentType == kType, desc->componentType, kType, nullptr);
    DAF_SAFE_ASSERT_UINT2_RETURN(desc->componentSubType == kSubType, desc->componentSubType, kSubType, nullptr);
    DAF_SAFE_ASSERT_UINT2_RETURN(desc->componentManufacturer == kManufacturer, desc->componentManufacturer, kManufacturer, nullptr);

    if (d_nextBufferSize == 0)
        d_nextBufferSize = 1156;

    if (d_isZero(d_nextSampleRate))
        d_nextSampleRate = 44100.0;

    if (d_nextBundlePath == nullptr)
    {
        static String bundlePath;

        String tmpPath(getBinaryFilename());
        tmpPath.truncate(tmpPath.rfind(DAF_OS_SEP));
        tmpPath.truncate(tmpPath.rfind(DAF_OS_SEP));

        if (tmpPath.endsWith(DAF_OS_SEP_STR "Contents"))
        {
            tmpPath.truncate(tmpPath.rfind(DAF_OS_SEP));
            bundlePath = tmpPath;
        }
        else
        {
            bundlePath = "error";
        }

        d_nextBundlePath = bundlePath.buffer();
    }

    d_nextCanRequestParameterValueChanges = true;

    return new AudioComponentPlugInInstance();
}

// --------------------------------------------------------------------------------------------------------------------
