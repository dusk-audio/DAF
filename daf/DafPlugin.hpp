/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2024 Filipe Coelho <falktx@falktx.com>
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

#ifndef DAF_PLUGIN_HPP_INCLUDED
#define DAF_PLUGIN_HPP_INCLUDED

#include "DafDetails.hpp"
#include "extra/LeakDetector.hpp"
#include "src/DafPluginChecks.h"

START_NAMESPACE_DAF

/* ------------------------------------------------------------------------------------------------------------
 * DAF Plugin */

/**
   @defgroup MainClasses Main Classes
   @{
 */

/**
   DAF Plugin class from where plugin instances are created.

   The public methods (Host state) are called from the plugin to get or set host information.@n
   They can be called from a plugin instance at anytime unless stated otherwise.@n
   All other methods are to be implemented by the plugin and will be called by the host.

   Shortly after a plugin instance is created, the various init* functions will be called by the host.@n
   Host will call activate() before run(), and deactivate() before the plugin instance is destroyed.@n
   The host may call deactivate right after activate and vice-versa, but never activate/deactivate consecutively.@n
   There is no limit on how many times run() is called, only that activate/deactivate will be called in between.

   The buffer size and sample rate values will remain constant between activate and deactivate.@n
   Buffer size is only a hint though, the host might call run() with a higher or lower number of frames.

   Some of this class functions are only available according to some macros.

   DAF_PLUGIN_WANT_PROGRAMS activates program related features.@n
   When enabled you need to implement initProgramName() and loadProgram().

   DAF_PLUGIN_WANT_STATE activates internal state features.@n
   When enabled you need to implement initState() and setState().

   The process function run() changes wherever DAF_PLUGIN_WANT_MIDI_INPUT is enabled or not.@n
   When enabled it provides midi input events.
 */
class Plugin
{
public:
   /**
      Plugin class constructor.@n
      You must set all parameter values to their defaults, matching ParameterRanges::def.
    */
    Plugin(uint32_t parameterCount, uint32_t programCount, uint32_t stateCount);

   /**
      Destructor.
    */
    virtual ~Plugin();

   /* --------------------------------------------------------------------------------------------------------
    * Host state */

   /**
      Get the current buffer size that will probably be used during processing, in frames.@n
      This value will remain constant between activate and deactivate.
      @note This value is only a hint!@n
            Hosts might call run() with a higher or lower number of frames.
      @see bufferSizeChanged(uint32_t)
    */
    uint32_t getBufferSize() const noexcept;

   /**
      Get the current sample rate that will be used during processing.@n
      This value will remain constant between activate and deactivate.
      @see sampleRateChanged(double)
    */
    double getSampleRate() const noexcept;

   /**
      Get the bundle path where the plugin resides.
      Can return null if the plugin is not available in a bundle (if it is a single binary).
      @see getBinaryFilename
      @see getResourcePath
    */
    const char* getBundlePath() const noexcept;

   /**
      Check if this plugin instance is a "dummy" one used for plugin meta-data/information export.@n
      When true no processing will be done, the plugin is created only to extract information.@n
      In DAF, LADSPA/DSSI, VST2 and VST3 formats create one global instance per plugin binary
      while LV2 creates one when generating turtle meta-data.
    */
    bool isDummyInstance() const noexcept;

   /**
      Check if this plugin instance is a "selftest" one used for automated plugin tests.@n
      To enable this mode build with `DAF_RUNTIME_TESTING` macro defined (i.e. set as compiler build flag),
      and run the JACK/Standalone executable with "selftest" as its only and single argument.

      A few basic DSP and UI tests will run in self-test mode, with once instance having this function returning true.@n
      You can use this chance to do a few tests of your own as well.
    */
    bool isSelfTestInstance() const noexcept;

#if DAF_PLUGIN_WANT_TIMEPOS
   /**
      Get the current host transport time position.@n
      This function should only be called during run().@n
      You can call this during other times, but the returned position is not guaranteed to be in sync.
      @note TimePosition is not supported in LADSPA and DSSI plugin formats.
    */
    const TimePosition& getTimePosition() const noexcept;
#endif

#if DAF_PLUGIN_WANT_LATENCY
   /**
      Change the plugin audio output latency to @a frames.@n
      This function should only be called in the constructor, activate() and run().
      @note This function is only available if DAF_PLUGIN_WANT_LATENCY is enabled.
    */
    void setLatency(uint32_t frames) noexcept;
#endif

#if DAF_PLUGIN_WANT_TAIL
   /**
      Tail length value meaning "the tail never ends" (a freeze, a feedback loop at unity).
    */
    static constexpr uint32_t kTailInfinite = 0xffffffffU;

