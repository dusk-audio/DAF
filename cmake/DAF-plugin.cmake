# DISTRHO Plugin Framework (DPF)
# Copyright (C) 2021 Jean Pierre Cimalando <jp-dev@inbox.ru>
# Copyright (C) 2022-2024 Filipe Coelho <falktx@falktx.com>
#
# SPDX-License-Identifier: ISC

# ------------------------------------------------------------------------------
# CMake support module for the Dusk Audio Framework
#
# The purpose of this module is to help building music plugins easily, when the
# project uses CMake as its build system.
#
# In order to use the helpers provided by this module, a plugin author should
# add DAF as a subproject, making the function `daf_add_plugin` available.
# The usage of this function is documented below in greater detail.
#
# Example project `CMakeLists.txt`:
#
# ```
# cmake_minimum_required(VERSION 3.7)
# project(MyPlugin)
#
# add_subdirectory(DAF)
#
# daf_add_plugin(MyPlugin
#   TARGETS clap lv2 vst2 vst3
#   FILES_DSP
#       src/MyPlugin.cpp
#   FILES_UI
#       src/MyUI.cpp)
#
# target_include_directories(MyPlugin
#   PUBLIC src)
# ```
#
# Important: note that properties, such as include directories, definitions,
# and linked libraries *must* be marked with `PUBLIC` so they take effect and
# propagate into all the plugin targets.

include(CMakeParseArguments)

# ------------------------------------------------------------------------------
# DAF public functions
# ------------------------------------------------------------------------------

