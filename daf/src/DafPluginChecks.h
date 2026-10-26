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

#ifndef DAF_PLUGIN_CHECKS_H_INCLUDED
#define DAF_PLUGIN_CHECKS_H_INCLUDED

#ifndef DAF_DETAILS_HPP_INCLUDED
# error wrong include order
#endif

#include "DafPluginInfo.h"

// --------------------------------------------------------------------------------------------------------------------
// Check if all required macros are defined

#ifndef DAF_PLUGIN_NAME
# error DAF_PLUGIN_NAME undefined!
#endif

#ifndef DAF_PLUGIN_NUM_INPUTS
# error DAF_PLUGIN_NUM_INPUTS undefined!
#endif

#ifndef DAF_PLUGIN_NUM_OUTPUTS
# error DAF_PLUGIN_NUM_OUTPUTS undefined!
#endif

#ifndef DAF_PLUGIN_URI
# error DAF_PLUGIN_URI undefined!
#endif

// --------------------------------------------------------------------------------------------------------------------
// Define optional macros if not done yet

#ifndef DAF_PLUGIN_HAS_UI
# define DAF_PLUGIN_HAS_UI 0
#endif

#ifndef DAF_PLUGIN_IS_RT_SAFE
# define DAF_PLUGIN_IS_RT_SAFE 0
#endif

#ifndef DAF_PLUGIN_IS_SYNTH
# define DAF_PLUGIN_IS_SYNTH 0
#endif

#ifndef DAF_PLUGIN_WANT_DIRECT_ACCESS
# define DAF_PLUGIN_WANT_DIRECT_ACCESS 0
#endif

#ifndef DAF_PLUGIN_WANT_LATENCY
# define DAF_PLUGIN_WANT_LATENCY 0
#endif

#ifndef DAF_PLUGIN_WANT_TAIL
# define DAF_PLUGIN_WANT_TAIL 0
#endif

#ifndef DAF_PLUGIN_WANT_MIDI_AS_MPE
# define DAF_PLUGIN_WANT_MIDI_AS_MPE 0
#endif

#ifndef DAF_PLUGIN_WANT_MIDI_OUTPUT
# define DAF_PLUGIN_WANT_MIDI_OUTPUT 0
#endif

#ifndef DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST
# define DAF_PLUGIN_WANT_PARAMETER_VALUE_CHANGE_REQUEST 0
#endif

#ifndef DAF_PLUGIN_WANT_PROGRAMS
# define DAF_PLUGIN_WANT_PROGRAMS 0
#endif

#ifndef DAF_PLUGIN_WANT_STATE
# define DAF_PLUGIN_WANT_STATE 0
#endif

#ifndef DAF_PLUGIN_WANT_FULL_STATE
# define DAF_PLUGIN_WANT_FULL_STATE 0
# define DAF_PLUGIN_WANT_FULL_STATE_WAS_NOT_SET
#endif

#ifndef DAF_PLUGIN_WANT_TIMEPOS
# define DAF_PLUGIN_WANT_TIMEPOS 0
#endif

// AU only: a plugin's one sidechain bus may run at its declared width or narrower, and when connected join the
// main-bus layout declared on its own for the output count, so a mono instance (Logic sets every input element of a
// mono track to one channel) runs keyed by a mono or a stereo sidechain (see Plugin::ioChanged()). A connection is
// still refused when the layout it completes cannot run at that moment; Logic sets every format before connecting.
#ifndef DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO
# define DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO 0
#endif
// An older DAF ignores the opt-in silently; a plugin that relies on it can test this.
#define DAF_SUPPORTS_SIDECHAIN_ON_EXTRA_IO 1

#if DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO && !defined(DAF_PLUGIN_EXTRA_IO)
# error DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO needs DAF_PLUGIN_EXTRA_IO
#endif

#ifndef DAF_UI_FILE_BROWSER
# define DAF_UI_FILE_BROWSER 0
#endif

#ifndef DAF_UI_WEB_VIEW
# define DAF_UI_WEB_VIEW 0
#endif

#ifndef DAF_UI_USER_RESIZABLE
# define DAF_UI_USER_RESIZABLE 0
#endif

// --------------------------------------------------------------------------------------------------------------------
// set UI type

#ifndef DAF_UI_USE_CAIRO
# define DAF_UI_USE_CAIRO 0
#endif

