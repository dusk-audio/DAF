#!/usr/bin/make -f
# Makefile for DGL #
# ---------------- #
# Created by falkTX
#

# The Demo, FileBrowserDialog, NanoImage and NanoSubWidgets targets link ../build/libdgl-*.a, which the
# top-level Makefile builds with USE_WEB_VIEW=true (it exports it for the example plugins). Match that
# default, or the build config sentinels in dgl/src/Application.cpp resolve to the "off" variant here and
# the "on" one in the library, and every one of those targets fails to link.
# The unit tests do not link the library and do not care either way; an explicit USE_WEB_VIEW still wins.
USE_WEB_VIEW ?= true

include ../Makefile.base.mk

# ---------------------------------------------------------------------------------------------------------------------

BUILD_C_FLAGS   += $(DGL_FLAGS) -I..
BUILD_CXX_FLAGS += $(DGL_FLAGS) -I.. -I../dgl/src/pugl-upstream/include -DDONT_SET_USING_DGL_NAMESPACE
LINK_FLAGS      += -lpthread

# TODO fix within pugl
BUILD_CXX_FLAGS += -Wno-extra -Wno-missing-field-initializers

ifeq ($(MACOS),true)
BUILD_CXX_FLAGS += -ObjC++ -DGL_SILENCE_DEPRECATION -Wno-deprecated-declarations
endif

# ---------------------------------------------------------------------------------------------------------------------

MANUAL_TESTS  =
UNIT_TESTS    = AudioUnitBufferList Color HostKeyFilter IdleCallbacks ImGuiKnobs Point StateParser

ifeq ($(MACOS),true)
UNIT_TESTS   += CocoaRetiredView FileDragCocoa
endif

ifeq ($(WINDOWS),true)
UNIT_TESTS   += FileDragWin
../build/tests/FileDragWin$(APP_EXT): LINK_FLAGS += -lole32 -luuid -lshell32
endif

# ImGuiKnobs builds Dear ImGui itself, headless, and includes the knobs the way DearImGui.cpp does.
../build/tests/ImGuiKnobs.cpp.o: BUILD_CXX_FLAGS += -I../widgets/imgui

# Build-time checks: the object compiling is the test, so it is never linked or run.
# StaticTarget is compiled as a plugin, so it needs a DafPluginInfo.h of its own.
BUILD_CHECKS  = StaticTarget
../build/tests/StaticTarget.cpp.o: BUILD_CXX_FLAGS += -Istatic-target

# format wrappers compiled into the test, which then acts as the host (see plugin-wrappers/)
WRAPPER_TESTS = CLAPWrapper LADSPAWrapper LV2Wrapper VST2Wrapper VST3Wrapper
ifeq ($(MACOS),true)
WRAPPER_TESTS += AUWrapper
../build/tests/AUWrapper$(APP_EXT): LINK_FLAGS += -framework AudioToolbox -framework CoreFoundation
# the AU wrapper's DAF_PLUGIN_SIDECHAIN_ON_EXTRA_IO, on a sidechain plugin of its own (see au-sidechain/)
WRAPPER_TESTS += AUSidechainLayout
../build/tests/AUSidechainLayout.cpp.o: BUILD_CXX_FLAGS += -Iau-sidechain -I../daf
../build/tests/AUSidechainLayout$(APP_EXT): LINK_FLAGS += -framework AudioToolbox -framework CoreFoundation
endif
../build/tests/%Wrapper.cpp.o: BUILD_CXX_FLAGS += -Iplugin-wrappers -I../daf
ifeq ($(LINUX),true)
../build/tests/%Wrapper$(APP_EXT): LINK_FLAGS += -ldl
endif

# the LV2 wrapper's TTL exporter, run on a plugin of its own (see lv2-preset-export/)
../build/tests/LV2PresetExport.cpp.o: BUILD_CXX_FLAGS += -Ilv2-preset-export -I../daf
ifeq ($(LINUX),true)
../build/tests/LV2PresetExport$(APP_EXT): LINK_FLAGS += -ldl
endif

ifeq ($(HAVE_CAIRO),true)
MANUAL_TESTS += Demo.cairo
endif

ifeq ($(HAVE_OPENGL),true)
MANUAL_TESTS += Demo.opengl
MANUAL_TESTS += FileBrowserDialog
MANUAL_TESTS += NanoImage
MANUAL_TESTS += NanoSubWidgets
endif