   /**
      Change the plugin audio tail length to @a frames.@n
      The tail is how long the output keeps carrying signal after the input goes silent
      (a reverb decay, a delay feedback path); hosts use it to decide when a bounce may
      stop and when an idle plugin may be suspended. Pass @ref kTailInfinite for a tail
      that never ends. 0 means no tail.@n
      This function should only be called in the constructor, activate() and run().
      Report it in frames at the sample rate passed to activate().
      @note This function is only available if DAF_PLUGIN_WANT_TAIL is enabled.
      @note VST3, AU and CLAP report the tail to the host; LV2 has no tail concept.
    */
    void setTail(uint32_t frames) noexcept;
#endif

#if DAF_PLUGIN_WANT_MIDI_OUTPUT
   /**
      Write a MIDI output event.@n
      This function must only be called during run().@n
      Returns false when the host buffer is full, in which case do not call this again until the next run().
    */
    bool writeMidiEvent(const MidiEvent& midiEvent) noexcept;
#endif

#if DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
   /**
      Check if parameter value change requests will work with the current plugin host.
      @note This function is only available if DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST is enabled.
      @see requestParameterValueChange(uint32_t, float)
    */
    bool canRequestParameterValueChanges() const noexcept;

   /**
      Request a parameter value change from the host.
      If successful, this function will automatically trigger a parameter update on the UI side as well.
      This function can fail, for example if the host is busy with the parameter for read-only automation.
      Some hosts simply do not have this functionality, which can be verified with canRequestParameterValueChanges().
      @note This function is only available if DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST is enabled.
    */
    bool requestParameterValueChange(uint32_t index, float value) noexcept;
#endif

#if DAF_PLUGIN_WANT_STATE
   /**
      Set state value and notify the host about the change.@n
      This function will call `setState()` and also trigger an update on the UI side as necessary.@n
      It must not be called during run.@n
      The state must be host readable.@n
      Returns false for an unknown key.@n
      On CLAP, VST2, VST3 and JACK it may be called from any thread other than the audio one:
      `setState()` is called right away, while the UI and the host's "modified" flag are updated
      later on the host's main thread. A state the host saves in the meantime has the new value.
      @note this function does nothing on DSSI plugin format, as DSSI only supports UI->DSP messages.

      TODO API under construction
    */
    bool updateStateValue(const char* key, const char* value) noexcept;
#endif

protected:
   /* --------------------------------------------------------------------------------------------------------
    * Information */

   /**
      Get the plugin name.@n
      Returns DAF_PLUGIN_NAME by default.
    */
    virtual const char* getName() const { return DAF_PLUGIN_NAME; }

   /**
      Get the plugin label.@n
      This label is a short restricted name consisting of only _, a-z, A-Z and 0-9 characters.
    */
   #ifdef DAF_PLUGIN_LABEL
    virtual const char* getLabel() const
    {
         return DAF_PLUGIN_LABEL;
    }
   #else
    virtual const char* getLabel() const = 0;
   #endif

   /**
      Get an extensive comment/description about the plugin.@n
      Optional, returns nothing by default.
    */
    virtual const char* getDescription() const
    {
       #ifdef DAF_PLUGIN_DESCRIPTION
        return DAF_PLUGIN_DESCRIPTION;
       #else
        return "";
       #endif
    }

   /**
      Get the plugin author/maker.
    */
   #ifdef DAF_PLUGIN_MAKER
    virtual const char* getMaker() const
    {
         return DAF_PLUGIN_MAKER;
    }
   #else
    virtual const char* getMaker() const = 0;
   #endif

