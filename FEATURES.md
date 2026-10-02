# DPF - Dusk Audio Framework

This file describes the available features for each plugin format.  
The limitations could be due to the plugin format itself or within DPF.  
If the limitation is within DPF, a link is provided to a description below on the reason for it.

| Feature             | JACK/Standalone                       | LADSPA             | DSSI                | LV2                           | VST2                       | VST3                       | CLAP                       | AU                         |  Feature            |
|---------------------|---------------------------------------|--------------------|---------------------|-------------------------------|----------------------------|----------------------------|----------------------------|----------------------------|---------------------|
| Audio port groups   | [Yes*](#jack-audio-port-groups)       | No                 | No                  | Yes                           | No                         | Yes                        | Yes                        | [No*](#work-in-progress)   | Audio port groups   |
| Audio port as CV    | Yes                                   | No                 | No                  | Yes                           | No                         | [Yes*](#vst3-cv)           | [No*](#work-in-progress)   | No                         | Audio port as CV    |
| Audio sidechan      | Yes                                   | No                 | No                  | Yes                           | [No*](#vst2-deprecated)    | Yes                        | Yes                        | [No*](#work-in-progress)   | Audio sidechan      |
| Bypass control      | No                                    | No                 | No                  | Yes                           | [No*](#vst2-deprecated)    | Yes                        | Yes                        | Yes                        | Bypass control      |
| MIDI input          | Yes                                   | No                 | Yes                 | Yes                           | Yes                        | Yes                        | Yes                        | Yes                        | MIDI input          |
| MIDI output         | Yes                                   | No                 | No                  | Yes                           | Yes                        | Yes                        | Yes                        | Yes                        | MIDI output         |
| Parameter changes   | Yes                                   | No                 | No                  | [No*](#lv2-parameter-changes) | Yes                        | Yes                        | Yes                        | Yes                        | Parameter changes   |
| Parameter groups    | No                                    | No                 | No                  | Yes                           | Yes                        | [No*](#work-in-progress)   | Yes                        | [No*](#work-in-progress)   | Parameter groups    |
| Parameter outputs   | No                                    | No                 | No                  | Yes                           | No                         | Yes                        | Yes                        | Yes                        | Parameter outputs   |
| Parameter triggers  | Yes                                   | No                 | No                  | Yes                           | [No*](#parameter-triggers) | [No*](#parameter-triggers) | [No*](#parameter-triggers) | [No*](#parameter-triggers) | Parameter triggers  |
| Programs            | [Yes*](#jack-parameters-and-programs) | [No*](#ladspa-rdf) | [Yes*](#dssi-state) | Yes                           | [No*](#vst2-programs)      | Yes                        | No                         | Yes                        | Programs            |
| States              | Yes                                   | No                 | [Yes*](#dssi-state) | Yes                           | Yes                        | Yes                        | Yes                        | Yes                        | States              |
| Full/internal state | Yes                                   | No                 | No                  | Yes                           | Yes                        | Yes                        | Yes                        | Yes                        | Full/internal state |
| Time position       | Yes                                   | No                 | No                  | Yes                           | Yes                        | Yes                        | Yes                        | Yes                        | Time position       |
| UI                  | [Yes*](#jack-custom-ui-only)          | No                 | External only       | Yes                           | Embed only                 | Embed only                 | Yes                        | Embed only                 | UI                  |
| UI bg/fg colors     | No                                    | No                 | No                  | Yes                           | No                         | No?                        | No                         | No                         | UI bg/fg colors     |
| UI direct access    | Yes                                   | No                 | No                  | Yes                           | Yes                        | Yes                        | Yes                        | Yes                        | UI direct access    |
| UI host-filebrowser | No                                    | No                 | No                  | Yes                           | [No*](#vst2-deprecated)    | [No*](#work-in-progress)   | [No*](#work-in-progress)   | No                         | UI host-filebrowser |
| UI host-resize      | Yes                                   | No                 | Yes                 | Yes                           | No                         | Yes                        | Yes                        | [Yes*](#au-host-resize)    | UI host-resize      |
| UI remote control   | No                                    | No                 | Yes                 | Yes                           | No                         | Yes                        | No                         | Yes                        | UI remote control   |
| UI send midi note   | Yes                                   | No                 | Yes                 | Yes                           | Yes                        | Yes                        | Yes                        | Yes                        | UI send midi note   |

For things that could be unclear:

- "States" refers to DPF API support, supporting key-value string pairs for internal state saving
- "Full state" refers to plugins updating their state internally without outside intervention (like host or UI)
- "UI direct access" means `DAF_PLUGIN_WANT_DIRECT_ACCESS` is possible, that is, running DSP and UI on the same process
- "UI remote control" means running the UI on a separate machine (for example over the network)
- An external UI on this table means that it cannot be embed into the host window, but the plugin can still provide one

# Special notes

## Parameter triggers

Trigger-style parameters (parameters which value is reset to its default every run) are only supported in JACK and LV2.  
For all other plugin formats DPF will simulate the behaviour through a parameter change request.

## JACK audio port groups

DPF will set JACK metadata information for grouping audio ports.  
This is not supported by most JACK applications at the moment.

## JACK parameters and programs

Under JACK/Stanlone mode, MIDI input events will trigger program and parameter changes.  
MIDI program change events work as expected (that is, MIDI program change 0 will load 1st plugin program).  
MIDI CCs are used for parameter changes (matching the `midiCC` value you set on each parameter).

## JACK custom UI only

There is no generic plugin editor view.  
If your plugin has no custom UI, the standalone executable will run but not show any window.

## LADSPA RDF

Programs for LADSPA could be done via LRDF but this is not supported in DPF.

## DSSI State

DSSI only supports state changes when called via UI, no "full state" possible.  
This also makes it impossibe to use programs and state at the same time with DSSI,
because in DPF changing programs can lead to state changes but there is no way to fetch this information on DSSI plugins.

To make it simpler to understand, think of DSSI programs and states as UI properties.  
Because in DPF changing the state happens from UI to DSP side, regular DSSI can be supported.  
But if we involve programs, they would need to pass through the UI in order to work. Which goes against DPF's design.

## LV2 parameter changes

Although this is already implemented in DPF (through a custom extension), this is not implemented on most hosts.  
So for now you can pretty much treat it as if not supported.

## VST2 deprecated

Not supported in DPF at the moment.  
It could eventually be, but likely not due to VST2 being phased out by Steinberg.  
Contact DPF authors if you require such a feature.

## VST2 programs

VST2 program support requires saving state of all programs in memory, which is very expensive and thus not done in DPF.

## VST3 CV

Although VST3 officially supports CV (Control Voltage) tagged audio ports,
at the moment no host supports such feature and thus it is not possible to validate it.

## VST3 class IDs across platforms

By default a DAF VST3 has a different class ID string on Windows than on Linux and macOS.
DAF writes the same 16 class ID bytes everywhere, but on Windows the VST3 SDK reads the first 8 of
them as the fields of a COM GUID, which byte-swaps them in the string. A project saved on Windows
then cannot find the plugin on Linux or macOS, and vice versa. CLAP, VST2 and AU are not affected.

Defining `DAF_VST3_CROSS_PLATFORM_UID` in `DafPluginInfo.h` lays the Windows class IDs out so that
their string matches the one on Linux and macOS. It is off by default, and recommended for new
plugins only:

> **Warning:** turning `DAF_VST3_CROSS_PLATFORM_UID` on changes the Windows class IDs of a plugin
> that has already shipped. Every existing Windows session using it stops finding the plugin.
> Linux and macOS class IDs are the same with or without it.

## CLAP output parameters

CLAP has no output parameters. DAF exposes them as `CLAP_PARAM_IS_READONLY` parameters whose value
the plugin changes on its own: a host reads them with `clap_plugin_params::get_value()` and never
sets them. Parameter events outside `CLAP_CORE_EVENT_SPACE_ID`, and any event for an output
parameter, are ignored in `process()` and `flush()` alike.

clap-validator 0.4 does not leave read-only parameters out of its parameter tests. For a plugin whose
outputs move while it runs (meters, host and transport information) `param-set-wrong-namespace`
therefore fails: it sends events in a foreign namespace, processes, and sees the outputs changed.
This is expected and not a bug in the plugin or the wrapper. What a plugin should do is give each
output, like each input, a default equal to the value it really starts with, so that
`param-default-values` passes.

## AU host-resize

AU v2 has no size negotiation of its own: the host simply resizes the view the plugin handed it,
and there is no way for the plugin to refuse a size or to ask for one other than by resizing itself.
DPF follows those host-driven resizes, and offers the plugin window resize affordance when the
plugin asked to be user-resizable, but the host has the final say on the size.

## Wayland support

DGL links against a single windowing backend chosen at build time.
X11 is used whenever the X11 development files are present; the native Wayland backend is selected
only on a machine without them (`HAVE_X11` unset, `HAVE_WAYLAND` set).
There is no runtime switch, and a dual-stack machine always builds X11.

What a Wayland build gives you:

| | Wayland build | X11 build on a Wayland session |
|---|---|---|
| JACK/Standalone | Native, plugin window is an xdg toplevel | Via XWayland |
| CLAP            | Native, floating window only (see below)  | Via XWayland, embedded |
| LV2             | Native, floating window via `ui:showInterface` (see below) | Via XWayland, embedded |
| VST2/VST3/AU    | Not usable, these formats can only embed  | Via XWayland, embedded |
| File dialogs    | xdg-desktop-portal only, no libsofd fallback | Portal, else libsofd |
| Web view UIs    | Not available                             | Available |

CLAP is the only format DPF targets that has a Wayland window API at all, and even there the CLAP
specification states that embedding is not supported for it: hosts must use floating windows.
A Wayland build therefore advertises `CLAP_WINDOW_API_WAYLAND` with `is_floating = true` and refuses
embedded creation, while an X11 build keeps advertising `CLAP_WINDOW_API_X11` exactly as before.

LV2 has no Wayland UI class, but it does have a host-agnostic escape hatch. A Wayland build declares
the generic `ui:UI` class instead of `ui:X11UI`, which is the case in which conforming hosts (Ardour,
jalv) fall back to `ui:showInterface` plus `ui:idleInterface` — both of which DPF already declares and
implements through the same floating-window path CLAP uses. An X11 build keeps declaring `ui:X11UI`.
Hosts that only know how to embed will not show the UI at all, which is unavoidable: Wayland has no
cross-process window reparenting.

VST2, VST3 and AU are embed-only with no equivalent fallback, so on a Wayland build those have no way
to show a UI.

Two known limitations of the Wayland backend, both inherent to the protocol rather than to DPF:

- File dialogs open centred on screen rather than over the plugin window. The portal wants a parent
  handle exported through xdg-foreign, which the current file-browser API has no way to carry.
- `clap_plugin_gui->set_transient()` only takes effect when host and plugin share a process. A
  `wl_surface` cannot cross a process boundary, and CLAP has no way to pass an xdg-foreign handle.

## Work in progress

Feature is possible, just not implemented yet in DPF.

## Widgets

The in-tree [widget kit](widgets/README.md) provides the Dear ImGui DGL bridge
(`widgets/imgui`), the ImGui-only Dusk console kit (`widgets/dusk`), and the generic
ResizeHandle. CMake consumers use `daf__add_widgets_dusk()` and
`daf__add_widgets_imgui(dgl-opengl3)` after creating the DGL target. The resulting
`daf-widgets-dusk` target has no DGL dependency; `daf-widgets-imgui` supplies ImGui
and its renderer. `DAF_WIDGETS_GALLERY` builds the standalone gallery in CMake;
`make -C widgets/tests` builds the gallery and ImGui demo without launching them.
