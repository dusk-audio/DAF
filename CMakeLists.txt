# DISTRHO Plugin Framework (DPF)
# Copyright (C) 2021 Jean Pierre Cimalando <jp-dev@inbox.ru>
# Copyright (C) 2022-2025 Filipe Coelho <falktx@falktx.com>
#
# SPDX-License-Identifier: ISC

cmake_minimum_required(VERSION 3.8...3.31)

# macOS deployment target.
#
# CMAKE_OSX_DEPLOYMENT_TARGET has to be set *before* project(), since project() runs the compiler
# ABI and feature detection with it and selects the SDK accordingly. Assigning it after project()
# does still reach most per-target compile flags at generate time, but the detection results then no
# longer match the flags actually used, so that is not a reliable place to set it.
#
# DAF is normally consumed as a subproject, where the parent project's project() call has already
# run and this file can no longer influence it. Parent projects must therefore set
# CMAKE_OSX_DEPLOYMENT_TARGET themselves (or export MACOSX_DEPLOYMENT_TARGET) before their own
# project() call. The default below only kicks in when DAF is the top-level project, and never
# overrides an explicit choice.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR
   AND NOT DEFINED CMAKE_SYSTEM_NAME
   AND NOT CMAKE_OSX_DEPLOYMENT_TARGET
   AND NOT DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
  # CMAKE_HOST_SYSTEM_NAME is filled in by CMakeDetermineSystem, which runs as part of project(),
  # so it cannot be relied on this early. Ask the host directly instead; on hosts without uname
  # (Windows) the call is a silent no-op and leaves the variable empty, which is fine here.
  execute_process(COMMAND uname -s
                  OUTPUT_VARIABLE DAF_HOST_UNAME
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
  # 11.0, as in Makefile.base.mk: the oldest macOS the libc++ of current SDKs still supports
  if(DAF_HOST_UNAME STREQUAL "Darwin")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version to target")
  endif()
  unset(DAF_HOST_UNAME)
endif()

project(DAF)

# ensure c++11 at minimum, the parent project can override
if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 11)
endif()

# check if we are building from this project, or are imported by another
if(PROJECT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(DAF_BUILD_FROM_HERE TRUE)
else()
  set(DAF_BUILD_FROM_HERE FALSE)
endif()

option(DAF_LIBRARIES "Build the libraries" "${DAF_BUILD_FROM_HERE}")
option(DAF_WIDGETS_GALLERY "Build the widget gallery" "${DAF_BUILD_FROM_HERE}")
option(DAF_EXAMPLES "Build the examples" "${DAF_BUILD_FROM_HERE}")

set(DAF_ROOT_DIR "${PROJECT_SOURCE_DIR}" CACHE INTERNAL
  "Root directory of the Dusk Audio Framework.")

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(DAF-plugin)

if(DAF_LIBRARIES)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(CAIRO "cairo")
    if(CAIRO_FOUND AND (NOT HAIKU))
      daf__add_dgl_cairo(TRUE TRUE TRUE)
    endif()
  endif()
  daf__add_dgl_external(TRUE TRUE)
  daf__add_dgl_opengl(TRUE TRUE TRUE)
  daf__add_dgl_opengl3(TRUE TRUE TRUE)
  daf__add_widgets_dusk()
  daf__add_widgets_imgui(dgl-opengl3)
endif()

if(DAF_EXAMPLES)
  find_package(PkgConfig QUIET)
  if(PKG_CONFIG_FOUND)
    pkg_check_modules(CAIRO "cairo")
    if(CAIRO_FOUND AND (NOT HAIKU))
      add_subdirectory("examples/CairoUI")
    endif()
  endif()
  add_subdirectory("examples/EmbedExternalUI")
  add_subdirectory("examples/FileHandling")
  add_subdirectory("examples/Info")
  add_subdirectory("examples/Latency")
  add_subdirectory("examples/Meters")
  add_subdirectory("examples/MidiThrough")
  add_subdirectory("examples/Parameters")
  add_subdirectory("examples/States")
  add_subdirectory("examples/WebMeters")
endif()

if(DAF_WIDGETS_GALLERY)
  add_subdirectory(widgets/tests)
endif()