   /**
      Get the plugin homepage.@n
      Optional, returns nothing by default.
    */
    virtual const char* getHomePage() const
    {
       #ifdef DAF_PLUGIN_HOMEPAGE
        return DAF_PLUGIN_HOMEPAGE;
       #else
        return "";
       #endif
    }

   /**
      Get the plugin license (a single line of text or a URL).@n
      For commercial plugins this should return some short copyright information.
    */
   #ifdef DAF_PLUGIN_LICENSE
    virtual const char* getLicense() const
    {
         return DAF_PLUGIN_LICENSE;
    }
   #else
    virtual const char* getLicense() const = 0;
   #endif

   /**
      Get the plugin version, in hexadecimal.
      @see d_version()
    */
    virtual uint32_t getVersion() const = 0;

   /**
      Get the plugin unique Id.@n
      This value is used by LADSPA, DSSI, VST2, VST3 and AUv2 plugin formats.@n
      @note It is preferred that you set DAF_PLUGIN_UNIQUE_ID macro instead of overriding this call,
            as that is required for AUv2 plugins anyhow.
      @see d_cconst()
    */
   #ifdef DAF_PLUGIN_UNIQUE_ID
    virtual int64_t getUniqueId() const
    {
         return d_cconst(STRINGIFY(DAF_PLUGIN_UNIQUE_ID));
    }
   #else
    virtual int64_t getUniqueId() const = 0;
   #endif

   /* --------------------------------------------------------------------------------------------------------
    * Init */

   /**
      Initialize the audio port @a index.@n
      This function will be called once, shortly after the plugin is created.
    */
    virtual void initAudioPort(bool input, uint32_t index, AudioPort& port);

   /**
      Initialize the parameter @a index.@n
      This function will be called once, shortly after the plugin is created.
    */
    virtual void initParameter(uint32_t index, Parameter& parameter);

   /**
      Initialize the port group @a groupId.@n
      This function will be called once,
      shortly after the plugin is created and all audio ports and parameters have been enumerated.
    */
    virtual void initPortGroup(uint32_t groupId, PortGroup& portGroup);

#if DAF_PLUGIN_WANT_PROGRAMS
   /**
      Set the name of the program @a index.@n
      This function will be called once, shortly after the plugin is created.@n
      Must be implemented by your plugin class only if DAF_PLUGIN_WANT_PROGRAMS is enabled.
    */
    virtual void initProgramName(uint32_t index, String& programName) = 0;
#endif

#if DAF_PLUGIN_WANT_STATE
   /**
      Initialize the state @a index.@n
      This function will be called once, shortly after the plugin is created.@n
      Must be implemented by your plugin class only if DAF_PLUGIN_WANT_STATE is enabled.
    */
    virtual void initState(uint32_t index, State& state);

    DAF_DEPRECATED_BY("initState(uint32_t,State&)")
    virtual void initState(uint32_t, String&, String&) {}

    DAF_DEPRECATED_BY("initState(uint32_t,State&)")
    virtual bool isStateFile(uint32_t) { return false; }
#endif

   /* --------------------------------------------------------------------------------------------------------
    * Internal data */

   /**
      Get the current value of a parameter.@n
      The host may call this function from any context, including realtime processing.
    */
    virtual float getParameterValue(uint32_t index) const;

   /**
      Whether this parameter supplies custom text conversion callbacks.
      Custom conversion callbacks must be nonallocating and safe to call concurrently.
      When true, a callback returning false reports invalid conversion; adapters must not
      fall back to generic numeric parsing or formatting.
    */
    virtual bool hasCustomParameterText(uint32_t) const { return false; }

   /**
      Format a parameter-domain value into @a text without allocating.
      Null text or zero capacity must be handled safely by the override. Returns false
      when conversion is unavailable or the buffer is insufficient.
    */
    virtual bool getParameterValueText(uint32_t, float, char*, uint32_t) const
    {
        return false;
    }

   /**
      Parse @a text into a parameter-domain value without allocating.
      Null text must be handled safely by the override. Returns false when conversion is
      unavailable or invalid.
    */
    virtual bool getParameterValueFromText(uint32_t, const char*, float&) const
    {
        return false;
    }

