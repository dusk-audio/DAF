#!/usr/bin/make -f
# The AU bundle AUViewDispose hosts (see ../AUViewDispose.cpp): bin/d_au_view_test.component #
# ------------------------------------------------------------------------------------------- #

NAME = d_au_view_test

FILES_DSP = ViewDisposePlugin.cpp
FILES_UI  = ViewDisposeUI.cpp

# The UI draws nothing, but AUViewDispose needs its paints: they read the plugin, the first one after a dispose edits
# a parameter, and one scenario has the UI resize itself from one. On a Mac with no GPU, as CI's virtual runners
# are, pugl finds no accelerated pixel format and falls back to NSOpenGLView's default legacy one, where the
# opengl3 renderer's core-profile shaders do not build, and it then skips every paint
# (Window::PrivateData::startContext). The opengl renderer paints whatever context it gets.
UI_TYPE = opengl

include ../../Makefile.plugins.mk

TARGETS = au

all: $(TARGETS)
