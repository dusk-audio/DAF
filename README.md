# DAF - Dusk Audio Framework
[![build](https://github.com/dusk-audio/DAF/actions/workflows/build.yml/badge.svg)](https://github.com/dusk-audio/DAF/actions/workflows/build.yml)
[![cmake](https://github.com/dusk-audio/DAF/actions/workflows/cmake.yml/badge.svg)](https://github.com/dusk-audio/DAF/actions/workflows/cmake.yml)
[![wayland](https://github.com/dusk-audio/DAF/actions/workflows/wayland.yml/badge.svg)](https://github.com/dusk-audio/DAF/actions/workflows/wayland.yml)

DAF is a C++ framework for audio plugins and audio applications. You write a plugin once, its DSP
against one plugin API and its interface against one windowing and graphics library, and the same
sources build as CLAP, VST3, VST2, LV2, Audio Unit, LADSPA and DSSI plugins and as a standalone
program, on Linux, macOS and Windows. The windowing and graphics library, DGL, also works on its
own, for application windows that are not plugins.

DAF is developed by [Dusk Audio](https://github.com/dusk-audio) and released under the ISC
licence. It started from the DISTRHO Plugin Framework and is developed independently; see
[Origins](#origins).


## Features

### Plugin formats

| Format | |
|---|---|
| CLAP | CLAP 1.2.10. Embedded editors on X11, macOS and Windows, floating editor windows on Wayland. |
| VST3 | Built on the in-tree, ISC-licensed travesty interface definitions; no Steinberg SDK is needed. |
| VST2 | Built on a clean-room, BSD-licensed interface. |
| LV2 | Embedded UIs, and floating UIs through `ui:showInterface` on Wayland. |
| Audio Unit | AU v2, on macOS. |
| LADSPA, DSSI | LADSPA without a UI, DSSI with an external one. |
| Standalone | JACK, or the system's audio through RtAudio (CoreAudio, WASAPI, ALSA, PulseAudio) or SDL2. |
| Static library | For a host that links the plugin into its own process and embeds its editor. |

The headers every format needs are in the tree, so there is no SDK to download. All of them are
under permissive terms except LADSPA's and DSSI's, which are LGPL; see [LICENSING.md](LICENSING.md).
[FEATURES.md](FEATURES.md) lists what each format supports.

### Plugin API

- **Parameters** with ranges, units, enumerations, groups, boolean, integer and logarithmic hints,
  output parameters for meters, triggers, and bypass designations. A plugin can supply its own
  value text in both directions (`getParameterValueText()`, `getParameterValueFromText()`);
  otherwise hosts show compact text with the unit, such as "0 dB" or "100 %". CLAP and VST3 hosts
  are never sent output parameters as edits, so meters stay out of their undo history, trigger
  parameters return to their default after each run in VST2, VST3 and CLAP, and a parameter value
  that is not finite, from the host or from saved state, never reaches the plugin.
- **State and programs.** Key/value string state saved with the host's session, full state the
  plugin changes on its own, and file-path state. A key marked `kStateIsRequired` makes a recall
  that lacks it fail instead of applying the rest, and `validateStateValue()` lets the plugin refuse
  a malformed value before anything has changed. `updateStateValue()` changes a state from the
  plugin side in VST2, VST3, CLAP and JACK, and `getCurrentProgram()` tells hosts which program is
  current after a recall.
- **Latency and tail.** `setLatency()`, and `setTail()` (with `DAF_PLUGIN_WANT_TAIL`) for reverbs,
  delays and anything else whose output outlasts its input: VST3, AU and CLAP hosts are told, so a
  bounce or an idle suspend does not cut the decay short. `kTailInfinite` is for a tail that never
  ends.
- **Audio ports**: port groups, sidechains, CV ports, and alternative main-bus layouts declared with
  `DAF_PLUGIN_EXTRA_IO`, which AU, CLAP and VST3 hosts can switch between and which reach the plugin
  through `ioChanged()`. With `DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO` an AU sidechain also runs narrower
  than declared, which Logic asks for on mono tracks.
- **MIDI** input and output, MPE in AU and CLAP, and notes sent from the UI.
- **Transport**: time position in the style of JACK transport, with `bpmValid` for a host tempo that
  comes without a complete bar and beat position.
- **DSP and UI** talk through parameters and state, which keeps them separable (DSSI, remote UIs),
  or share the plugin instance with `DAF_PLUGIN_WANT_DIRECT_ACCESS`.
- **VST3 class IDs that match across platforms**, opt-in with `DAF_VST3_CROSS_PLATFORM_UID` for new
  plugins, so a session saved on Windows finds the plugin on Linux and macOS.

### Windowing, graphics and UI

DGL provides windows, a widget tree, mouse, keyboard and scroll events, timers and idle callbacks,
the clipboard and file dialogs, on native windowing for X11, Wayland, macOS and Windows built on
[pugl](dgl/src/pugl-upstream/README.md).

- **Rendering**: OpenGL 3 core profile (the default on macOS), OpenGL 2 (the default elsewhere),
  OpenGL ES 2 and 3 (not on macOS) and Cairo, with NanoVG vector drawing on OpenGL. A UI can also
  be a web view, which on Linux needs X11 (XWayland included), or a window another toolkit or
  process draws.
- **Widgets**: Dear ImGui inside DGL widgets, and the Dusk widget kit of knobs, faders, meters,
  button banks and text fields drawn with Dear ImGui; see [widgets/README.md](widgets/README.md).
- **High-DPI**: windows know their scale factor and can scale their content automatically. Host
  scale changes reach the UI, including in AU hosts that resize the view or move it between a
  standard and a Retina display.
- **File drag and drop**: a top-level window can receive files dragged from other applications
  (`Window::setAcceptingFileDrops()`, then `onFileDragEnter()` to `onFileDrop()`) on X11, Wayland,
  macOS and Windows, and start a drag of a file into other applications (`Window::startFileDrag()`)
  on X11, macOS and Windows.
- **File dialogs**: native on macOS and Windows; xdg-desktop-portal on Linux, with a built-in
  dialog as the fallback on X11.
- **Keyboard focus in plugin editors**: keys an embedded editor does not use go to the host, so
  host shortcuts keep working. On Windows, CLAP and VST2 editors take the focus when clicked and
  hand it back; on X11, editors do not steal it, and their X errors stay out of the host's error
  handler. Widgets are told of focus changes (`Widget::onFocusChanged()`), and the pointer can be
  hidden for drag gestures (`kMouseCursorNone`).
- **Linux**: X11 and native Wayland, as separate builds or in one binary that picks at runtime; see
  [below](#linux-x11-wayland-or-both).

### Testing

DAF builds as C++11 or later, and CI builds it as C++11 and as C++20 with warnings as errors. Every
push that changes code runs Make builds on Linux and macOS, CMake builds on Linux (x86_64 and
arm64), macOS and Windows (MSVC), the unit and format-wrapper tests under Xvfb and a headless
Weston, and the example plugins through clap-validator, pluginval (VST3) and LV2 validation.


## Platforms

| Platform | Architectures | Compiler | Windowing |
|---|---|---|---|
| Linux | x86_64, arm64 | GCC or Clang | X11, Wayland, or both |
| macOS 11 and later | arm64 and x86_64, as universal binaries | Xcode | Cocoa |
| Windows | x64 | MSVC (Visual Studio 2022) | Win32 |

CI builds every row, with GCC on Linux, and Ubuntu 22.04 is the oldest Linux it builds on. The build
system still knows other targets, among them 32-bit systems, MinGW, Windows on Arm, the BSDs, Haiku
and WebAssembly, but CI does not build them.


## Getting started

### Requirements

- A C++11 compiler: GCC or Clang on Linux, Xcode on macOS, Visual Studio 2022 on Windows.
- CMake 3.15 or newer, or GNU Make. MSVC builds use CMake.
- On Linux, the development files for the windowing system. These are the Debian and Ubuntu
  packages CI installs:

  ```sh
  # X11 and OpenGL, and D-Bus for portal file dialogs
  sudo apt install libx11-dev libxcursor-dev libxext-dev libxrandr-dev libgl1-mesa-dev libdbus-1-dev
  # Wayland, for a machine without X11 or for DGL_BACKEND=wayland or both
  sudo apt install libwayland-dev libxkbcommon-dev libegl-dev
  # optional: Cairo UIs, standalone audio without JACK, and liblo for DSSI UIs
  sudo apt install libcairo2-dev libasound2-dev libpulse-dev libsdl2-dev liblo-dev
  ```

  JACK is loaded at runtime when it is installed, so it needs no development files.

### A first plugin

A project keeps DAF in a directory of its own, as a submodule, a subtree or a plain copy:

```
MyPlugins/
├── CMakeLists.txt
├── DAF/
└── plugins/Gain/
    ├── DafPluginInfo.h
    └── GainPlugin.cpp
```

`DafPluginInfo.h` describes the plugin. The URI, the CLAP ID, the four-character brand and plugin
IDs and `getUniqueId()` below are what hosts and saved sessions know the plugin by, so choose them
once and keep them. [daf/DafInfo.hpp](daf/DafInfo.hpp) documents every macro.

```c
#ifndef DAF_PLUGIN_INFO_H_INCLUDED
#define DAF_PLUGIN_INFO_H_INCLUDED

#define DAF_PLUGIN_BRAND     "Example"
#define DAF_PLUGIN_NAME      "Gain"
#define DAF_PLUGIN_URI       "https://example.com/plugins/gain"
#define DAF_PLUGIN_CLAP_ID   "com.example.gain"
#define DAF_PLUGIN_BRAND_ID  Exmp
#define DAF_PLUGIN_UNIQUE_ID xGan

#define DAF_PLUGIN_HAS_UI      0
#define DAF_PLUGIN_IS_RT_SAFE  1
#define DAF_PLUGIN_NUM_INPUTS  2
#define DAF_PLUGIN_NUM_OUTPUTS 2

#endif
```

`GainPlugin.cpp` is the plugin itself:

```cpp
#include "DafPlugin.hpp"

#include <cmath>

START_NAMESPACE_DAF

class GainPlugin : public Plugin
{
public:
    GainPlugin() : Plugin(1, 0, 0) {} // 1 parameter, 0 programs, 0 states

protected:
    const char* getLabel() const override { return "Gain"; }
    const char* getMaker() const override { return "Example"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }
    int64_t getUniqueId() const override { return d_cconst('x', 'G', 'a', 'n'); }

    void initParameter(uint32_t, Parameter& parameter) override
    {
        parameter.hints = kParameterIsAutomatable;
        parameter.name = "Gain";
        parameter.symbol = "gain";
        parameter.unit = "dB";
        parameter.ranges.def = 0.0f;
        parameter.ranges.min = -60.0f;
        parameter.ranges.max = 12.0f;
    }

    float getParameterValue(uint32_t) const override { return fGainDb; }

    void setParameterValue(uint32_t, float value) override
    {
        fGainDb = value;
        fGain = std::pow(10.0f, value / 20.0f);
    }

    void run(const float** inputs, float** outputs, uint32_t frames) override
    {
        for (uint32_t ch = 0; ch < DAF_PLUGIN_NUM_OUTPUTS; ++ch)
            for (uint32_t i = 0; i < frames; ++i)
                outputs[ch][i] = inputs[ch][i] * fGain;
    }

private:
    float fGainDb = 0.0f;
    float fGain = 1.0f;

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GainPlugin)
};

Plugin* createPlugin()
{
    return new GainPlugin();
}

END_NAMESPACE_DAF
```

With CMake, `CMakeLists.txt` adds DAF and declares the plugin:

```cmake
cmake_minimum_required(VERSION 3.15)
project(MyPlugins)

add_subdirectory(DAF)

daf_add_plugin(d_gain
  TARGETS lv2 vst3 clap
  FILES_DSP
      plugins/Gain/GainPlugin.cpp)

target_include_directories(d_gain PUBLIC plugins/Gain)
```

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

The plugins land in `build/bin` as `d_gain.clap`, `d_gain.lv2/` and `d_gain.vst3/`. Add `au` to
`TARGETS` on macOS, `vst2`, `ladspa` or `dssi` for those formats, and `jack` for a standalone
program. A plugin with an interface lists its UI sources under `FILES_UI`, sets
`DAF_PLUGIN_HAS_UI` to 1 and subclasses `UI` from [daf/DafUI.hpp](daf/DafUI.hpp).

With Make, the plugin directory gets a `Makefile` instead:

```make
NAME = d_gain
FILES_DSP = GainPlugin.cpp

include ../../DAF/Makefile.plugins.mk

TARGETS += lv2_dsp vst3 clap

all: $(TARGETS)
```

`make -C plugins/Gain` writes the plugins to `bin/` at the top of the project. LV2 bundles also need
their Turtle files, generated from the built binaries; run this from the top of the project:

```sh
make -C DAF/utils/lv2-ttl-generator
DAF/utils/generate-ttl.sh
```

### A first application

DGL works without a plugin around it. `daf_add_executable()` creates a program target with the UI
type you choose:

```cmake
daf_add_executable(hello UI_TYPE opengl3)
target_sources(hello PRIVATE apps/Hello/HelloApp.cpp)
```

```cpp
#include "Application.hpp"
#include "OpenGL.hpp"
#include "StandaloneWindow.hpp"

START_NAMESPACE_DGL

class HelloWindow : public StandaloneWindow
{
public:
    explicit HelloWindow(Application& app)
        : StandaloneWindow(app)
    {
        setTitle("Hello DAF");
        setSize(480, 320);
        setResizable(true);
    }

protected:
    void onDisplay() override
    {
        glClearColor(0.1f, 0.1f, 0.12f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
};

END_NAMESPACE_DGL

int main()
{
    Application app;
    HelloWindow window(app);
    window.show();
    app.exec();
    return 0;
}
```

Built with `DGL_BACKEND=both` on Linux, the same binary opens on Wayland where a compositor is
running and on X11 everywhere else.

### Examples

[examples/](examples/) has a plugin for each part of the API:

| Example | Shows |
|---|---|
| [Parameters](examples/Parameters) | Parameters |
| [States](examples/States) | State |
| [FileHandling](examples/FileHandling) | File-path state |
| [Info](examples/Info) | Host and transport information, shown in the UI |
| [Latency](examples/Latency) | Reporting latency |
| [Meters](examples/Meters) | Output parameters as meters |
| [WebMeters](examples/WebMeters) | The same meters in a web view UI |
| [Metronome](examples/Metronome) | Tempo sync |
| [MidiThrough](examples/MidiThrough) | MIDI input and output |
| [SendNote](examples/SendNote) | MIDI notes sent from the UI |
| [CVPort](examples/CVPort) | Audio ports used as CV |
| [CairoUI](examples/CairoUI) | A UI drawn with Cairo |
| [ExternalUI](examples/ExternalUI) | A UI running outside the plugin, in another process or on another machine |
| [EmbedExternalUI](examples/EmbedExternalUI) | A window from outside DAF, embedded in the host's editor |

At the top of this repository, `make` builds DGL, the widget gallery and every example, and
`make -C tests run` runs the tests, which need a display. A top-level CMake build does the same,
apart from the tests.

### Build options

| | Make | CMake | Default |
|---|---|---|---|
| Linux windowing | `DGL_BACKEND` | `-DDGL_BACKEND` | `auto`; also `x11`, `wayland`, `both` |
| UI type | `UI_TYPE` | `UI_TYPE` argument of `daf_add_plugin()` and `daf_add_executable()` | `opengl`, and `opengl3` on macOS; also `gles2`, `gles3`, `cairo`, `external`, `webview` |
| File dialog and web view APIs | `USE_FILE_BROWSER=true`, `USE_WEB_VIEW=true` | `USE_FILE_BROWSER`, `USE_WEB_VIEW` arguments | off |
| Standalone audio | `FORCE_NATIVE_AUDIO_FALLBACK=true` (native only), `SKIP_NATIVE_AUDIO_FALLBACK=true` (JACK only) | arguments of the same names | JACK when it runs, otherwise native audio |
| macOS deployment target | `MACOSX_DEPLOYMENT_TARGET` | `CMAKE_OSX_DEPLOYMENT_TARGET` | `11.0` |
| Universal macOS binaries | `make macos-universal-11.0` | `'-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64'` | |
| Separate build directories per configuration | `BUILD_DIR_SUFFIX` | a separate build directory | |
| What DAF builds itself | | `DAF_LIBRARIES`, `DAF_EXAMPLES`, `DAF_WIDGETS_GALLERY` | on when DAF is the top-level project |

`make features` prints what the Make build found on the system.

Make builds compile with `-ffast-math`, which lets the compiler assume no value is ever infinite or
NaN: GCC and Clang then fold `std::isfinite`, `std::isnan` and `std::isinf` to a constant, and the
check disappears. Test values that come from outside your code with `d_isFinite()` from
[daf/DafUtils.hpp](daf/DafUtils.hpp) instead.


## Linux: X11, Wayland or both

DGL on Linux is built for one windowing system unless asked otherwise: X11 whenever the X11
development files are installed, native Wayland where they are not. `DGL_BACKEND` chooses
explicitly:

| `DGL_BACKEND` | Builds |
|---|---|
| `auto` (default) | X11 if its development files are installed, otherwise Wayland |
| `x11` | X11 only |
| `wayland` | Wayland only, also on a machine with X11 installed |
| `both` | X11 and Wayland in one binary, which picks one when it opens its first window |

A `both` binary needs both sets of development files to build, but does not link the Wayland
libraries: it loads them the first time it tries Wayland, so it starts and runs on X11 on a machine
without them. A standalone program prefers a running Wayland compositor and falls back to X11;
`DAF_WINDOW_BACKEND=x11` or `DAF_WINDOW_BACKEND=wayland` forces one, and an `Application` can ask for
one itself with `kWindowSystemX11` or `kWindowSystemWayland`. A plugin UI prefers X11, since hosts
embed editors through X11 windows, under XWayland too.

Wayland cannot embed one process's window in another's, which decides what plugins can do there:

- CLAP offers floating Wayland windows (`CLAP_WINDOW_API_WAYLAND`): as its only window API in a
  Wayland build, and next to X11, which stays preferred, in a `both` build.
- LV2 declares the generic `ui:UI` class in a Wayland build, so conforming hosts open the UI as a
  floating window through `ui:showInterface`.
- VST2 and VST3 can only embed. A Wayland build has no editor for them, and in a `both` build they
  refuse to embed where no X server is available, rather than hand the host a Wayland surface.

[Wayland support](FEATURES.md#wayland-support) in FEATURES.md has the full picture, and
[dgl/src/runtime-backend.md](dgl/src/runtime-backend.md) describes how the `both` build works. When
switching one checkout between backends, give each its own `BUILD_DIR_SUFFIX` and remove the
affected `bin/` bundles, otherwise objects and binaries from the other backend are silently reused.


## Documentation

- **API reference.** The headers are documented in place: [daf/DafPlugin.hpp](daf/DafPlugin.hpp)
  for the `Plugin` class, [daf/DafUI.hpp](daf/DafUI.hpp) for `UI`, [daf/DafInfo.hpp](daf/DafInfo.hpp)
  for every `DafPluginInfo.h` macro, [daf/DafDetails.hpp](daf/DafDetails.hpp) for parameters, ports,
  state and time position, and [dgl/](dgl/) for windows, widgets and drawing. `doxygen daf.doxygen`
  renders them as HTML into `docs/`, and a rendered copy is published at
  [duskaudio.com/DAF](https://duskaudio.com/DAF/).
- [FEATURES.md](FEATURES.md): what each plugin format supports, and why where it does not.
- [dgl/src/runtime-backend.md](dgl/src/runtime-backend.md): choosing X11 or Wayland at runtime.
- [dgl/src/drop-targets.md](dgl/src/drop-targets.md): receiving file drops.
- [widgets/README.md](widgets/README.md): the Dear ImGui bridge and the Dusk widget kit.
- [LICENSING.md](LICENSING.md): the licences of the format headers and bundled libraries.


## Repository layout

| Path | Contents |
|------|----------|
| `daf/` | Plugin API headers |
| `daf/src/` | Format wrappers, and the format headers they build on |
| `daf/extra/` | Utilities: strings, threads, mutexes, ring buffers, value smoothing, file dialogs, web views |
| `dgl/` | DGL: windows, widgets, events and drawing |
| `dgl/src/pugl-upstream/` | pugl, the native windowing layer under DGL |
| `dgl/src/pugl-extra/` | pugl backends added in this tree: Wayland, with its protocol bindings, Haiku and WebAssembly |
| `widgets/` | Dear ImGui bridge, Dusk widget kit, resize handle and widget gallery |
| `cmake/` | `daf_add_plugin()` and `daf_add_executable()` |
| `examples/` | Example plugins |
| `tests/` | Unit, format-wrapper and integration tests |
| `utils/` | LV2 Turtle generation, bundling and packaging scripts |

pugl and the widget kit are ordinary directories of this repository, not submodules, so one DAF
revision pins the whole framework. Their former repositories, `dusk-audio/pugl` and
`dusk-audio/DAF-Widgets`, are archived and keep their earlier history; development continues here.


## Releases and support

Releases are tagged and published on the
[releases page](https://github.com/dusk-audio/DAF/releases). Between releases `main` moves on, and
CI runs on every push to it. Projects pin one tag or commit.

Bug reports and questions belong on the
[issue tracker](https://github.com/dusk-audio/DAF/issues), for every part of the tree, including
code that came from DPF: it is maintained here.


## Origins

DAF started from the excellent [DISTRHO Plugin Framework (DPF)](https://github.com/DISTRHO/DPF) by
Filipe Coelho and contributors, taken at `4238e1c7` (2025-10-23). Its foundation is their work.
[Dusk Audio](https://github.com/dusk-audio) took the code to move faster on platform work than an
upstream contribution cycle allows, and develops it as its own framework.

Development is independent in both directions: nothing here is submitted to DISTRHO, and no DISTRHO
commit is merged in. This is a standalone repository, not a GitHub fork. It was renamed from DPF to
DAF in August 2026, because two repositories with the same name, one upstream and one ours, could
send people and tooling to the wrong tree. The widget kit likewise started from DISTRHO's
DPF-Widgets, and DGL's windowing rests on David Robillard's pugl.

### Coming from DPF

Porting a DPF plugin is a mechanical rename:

| DPF | DAF |
|---|---|
| `DISTRHO_*` macros | `DAF_*` |
| `START_NAMESPACE_DISTRHO`, `END_NAMESPACE_DISTRHO` | `START_NAMESPACE_DAF`, `END_NAMESPACE_DAF` |
| `distrho/DistrhoPlugin.hpp`, `distrho/DistrhoUI.hpp` | `daf/DafPlugin.hpp`, `daf/DafUI.hpp` |
| `DistrhoPluginInfo.h` | `DafPluginInfo.h` |
| `dpf_add_plugin()` | `daf_add_plugin()` |

DGL's names are unchanged. A ported plugin keeps its identity: the URI, CLAP ID and AU codes come
from `DafPluginInfo.h` as before, and VST3 class IDs are still hashed with `'D','P','F',' '`, so a
VST3 plugin already shipped from DPF keeps its class ID in every saved session.

Sessions saved with a DPF build still load. DAF reads the VST3 and CLAP state DPF wrote, with its
`__dpf_` markers, LV2 state stored under `urn:distrho:`, and a bypass parameter saved as
`dpf_bypass`, and writes the DAF forms back. The one identifier hosts see change is the LV2 symbol
of a reset-designated port, now `daf_reset`, which only ever carries a trigger.

Behaviour worth checking after a port:

- macOS UIs default to `UI_TYPE=opengl3`. A UI that draws with legacy OpenGL 2 sets
  `UI_TYPE=opengl` explicitly; `gles2` and `gles3` are refused on macOS.
- C++11 is the minimum language version.
- VST3 `setProcessing(false)` pauses processing instead of calling `deactivate()`, so DSP state, a
  reverb tail for one, survives a transport stop.
- Output parameters are no longer reported to CLAP and VST3 hosts as parameter edits.


## Licence

DAF is released under the ISC licence, the same terms as DPF: it may be used, copied, modified and
distributed for any purpose, with or without fee, as long as the copyright and permission notices
are kept. Some plugin formats bring headers under their own terms; [LICENSING.md](LICENSING.md)
lists them.

DAF is a derivative work of [DISTRHO/DPF](https://github.com/DISTRHO/DPF), used and modified under
those ISC terms, and the original copyright notices are retained in every file that carries them.