#ifndef DAF_UI_USE_CUSTOM
# define DAF_UI_USE_CUSTOM 0
#endif

#ifndef DAF_UI_USE_EXTERNAL
# define DAF_UI_USE_EXTERNAL 0
#endif

#ifndef DAF_UI_USE_NANOVG
# define DAF_UI_USE_NANOVG 0
#endif

#ifndef DAF_UI_USE_WEB_VIEW
# define DAF_UI_USE_WEB_VIEW 0
#endif

// --------------------------------------------------------------------------------------------------------------------
// Define DAF_UI_WEB_VIEW if needed

#if DAF_UI_USE_WEB_VIEW && !DAF_UI_WEB_VIEW
# undef DAF_UI_WEB_VIEW
# define DAF_UI_WEB_VIEW 1
#endif

// --------------------------------------------------------------------------------------------------------------------
// Define DAF_UI_URI if needed

#ifndef DAF_UI_URI
# define DAF_UI_URI DAF_PLUGIN_URI "#DAF_UI"
#endif

// --------------------------------------------------------------------------------------------------------------------
// Test for wrong compiler macros

#if defined(DAF_PLUGIN_HAS_EMBED_UI)
# warning DAF_PLUGIN_HAS_EMBED_UI has been removed, it is now always on
#endif

#if defined(DAF_PLUGIN_HAS_EXTERNAL_UI)
# error DAF_PLUGIN_HAS_EXTERNAL_UI has been replaced by DAF_UI_USE_EXTERNAL
#endif

#ifdef DAF_UI_FILEBROWSER
# error typo detected use DAF_UI_FILE_BROWSER instead of DAF_UI_FILEBROWSER
#endif

#ifdef DAF_UI_WEBVIEW
# error typo detected use DAF_UI_WEB_VIEW instead of DAF_UI_WEBVIEW
#endif

#ifdef DAF_UI_USE_WEBVIEW
# error typo detected use DAF_UI_USE_WEB_VIEW instead of DAF_UI_USE_WEBVIEW
#endif

#if DAF_UI_FILE_BROWSER && !defined(DGL_USE_FILE_BROWSER)
# error invalid build config: file browser requested but `USE_FILE_BROWSER` build option is not set
#endif

#if DAF_UI_WEB_VIEW && !defined(DGL_USE_WEB_VIEW)
# error invalid build config: web view requested but `USE_WEB_VIEW` build option is not set
#endif

// --------------------------------------------------------------------------------------------------------------------
// Test if synth has audio outputs

#if DAF_PLUGIN_IS_SYNTH && DAF_PLUGIN_NUM_OUTPUTS == 0
# error Synths need audio output to work!
#endif

// --------------------------------------------------------------------------------------------------------------------
// Test if MIDI as MPE enabled where it doesn't make sense

#if DAF_PLUGIN_WANT_MIDI_AS_MPE && ! (DAF_PLUGIN_WANT_MIDI_INPUT || DAF_PLUGIN_WANT_MIDI_OUTPUT)
# error MIDI as MPE needs MIDI input or output to work!
#endif

// --------------------------------------------------------------------------------------------------------------------
// Enable MIDI input if synth, test if midi-input disabled when synth

#ifndef DAF_PLUGIN_WANT_MIDI_INPUT
# define DAF_PLUGIN_WANT_MIDI_INPUT DAF_PLUGIN_IS_SYNTH
#elif DAF_PLUGIN_IS_SYNTH && ! DAF_PLUGIN_WANT_MIDI_INPUT
# error Synths need MIDI input to work!
#endif

// --------------------------------------------------------------------------------------------------------------------
// Enable state if plugin wants state files (deprecated)

#ifdef DAF_PLUGIN_WANT_STATEFILES
# warning DAF_PLUGIN_WANT_STATEFILES is deprecated
# undef DAF_PLUGIN_WANT_STATEFILES
# if ! DAF_PLUGIN_WANT_STATE
#  undef DAF_PLUGIN_WANT_STATE
#  define DAF_PLUGIN_WANT_STATE 1
# endif
#endif

// --------------------------------------------------------------------------------------------------------------------
// Enable full state if plugin exports presets

#if DAF_PLUGIN_WANT_PROGRAMS && DAF_PLUGIN_WANT_STATE && defined(DAF_PLUGIN_WANT_FULL_STATE_WAS_NOT_SET)
# warning Plugins with programs and state should implement full state API too
# undef DAF_PLUGIN_WANT_FULL_STATE
# define DAF_PLUGIN_WANT_FULL_STATE 1
#endif

