// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

/* A DGL_BACKEND=both build loads its Wayland libraries when Wayland is first tried, rather than linking
   them, so that a machine without them still starts the program and runs it on X11. See
   dgl/src/runtime-backend.md.

   pugl.cpp includes this before the libwayland, xkbcommon and EGL headers. Each macro turns one of the
   functions the Wayland backend calls into a call through a pointer: the header's declaration of
   `wl_display_connect` becomes the declaration of an accessor `dgl_wl_display_connect()` returning a
   pointer of the function's own type, and every call becomes `(*dgl_wl_display_connect())(...)`.
   wayland-loader.hpp defines the accessors and fills the pointers from dlopen.

   The two lists have to agree, and the build says so when they do not: an accessor declared here but
   not defined there is an undefined symbol at link time, and an entry there with no macro here does
   not compile. A call to any other function of these libraries is an undefined symbol as well, since
   a both build does not link them. */

#ifndef DGL_WAYLAND_LOADER_NAMES_H_INCLUDED
#define DGL_WAYLAND_LOADER_NAMES_H_INCLUDED

// libwayland-client
#define wl_display_cancel_read       (*dgl_wl_display_cancel_read())
#define wl_display_connect           (*dgl_wl_display_connect())
#define wl_display_disconnect        (*dgl_wl_display_disconnect())
#define wl_display_dispatch_pending  (*dgl_wl_display_dispatch_pending())
#define wl_display_flush             (*dgl_wl_display_flush())
#define wl_display_get_fd            (*dgl_wl_display_get_fd())
#define wl_display_prepare_read      (*dgl_wl_display_prepare_read())
#define wl_display_read_events       (*dgl_wl_display_read_events())
#define wl_display_roundtrip         (*dgl_wl_display_roundtrip())
#define wl_proxy_add_listener        (*dgl_wl_proxy_add_listener())
#define wl_proxy_destroy             (*dgl_wl_proxy_destroy())
#define wl_proxy_get_user_data       (*dgl_wl_proxy_get_user_data())
#define wl_proxy_get_version         (*dgl_wl_proxy_get_version())
#define wl_proxy_marshal_flags       (*dgl_wl_proxy_marshal_flags())
#define wl_proxy_set_user_data       (*dgl_wl_proxy_set_user_data())

// libwayland-cursor
#define wl_cursor_image_get_buffer   (*dgl_wl_cursor_image_get_buffer())
#define wl_cursor_theme_destroy      (*dgl_wl_cursor_theme_destroy())
#define wl_cursor_theme_get_cursor   (*dgl_wl_cursor_theme_get_cursor())
#define wl_cursor_theme_load         (*dgl_wl_cursor_theme_load())

// libwayland-egl
#define wl_egl_window_create         (*dgl_wl_egl_window_create())
#define wl_egl_window_destroy        (*dgl_wl_egl_window_destroy())
#define wl_egl_window_resize         (*dgl_wl_egl_window_resize())

// libxkbcommon
#define xkb_compose_state_feed            (*dgl_xkb_compose_state_feed())
#define xkb_compose_state_get_status      (*dgl_xkb_compose_state_get_status())
#define xkb_compose_state_get_utf8        (*dgl_xkb_compose_state_get_utf8())
#define xkb_compose_state_new             (*dgl_xkb_compose_state_new())
#define xkb_compose_state_reset           (*dgl_xkb_compose_state_reset())
#define xkb_compose_state_unref           (*dgl_xkb_compose_state_unref())
#define xkb_compose_table_new_from_locale (*dgl_xkb_compose_table_new_from_locale())
#define xkb_compose_table_unref           (*dgl_xkb_compose_table_unref())
#define xkb_context_new                   (*dgl_xkb_context_new())
#define xkb_context_unref                 (*dgl_xkb_context_unref())
#define xkb_keymap_key_get_syms_by_level  (*dgl_xkb_keymap_key_get_syms_by_level())
#define xkb_keymap_key_repeats            (*dgl_xkb_keymap_key_repeats())
#define xkb_keymap_new_from_string        (*dgl_xkb_keymap_new_from_string())
#define xkb_keymap_unref                  (*dgl_xkb_keymap_unref())
#define xkb_keysym_to_utf32               (*dgl_xkb_keysym_to_utf32())
#define xkb_state_key_get_layout          (*dgl_xkb_state_key_get_layout())
#define xkb_state_key_get_one_sym         (*dgl_xkb_state_key_get_one_sym())
#define xkb_state_key_get_utf8            (*dgl_xkb_state_key_get_utf8())
#define xkb_state_mod_name_is_active      (*dgl_xkb_state_mod_name_is_active())
#define xkb_state_new                     (*dgl_xkb_state_new())
#define xkb_state_unref                   (*dgl_xkb_state_unref())
#define xkb_state_update_mask             (*dgl_xkb_state_update_mask())

// libEGL. EGLAPI would declare each accessor visibility("default"), see wayland-loader.hpp.
#define EGLAPI
#define eglBindAPI              (*dgl_eglBindAPI())
#define eglChooseConfig         (*dgl_eglChooseConfig())
#define eglCreateContext        (*dgl_eglCreateContext())
#define eglCreateWindowSurface  (*dgl_eglCreateWindowSurface())
#define eglDestroyContext       (*dgl_eglDestroyContext())
#define eglDestroySurface       (*dgl_eglDestroySurface())
#define eglGetConfigAttrib      (*dgl_eglGetConfigAttrib())
#define eglGetCurrentContext    (*dgl_eglGetCurrentContext())
#define eglGetDisplay           (*dgl_eglGetDisplay())
#define eglGetPlatformDisplay   (*dgl_eglGetPlatformDisplay())
#define eglGetProcAddress       (*dgl_eglGetProcAddress())
#define eglInitialize           (*dgl_eglInitialize())
#define eglMakeCurrent          (*dgl_eglMakeCurrent())
#define eglSwapBuffers          (*dgl_eglSwapBuffers())
#define eglSwapInterval         (*dgl_eglSwapInterval())
#define eglTerminate            (*dgl_eglTerminate())

#endif // DGL_WAYLAND_LOADER_NAMES_H_INCLUDED
