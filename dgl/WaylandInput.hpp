// SPDX-License-Identifier: ISC

#ifndef DGL_WAYLAND_INPUT_HPP_INCLUDED
#define DGL_WAYLAND_INPUT_HPP_INCLUDED

#include "Base.hpp"

struct wl_keyboard_listener;
struct wl_pointer_listener;

START_NAMESPACE_DGL

/**
   Input callbacks for an application borrowing the host's Wayland seat.

   Forward every native callback in protocol order, using data as its first argument. Do not
   install these listeners on the host's proxies. Duplicate the keymap file descriptor before
   forwarding it; the keyboard callback closes its copy. Keep the application alive across calls.

   Query consumed immediately after a key or button callback. A consumed event must not reach
   host widgets or start host key repeat. Host seat bookkeeping still runs for every event.
   hasKeyboardFocus lets the host suppress widget modifier notifications while updating its own
   modifier state. releaseFocus returns keyboard input to the host without losing held releases.

   Stop forwarding before reset or application destruction. Call reset before replacing input
   proxies. Before resuming, replay the keymap, repeat settings, keyboard focus and held keys,
   modifier masks, and pointer focus and coordinates. All calls use the display owner's thread.
*/
struct WaylandInput {
    const ::wl_keyboard_listener* keyboard;
    const ::wl_pointer_listener* pointer;
    void* data;
    bool (*consumed)(void*);
    bool (*hasKeyboardFocus)(void*);
    void (*releaseFocus)(void*);
    void (*reset)(void*);
};

END_NAMESPACE_DGL

#endif