# daf_add_plugin(name <args...>)
# ------------------------------------------------------------------------------
#
# Add a plugin built using the Dusk Audio Framework.
#
# ------------------------------------------------------------------------------
# Created targets:
#
#   `<name>`
#       static library: the common part of the plugin
#       The public properties set on this target apply to both DSP and UI.
#
#   `<name>-dsp`
#       static library: the DSP part of the plugin
#       The public properties set on this target apply to the DSP only.
#
#   `<name>-ui`
#       static library: the UI part of the plugin
#       The public properties set on this target apply to the UI only.
#
#   `<name>-<target>` for each target specified with the `TARGETS` argument.
#       This is target-dependent and not intended for public use.
#
# ------------------------------------------------------------------------------
# Arguments:
#
#   `TARGETS` <tgt1>...<tgtN>
#       a list of one of more of the following target types:
#       `jack`, `ladspa`, `dssi`, `lv2`, `vst2`, `vst3`, `clap`
#
#   `UI_TYPE` <type>
#       the user interface type, can be one of the following:
#          - cairo
#          - external
#          - gles2 (not available on macOS)
#          - gles3 (not available on macOS)
#          - opengl (default everywhere except macOS)
#          - opengl3 (default on macOS)
#          - vulkan
#          - webview
#
#   `FILES_COMMON` <file1>...<fileN>
#       list of sources which are part of both DSP and UI
#
#   `FILES_DSP` <file1>...<fileN>
#       list of sources which are part of the DSP
#
#   `FILES_UI` <file1>...<fileN>
#       list of sources which are part of the UI
#       empty indicates the plugin does not have UI
#
#   `MODGUI_CLASS_NAME`
#       class name to use for modgui builds
#
#   `MONOLITHIC`
#       build LV2 as a single binary for UI and DSP
#
#   `NO_SHARED_RESOURCES`
#       do not build DAF shared resources (fonts, etc)
#
#   `FORCE_NATIVE_AUDIO_FALLBACK`
#       force the JACK/Standalone format to use native audio fallback instead of JACK
#
#   `SKIP_NATIVE_AUDIO_FALLBACK`
#       force the JACK/Standalone format to always use JACK, skipping native audio fallback
#
#   `USE_FILE_BROWSER`
#       enable file browser dialog APIs
#
#   `USE_WEB_VIEW`
#       enable web browser view APIs
#
function(daf_add_plugin NAME)
  set(options MONOLITHIC NO_SHARED_RESOURCES FORCE_NATIVE_AUDIO_FALLBACK SKIP_NATIVE_AUDIO_FALLBACK USE_FILE_BROWSER USE_WEB_VIEW)
  set(oneValueArgs MODGUI_CLASS_NAME UI_TYPE)
  set(multiValueArgs FILES_COMMON FILES_DSP FILES_UI TARGETS)
  cmake_parse_arguments(_daf_plugin "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # macOS defaults to opengl3, every other platform keeps opengl. Mirrors the same default in
  # Makefile.plugins.mk.
  #
  # WHY: UI_TYPE=opengl builds dgl/src/OpenGL2.cpp, which draws with glBegin/glEnd immediate mode, and makes
  # dgl/src/pugl.cpp ask for a *compatibility* profile at version 2, which mac_gl.m turns into
  # NSOpenGLProfileVersionLegacy -- the frozen OpenGL 2.1 context. Immediate mode exists in no replacement
  # Apple has shipped or is likely to: ANGLE, Metal and every other GL-exit path speaks core profile only.
  # opengl3 builds dgl/src/OpenGL3.cpp (shaders + VBOs, NANOVG_GL3) against NSOpenGLProfileVersion3_2Core,
  # which is the only renderer with a future on macOS. An explicit UI_TYPE still wins.
  if("${_daf_plugin_UI_TYPE}" STREQUAL "")
    if(APPLE)
      set(_daf_plugin_UI_TYPE "opengl3")
    else()
      set(_daf_plugin_UI_TYPE "opengl")
    endif()
  endif()

  # There is no OpenGL ES on macOS; dgl/OpenGL-include.hpp errors out on DGL_USE_GLES for DAF_OS_MAC.
  # Caught here so the failure names the fix instead of surfacing as a preprocessor error inside DGL.
  if(APPLE AND (_daf_plugin_UI_TYPE STREQUAL "gles2" OR _daf_plugin_UI_TYPE STREQUAL "gles3"))
    message(FATAL_ERROR "UI_TYPE ${_daf_plugin_UI_TYPE} is not supported on macOS, use opengl3 instead")
  endif()

  # Plain booleans, not the $<BOOL:...> generator expressions these used to pass: the
  # daf__add_dgl_* functions test their arguments with if(), which sees any generator expression
  # as a non-empty string and therefore always took the "enabled" branch. That silently ignored
  # NO_SHARED_RESOURCES and turned the file browser and the web view on for every plugin, WebKit
  # linkage included. All three options are fixed at configure time, so real booleans are all
  # they need; the functions that do use these in a generator-expression context ($<BOOL:${...}>)
  # are equally happy with TRUE/FALSE.
  if(_daf_plugin_NO_SHARED_RESOURCES)
    set(_daf_shared_resources FALSE)
  else()
    set(_daf_shared_resources TRUE)
  endif()

  if(_daf_plugin_USE_FILE_BROWSER)
    set(_daf_file_browser TRUE)
  else()
    set(_daf_file_browser FALSE)
  endif()

  if(_daf_plugin_USE_WEB_VIEW)
    set(_daf_web_view TRUE)
  else()
    set(_daf_web_view FALSE)
  endif()

  set(_dgl_library)
  if(_daf_plugin_FILES_UI)
    if(_daf_plugin_UI_TYPE STREQUAL "cairo")
      daf__add_dgl_cairo(${_daf_shared_resources}
                         ${_daf_file_browser}
                         ${_daf_web_view})
      set(_dgl_library dgl-cairo)
    elseif(_daf_plugin_UI_TYPE STREQUAL "external")
      daf__add_dgl_external(${_daf_file_browser}
                            ${_daf_web_view})
      set(_dgl_library dgl-external)
    elseif(_daf_plugin_UI_TYPE STREQUAL "gles2")
      daf__add_dgl_gles2(${_daf_shared_resources}
                         ${_daf_file_browser}
                         ${_daf_web_view})
      set(_dgl_library dgl-gles2)
    elseif(_daf_plugin_UI_TYPE STREQUAL "gles3")
      daf__add_dgl_gles3(${_daf_shared_resources}
                         ${_daf_file_browser}
                         ${_daf_web_view})
      set(_dgl_library dgl-gles3)
    elseif(_daf_plugin_UI_TYPE STREQUAL "opengl")
      daf__add_dgl_opengl(${_daf_shared_resources}
                          ${_daf_file_browser}
                          ${_daf_web_view})
      set(_dgl_library dgl-opengl)
    elseif(_daf_plugin_UI_TYPE STREQUAL "opengl3")
      daf__add_dgl_opengl3(${_daf_shared_resources}
                           ${_daf_file_browser}
                           ${_daf_web_view})
      set(_dgl_library dgl-opengl3)
    elseif(_daf_plugin_UI_TYPE STREQUAL "vulkan")
      daf__add_dgl_vulkan(${_daf_shared_resources}
                          ${_daf_file_browser}
                          ${_daf_web_view})
      set(_dgl_library dgl-vulkan)
    elseif(_daf_plugin_UI_TYPE STREQUAL "webview")
      set(_daf_plugin_USE_WEB_VIEW TRUE)
      set(_daf_web_view TRUE)
      daf__add_dgl_external(${_daf_file_browser}
                            ${_daf_web_view})
      set(_dgl_library dgl-external)
    else()
      message(FATAL_ERROR "Unrecognized UI type for plugin: ${_daf_plugin_UI_TYPE}")
    endif()
  else()
    set(_daf_plugin_UI_TYPE "")
  endif()

  set(_dgl_has_ui OFF)
  if(_dgl_library)
    set(_dgl_has_ui ON)
  endif()

  ###
  daf__ensure_sources_non_empty(_daf_plugin_FILES_COMMON)
  daf__ensure_sources_non_empty(_daf_plugin_FILES_DSP)
  daf__ensure_sources_non_empty(_daf_plugin_FILES_UI)

  ###
  daf__add_static_library("${NAME}" ${_daf_plugin_FILES_COMMON})
  target_include_directories("${NAME}" PUBLIC
    "${DAF_ROOT_DIR}/daf")

  if(_daf_plugin_USE_FILE_BROWSER)
    target_compile_definitions("${NAME}" PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(_daf_plugin_USE_WEB_VIEW)
    target_compile_definitions("${NAME}" PUBLIC "DGL_USE_WEB_VIEW")
  endif()

  if(_daf_plugin_MODGUI_CLASS_NAME)
    target_compile_definitions("${NAME}" PUBLIC "DAF_PLUGIN_MODGUI_CLASS_NAME=\"${_daf_plugin_MODGUI_CLASS_NAME}\"")
  endif()

  find_package(Threads)
  target_link_libraries("${NAME}" PUBLIC "${CMAKE_THREAD_LIBS_INIT}")

  if((NOT WIN32) AND (NOT APPLE) AND (NOT HAIKU))
    target_link_libraries("${NAME}" PRIVATE "dl")
  endif()

  if(_dgl_library)
    # make sure that all code will see DGL_* definitions
    target_link_libraries("${NAME}" PUBLIC
      "${_dgl_library}-definitions"
      dgl-system-libs-definitions
      dgl-system-libs)
  endif()

  daf__add_static_library("${NAME}-dsp" ${_daf_plugin_FILES_DSP})
  target_link_libraries("${NAME}-dsp" PUBLIC "${NAME}")

  if(_dgl_library)
    daf__add_static_library("${NAME}-ui" ${_daf_plugin_FILES_UI})
    target_link_libraries("${NAME}-ui" PUBLIC "${NAME}" ${_dgl_library})
    if((NOT WIN32) AND (NOT APPLE) AND (NOT HAIKU))
      target_link_libraries("${NAME}-ui" PRIVATE "dl")
      if(LINUX AND _daf_plugin_USE_WEB_VIEW)
        execute_process(COMMAND ${CMAKE_C_COMPILER} -print-file-name=Scrt1.o
          OUTPUT_STRIP_TRAILING_WHITESPACE
          OUTPUT_VARIABLE _daf_plugin_shared_crt)
        target_link_libraries("${NAME}-ui" PRIVATE "rt")
      endif()
    endif()
    # add the files containing C++17 or Objective-C classes
    daf__add_plugin_specific_ui_sources("${NAME}-ui" "${_daf_plugin_USE_WEB_VIEW}")
  else()
    add_library("${NAME}-ui" INTERFACE)
  endif()

  ###
  foreach(_target ${_daf_plugin_TARGETS})
    if(_target STREQUAL "jack")
      daf__build_jack("${NAME}"
                      "${_dgl_has_ui}"
                      "${_daf_plugin_FORCE_NATIVE_AUDIO_FALLBACK}"
                      "${_daf_plugin_SKIP_NATIVE_AUDIO_FALLBACK}"
                      "${_daf_plugin_USE_FILE_BROWSER}")
    elseif(_target STREQUAL "ladspa")
      daf__build_ladspa("${NAME}")
    elseif(_target STREQUAL "dssi")
      daf__build_dssi("${NAME}" "${_dgl_has_ui}")
    elseif(_target STREQUAL "lv2")
      daf__build_lv2("${NAME}" "${_dgl_has_ui}" "${_daf_plugin_MONOLITHIC}" "${_daf_plugin_shared_crt}")
    elseif(_target STREQUAL "vst2")
      daf__build_vst2("${NAME}" "${_dgl_has_ui}" "${_daf_plugin_shared_crt}")
    elseif(_target STREQUAL "vst3")
      daf__build_vst3("${NAME}" "${_dgl_has_ui}" "${_daf_plugin_shared_crt}")
    elseif(_target STREQUAL "clap")
      daf__build_clap("${NAME}" "${_dgl_has_ui}" "${_daf_plugin_shared_crt}")
    elseif(_target STREQUAL "au")
      if (APPLE)
        daf__build_au("${NAME}" "${_dgl_has_ui}")
      endif()
    elseif(_target STREQUAL "static")
      daf__build_static("${NAME}" "${_dgl_has_ui}")
    else()
      message(FATAL_ERROR "Unrecognized target type for plugin: ${_target}")
    endif()
  endforeach()
endfunction()

# daf_add_executable(target <args...>)
# ------------------------------------------------------------------------------
#
# Add a simple executable built using the Dusk Audio Framework.
#
# ------------------------------------------------------------------------------
# Arguments:
#
#   `UI_TYPE` <type>
#       the user interface type, can be one of the following:
#          - cairo
#          - external
#          - gles2 (not available on macOS)
#          - gles3 (not available on macOS)
#          - opengl (default everywhere except macOS)
#          - opengl3 (default on macOS)
#          - vulkan
#          - webview
#
#   `NO_SHARED_RESOURCES`
#       do not build DAF shared resources (fonts, etc)
#
#   `USE_FILE_BROWSER`
#       enable file browser dialog APIs
#
#   `USE_WEB_VIEW`
#       enable web browser view APIs
#
function(daf_add_executable NAME)
  set(options NO_SHARED_RESOURCES USE_FILE_BROWSER USE_WEB_VIEW)
  set(oneValueArgs UI_TYPE)
  cmake_parse_arguments(_daf_plugin "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # macOS defaults to opengl3; see the long rationale in daf_add_plugin above.
  if("${_daf_plugin_UI_TYPE}" STREQUAL "")
    if(APPLE)
      set(_daf_plugin_UI_TYPE "opengl3")
    else()
      set(_daf_plugin_UI_TYPE "opengl")
    endif()
  endif()

  # There is no OpenGL ES on macOS; see daf_add_plugin above.
  if(APPLE AND (_daf_plugin_UI_TYPE STREQUAL "gles2" OR _daf_plugin_UI_TYPE STREQUAL "gles3"))
    message(FATAL_ERROR "UI_TYPE ${_daf_plugin_UI_TYPE} is not supported on macOS, use opengl3 instead")
  endif()

  # Plain booleans, not the $<BOOL:...> generator expressions these used to pass: the
  # daf__add_dgl_* functions test their arguments with if(), which sees any generator expression
  # as a non-empty string and therefore always took the "enabled" branch. That silently ignored
  # NO_SHARED_RESOURCES and turned the file browser and the web view on for every plugin, WebKit
  # linkage included. All three options are fixed at configure time, so real booleans are all
  # they need; the functions that do use these in a generator-expression context ($<BOOL:${...}>)
  # are equally happy with TRUE/FALSE.
  if(_daf_plugin_NO_SHARED_RESOURCES)
    set(_daf_shared_resources FALSE)
  else()
    set(_daf_shared_resources TRUE)
  endif()

  if(_daf_plugin_USE_FILE_BROWSER)
    set(_daf_file_browser TRUE)
  else()
    set(_daf_file_browser FALSE)
  endif()

  if(_daf_plugin_USE_WEB_VIEW)
    set(_daf_web_view TRUE)
  else()
    set(_daf_web_view FALSE)
  endif()

  set(_dgl_library)
  if(_daf_plugin_UI_TYPE STREQUAL "cairo")
    daf__add_dgl_cairo(${_daf_shared_resources}
                       ${_daf_file_browser}
                       ${_daf_web_view})
    set(_dgl_library dgl-cairo)
  elseif(_daf_plugin_UI_TYPE STREQUAL "external")
    daf__add_dgl_external(${_daf_file_browser}
                          ${_daf_web_view})
    set(_dgl_library dgl-external)
  elseif(_daf_plugin_UI_TYPE STREQUAL "gles2")
    daf__add_dgl_gles2(${_daf_shared_resources}
                       ${_daf_file_browser}
                       ${_daf_web_view})
    set(_dgl_library dgl-gles2)
  elseif(_daf_plugin_UI_TYPE STREQUAL "gles3")
    daf__add_dgl_gles3(${_daf_shared_resources}
                       ${_daf_file_browser}
                       ${_daf_web_view})
    set(_dgl_library dgl-gles3)
  elseif(_daf_plugin_UI_TYPE STREQUAL "opengl")
    daf__add_dgl_opengl(${_daf_shared_resources}
                        ${_daf_file_browser}
                        ${_daf_web_view})
    set(_dgl_library dgl-opengl)
  elseif(_daf_plugin_UI_TYPE STREQUAL "opengl3")
    daf__add_dgl_opengl3(${_daf_shared_resources}
                         ${_daf_file_browser}
                         ${_daf_web_view})
    set(_dgl_library dgl-opengl3)
  elseif(_daf_plugin_UI_TYPE STREQUAL "vulkan")
    daf__add_dgl_vulkan(${_daf_shared_resources}
                        ${_daf_file_browser}
                        ${_daf_web_view})
    set(_dgl_library dgl-vulkan)
  elseif(_daf_plugin_UI_TYPE STREQUAL "webview")
    set(_daf_plugin_USE_WEB_VIEW TRUE)
    set(_daf_web_view TRUE)
    daf__add_dgl_external(${_daf_file_browser}
                          ${_daf_web_view})
    set(_dgl_library dgl-external)
  else()
    message(FATAL_ERROR "Unrecognized UI type for executable: ${_daf_plugin_UI_TYPE}")
  endif()

  set(_dgl_has_ui OFF)
  if(_dgl_library)
    set(_dgl_has_ui ON)
  endif()

  daf__create_dummy_source_list(_no_srcs)
  daf__add_executable("${NAME}" ${_no_srcs})
  target_include_directories("${NAME}" PUBLIC "${DAF_ROOT_DIR}/daf")
  set_target_properties("${NAME}" PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<0:>"
    OUTPUT_NAME "${NAME}")

  if(EMSCRIPTEN)
    configure_file("${DAF_ROOT_DIR}/utils/emscripten.html.in"
      "${PROJECT_BINARY_DIR}/bin/${NAME}.html" @ONLY)
    target_link_options("${NAME}"
      PRIVATE
        -sEXPORTED_RUNTIME_METHODS=dynCall)
  endif()

  if(_daf_plugin_USE_FILE_BROWSER)
    target_compile_definitions("${NAME}" PUBLIC "DGL_USE_FILE_BROWSER")
    if(EMSCRIPTEN)
      target_link_options("${NAME}" PRIVATE -sEXPORTED_RUNTIME_METHODS=FS,cwrap)
    endif()
  endif()

  if(_daf_plugin_USE_WEB_VIEW)
    target_compile_definitions("${NAME}" PUBLIC "DGL_USE_WEB_VIEW")
  endif()

  if((NOT WIN32) AND (NOT APPLE) AND (NOT HAIKU))
    target_link_libraries("${NAME}" PRIVATE "dl")
  endif()

  if(_dgl_library)
    # make sure that all code will see DGL_* definitions
    target_link_libraries("${NAME}" PUBLIC
      "${_dgl_library}"
      "${_dgl_library}-definitions"
      dgl-system-libs-definitions
      dgl-system-libs)
    # extra linkage for linux web view
    if(LINUX AND _daf_plugin_USE_WEB_VIEW)
      target_link_libraries("${NAME}" PRIVATE "rt")
    endif()
    # add the files containing C++17 or Objective-C classes
    daf__add_plugin_specific_ui_sources("${NAME}" "${_daf_plugin_USE_WEB_VIEW}")
  endif()
endfunction()

# ------------------------------------------------------------------------------
# DAF private functions (prefixed with `daf__`)
# ------------------------------------------------------------------------------

# Note: The $<0:> trick is to prevent MSVC from appending the build type
#       to the output directory.
#

# daf__build_jack
# ------------------------------------------------------------------------------
#
# Add build rules for a JACK/Standalone program.
#
function(daf__build_jack NAME HAS_UI FORCE_NATIVE_AUDIO_FALLBACK SKIP_NATIVE_AUDIO_FALLBACK USE_FILE_BROWSER)
  daf__create_dummy_source_list(_no_srcs)

  daf__add_executable("${NAME}-jack" ${_no_srcs})
  daf__add_plugin_main("${NAME}-jack" "jack")
  daf__add_ui_main("${NAME}-jack" "jack" "${HAS_UI}")
  target_link_libraries("${NAME}-jack" PRIVATE "${NAME}-dsp" "${NAME}-ui")
  set_target_properties("${NAME}-jack" PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<0:>"
    OUTPUT_NAME "${NAME}")
  daf__set_msvc_debug_file_names("${NAME}-jack")

  if(EMSCRIPTEN)
    configure_file("${DAF_ROOT_DIR}/utils/emscripten.html.in"
      "${PROJECT_BINARY_DIR}/bin/${NAME}.html" @ONLY)
    target_link_options("${NAME}-jack"
      PRIVATE
        -sEXPORTED_RUNTIME_METHODS=dynCall
        $<$<BOOL:${USE_FILE_BROWSER}>:-sEXPORTED_RUNTIME_METHODS=FS,cwrap>)
  endif()

  if(NOT FORCE_NATIVE_AUDIO_FALLBACK)
    target_compile_definitions("${NAME}" PUBLIC "HAVE_JACK")
    if(NOT MSVC)
      # RtAudio uses this to include <sys/time.h> and call gettimeofday(), neither of which MSVC has.
      # It only refines RtAudio's stream-time bookkeeping, so leaving it off there is harmless.
      target_compile_definitions("${NAME}-jack" PRIVATE "HAVE_GETTIMEOFDAY")
    endif()
  endif()

  if(SKIP_NATIVE_AUDIO_FALLBACK)
    return()
  endif()

  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(SDL2 "sdl2")
  else()
    set(SDL2_FOUND FALSE)
  endif()
  if(SDL2_FOUND)
    target_compile_definitions("${NAME}" PUBLIC "HAVE_SDL2")
    target_include_directories("${NAME}-jack" PRIVATE ${SDL2_STATIC_INCLUDE_DIRS})
    target_link_libraries("${NAME}-jack" PRIVATE ${SDL2_STATIC_LIBRARIES})
    daf__target_link_directories("${NAME}-jack" "${SDL2_STATIC_LIBRARY_DIRS}")
  endif()

  if(APPLE OR WIN32)
    target_compile_definitions("${NAME}" PUBLIC "HAVE_RTAUDIO")
  elseif(EMSCRIPTEN)
  else()
    pkg_check_modules(ALSA "alsa")
    pkg_check_modules(PULSEAUDIO "libpulse-simple")
    if(ALSA_FOUND)
      target_compile_definitions("${NAME}" PUBLIC "HAVE_ALSA")
      target_include_directories("${NAME}-jack" PRIVATE ${ALSA_INCLUDE_DIRS})
      target_link_libraries("${NAME}-jack" PRIVATE ${ALSA_LIBRARIES})
      daf__target_link_directories("${NAME}-jack" "${ALSA_LIBRARY_DIRS}")
    endif()
    if(PULSEAUDIO_FOUND)
      target_compile_definitions("${NAME}" PUBLIC "HAVE_PULSEAUDIO")
      target_include_directories("${NAME}-jack" PRIVATE ${PULSEAUDIO_INCLUDE_DIRS})
      target_link_libraries("${NAME}-jack" PRIVATE ${PULSEAUDIO_LIBRARIES})
      daf__target_link_directories("${NAME}-jack" "${PULSEAUDIO_LIBRARY_DIRS}")
    endif()
    if(ALSA_FOUND OR PULSEAUDIO_FOUND)
      target_compile_definitions("${NAME}" PUBLIC "HAVE_RTAUDIO")
    endif()
  endif()

  # for RtAudio native fallback
  if(APPLE)
    find_library(APPLE_COREAUDIO_FRAMEWORK "CoreAudio")
    find_library(APPLE_COREFOUNDATION_FRAMEWORK "CoreFoundation")
    find_library(APPLE_COREMIDI_FRAMEWORK "CoreMIDI")
    target_link_libraries("${NAME}-jack" PRIVATE
      "${APPLE_COREAUDIO_FRAMEWORK}"
      "${APPLE_COREFOUNDATION_FRAMEWORK}"
      "${APPLE_COREMIDI_FRAMEWORK}")
  elseif(WIN32)
    target_link_libraries("${NAME}-jack" PRIVATE "ksuser" "mfplat" "mfuuid" "ole32" "winmm" "wmcodecdspuuid")
    if(HAS_UI)
      # a standalone with a UI must not spawn a console window alongside it
      set_target_properties("${NAME}-jack" PROPERTIES WIN32_EXECUTABLE TRUE)
      if(MSVC)
        # WIN32_EXECUTABLE selects /SUBSYSTEM:WINDOWS, whose default entry point is
        # WinMainCRTStartup and thus requires a WinMain(). DAF standalones define a regular main(),
        # so point the linker at the console startup routine instead.
        # (MinGW needs no equivalent, its crt entry points all funnel into main().)
        target_link_options("${NAME}-jack" PRIVATE "/ENTRY:mainCRTStartup")
      endif()
    endif()
  endif()
endfunction()

# daf__build_ladspa
# ------------------------------------------------------------------------------
#
# Add build rules for a LADSPA plugin.
#
function(daf__build_ladspa NAME)
  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-ladspa" ${_no_srcs})
  daf__add_plugin_main("${NAME}-ladspa" "ladspa")
  daf__set_module_export_list("${NAME}-ladspa" "ladspa")
  target_link_libraries("${NAME}-ladspa" PRIVATE "${NAME}-dsp")
  set_target_properties("${NAME}-ladspa" PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<0:>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/ladspa/$<0:>"
    OUTPUT_NAME "${NAME}-ladspa"
    PREFIX "")
endfunction()

# daf__build_dssi
# ------------------------------------------------------------------------------
#
# Add build rules for a DSSI plugin.
#
function(daf__build_dssi NAME HAS_UI)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBLO "liblo")
  else()
    set(LIBLO_FOUND FALSE)
  endif()
  if(NOT LIBLO_FOUND)
    daf__warn_once_only(missing_liblo
      "liblo is not found, skipping the `dssi` plugin targets")
    return()
  endif()

  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-dssi" ${_no_srcs})
  daf__add_plugin_main("${NAME}-dssi" "dssi")
  daf__set_module_export_list("${NAME}-dssi" "dssi")
  target_link_libraries("${NAME}-dssi" PRIVATE "${NAME}-dsp")
  set_target_properties("${NAME}-dssi" PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<0:>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/dssi/$<0:>"
    OUTPUT_NAME "${NAME}-dssi"
    PREFIX "")

  if(HAS_UI)
    daf__add_executable("${NAME}-dssi-ui" ${_no_srcs})
    daf__add_ui_main("${NAME}-dssi-ui" "dssi" "${HAS_UI}")
    target_link_libraries("${NAME}-dssi-ui" PRIVATE "${NAME}-ui")
    set_target_properties("${NAME}-dssi-ui" PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}-dssi/$<0:>"
      OUTPUT_NAME "${NAME}_ui")

    target_compile_definitions("${NAME}" PUBLIC "HAVE_LIBLO")
    target_include_directories("${NAME}-dssi-ui" PRIVATE ${LIBLO_INCLUDE_DIRS})
    target_link_libraries("${NAME}-dssi-ui" PRIVATE ${LIBLO_LIBRARIES})
    daf__target_link_directories("${NAME}-dssi-ui" "${LIBLO_LIBRARY_DIRS}")
  endif()
endfunction()

# daf__build_lv2
# ------------------------------------------------------------------------------
#
# Add build rules for an LV2 plugin.
#
function(daf__build_lv2 NAME HAS_UI MONOLITHIC EXTRA_UI_LINK_OPTS)
  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-lv2" ${_no_srcs})
  daf__add_plugin_main("${NAME}-lv2" "lv2")
  if(HAS_UI AND MONOLITHIC)
    daf__set_module_export_list("${NAME}-lv2" "lv2")
  else()
    daf__set_module_export_list("${NAME}-lv2" "lv2-dsp")
  endif()
  target_link_libraries("${NAME}-lv2" PRIVATE "${NAME}-dsp")
  set_target_properties("${NAME}-lv2" PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.lv2/$<0:>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/lv2/$<0:>"
    OUTPUT_NAME "${NAME}_dsp"
    PREFIX "")

  # helper property for custom outside handling
  set_target_properties("${NAME}" PROPERTIES
    LV2_BUNDLE "${PROJECT_BINARY_DIR}/bin/${NAME}.lv2")

  if(HAS_UI)
    if(MONOLITHIC)
      daf__add_ui_main("${NAME}-lv2" "lv2" "${HAS_UI}")
      target_link_libraries("${NAME}-lv2" PRIVATE "${NAME}-ui")
      target_link_options("${NAME}-lv2" PRIVATE "${EXTRA_UI_LINK_OPTS}")
      set_target_properties("${NAME}-lv2" PROPERTIES
        OUTPUT_NAME "${NAME}")
    else()
      daf__add_module("${NAME}-lv2-ui" ${_no_srcs})
      daf__add_ui_main("${NAME}-lv2-ui" "lv2" "${HAS_UI}")
      daf__set_module_export_list("${NAME}-lv2-ui" "lv2-ui")
      target_link_options("${NAME}-lv2-ui" PRIVATE "${EXTRA_UI_LINK_OPTS}")
      target_link_libraries("${NAME}-lv2-ui" PRIVATE "${NAME}-ui")
      set_target_properties("${NAME}-lv2-ui" PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.lv2/$<0:>"
        ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/lv2/$<0:>"
        OUTPUT_NAME "${NAME}_ui"
        PREFIX "")
    endif()
  endif()

  daf__add_lv2_ttl_generator()
  add_dependencies("${NAME}-lv2" lv2_ttl_generator)

  separate_arguments(CMAKE_CROSSCOMPILING_EMULATOR)

  add_custom_command(TARGET "${NAME}-lv2" POST_BUILD
    COMMAND
    ${CMAKE_CROSSCOMPILING_EMULATOR}
    "$<TARGET_FILE:lv2_ttl_generator>"
    "$<TARGET_FILE:${NAME}-lv2>"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.lv2")
endfunction()

# daf__build_vst2
# ------------------------------------------------------------------------------
#
# Add build rules for a VST2 plugin.
#
function(daf__build_vst2 NAME HAS_UI EXTRA_UI_LINK_OPTS)
  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-vst2" ${_no_srcs})
  daf__add_plugin_main("${NAME}-vst2" "vst2")
  daf__add_ui_main("${NAME}-vst2" "vst2" "${HAS_UI}")
  daf__set_module_export_list("${NAME}-vst2" "vst2")
  target_link_libraries("${NAME}-vst2" PRIVATE "${NAME}-dsp" "${NAME}-ui")
  target_link_options("${NAME}-vst2" PRIVATE "${EXTRA_UI_LINK_OPTS}")
  set_target_properties("${NAME}-vst2" PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<0:>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/vst2/$<0:>"
    OUTPUT_NAME "${NAME}-vst2"
    PREFIX "")
  if(APPLE)
    set_target_properties("${NAME}-vst2" PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.vst/Contents/MacOS/$<0:>"
      OUTPUT_NAME "${NAME}"
      SUFFIX "")
    set(INFO_PLIST_PROJECT_NAME "${NAME}")
    configure_file("${DAF_ROOT_DIR}/utils/plugin.bundle/Contents/Info.plist"
      "${PROJECT_BINARY_DIR}/bin/${NAME}.vst/Contents/Info.plist" @ONLY)
    file(COPY "${DAF_ROOT_DIR}/utils/plugin.bundle/Contents/PkgInfo"
      DESTINATION "${PROJECT_BINARY_DIR}/bin/${NAME}.vst/Contents")
  endif()
endfunction()

# daf__determine_vst3_package_architecture
# ------------------------------------------------------------------------------
#
# Determines the package architecture for a VST3 plugin target.
#
function(daf__determine_vst3_package_architecture OUTPUT_VARIABLE)
  # if set by variable, override the detection
  if(DAF_VST3_ARCHITECTURE)
    set("${OUTPUT_VARIABLE}" "${DAF_VST3_ARCHITECTURE}" PARENT_SCOPE)
    return()
  endif()

  # not used on Apple, which supports universal binary
  if(APPLE)
    set("${OUTPUT_VARIABLE}" "universal" PARENT_SCOPE)
    return()
  endif()

  # identify the target processor (special case of MSVC, problematic sometimes)
  if(MSVC)
    set(vst3_system_arch "${MSVC_CXX_ARCHITECTURE_ID}")
  else()
    set(vst3_system_arch "${CMAKE_SYSTEM_PROCESSOR}")
  endif()

  # transform the processor name to a format that VST3 recognizes
  # see https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Locations+Format/Plugin+Format.html
  if(vst3_system_arch MATCHES "^(x86_64|amd64|AMD64|x64|X64)$")
    set(vst3_package_arch "x86_64")
  elseif(vst3_system_arch MATCHES "^(ARM)$")
    set(vst3_package_arch "arm")
  elseif(vst3_system_arch MATCHES "^(ARM64)$")
    set(vst3_package_arch "arm64")
  elseif(vst3_system_arch MATCHES "^(ARM64EC)$")
    set(vst3_package_arch "arm64x")
  elseif(vst3_system_arch MATCHES "^(i.86|x86|X86)$")
    if(WIN32)
      set(vst3_package_arch "x86")
    else()
      set(vst3_package_arch "i386")
    endif()
  elseif(vst3_system_arch MATCHES "^(armv[3-9][a-z]*|aarch64|loongarch64|ppc(64)?(le)?)$")
    set(vst3_package_arch "${vst3_system_arch}")
  else()
    message(FATAL_ERROR "We don't know this architecture for VST3: ${vst3_system_arch}.")
  endif()

  # TODO: the detections for Windows arm/arm64 when supported

  set("${OUTPUT_VARIABLE}" "${vst3_package_arch}" PARENT_SCOPE)
endfunction()

# daf__build_vst3
# ------------------------------------------------------------------------------
#
# Add build rules for a VST3 plugin.
#
function(daf__build_vst3 NAME HAS_UI EXTRA_UI_LINK_OPTS)
  daf__determine_vst3_package_architecture(vst3_arch)

  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-vst3" ${_no_srcs})
  daf__add_plugin_main("${NAME}-vst3" "vst3")
  daf__add_ui_main("${NAME}-vst3" "vst3" "${HAS_UI}")
  daf__set_module_export_list("${NAME}-vst3" "vst3")
  target_link_libraries("${NAME}-vst3" PRIVATE "${NAME}-dsp" "${NAME}-ui")
  target_link_options("${NAME}-vst3" PRIVATE "${EXTRA_UI_LINK_OPTS}")
  set_target_properties("${NAME}-vst3" PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/vst3/$<0:>"
    OUTPUT_NAME "${NAME}"
    PREFIX "")

  if(APPLE)
    set_target_properties("${NAME}-vst3" PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.vst3/Contents/MacOS/$<0:>"
      SUFFIX "")
  elseif(WIN32)
    set_target_properties("${NAME}-vst3" PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.vst3/Contents/${vst3_arch}-win/$<0:>" SUFFIX ".vst3")
  else()
    set_target_properties("${NAME}-vst3" PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.vst3/Contents/${vst3_arch}-linux/$<0:>")
  endif()

  if(APPLE)
    # Uses the same macOS bundle template as VST2
    set(INFO_PLIST_PROJECT_NAME "${NAME}")
    configure_file("${DAF_ROOT_DIR}/utils/plugin.bundle/Contents/Info.plist"
     "${PROJECT_BINARY_DIR}/bin/${NAME}.vst3/Contents/Info.plist" @ONLY)
    file(COPY "${DAF_ROOT_DIR}/utils/plugin.bundle/Contents/PkgInfo"
     DESTINATION "${PROJECT_BINARY_DIR}/bin/${NAME}.vst3/Contents")
  endif()
endfunction()

# daf__build_clap
# ------------------------------------------------------------------------------
#
# Add build rules for a CLAP plugin.
#
function(daf__build_clap NAME HAS_UI EXTRA_UI_LINK_OPTS)
  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-clap" ${_no_srcs})
  daf__add_plugin_main("${NAME}-clap" "clap")
  daf__add_ui_main("${NAME}-clap" "clap" "${HAS_UI}")
  daf__set_module_export_list("${NAME}-clap" "clap")
  target_link_libraries("${NAME}-clap" PRIVATE "${NAME}-dsp" "${NAME}-ui")
  target_link_options("${NAME}-clap" PRIVATE "${EXTRA_UI_LINK_OPTS}")
  set_target_properties("${NAME}-clap" PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<0:>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/clap/$<0:>"
    OUTPUT_NAME "${NAME}"
    PREFIX ""
    SUFFIX ".clap")
  daf__set_msvc_debug_file_names("${NAME}-clap")

  if(APPLE)
    set_target_properties("${NAME}-clap" PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.clap/Contents/MacOS/$<0:>"
      OUTPUT_NAME "${NAME}"
      SUFFIX "")
    set(INFO_PLIST_PROJECT_NAME "${NAME}")
    configure_file("${DAF_ROOT_DIR}/utils/plugin.bundle/Contents/Info.plist"
      "${PROJECT_BINARY_DIR}/bin/${NAME}.clap/Contents/Info.plist" @ONLY)
    file(COPY "${DAF_ROOT_DIR}/utils/plugin.bundle/Contents/PkgInfo"
      DESTINATION "${PROJECT_BINARY_DIR}/bin/${NAME}.clap/Contents")
  endif()
endfunction()

# daf__build_au
# ------------------------------------------------------------------------------
#
# Add build rules for an AUv2 plugin.
#
function(daf__build_au NAME HAS_UI)
  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-au" ${_no_srcs})
  daf__add_plugin_main("${NAME}-au" "au")
  daf__add_ui_main("${NAME}-au" "au" "${HAS_UI}")
  daf__set_module_export_list("${NAME}-au" "au")
  find_library(APPLE_AUDIOTOOLBOX_FRAMEWORK "AudioToolbox")
  find_library(APPLE_AUDIOUNIT_FRAMEWORK "AudioUnit")
  find_library(APPLE_COREFOUNDATION_FRAMEWORK "CoreFoundation")
  target_compile_options("${NAME}-au" PRIVATE "-ObjC++")
  target_link_libraries("${NAME}-au" PRIVATE
    "${NAME}-dsp"
    "${NAME}-ui"
    "${APPLE_AUDIOTOOLBOX_FRAMEWORK}"
    "${APPLE_AUDIOUNIT_FRAMEWORK}"
    "${APPLE_COREFOUNDATION_FRAMEWORK}")
  set_target_properties("${NAME}-au" PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.component/Contents/MacOS/$<0:>"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/obj/au/$<0:>"
    OUTPUT_NAME "${NAME}"
    PREFIX ""
    SUFFIX "")

  daf__add_executable("${NAME}-export" ${_no_srcs})
  daf__add_plugin_main("${NAME}-export" "export")
  daf__add_ui_main("${NAME}-export" "export" "${HAS_UI}")
  target_link_libraries("${NAME}-export" PRIVATE "${NAME}-dsp" "${NAME}-ui")

  separate_arguments(CMAKE_CROSSCOMPILING_EMULATOR)

  add_custom_command(TARGET "${NAME}-au" POST_BUILD
    COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} "$<TARGET_FILE:${NAME}-export>" "${NAME}"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}/bin/${NAME}.component/Contents")

  add_dependencies("${NAME}-au" "${NAME}-export")

  file(COPY "${DAF_ROOT_DIR}/utils/plugin.bundle/Contents/PkgInfo"
    DESTINATION "${PROJECT_BINARY_DIR}/bin/${NAME}.component/Contents")
endfunction()

# daf__build_static
# ------------------------------------------------------------------------------
#
# Add build rules for a static library.
#
function(daf__build_static NAME HAS_UI)
  daf__create_dummy_source_list(_no_srcs)

  daf__add_module("${NAME}-static" ${_no_srcs} STATIC)
  daf__add_plugin_main("${NAME}-static" "static")
  daf__add_ui_main("${NAME}-static" "static" "${HAS_UI}")
  target_link_libraries("${NAME}-static" PRIVATE "${NAME}-dsp" "${NAME}-ui")

  get_target_property(dsp_srcs "${NAME}-dsp" SOURCES)
  get_target_property(ui_srcs "${NAME}-ui" SOURCES)
  foreach(src ${dsp_srcs})
    target_sources("${NAME}-static" PRIVATE ${src})
  endforeach()
  foreach(src ${ui_srcs})
    target_sources("${NAME}-static" PRIVATE ${src})
  endforeach()

  set_target_properties("${NAME}-static" PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin/$<0:>"
    OUTPUT_NAME "${NAME}"
    PREFIX "")
endfunction()

# daf__add_dgl_cairo
# ------------------------------------------------------------------------------
#
# Add the Cairo variant of DGL, if not already available.
#
function(daf__add_dgl_cairo SHARED_RESOURCES USE_FILE_BROWSER USE_WEB_VIEW)
  if(TARGET dgl-cairo)
    return()
  endif()

  find_package(PkgConfig REQUIRED)
  pkg_check_modules(CAIRO "cairo" REQUIRED)

  link_directories(${CAIRO_LIBRARY_DIRS})

  daf__add_static_library(dgl-cairo STATIC
    "${DAF_ROOT_DIR}/dgl/src/Application.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ApplicationPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Color.cpp"
    "${DAF_ROOT_DIR}/dgl/src/EventHandlers.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Geometry.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBase.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBaseWidgets.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Layout.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Widget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Window.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WindowPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Cairo.cpp")
  if(SHARED_RESOURCES)
    target_sources(dgl-cairo PRIVATE "${DAF_ROOT_DIR}/dgl/src/Resources.cpp")
  else()
    target_compile_definitions(dgl-cairo PUBLIC "DGL_NO_SHARED_RESOURCES")
  endif()
  if(APPLE)
    target_sources(dgl-cairo PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.mm")
  else()
    target_sources(dgl-cairo PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.cpp")
  endif()
  target_include_directories(dgl-cairo PUBLIC
    "${DAF_ROOT_DIR}/dgl")
  target_include_directories(dgl-cairo PUBLIC
    "${DAF_ROOT_DIR}/dgl/src/pugl-upstream/include")

  if(USE_FILE_BROWSER)
    target_compile_definitions(dgl-cairo PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(USE_WEB_VIEW)
    target_compile_definitions(dgl-cairo PUBLIC "DGL_USE_WEB_VIEW")
    if(APPLE)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries(dgl-cairo PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    elseif(WIN32)
      target_sources(dgl-cairo PRIVATE
        "${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp")
      set_source_files_properties("${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp"
        PROPERTIES
          COMPILE_FLAGS
            $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    endif()
  endif()

  daf__add_dgl_system_libs()
  target_link_libraries(dgl-cairo PRIVATE dgl-system-libs)

  add_library(dgl-cairo-definitions INTERFACE)
  target_compile_definitions(dgl-cairo-definitions INTERFACE "DGL_CAIRO" "HAVE_CAIRO" "HAVE_DGL")

  target_include_directories(dgl-cairo PUBLIC ${CAIRO_INCLUDE_DIRS})
  if(MINGW)
    target_link_libraries(dgl-cairo PRIVATE ${CAIRO_STATIC_LIBRARIES})
  else()
    target_link_libraries(dgl-cairo PRIVATE ${CAIRO_LIBRARIES})
  endif()
  target_link_libraries(dgl-cairo PRIVATE dgl-cairo-definitions)
endfunction()

# daf__add_dgl_external
# ------------------------------------------------------------------------------
#
# Add the external variant of DGL, if not already available.
#
function(daf__add_dgl_external USE_FILE_BROWSER USE_WEB_VIEW)
  if(TARGET dgl-external)
    return()
  endif()

  daf__add_static_library(dgl-external STATIC
    "${DAF_ROOT_DIR}/dgl/src/Application.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ApplicationPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Color.cpp"
    "${DAF_ROOT_DIR}/dgl/src/EventHandlers.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Geometry.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBase.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBaseWidgets.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Layout.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Widget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Window.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WindowPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Stub.cpp")

  if(APPLE)
    target_sources(dgl-external PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.mm")
  else()
    target_sources(dgl-external PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.cpp")
  endif()
  target_include_directories(dgl-external PUBLIC
    "${DAF_ROOT_DIR}/dgl")
  target_include_directories(dgl-external PUBLIC
    "${DAF_ROOT_DIR}/dgl/src/pugl-upstream/include")

  if(USE_FILE_BROWSER)
    target_compile_definitions(dgl-external PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(USE_WEB_VIEW)
    target_compile_definitions(dgl-external PUBLIC "DGL_USE_WEB_VIEW")
    if(APPLE)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries(dgl-external PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    elseif(WIN32)
      target_sources(dgl-external PRIVATE
        "${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp")
      set_source_files_properties("${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp"
        PROPERTIES
          COMPILE_FLAGS
            $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    endif()
  endif()

  daf__add_dgl_system_libs()
  target_compile_definitions(dgl-external PUBLIC "DGL_NO_SHARED_RESOURCES")
  target_link_libraries(dgl-external PRIVATE dgl-system-libs)

  add_library(dgl-external-definitions INTERFACE)
  target_compile_definitions(dgl-external-definitions INTERFACE "DGL_EXTERNAL" "HAVE_DGL")

  target_include_directories(dgl-external PUBLIC "${OPENGL_INCLUDE_DIR}")
  target_link_libraries(dgl-external PRIVATE dgl-external-definitions "${OPENGL_gl_LIBRARY}")
endfunction()

# daf__add_dgl_gles2
# ------------------------------------------------------------------------------
#
# Add the GLESv2 variant of DGL, if not already available.
#
function(daf__add_dgl_gles2 SHARED_RESOURCES USE_FILE_BROWSER USE_WEB_VIEW)
  if(TARGET dgl-gles2)
    return()
  endif()

  if(NOT OpenGL_GL_PREFERENCE)
    set(OpenGL_GL_PREFERENCE "LEGACY")
  endif()

  find_package(OpenGL REQUIRED)

  daf__add_static_library(dgl-gles2 STATIC
    "${DAF_ROOT_DIR}/dgl/src/Application.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ApplicationPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Color.cpp"
    "${DAF_ROOT_DIR}/dgl/src/EventHandlers.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Geometry.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBase.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBaseWidgets.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Layout.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Widget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Window.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WindowPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL3.cpp"
    "${DAF_ROOT_DIR}/dgl/src/NanoVG.cpp")
  if(SHARED_RESOURCES)
    target_sources(dgl-gles2 PRIVATE "${DAF_ROOT_DIR}/dgl/src/Resources.cpp")
  else()
    target_compile_definitions(dgl-gles2 PUBLIC "DGL_NO_SHARED_RESOURCES")
  endif()
  if(APPLE)
    target_sources(dgl-gles2 PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.mm")
  else()
    target_sources(dgl-gles2 PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.cpp")
  endif()
  target_include_directories(dgl-gles2 PUBLIC
    "${DAF_ROOT_DIR}/dgl")
  target_include_directories(dgl-gles2 PUBLIC
    "${DAF_ROOT_DIR}/dgl/src/pugl-upstream/include")

  if(APPLE)
    target_compile_definitions(dgl-gles2 PUBLIC "GL_SILENCE_DEPRECATION")
  endif()

  if(USE_FILE_BROWSER)
    target_compile_definitions(dgl-gles2 PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(USE_WEB_VIEW)
    target_compile_definitions(dgl-gles2 PUBLIC "DGL_USE_WEB_VIEW")
    if(APPLE)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries(dgl-gles2 PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    elseif(WIN32)
      target_sources(dgl-gles2 PRIVATE
        "${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp")
      set_source_files_properties("${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp"
        PROPERTIES
          COMPILE_FLAGS
            $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    endif()
  endif()

  daf__add_dgl_system_libs()
  target_link_libraries(dgl-gles2 PRIVATE dgl-system-libs)
  target_link_options(dgl-gles2
    INTERFACE
      $<$<BOOL:${EMSCRIPTEN}>:-sMIN_WEBGL_VERSION=2>
      $<$<BOOL:${EMSCRIPTEN}>:-sMAX_WEBGL_VERSION=2>
  )

  add_library(dgl-gles2-definitions INTERFACE)
  target_compile_definitions(dgl-gles2-definitions
    INTERFACE
      DGL_USE_OPENGL3
      DGL_USE_GLES
      DGL_USE_GLES2
      DGL_OPENGL
      HAVE_OPENGL
      HAVE_DGL
  )

  target_include_directories(dgl-gles2 PUBLIC "${OPENGL_INCLUDE_DIR}")
  target_link_libraries(dgl-gles2 PRIVATE dgl-gles2-definitions "${OPENGL_gl_LIBRARY}")
endfunction()

# daf__add_dgl_gles3
# ------------------------------------------------------------------------------
#
# Add the GLESv3 variant of DGL, if not already available.
#
function(daf__add_dgl_gles3 SHARED_RESOURCES USE_FILE_BROWSER USE_WEB_VIEW)
  if(TARGET dgl-gles3)
    return()
  endif()

  if(NOT OpenGL_GL_PREFERENCE)
    set(OpenGL_GL_PREFERENCE "LEGACY")
  endif()

  find_package(OpenGL REQUIRED)

  daf__add_static_library(dgl-gles3 STATIC
    "${DAF_ROOT_DIR}/dgl/src/Application.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ApplicationPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Color.cpp"
    "${DAF_ROOT_DIR}/dgl/src/EventHandlers.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Geometry.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBase.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBaseWidgets.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Layout.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Widget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Window.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WindowPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL3.cpp"
    "${DAF_ROOT_DIR}/dgl/src/NanoVG.cpp")
  if(SHARED_RESOURCES)
    target_sources(dgl-gles3 PRIVATE "${DAF_ROOT_DIR}/dgl/src/Resources.cpp")
  else()
    target_compile_definitions(dgl-gles3 PUBLIC "DGL_NO_SHARED_RESOURCES")
  endif()
  if(APPLE)
    target_sources(dgl-gles3 PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.mm")
  else()
    target_sources(dgl-gles3 PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.cpp")
  endif()
  target_include_directories(dgl-gles3 PUBLIC
    "${DAF_ROOT_DIR}/dgl")
  target_include_directories(dgl-gles3 PUBLIC
    "${DAF_ROOT_DIR}/dgl/src/pugl-upstream/include")

  if(APPLE)
    target_compile_definitions(dgl-gles3 PUBLIC "GL_SILENCE_DEPRECATION")
  endif()

  if(USE_FILE_BROWSER)
    target_compile_definitions(dgl-gles3 PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(USE_WEB_VIEW)
    target_compile_definitions(dgl-gles3 PUBLIC "DGL_USE_WEB_VIEW")
    if(APPLE)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries(dgl-gles3 PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    elseif(WIN32)
      target_sources(dgl-gles3 PRIVATE
        "${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp")
      set_source_files_properties("${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp"
        PROPERTIES
          COMPILE_FLAGS
            $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    endif()
  endif()

  daf__add_dgl_system_libs()
  target_link_libraries(dgl-gles3 PRIVATE dgl-system-libs)
  target_link_options(dgl-gles3
    INTERFACE
      $<$<BOOL:${EMSCRIPTEN}>:-sMIN_WEBGL_VERSION=3>
      $<$<BOOL:${EMSCRIPTEN}>:-sMAX_WEBGL_VERSION=3>
  )

  add_library(dgl-gles3-definitions INTERFACE)
  target_compile_definitions(dgl-gles3-definitions
    INTERFACE
      DGL_USE_OPENGL3
      DGL_USE_GLES
      DGL_USE_GLES3
      DGL_OPENGL
      HAVE_OPENGL
      HAVE_DGL
  )

  target_include_directories(dgl-gles3 PUBLIC "${OPENGL_INCLUDE_DIR}")
  target_link_libraries(dgl-gles3 PRIVATE dgl-gles3-definitions "${OPENGL_gl_LIBRARY}")
endfunction()

# daf__add_dgl_opengl
# ------------------------------------------------------------------------------
#
# Add the OpenGL variant of DGL, if not already available.
#
function(daf__add_dgl_opengl SHARED_RESOURCES USE_FILE_BROWSER USE_WEB_VIEW)
  if(TARGET dgl-opengl)
    return()
  endif()

  if(NOT OpenGL_GL_PREFERENCE)
    set(OpenGL_GL_PREFERENCE "LEGACY")
  endif()

  find_package(OpenGL REQUIRED)

  daf__add_static_library(dgl-opengl STATIC
    "${DAF_ROOT_DIR}/dgl/src/Application.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ApplicationPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Color.cpp"
    "${DAF_ROOT_DIR}/dgl/src/EventHandlers.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Geometry.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBase.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBaseWidgets.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Layout.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Widget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Window.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WindowPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL2.cpp"
    "${DAF_ROOT_DIR}/dgl/src/NanoVG.cpp")
  if(SHARED_RESOURCES)
    target_sources(dgl-opengl PRIVATE "${DAF_ROOT_DIR}/dgl/src/Resources.cpp")
  else()
    target_compile_definitions(dgl-opengl PUBLIC "DGL_NO_SHARED_RESOURCES")
  endif()
  if(APPLE)
    target_sources(dgl-opengl PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.mm")
  else()
    target_sources(dgl-opengl PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.cpp")
  endif()
  target_include_directories(dgl-opengl PUBLIC
    "${DAF_ROOT_DIR}/dgl")
  target_include_directories(dgl-opengl PUBLIC
    "${DAF_ROOT_DIR}/dgl/src/pugl-upstream/include")

  if(APPLE)
    target_compile_definitions(dgl-opengl PUBLIC "GL_SILENCE_DEPRECATION")
  endif()

  if(USE_FILE_BROWSER)
    target_compile_definitions(dgl-opengl PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(USE_WEB_VIEW)
    target_compile_definitions(dgl-opengl PUBLIC "DGL_USE_WEB_VIEW")
    if(APPLE)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries(dgl-opengl PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    elseif(WIN32)
      target_sources(dgl-opengl PRIVATE
        "${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp")
      set_source_files_properties("${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp"
        PROPERTIES
          COMPILE_FLAGS
            $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    endif()
  endif()

  daf__add_dgl_system_libs()
  target_link_libraries(dgl-opengl PRIVATE dgl-system-libs)
  target_link_options(dgl-opengl
    INTERFACE
      $<$<BOOL:${EMSCRIPTEN}>:-sLEGACY_GL_EMULATION>
      $<$<BOOL:${EMSCRIPTEN}>:-sGL_UNSAFE_OPTS=0>
  )

  add_library(dgl-opengl-definitions INTERFACE)
  target_compile_definitions(dgl-opengl-definitions
    INTERFACE
      DGL_OPENGL
      HAVE_OPENGL
      HAVE_DGL
  )

  target_include_directories(dgl-opengl PUBLIC "${OPENGL_INCLUDE_DIR}")
  target_link_libraries(dgl-opengl PRIVATE dgl-opengl-definitions "${OPENGL_gl_LIBRARY}")
endfunction()

# daf__add_dgl_opengl3
# ------------------------------------------------------------------------------
#
# Add the OpenGL3 variant of DGL, if not already available.
#
function(daf__add_dgl_opengl3 SHARED_RESOURCES USE_FILE_BROWSER USE_WEB_VIEW)
  if(TARGET dgl-opengl3)
    return()
  endif()

  if(NOT OpenGL_GL_PREFERENCE)
    set(OpenGL_GL_PREFERENCE "LEGACY")
  endif()

  find_package(OpenGL REQUIRED)

  daf__add_static_library(dgl-opengl3 STATIC
    "${DAF_ROOT_DIR}/dgl/src/Application.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ApplicationPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Color.cpp"
    "${DAF_ROOT_DIR}/dgl/src/EventHandlers.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Geometry.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBase.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBaseWidgets.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Layout.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Widget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Window.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WindowPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL.cpp"
    "${DAF_ROOT_DIR}/dgl/src/OpenGL3.cpp"
    "${DAF_ROOT_DIR}/dgl/src/NanoVG.cpp")
  if(SHARED_RESOURCES)
    target_sources(dgl-opengl3 PRIVATE "${DAF_ROOT_DIR}/dgl/src/Resources.cpp")
  else()
    target_compile_definitions(dgl-opengl3 PUBLIC "DGL_NO_SHARED_RESOURCES")
  endif()
  if(APPLE)
    target_sources(dgl-opengl3 PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.mm")
  else()
    target_sources(dgl-opengl3 PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.cpp")
  endif()
  target_include_directories(dgl-opengl3 PUBLIC
    "${DAF_ROOT_DIR}/dgl")
  target_include_directories(dgl-opengl3 PUBLIC
    "${DAF_ROOT_DIR}/dgl/src/pugl-upstream/include")

  if(APPLE)
    target_compile_definitions(dgl-opengl3 PUBLIC "GL_SILENCE_DEPRECATION")
  endif()

  if(USE_FILE_BROWSER)
    target_compile_definitions(dgl-opengl3 PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(USE_WEB_VIEW)
    target_compile_definitions(dgl-opengl3 PUBLIC "DGL_USE_WEB_VIEW")
    if(APPLE)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries(dgl-opengl3 PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    elseif(WIN32)
      target_sources(dgl-opengl3 PRIVATE
        "${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp")
      set_source_files_properties("${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp"
        PROPERTIES
          COMPILE_FLAGS
            $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    endif()
  endif()

  daf__add_dgl_system_libs()
  target_link_libraries(dgl-opengl3 PRIVATE dgl-system-libs)

  add_library(dgl-opengl3-definitions INTERFACE)
  target_compile_definitions(dgl-opengl3-definitions
    INTERFACE
      DGL_USE_OPENGL3
      DGL_OPENGL
      HAVE_OPENGL
      HAVE_DGL
  )

  target_include_directories(dgl-opengl3 PUBLIC "${OPENGL_INCLUDE_DIR}")
  target_link_libraries(dgl-opengl3 PRIVATE dgl-opengl3-definitions "${OPENGL_gl_LIBRARY}")
endfunction()

# daf__add_dgl_vulkan
# ------------------------------------------------------------------------------
#
# Add the Vulkan variant of DGL, if not already available.
#
function(daf__add_dgl_vulkan SHARED_RESOURCES USE_FILE_BROWSER USE_WEB_VIEW)
  if(TARGET dgl-vulkan)
    return()
  endif()

  find_package(Vulkan REQUIRED)

  daf__add_static_library(dgl-vulkan STATIC
    "${DAF_ROOT_DIR}/dgl/src/Application.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ApplicationPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Color.cpp"
    "${DAF_ROOT_DIR}/dgl/src/EventHandlers.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Geometry.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBase.cpp"
    "${DAF_ROOT_DIR}/dgl/src/ImageBaseWidgets.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Layout.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/SubWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/TopLevelWidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Widget.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WidgetPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Window.cpp"
    "${DAF_ROOT_DIR}/dgl/src/WindowPrivateData.cpp"
    "${DAF_ROOT_DIR}/dgl/src/Vulkan.cpp")
  if(SHARED_RESOURCES)
    target_sources(dgl-vulkan PRIVATE "${DAF_ROOT_DIR}/dgl/src/Resources.cpp")
  else()
    target_compile_definitions(dgl-vulkan PUBLIC "DGL_NO_SHARED_RESOURCES")
  endif()
  if(APPLE)
    target_sources(dgl-vulkan PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.mm")
  else()
    target_sources(dgl-vulkan PRIVATE
      "${DAF_ROOT_DIR}/dgl/src/pugl.cpp")
  endif()
  target_include_directories(dgl-vulkan PUBLIC
    "${DAF_ROOT_DIR}/dgl")
  target_include_directories(dgl-vulkan PUBLIC
    "${DAF_ROOT_DIR}/dgl/src/pugl-upstream/include")

  if(APPLE)
    target_compile_definitions(dgl-vulkan PUBLIC "GL_SILENCE_DEPRECATION")
  endif()

  if(USE_FILE_BROWSER)
    target_compile_definitions(dgl-vulkan PUBLIC "DGL_USE_FILE_BROWSER")
  endif()

  if(USE_WEB_VIEW)
    target_compile_definitions(dgl-vulkan PUBLIC "DGL_USE_WEB_VIEW")
    if(APPLE)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries(dgl-vulkan PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    elseif(WIN32)
      target_sources(dgl-vulkan PRIVATE
        "${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp")
      set_source_files_properties("${DAF_ROOT_DIR}/dgl/src/WebViewWin32.cpp"
        PROPERTIES
          COMPILE_FLAGS
            $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    endif()
  endif()

  daf__add_dgl_system_libs()
  target_link_libraries(dgl-vulkan PRIVATE dgl-system-libs)

  add_library(dgl-vulkan-definitions INTERFACE)
  target_compile_definitions(dgl-vulkan-definitions INTERFACE "DGL_VULKAN" "HAVE_VULKAN" "HAVE_DGL")

  target_include_directories(dgl-vulkan PUBLIC "${OPENGL_INCLUDE_DIR}")
  target_link_libraries(dgl-vulkan PRIVATE dgl-vulkan-definitions "${OPENGL_gl_LIBRARY}")
endfunction()

# daf__add_plugin_specific_ui_sources
# ------------------------------------------------------------------------------
#
# Compile system specific files
#
function(daf__add_plugin_specific_ui_sources NAME USE_WEB_VIEW)
  if(APPLE)
    target_sources("${NAME}" PRIVATE
      "${DAF_ROOT_DIR}/daf/DafUI_macOS.mm")
    if(USE_WEB_VIEW)
      find_library(APPLE_WEBKIT_FRAMEWORK "WebKit")
      target_link_libraries("${NAME}" PRIVATE "${APPLE_WEBKIT_FRAMEWORK}")
    endif()
  elseif(WIN32 AND USE_WEB_VIEW)
    target_sources("${NAME}" PRIVATE
      "${DAF_ROOT_DIR}/daf/DafUI_win32.cpp")
    set_source_files_properties("${DAF_ROOT_DIR}/daf/DafUI_win32.cpp"
      PROPERTIES
        COMPILE_FLAGS
          $<IF:$<BOOL:${MSVC}>,/std:c++17,-std=gnu++17>)
    target_link_libraries("${NAME}" PRIVATE "ole32" "uuid")
  endif()
endfunction()

# Widget libraries
# ------------------------------------------------------------------------------

function(daf__add_widgets_dusk)
  if(TARGET daf-widgets-dusk)
    return()
  endif()
  daf__add_static_library(daf-widgets-dusk STATIC
    "${DAF_ROOT_DIR}/widgets/dusk/DuskWidgets.cpp")
  target_include_directories(daf-widgets-dusk PUBLIC
    "${DAF_ROOT_DIR}/widgets/dusk"
    "${DAF_ROOT_DIR}/widgets/imgui")
  target_compile_features(daf-widgets-dusk PUBLIC cxx_std_11)
endfunction()

function(daf__add_widgets_imgui DGL_TARGET)
  if(TARGET daf-widgets-imgui)
    return()
  endif()
  if(NOT TARGET ${DGL_TARGET} OR
     NOT (DGL_TARGET STREQUAL "dgl-opengl3" OR DGL_TARGET STREQUAL "dgl-opengl"))
    message(FATAL_ERROR "Widget bridge requires an existing dgl-opengl3 or dgl-opengl target")
  endif()
  daf__add_widgets_dusk()
  # DearImGui.cpp includes ImGui, the renderer, knobs and toggles as one translation unit.
  daf__add_static_library(daf-widgets-imgui STATIC
    "${DAF_ROOT_DIR}/widgets/imgui/DearImGui.cpp")
  target_include_directories(daf-widgets-imgui PUBLIC "${DAF_ROOT_DIR}/widgets/imgui")
  target_link_libraries(daf-widgets-imgui PUBLIC
    ${DGL_TARGET} ${DGL_TARGET}-definitions dgl-system-libs-definitions daf-widgets-dusk)
  target_compile_features(daf-widgets-imgui PUBLIC cxx_std_11)
  target_compile_options(daf-widgets-imgui PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-cpp;-Wno-stringop-overflow>
    $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-cpp>)
  if(MSVC AND DGL_TARGET STREQUAL "dgl-opengl3")
    target_sources(daf-widgets-imgui PRIVATE "${DAF_ROOT_DIR}/widgets/imgui/Win32GlLoader.cpp")
    # A basename keeps /FI working when the checkout path contains spaces.
    set_source_files_properties("${DAF_ROOT_DIR}/widgets/imgui/DearImGui.cpp"
      PROPERTIES COMPILE_OPTIONS "/FIWin32GlLoader.h")
  endif()
endfunction()

# daf__add_dgl_system_libs
# ------------------------------------------------------------------------------
#
# Find system libraries required by DGL and add them as an interface target.
#
function(daf__add_dgl_system_libs)
  if(TARGET dgl-system-libs)
    return()
  endif()
  add_library(dgl-system-libs INTERFACE)
  add_library(dgl-system-libs-definitions INTERFACE)
  if(APPLE)
    find_library(APPLE_COCOA_FRAMEWORK "Cocoa")
    find_library(APPLE_COREVIDEO_FRAMEWORK "CoreVideo")
    target_link_libraries(dgl-system-libs INTERFACE "${APPLE_COCOA_FRAMEWORK}" "${APPLE_COREVIDEO_FRAMEWORK}")
  elseif(EMSCRIPTEN)
  elseif(HAIKU)
    target_link_libraries(dgl-system-libs INTERFACE "be")
  elseif(WIN32)
    target_link_libraries(dgl-system-libs INTERFACE "comdlg32" "dwmapi" "gdi32")
  else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(DBUS "dbus-1")
    if(DBUS_FOUND)
      target_compile_definitions(dgl-system-libs-definitions INTERFACE "HAVE_DBUS")
      target_include_directories(dgl-system-libs INTERFACE "${DBUS_INCLUDE_DIRS}")
      target_link_libraries(dgl-system-libs INTERFACE "${DBUS_LIBRARIES}")
    endif()
    # X11 is no longer REQUIRED on its own: a Wayland-only system (no libx11-dev) is a valid target.
    # Note that X11 and Wayland are commonly both installed, and in that case X11 still wins -- it is
    # the backend DGL compiles against, because plugin hosts embed UIs via X11 and standalones can
    # fall back to XWayland. See dgl/src/pugl.hpp for the same policy expressed in the preprocessor.
    # Only the client libraries are probed: the xdg-shell, xdg-decoration, viewporter and
    # fractional-scale bindings are pre-generated and vendored in dgl/src/pugl-extra/wayland-protocols,
    # so neither the wayland-protocols data package nor wayland-scanner has to be installed.
    find_package(X11)
    pkg_check_modules(WAYLAND "wayland-client" "wayland-egl" "wayland-cursor" "xkbcommon" "egl")
    if(NOT X11_FOUND AND NOT WAYLAND_FOUND)
      message(FATAL_ERROR
        "DGL needs a windowing backend, but neither was found.\n"
        "  X11     : install the X11 development files (e.g. libx11-dev / libX11-devel).\n"
        "  Wayland : install wayland-client, wayland-egl, wayland-cursor, xkbcommon and egl "
        "development files (e.g. libwayland-dev, libxkbcommon-dev, libegl-dev).\n"
        "Installing either one is enough; X11 is preferred when both are present.")
    endif()

    # auto keeps the historical behaviour, where a machine that has the X11 development files
    # installed builds the X11 backend whether or not it ever runs an X server. An application
    # targeting Wayland has to be able to say so, since on a dual-stack developer box the
    # accident is silent: everything builds and runs, just against XWayland.
    set(DGL_BACKEND "auto" CACHE STRING "DGL windowing backend on Linux: auto, x11 or wayland")
    set_property(CACHE DGL_BACKEND PROPERTY STRINGS auto x11 wayland)
    if(NOT DGL_BACKEND MATCHES "^(auto|x11|wayland)$")
      message(FATAL_ERROR "DGL_BACKEND must be auto, x11 or wayland, not '${DGL_BACKEND}'.")
    endif()
    if(DGL_BACKEND STREQUAL "x11" AND NOT X11_FOUND)
      message(FATAL_ERROR "DGL_BACKEND=x11, but the X11 development files were not found.")
    endif()
    if(DGL_BACKEND STREQUAL "wayland" AND NOT WAYLAND_FOUND)
      message(FATAL_ERROR
        "DGL_BACKEND=wayland, but wayland-client, wayland-egl, wayland-cursor, xkbcommon or egl "
        "development files were not found.")
    endif()

    if(X11_FOUND AND NOT DGL_BACKEND STREQUAL "wayland")
      target_compile_definitions(dgl-system-libs-definitions INTERFACE "HAVE_X11")
      target_include_directories(dgl-system-libs INTERFACE "${X11_INCLUDE_DIR}")
      target_link_libraries(dgl-system-libs INTERFACE "${X11_X11_LIB}")
      if(X11_Xcursor_FOUND)
        target_compile_definitions(dgl-system-libs-definitions INTERFACE "HAVE_XCURSOR")
        target_link_libraries(dgl-system-libs INTERFACE "${X11_Xcursor_LIB}")
      endif()
      if(X11_Xext_FOUND)
        target_compile_definitions(dgl-system-libs-definitions INTERFACE "HAVE_XEXT")
        target_link_libraries(dgl-system-libs INTERFACE "${X11_Xext_LIB}")
      endif()
      if(X11_Xrandr_FOUND)
        target_compile_definitions(dgl-system-libs-definitions INTERFACE "HAVE_XRANDR")
        target_link_libraries(dgl-system-libs INTERFACE "${X11_Xrandr_LIB}")
      endif()
      if(X11_XSync_FOUND)
        target_compile_definitions(dgl-system-libs-definitions INTERFACE "HAVE_XSYNC")
        # FindX11 only looks for X11/extensions/sync.h here and defines no X11_XSync_LIB: the XSync
        # entry points live in libXext, so that is what has to be linked.
        target_link_libraries(dgl-system-libs INTERFACE "${X11_Xext_LIB}")
      endif()
      message(STATUS "DGL windowing backend: X11")
    else()
      # Reached when X11 is absent, or when DGL_BACKEND asked for Wayland. A dual-stack build
      # that did not ask keeps taking X11, so no existing build starts dragging in unused
      # libraries.
      target_compile_definitions(dgl-system-libs-definitions INTERFACE "HAVE_WAYLAND")
      target_include_directories(dgl-system-libs INTERFACE "${WAYLAND_INCLUDE_DIRS}")
      # Not daf__target_link_directories: it uses PUBLIC, which an INTERFACE library cannot take.
      link_directories(${WAYLAND_LIBRARY_DIRS})
      target_link_libraries(dgl-system-libs INTERFACE "${WAYLAND_LIBRARIES}")
      # The clipboard code calls pthread_sigmask to keep SIGPIPE off the GUI thread during a
      # transfer. glibc 2.34 and later have it in libc, but older ones keep it in libpthread.
      set(THREADS_PREFER_PTHREAD_FLAG TRUE)
      find_package(Threads REQUIRED)
      target_link_libraries(dgl-system-libs INTERFACE Threads::Threads)
      if(X11_FOUND)
        message(STATUS "DGL windowing backend: Wayland (requested)")
      else()
        message(STATUS "DGL windowing backend: Wayland (X11 not found)")
      endif()
    endif()
   endif()

   if(MSVC)
     # The MSVC toolchain has no GL/glext.h nor KHR/khrplatform.h, so use the copies vendored in
     # dgl/src/khronos (see the README there). These used to be downloaded at configure time, which
     # made the build depend on the network and on whatever the registry happened to serve that day.
     target_include_directories(dgl-system-libs-definitions INTERFACE "${DAF_ROOT_DIR}/dgl/src/khronos")
   endif()

  target_link_libraries(dgl-system-libs INTERFACE dgl-system-libs-definitions)
endfunction()

# daf__add_executable
# ------------------------------------------------------------------------------
#
# Adds an executable target, and set some default properties on the target.
#
function(daf__add_executable NAME)
  add_executable("${NAME}" ${ARGN})
  daf__set_target_defaults("${NAME}")
  if(MINGW)
    target_link_libraries("${NAME}" PRIVATE "-static")
  endif()
endfunction()

# daf__add_module
# ------------------------------------------------------------------------------
#
# Adds a module target, and set some default properties on the target.
#
function(daf__add_module NAME)
  add_library("${NAME}" MODULE ${ARGN})
  daf__set_target_defaults("${NAME}")
  if(APPLE)
    set_target_properties("${NAME}" PROPERTIES SUFFIX ".dylib")
  elseif(MINGW)
    target_link_libraries("${NAME}" PRIVATE "-static")
  endif()
endfunction()

# daf__add_static_library
# ------------------------------------------------------------------------------
#
# Adds a static library target, and set some default properties on the target.
#
function(daf__add_static_library NAME)
  add_library("${NAME}" STATIC ${ARGN})
  daf__set_target_defaults("${NAME}")
endfunction()

# daf__set_module_export_list
# ------------------------------------------------------------------------------
#
# Applies a list of exported symbols to the module target.
#
function(daf__set_module_export_list NAME EXPORTS)
  if(WIN32)
    target_sources("${NAME}" PRIVATE "${DAF_ROOT_DIR}/utils/symbols/${EXPORTS}.def")
  elseif(APPLE)
    set_property(TARGET "${NAME}" APPEND PROPERTY LINK_OPTIONS
      "-Xlinker" "-exported_symbols_list"
      "-Xlinker" "${DAF_ROOT_DIR}/utils/symbols/${EXPORTS}.exp")
  else()
    set_property(TARGET "${NAME}" APPEND PROPERTY LINK_OPTIONS
      "-Xlinker" "--version-script=${DAF_ROOT_DIR}/utils/symbols/${EXPORTS}.version")
  endif()
endfunction()

# daf__set_msvc_debug_file_names
# ------------------------------------------------------------------------------
#
# Names the MSVC linker's .pdb and .ilk files after the target instead of the
# output file.
#
# The JACK program and the Windows CLAP module both land in bin/ as `NAME`
# (NAME.exe and NAME.clap). The linker derives the .pdb and the incremental-link
# .ilk from the output base name, so both targets would write bin/NAME.pdb and
# bin/NAME.ilk, and a parallel Debug or RelWithDebInfo build fails at random.
# Naming them after the target (NAME-jack.pdb, NAME-clap.ilk, ...) leaves the
# shipped binaries' names unchanged. Every other format already has a unique
# output name or a directory of its own.
#
function(daf__set_msvc_debug_file_names TARGET)
  if(NOT MSVC)
    return()
  endif()
  set_target_properties("${TARGET}" PROPERTIES PDB_NAME "${TARGET}")
  # Only link.exe writes .ilk files (lld-link does not link incrementally, and
  # may not know the option), and only under /INCREMENTAL, which CMake's Debug
  # and RelWithDebInfo flags enable. The file is not shipped; it goes next to
  # the binary, where the output directory is known to exist.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    get_target_property(_type "${TARGET}" TYPE)
    if(_type STREQUAL "EXECUTABLE")
      get_target_property(_dir "${TARGET}" RUNTIME_OUTPUT_DIRECTORY)
    else()
      get_target_property(_dir "${TARGET}" LIBRARY_OUTPUT_DIRECTORY)
    endif()
    target_link_options("${TARGET}" PRIVATE "/ILK:${_dir}${TARGET}.ilk")
  endif()
endfunction()

# daf__set_target_defaults
# ------------------------------------------------------------------------------
#
# Set default properties which must apply to all DAF-defined targets.
#
function(daf__set_target_defaults NAME)
  set_target_properties("${NAME}" PROPERTIES
    POSITION_INDEPENDENT_CODE TRUE
    C_VISIBILITY_PRESET "hidden"
    CXX_VISIBILITY_PRESET "hidden"
    VISIBILITY_INLINES_HIDDEN TRUE)
  if(WIN32)
    target_compile_definitions("${NAME}" PUBLIC "NOMINMAX")
  endif()
  if (MINGW)
    target_compile_options("${NAME}" PUBLIC "-mstackrealign")
  endif()
  if (MSVC)
    target_compile_options("${NAME}" PUBLIC "/UTF-8")
    target_compile_definitions("${NAME}" PUBLIC "_CRT_SECURE_NO_WARNINGS")
  endif()
  if (CMAKE_COMPILER_IS_GNUCXX)
    target_compile_options("${NAME}" PUBLIC "-fno-gnu-unique")
  endif()
  if ((NOT APPLE) AND (NOT EMSCRIPTEN) AND (NOT MSVC))
    target_link_options("${NAME}" PUBLIC "-Wl,--no-undefined")
  endif()
endfunction()

# daf__add_plugin_main
# ------------------------------------------------------------------------------
#
# Adds plugin code to the given target.
#
function(daf__add_plugin_main NAME TARGET)
  target_sources("${NAME}" PRIVATE
    "${DAF_ROOT_DIR}/daf/DafPluginMain.cpp")
  daf__add_plugin_target_definition("${NAME}" "${TARGET}")
endfunction()

# daf__add_ui_main
# ------------------------------------------------------------------------------
#
# Adds UI code to the given target (only if the target has UI).
#
function(daf__add_ui_main NAME TARGET HAS_UI)
  if(HAS_UI)
    target_sources("${NAME}" PRIVATE
      "${DAF_ROOT_DIR}/daf/DafUIMain.cpp")
    daf__add_plugin_target_definition("${NAME}" "${TARGET}")
  endif()
endfunction()

# daf__add_plugin_target_definition
# ------------------------------------------------------------------------------
#
# Adds the plugins target macro definition.
# This selects which entry file is compiled according to the target type.
#
function(daf__add_plugin_target_definition NAME TARGET)
  string(TOUPPER "${TARGET}" _upperTarget)
  target_compile_definitions("${NAME}" PRIVATE "DAF_PLUGIN_TARGET_${_upperTarget}")
endfunction()

# daf__add_lv2_ttl_generator
# ------------------------------------------------------------------------------
#
# Build the LV2 TTL generator.
#
function(daf__add_lv2_ttl_generator)
  if(TARGET lv2_ttl_generator)
    return()
  endif()
  add_executable(lv2_ttl_generator "${DAF_ROOT_DIR}/utils/lv2-ttl-generator/lv2_ttl_generator.c")
  if((NOT WIN32) AND (NOT APPLE) AND (NOT HAIKU))
    target_link_libraries(lv2_ttl_generator PRIVATE "dl")
  endif()
endfunction()

# daf__ensure_sources_non_empty
# ------------------------------------------------------------------------------
#
# Ensure the given source list contains at least one file.
# The function appends an empty source file to the list if necessary.
# This is useful when CMake does not permit to add targets without sources.
#
function(daf__ensure_sources_non_empty VAR)
  if(NOT "" STREQUAL "${${VAR}}")
    return()
  endif()
  set(_file "${CMAKE_CURRENT_BINARY_DIR}/_daf_empty.c")
  if(NOT EXISTS "${_file}")
    file(WRITE "${_file}" "")
  endif()
  set("${VAR}" "${_file}" PARENT_SCOPE)
endfunction()

# daf__create_dummy_source_list
# ------------------------------------------------------------------------------
#
# Create a dummy source list which is equivalent to compiling nothing.
# This is only for compatibility with older CMake versions, which refuse to add
# targets without any sources.
#
macro(daf__create_dummy_source_list VAR)
  set("${VAR}")
  if(CMAKE_VERSION VERSION_LESS "3.11")
    daf__ensure_sources_non_empty("${VAR}")
  endif()
endmacro()

# daf__target_link_directories
# ------------------------------------------------------------------------------
#
# Call `target_link_directories` if cmake >= 3.13,
# otherwise fallback to global `link_directories`.
#
macro(daf__target_link_directories NAME DIRS)
  if(CMAKE_VERSION VERSION_GREATER_EQUAL "3.13")
    target_link_directories("${NAME}" PUBLIC ${DIRS})
  else()
    link_directories(${DIRS})
  endif()
endmacro()

# daf__warn_once
# ------------------------------------------------------------------------------
#
# Prints a warning message once only.
#
function(daf__warn_once_only TOKEN MESSAGE)
  get_property(_warned GLOBAL PROPERTY "daf__have_warned_${TOKEN}")
  if(NOT _warned)
    set_property(GLOBAL PROPERTY "daf__have_warned_${TOKEN}" TRUE)
    message(WARNING "${MESSAGE}")
  endif()
endfunction()
