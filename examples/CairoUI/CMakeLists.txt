# CMake file for DAF example plugins #
# ----------------------------------- #

daf_add_plugin(d_cairoui
  TARGETS clap dssi jack lv2 vst2 vst3
  UI_TYPE cairo
  FILES_DSP
      CairoExamplePlugin.cpp
  FILES_UI
      Artwork.cpp
      CairoExampleUI.cpp)

target_include_directories(
  d_cairoui PUBLIC ".")