ifneq ($(WASM),true)
UNIT_TESTS   += Application Thread
UNIT_TESTS   += $(WRAPPER_TESTS)
ifneq ($(WINDOWS),true)
UNIT_TESTS   += LV2PresetExport
endif
ifeq ($(HAVE_CAIRO),true)
UNIT_TESTS   += Window.cairo
endif
ifeq ($(HAVE_OPENGL),true)
UNIT_TESTS   += NanoSubWidgetsClip
UNIT_TESTS   += Window.opengl
endif
ifeq ($(HAVE_STUB),true)
UNIT_TESTS   += KnobEventHandler.stub
UNIT_TESTS   += Widget.stub
UNIT_TESTS   += Window.stub
endif
ifeq ($(HAVE_VULKAN),true)
UNIT_TESTS   += Window.vulkan
endif
endif

ifeq ($(DGL_BACKEND_WAYLAND),true)
UNIT_TESTS   += Wayland
endif

# opens a GL window on whichever backend a DGL_BACKEND=both build picks in the environment it runs in
ifeq ($(DGL_RUNTIME_BACKEND)$(HAVE_OPENGL),truetrue)
UNIT_TESTS   += RuntimeBackend
../build/tests/RuntimeBackend.cpp.o: BUILD_CXX_FLAGS += $(OPENGL_FLAGS) -DDGL_OPENGL -DDGL_USE_OPENGL3
../build/tests/RuntimeBackend$(APP_EXT): LINK_FLAGS += $(OPENGL_LIBS)
endif

# drives the X11 backend directly, so it needs one (macOS uses Cocoa even where X11 is installed)
ifeq ($(LINUX),true)
ifeq ($(HAVE_X11),true)
UNIT_TESTS   += X11Focus
endif
endif

# leaves out tests the environment cannot run, e.g. SKIP_UNIT_TESTS=X11Focus under a Wayland compositor
# with no X server (see headless-wayland.sh)
UNIT_TESTS    := $(filter-out $(SKIP_UNIT_TESTS),$(UNIT_TESTS))

MANUAL_TARGETS = $(MANUAL_TESTS:%=../build/tests/%$(APP_EXT))
UNIT_TARGET    = $(UNIT_TESTS:%=../build/tests/%$(APP_EXT))
CHECK_TARGETS  = $(BUILD_CHECKS:%=../build/tests/%.cpp.o)

ALL_OBJS  = $(MANUAL_TESTS:%=../build/tests/%.cpp.o)
ALL_OBJS += $(UNIT_TESTS:%=../build/tests/%.cpp.o)
ALL_OBJS += $(CHECK_TARGETS)

# ---------------------------------------------------------------------------------------------------------------------

all: $(MANUAL_TARGETS) $(UNIT_TARGET) $(CHECK_TARGETS)

# ---------------------------------------------------------------------------------------------------------------------

Demo.opengl: ../build/tests/Demo.opengl$(APP_EXT)
FileBrowserDialog: ../build/tests/FileBrowserDialog$(APP_EXT)
NanoImage: ../build/tests/NanoImage$(APP_EXT)
NanoSubWidgets: ../build/tests/NanoSubWidgets$(APP_EXT)

# ---------------------------------------------------------------------------------------------------------------------

define RUN_TEST

	${1}
endef

# 	valgrind --leak-check=full $@

run: $(UNIT_TARGET) $(CHECK_TARGETS)
	$(foreach TEST,$(UNIT_TARGET),$(call RUN_TEST,$(TEST)))

# ---------------------------------------------------------------------------------------------------------------------

clean:
	rm -rf ../build/tests

# ---------------------------------------------------------------------------------------------------------------------
# building steps

../build/tests/%.c.o: %.c
	-@mkdir -p ../build/tests
	@echo "Compiling $<"
	$(SILENT)$(CC) $< $(BUILD_C_FLAGS) -c -o $@

../build/tests/%.cpp.o: %.cpp
	-@mkdir -p ../build/tests
	@echo "Compiling $<"
	$(SILENT)$(CXX) $< $(BUILD_CXX_FLAGS) -c -o $@

../build/tests/%.cpp.cairo.o: %.cpp
	-@mkdir -p ../build/tests
	@echo "Compiling $< (Cairo)"
	$(SILENT)$(CXX) $< $(BUILD_CXX_FLAGS) $(CAIRO_FLAGS) -DDGL_CAIRO -c -o $@

