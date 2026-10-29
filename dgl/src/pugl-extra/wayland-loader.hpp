// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

/* The other half of wayland-loader-names.h, included by pugl.cpp after the libwayland, xkbcommon and EGL
   headers of a DGL_BACKEND=both build: the accessors those headers now declare, and the code that loads
   the libraries behind them.

   Functions are reached through pointers filled by dlsym, which also covers the variadic
   wl_proxy_marshal_flags every protocol request goes through. The wl_*_interface objects cannot be done
   that way: the generated protocol tables take their addresses in static initializers, long before
   anything could be loaded. They come from the core protocol vendored in wayland-protocols/ instead,
   compiled into DGL like the other protocol code (see the comment on it in pugl.cpp); libwayland copes
   with a second copy of an interface, as it compares interfaces by name when the pointers differ. Lazy
   PLT binding would have covered neither: data references are bound at load time, and a library named
   in DT_NEEDED has to exist for the program to start at all.

   The accessors are hidden in every build, so that plugins sharing a host cannot interpose each
   other's: the attribute on the definitions below is enough, provided no header declared them
   visibility("default") first, which is why wayland-loader-names.h empties EGLAPI and pugl.cpp
   rewrites xkbcommon's attribute. */

#ifndef DGL_WAYLAND_LOADER_HPP_INCLUDED
#define DGL_WAYLAND_LOADER_HPP_INCLUDED

#define DGL_WAYLAND_CLIENT_FUNCTIONS(X) \
    X(wl_display_cancel_read) \
    X(wl_display_connect) \
    X(wl_display_disconnect) \
    X(wl_display_dispatch_pending) \
    X(wl_display_flush) \
    X(wl_display_get_fd) \
    X(wl_display_prepare_read) \
    X(wl_display_read_events) \
    X(wl_display_roundtrip) \
    X(wl_proxy_add_listener) \
    X(wl_proxy_destroy) \
    X(wl_proxy_get_user_data) \
    X(wl_proxy_get_version) \
    X(wl_proxy_marshal_flags) \
    X(wl_proxy_set_user_data)

#define DGL_WAYLAND_CURSOR_FUNCTIONS(X) \
    X(wl_cursor_image_get_buffer) \
    X(wl_cursor_theme_destroy) \
    X(wl_cursor_theme_get_cursor) \
    X(wl_cursor_theme_load)

#define DGL_XKBCOMMON_FUNCTIONS(X) \
    X(xkb_compose_state_feed) \
    X(xkb_compose_state_get_status) \
    X(xkb_compose_state_get_utf8) \
    X(xkb_compose_state_new) \
    X(xkb_compose_state_reset) \
    X(xkb_compose_state_unref) \
    X(xkb_compose_table_new_from_locale) \
    X(xkb_compose_table_unref) \
    X(xkb_context_new) \
    X(xkb_context_unref) \
    X(xkb_keymap_key_get_syms_by_level) \
    X(xkb_keymap_key_repeats) \
    X(xkb_keymap_new_from_string) \
    X(xkb_keymap_unref) \
    X(xkb_keysym_to_utf32) \
    X(xkb_state_key_get_layout) \
    X(xkb_state_key_get_one_sym) \
    X(xkb_state_key_get_utf8) \
    X(xkb_state_mod_name_is_active) \
    X(xkb_state_new) \
    X(xkb_state_unref) \
    X(xkb_state_update_mask)

#define DGL_WAYLAND_EGL_FUNCTIONS(X) \
    X(wl_egl_window_create) \
    X(wl_egl_window_destroy) \
    X(wl_egl_window_resize)

#define DGL_EGL_FUNCTIONS(X) \
    X(eglBindAPI) \
    X(eglChooseConfig) \
    X(eglCreateContext) \
    X(eglCreateWindowSurface) \
    X(eglDestroyContext) \
    X(eglDestroySurface) \
    X(eglGetConfigAttrib) \
    X(eglGetCurrentContext) \
    X(eglGetDisplay) \
    X(eglGetPlatformDisplay) \
    X(eglGetProcAddress) \
    X(eglInitialize) \
    X(eglMakeCurrent) \
    X(eglSwapBuffers) \
    X(eglSwapInterval) \
    X(eglTerminate)

#define DGL_WAYLAND_DEFINE_ACCESSOR(name) \
    static decltype(dgl_##name()) dgl_pointer_##name = nullptr; \
    extern "C" __attribute__((visibility("hidden"))) decltype(dgl_##name()) dgl_##name() \
    { return dgl_pointer_##name; }

DGL_WAYLAND_CLIENT_FUNCTIONS(DGL_WAYLAND_DEFINE_ACCESSOR)
DGL_WAYLAND_CURSOR_FUNCTIONS(DGL_WAYLAND_DEFINE_ACCESSOR)
DGL_XKBCOMMON_FUNCTIONS(DGL_WAYLAND_DEFINE_ACCESSOR)
#ifdef DGL_OPENGL
DGL_WAYLAND_EGL_FUNCTIONS(DGL_WAYLAND_DEFINE_ACCESSOR)
DGL_EGL_FUNCTIONS(DGL_WAYLAND_DEFINE_ACCESSOR)
#endif

#undef DGL_WAYLAND_DEFINE_ACCESSOR

// Fills the pointers of one library, returning what was missing, or null.
#define DGL_WAYLAND_LOAD_SYMBOL(name)                                   \
    {                                                                   \
        void* const symbol = dlsym(library, #name);                     \
        if (symbol == nullptr)                                          \
            return dglWaylandFailure(soname, #name);                   \
        std::memcpy(&dgl_pointer_##name, &symbol, sizeof(symbol));      \
    }

static const char* dglWaylandFailure(const char* const soname, const char* const what)
{
    static char message[512];
    std::snprintf(message, sizeof(message), "%s: %s", soname, what);
    return message;
}

// Loads the libraries once per process and keeps them loaded: Wayland objects outlive any one world,
// and EGL in particular cannot be unloaded safely. Returns null on success, or what could not be loaded.
static const char* dglWaylandLoad()
{
    static const char* const failure = []() -> const char*
    {
        const char* soname;
        void* library;

       #define DGL_WAYLAND_OPEN(name)                                   \
        soname  = name;                                                 \
        library = dlopen(soname, RTLD_NOW | RTLD_LOCAL);                \
        if (library == nullptr)                                         \
            return dglWaylandFailure(soname, dlerror());

        DGL_WAYLAND_OPEN("libwayland-client.so.0")
        DGL_WAYLAND_CLIENT_FUNCTIONS(DGL_WAYLAND_LOAD_SYMBOL)

        DGL_WAYLAND_OPEN("libwayland-cursor.so.0")
        DGL_WAYLAND_CURSOR_FUNCTIONS(DGL_WAYLAND_LOAD_SYMBOL)

        DGL_WAYLAND_OPEN("libxkbcommon.so.0")
        DGL_XKBCOMMON_FUNCTIONS(DGL_WAYLAND_LOAD_SYMBOL)

       #ifdef DGL_OPENGL
        DGL_WAYLAND_OPEN("libwayland-egl.so.1")
        DGL_WAYLAND_EGL_FUNCTIONS(DGL_WAYLAND_LOAD_SYMBOL)

        DGL_WAYLAND_OPEN("libEGL.so.1")
        DGL_EGL_FUNCTIONS(DGL_WAYLAND_LOAD_SYMBOL)
       #endif

       #undef DGL_WAYLAND_OPEN
        return nullptr;
    }();

    return failure;
}

#undef DGL_WAYLAND_LOAD_SYMBOL

#endif // DGL_WAYLAND_LOADER_HPP_INCLUDED