   /**
      Change a parameter value.@n
      The host may call this function from any context, including realtime processing.@n
      When a parameter is marked as automatable, you must ensure no non-realtime operations are performed.
      @note This function will only be called for parameter inputs.
    */
    virtual void setParameterValue(uint32_t index, float value);

#if DAF_PLUGIN_WANT_PROGRAMS
   /**
      Load a program.@n
      The host may call this function from any context, including realtime processing.@n
      Must be implemented by your plugin class only if DAF_PLUGIN_WANT_PROGRAMS is enabled.
    */
    virtual void loadProgram(uint32_t index);

   /**
      Optional identity readback after a complete state recall.@n
      Return the current factory program, or -1 when no program is known.
      This getter must be safe to call concurrently with processing.
    */
    virtual int32_t getCurrentProgram() const { return -1; }
#endif

#if DAF_PLUGIN_WANT_FULL_STATE
   /**
      Get the value of an internal state.@n
      The host may call this function from any non-realtime context.@n
      Must be implemented by your plugin class if DAF_PLUGIN_WANT_FULL_STATE is enabled.
      @note The use of this function breaks compatibility with the DSSI format.
    */
    virtual String getState(const char* key) const;
#endif

#if DAF_PLUGIN_WANT_STATE
   /**
      Change an internal state @a key to @a value.@n
      Must be implemented by your plugin class only if DAF_PLUGIN_WANT_STATE is enabled.
    */
    virtual void setState(const char* key, const char* value);

   /** Validate a state value without changing the plugin. Called outside the audio callback.
       Override for complete sound snapshots so wrappers can reject a malformed recall before
       applying its program or parameters. The default preserves existing plugin behavior. */
    virtual bool validateStateValue(const char*, const char*) const { return true; }
#endif

   /* --------------------------------------------------------------------------------------------------------
    * Audio/MIDI Processing */

   /**
      Activate this plugin.
    */
    virtual void activate() {}

   /**
      Deactivate this plugin.
    */
    virtual void deactivate() {}

#if DAF_PLUGIN_WANT_MIDI_INPUT
   /**
      Run/process function for plugins with MIDI input.
      @note Some parameters might be null if there are no audio inputs/outputs or MIDI events.
    */
    virtual void run(const float** inputs, float** outputs, uint32_t frames,
                     const MidiEvent* midiEvents, uint32_t midiEventCount) = 0;
#else
   /**
      Run/process function for plugins without MIDI input.
      @note Some parameters might be null if there are no audio inputs or outputs.
    */
    virtual void run(const float** inputs, float** outputs, uint32_t frames) = 0;
#endif

   /* --------------------------------------------------------------------------------------------------------
    * Callbacks (optional) */

   /**
      Optional callback to inform the plugin about a buffer size change.@n
      This function will only be called when the plugin is deactivated.
      @note This value is only a hint!@n
            Hosts might call run() with a higher or lower number of frames.
      @see getBufferSize()
    */
    virtual void bufferSizeChanged(uint32_t newBufferSize);

   /**
      Optional callback to inform the plugin about a sample rate change.@n
      This function will only be called when the plugin is deactivated.
      @see getSampleRate()
    */
    virtual void sampleRateChanged(double newSampleRate);

   /**
      Optional callback to inform the plugin about audio port IO changes.@n
      This function will only be called when the plugin is deactivated.@n
      Used by AU, and by CLAP/VST3 for supported main-bus configurations
      declared with DAF_PLUGIN_EXTRA_IO.
      @see DAF_PLUGIN_EXTRA_IO
    */
    virtual void ioChanged(uint16_t numInputs, uint16_t numOutputs);

    // -------------------------------------------------------------------------------------------------------

private:
    struct PrivateData;
    PrivateData* const pData;
    friend class PluginExporter;

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Plugin)
};

/** @} */

/* ------------------------------------------------------------------------------------------------------------
 * Create plugin, entry point */

/**
   @defgroup EntryPoints Entry Points
   @{
 */

/**
   Create an instance of the Plugin class.@n
   This is the entry point for DAF plugins.@n
   DAF will call this to either create an instance of your plugin for the host
   or to fetch some initial information for internal caching.
 */
extern Plugin* createPlugin();

/** @} */

// -----------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_PLUGIN_HPP_INCLUDED