../build/tests/%.cpp.opengl.o: %.cpp
	-@mkdir -p ../build/tests
	@echo "Compiling $< (OpenGL)"
	$(SILENT)$(CXX) $< $(BUILD_CXX_FLAGS) $(OPENGL_FLAGS) -DDGL_OPENGL -c -o $@

../build/tests/%.cpp.stub.o: %.cpp
	-@mkdir -p ../build/tests
	@echo "Compiling $< (Stub)"
	$(SILENT)$(CXX) $< $(BUILD_CXX_FLAGS) -c -o $@

../build/tests/%.cpp.vulkan.o: %.cpp
	-@mkdir -p ../build/tests
	@echo "Compiling $< (Vulkan)"
	$(SILENT)$(CXX) $< $(BUILD_CXX_FLAGS) $(OPENGL_FLAGS) -DDGL_VULKAN -c -o $@

# ---------------------------------------------------------------------------------------------------------------------
# linking steps

../build/tests/%$(APP_EXT): ../build/tests/%.cpp.o
	@echo "Linking $*"
	$(SILENT)$(CXX) $< $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) -o $@

../build/tests/%.cairo$(APP_EXT): ../build/tests/%.cpp.cairo.o
	@echo "Linking $*"
	$(SILENT)$(CXX) $< $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(CAIRO_LIBS) -o $@

../build/tests/%.opengl$(APP_EXT): ../build/tests/%.cpp.opengl.o
	@echo "Linking $*"
	$(SILENT)$(CXX) $< $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(OPENGL_LIBS) -o $@

../build/tests/%.stub$(APP_EXT): ../build/tests/%.cpp.stub.o
	@echo "Linking $*"
	$(SILENT)$(CXX) $< $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) -o $@

../build/tests/%.vulkan$(APP_EXT): ../build/tests/%.cpp.vulkan.o
	@echo "Linking $*"
	$(SILENT)$(CXX) $< $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(VULKAN_LIBS) -o $@

# ---------------------------------------------------------------------------------------------------------------------
# linking steps (special, links against DGL static lib)

../build/tests/Demo.cairo$(APP_EXT): ../build/tests/Demo.cpp.cairo.o ../build/libdgl-cairo.a
	@echo "Linking Demo (Cairo)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(CAIRO_LIBS) -o $@

../build/tests/Demo.opengl$(APP_EXT): ../build/tests/Demo.cpp.opengl.o ../build/libdgl-opengl.a
	@echo "Linking Demo (OpenGL)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(OPENGL_LIBS) -o $@

../build/tests/Demo.vulkan$(APP_EXT): ../build/tests/Demo.cpp.vulkan.o ../build/libdgl-vulkan.a
	@echo "Linking Demo (OpenGL)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(VULKAN_LIBS) -o $@

../build/tests/FileBrowserDialog$(APP_EXT): ../build/tests/FileBrowserDialog.cpp.o ../build/libdgl-opengl.a
	@echo "Linking FileBrowserDialog (OpenGL)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(OPENGL_LIBS) -o $@

../build/tests/NanoImage$(APP_EXT): ../build/tests/NanoImage.cpp.o ../build/libdgl-opengl.a
	@echo "Linking NanoImage (OpenGL)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(OPENGL_LIBS) -o $@

../build/tests/NanoSubWidgets$(APP_EXT): ../build/tests/NanoSubWidgets.cpp.o ../build/libdgl-opengl.a
	@echo "Linking NanoSubWidgets (OpenGL)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(OPENGL_LIBS) -o $@

../build/tests/NanoSubWidgetsClip$(APP_EXT): ../build/tests/NanoSubWidgetsClip.cpp.o ../build/libdgl-opengl.a
	@echo "Linking NanoSubWidgetsClip (OpenGL)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) $(OPENGL_LIBS) -o $@

../build/tests/KnobEventHandler.stub$(APP_EXT): ../build/tests/KnobEventHandler.cpp.stub.o ../build/libdgl-stub.a
	@echo "Linking KnobEventHandler (Stub)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) -o $@

../build/tests/Widget.stub$(APP_EXT): ../build/tests/Widget.cpp.stub.o ../build/libdgl-stub.a
	@echo "Linking Widget (Stub)"
	$(SILENT)$(CXX) $^ $(LINK_FLAGS) $(DGL_SYSTEM_LIBS) -o $@

# ---------------------------------------------------------------------------------------------------------------------

.PHONY: Demo.opengl FileBrowserDialog NanoImage NanoSubWidgets

-include $(ALL_OBJS:%.o=%.d)

# ---------------------------------------------------------------------------------------------------------------------