// --------------------------------------------------------------------------------------------------------------------
// Disable UI if DGL is not available

#if DAF_PLUGIN_HAS_UI && !defined(HAVE_DGL)
# undef DAF_PLUGIN_HAS_UI
# define DAF_PLUGIN_HAS_UI 0
#endif

// --------------------------------------------------------------------------------------------------------------------
// Make sure both default width and height are provided

#if defined(DAF_UI_DEFAULT_WIDTH) && !defined(DAF_UI_DEFAULT_HEIGHT)
# error DAF_UI_DEFAULT_WIDTH is defined but DAF_UI_DEFAULT_HEIGHT is not
#endif

#if defined(DAF_UI_DEFAULT_HEIGHT) && !defined(DAF_UI_DEFAULT_WIDTH)
# error DAF_UI_DEFAULT_HEIGHT is defined but DAF_UI_DEFAULT_WIDTH is not
#endif

// --------------------------------------------------------------------------------------------------------------------
// Define DAF_PLUGIN_AU_TYPE if needed

#ifndef DAF_PLUGIN_AU_TYPE
# if (DAF_PLUGIN_WANT_MIDI_INPUT || DAF_PLUGIN_WANT_MIDI_OUTPUT) && DAF_PLUGIN_NUM_INPUTS != 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
#  define DAF_PLUGIN_AU_TYPE aumf /* kAudioUnitType_MusicEffect */
# elif (DAF_PLUGIN_WANT_MIDI_INPUT || DAF_PLUGIN_WANT_MIDI_OUTPUT) && DAF_PLUGIN_NUM_INPUTS + DAF_PLUGIN_NUM_OUTPUTS != 0
#  define DAF_PLUGIN_AU_TYPE aumu /* kAudioUnitType_MusicDevice */
# elif DAF_PLUGIN_WANT_MIDI_INPUT || DAF_PLUGIN_WANT_MIDI_OUTPUT
#  define DAF_PLUGIN_AU_TYPE aumi /* kAudioUnitType_MIDIProcessor */
# elif DAF_PLUGIN_NUM_INPUTS == 0 && DAF_PLUGIN_NUM_OUTPUTS != 0
#  define DAF_PLUGIN_AU_TYPE augn /* kAudioUnitType_Generator */
# else
#  define DAF_PLUGIN_AU_TYPE aufx /* kAudioUnitType_Effect */
# endif
#endif

// --------------------------------------------------------------------------------------------------------------------
// Check that symbol macros are well defined

#ifdef DAF_PROPER_CPP11_SUPPORT

#ifdef DAF_PLUGIN_AU_TYPE
static_assert(sizeof(STRINGIFY(DAF_PLUGIN_AU_TYPE)) == 5, "The macro DAF_PLUGIN_AU_TYPE has incorrect length");
# if DAF_PLUGIN_NUM_INPUTS == 0 || DAF_PLUGIN_NUM_OUTPUTS == 0
static constexpr const char _aut[5] = STRINGIFY(DAF_PLUGIN_AU_TYPE);
static_assert(_aut[0] != 'a' || _aut[0] != 'u' || _aut[0] != 'm' || _aut[0] != 'u',
              "The 'aumu' type requires both audio input and output");
# endif
#endif

#ifdef DAF_PLUGIN_BRAND_ID
static_assert(sizeof(STRINGIFY(DAF_PLUGIN_BRAND_ID)) == 5, "The macro DAF_PLUGIN_BRAND_ID has incorrect length");
#endif

#ifdef DAF_PLUGIN_UNIQUE_ID
static_assert(sizeof(STRINGIFY(DAF_PLUGIN_UNIQUE_ID)) == 5, "The macro DAF_PLUGIN_UNIQUE_ID has incorrect length");
#endif

#endif

// --------------------------------------------------------------------------------------------------------------------
// Prevent users from messing about with DAF internals

#ifdef DAF_UI_IS_STANDALONE
# error DAF_UI_IS_STANDALONE must not be defined
#endif

#ifdef DAF_UI_LINUX_WEBVIEW_START
# error DAF_UI_LINUX_WEBVIEW_START must not be defined
#endif

// --------------------------------------------------------------------------------------------------------------------

#endif // DAF_PLUGIN_CHECKS_H_INCLUDED
