# Runtime windowing backend (`DGL_BACKEND=both`)

By default DGL on Linux is built for exactly one windowing system: X11 whenever its development
files are installed, the native Wayland backend only where they are not. `DGL_BACKEND=both` builds
both into one binary, which picks X11 or Wayland when it opens its first window. One Linux binary
then runs natively on a Wayland desktop and on X11 where there is no compositor.

It is an explicit opt-in. `auto` still means X11 whenever X11 is installed, and an X11-only or
Wayland-only build compiles to exactly the symbols it would without this support.

## Build switch

Make and CMake both take `DGL_BACKEND` with the same values: `auto` (the default, X11 whenever its
development files are installed), `x11` (X11, or an error without its development files), `wayland`
(Wayland alone, built as on a system without the X11 development files) and `both`.

| `both` | Make | CMake |
|---|---|---|
| Option | `DGL_BACKEND=both` | `-DDGL_BACKEND=both` |
| Requires | every X11 and every Wayland development file | same |
| Defines | `HAVE_X11`, `HAVE_WAYLAND`, `DGL_RUNTIME_BACKEND` | same, on `dgl-system-libs-definitions` |
| Links | X11 libraries, `gl x11`, `-ldl` | X11 libraries, `OPENGL_gl_LIBRARY`, `CMAKE_DL_LIBS` |

`make features` reports `DGL_RUNTIME_BACKEND: Yes` for such a build. `both` is for Linux and the other
X11/Wayland systems; on macOS, Windows, Haiku and WebAssembly both build systems stop on it, and
only warn about `x11` or `wayland`, which mean nothing there. It has no GLES or Vulkan flavour (see
the end of this note), and needs every Wayland development file to build, but links none of the
Wayland libraries, see below.

## Loading Wayland at runtime

A `both` binary has no DT_NEEDED entry for libwayland-client, libwayland-cursor, libwayland-egl,
libxkbcommon or libEGL. It starts, and runs on X11 exactly as an X11 build does, on a machine that
has none of them. They are loaded with `dlopen` the first time `puglNewWorld` tries Wayland, once
per process, and never unloaded. If one is missing, or lacks a function DGL uses, the automatic
choice goes on to X11 and a forced `DAF_WINDOW_BACKEND=wayland` fails, naming what was missing.

- Functions: `pugl-extra/wayland-loader-names.h`, included before the library headers, turns each
  function the Wayland backend calls into a call through a pointer of the same type (the header's
  declaration becomes that of an accessor returning it). `pugl-extra/wayland-loader.hpp` defines the
  accessors and fills the pointers with `dlsym`. Calls through a pointer of the real type also cover
  `wl_proxy_marshal_flags`, the variadic function every protocol request goes through. The build
  checks the two lists against each other and against the code: an accessor declared but not
  defined, an entry with no accessor, or a call to any other function of these libraries, each
  fails to compile or to link.
- Data: the protocol code refers to the `wl_*_interface` objects of libwayland-client, and the
  generated tables take their addresses in static initializers, before anything could be loaded.
  Lazy PLT binding would not help with those (data references are bound when the program loads, and
  a library named in DT_NEEDED has to exist for it to start at all), and neither would `dlsym`. A
  `both` build therefore compiles in the core protocol vendored in `pugl-extra/wayland-protocols`,
  header and interface objects, generated from the same `wayland.xml`. libwayland copes with a second
  copy of an interface: it compares interfaces by name when the pointers differ.
- Visibility: the accessors are hidden in every build, so that two DAF plugins in one host cannot
  interpose each other's; for that, `EGLAPI` is emptied and xkbcommon's own `visibility("default")`
  rewritten before their headers declare them. The vendored interface objects are hidden wherever
  the build hides by default, which every release build does, exactly like the other generated
  protocol code (see the comment on it in pugl.cpp).

libGL stays linked: the X11 side needs GLX from it anyway, and with glvnd its GL entry points serve
EGL contexts as well.

## How both backends live in one unity build

`dgl/src/pugl.cpp` is a unity build. Its per-platform half, pugl itself plus DGL's extensions that
reach into pugl's internals, lives in `dgl/src/pugl-platform.hpp`. A single-backend build includes
it once. A `both` build includes it twice through `dgl/src/pugl-dispatch.hpp`: once inside namespace
`pugl_x11` with `DGL_PUGL_X11` defined, once inside `pugl_wl` with `DGL_PUGL_WAYLAND` defined.

Each pass is a complete pugl of its own: view and world structs and their internals, the platform
functions, `common.c` and `internal.c`, the graphics backends and the DGL extensions. The 42
same-named functions, the two `Impl` structs per backend and every `static` helper therefore stop
colliding, and neither `x11.c` nor `wayland.c` has to change for it. Between the passes
the include guards of the pugl headers that declare things (`types.h`, `platform.h`, `internal.h`,
`stub.h`, `pugl/gl.h`, `pugl/stub.h`, `pugl/cairo.h`) are reset, so the second pass declares them
again in its own namespace. The system headers of both backends, the generated Wayland protocol
code and the GLX and EGL headers are all included at global scope before the namespaces open.

What the passes share is what lives at DGL scope: pugl.h's public types (events, enums, geometry,
`PuglEventFunc`) and the two handle types `PuglView` and `PuglWorld`. The handles tie the passes
together:

- At DGL scope `PuglViewImpl` and `PuglWorldImpl` are small structs recording which backend an
  object belongs to.
- Each pass derives its own `PuglViewImpl` and `PuglWorldImpl` from them, through the
  `PUGL_VIEW_IMPL_BASE` and `PUGL_WORLD_IMPL_BASE` hooks in pugl's `types.h`, which are empty for
  every other build. A pass object converts to the public handle implicitly, so a pass calls DGL's
  event function with a pass view unchanged, and a handle converts back with a `static_cast`.
- Inside a pass, `PuglView`, `PuglWorld` and `PuglBackend` are redeclared to name the pass's types,
  which is all the backend sources need.
- `pugl-dispatch.hpp` defines the public pugl API and DGL's extensions at DGL scope. Each forwards
  to the pass that owns its argument with a plain test of the recorded backend. There are no
  function-pointer tables and no thunks; each forwarder is one line.

Why this and not something else:

- Making `view->impl` go through a per-backend cast in the backend sources would also work, but
  it touches hundreds of lines in `x11.c` and `wayland.c`. The namespaces need none.
- One backend for the whole process would be slightly simpler, but choosing per world is what lets
  a CLAP plugin open the Wayland world its host asked for while another stays on X11, or an
  embedded editor follow the backend of its host's window.
- Per-backend event handling stays inside its pass, so file drag and drop slots in the way outgoing
  drags already do: `FileDragX11.hpp` is compiled in the X11 pass, a Wayland counterpart
  goes in the Wayland pass, and `puglStartFileDrag` forwards to whichever owns the view. Drops
  arrive as pugl events at DGL's one event function, whatever the backend.

A call inside a pass to a public function that the same pass only defines further down finds the
DGL-scope forwarder through the base class (argument-dependent lookup), and reaches the same function
one step later. Eleven calls do that, found by building without optimisation and listing every call
from a pass into DGL scope:

| Called | From |
|---|---|
| `puglGetSizeHint` | `puglUpdateSizeHints` in both passes |
| `puglGetTime` | `puglUpdate` in `x11.c` |
| `puglGetViewStyle` | `puglSetViewStyle` in both passes |
| `puglGetVisible` | `flushExposures` in `x11.c`, `puglWaylandFlushExposures` in `wayland.c` |
| `puglObscureRegion` | `puglObscureView` in both passes |
| `puglObscureView` | `puglShow` in `x11.c` |
| `puglSetTransientParent` | `puglRealize` in `x11.c` |

All of them are made on a world or view that is complete and tagged with its backend by then. An
untagged one reaching a forwarder would be memory corruption, or an object pugl did not make, and
stops the program with a message rather than guessing a backend. Besides these calls, the passes
share `PuglFileDrag`, the base class `FileDragX11.hpp` derives from.

## Selection rule

`puglNewWorld` picks the backend once per world, and every view in that world stays on it.

1. For a program, `DAF_WINDOW_BACKEND=x11` or `DAF_WINDOW_BACKEND=wayland` forces that backend. If
   it cannot be used, world creation fails with a message saying so. Any other value is reported and
   ignored.
2. A program (`PUGL_PROGRAM`, a standalone application) tries Wayland first, but only when
   `WAYLAND_DISPLAY` or `WAYLAND_SOCKET` is set: libwayland would otherwise try `wayland-0` on its
   own. Wayland counts only if its libraries load, the compositor accepts the connection and offers
   `wl_compositor` and `xdg_wm_base`, and, in an OpenGL build, EGL on that display can make the
   kind of context the build renders with (desktop GL, or GLES for a GLES build). Then X11, if
   `XOpenDisplay` succeeds.
3. A module (`PUGL_MODULE`, a plugin UI) tries X11 first, and `DAF_WINDOW_BACKEND` does not apply
   to it. Hosts embed plugin UIs through an X11 window id in every format DAF supports, and the
   plugin wrappers of a `both` build keep advertising exactly that (LV2 `ui:X11UI`, the CLAP X11
   window API), so a module takes an X server, XWayland included, whenever there is one. Wayland
   follows, under the same condition as for a program. An LV2 UI that the host wants to embed
   fails to instantiate when it could not open on X11, rather than hand the host a Wayland surface
   as an X11 window.
4. With nothing left, `puglNewWorld` prints which of `WAYLAND_DISPLAY` and `DISPLAY` it saw and
   returns null.

`WAYLAND_SOCKET` hands the process one connection, and libwayland takes it over on the first
connect, unsetting the variable. So the first world that tries Wayland owns that connection, and
when the attempt fails (a missing global, or EGL) the connection is closed with the world, leaving
later worlds `WAYLAND_DISPLAY` alone to try. A Wayland-only build is no different there: only its
first world can have the socket.

DGL code that differs per backend tests `DGL_VIEW_IS_X11(view)` or `DGL_VIEW_IS_WAYLAND(view)`
inside `#ifdef DGL_USING_X11` or `#ifdef DGL_USING_WAYLAND`. A `both` build defines both
`DGL_USING_*` macros and answers at runtime (`puglViewIsX11`, `puglWorldIsX11`); a single-backend
build answers with a constant, so the test folds away there. The places that need it:

- `WindowPrivateData.cpp`: following pugl's scale factor on configure (Wayland), the clipboard
  wait loops, the X11 window type, forwarding unused keys to an X11 parent, and treating a failed
  Wayland offer as final.
- `Window.cpp`: `getPortalParentHandle` answers `x11:` or `wayland:` by the view's backend.
- The file browser and web view of a `both` build are the X11 ones. For a Wayland view,
  `openFileBrowser` passes `fileBrowserCreate` the view's backend, and the file browser then opens
  no X11 connection: the portal gets no parent and centres the dialog, as in a Wayland-only build,
  and without a portal the call fails instead of falling back to the X11-only libsofd dialog. A
  Wayland view gets no web view either, which a Wayland-only build cannot create.
- `DafUI.cpp`: a UI whose world is on Wayland takes no desktop scale factor (0) and follows the
  scale its compositor gives the window, as in a Wayland-only build, without reading `Xft.dpi` from
  an X server that may be reachable too; `DAF_SCALE_FACTOR` and a scale from the host still apply.
  `getDesktopScaleFactor` reports 0 as well where no X server answers, which is what the CLAP
  wrapper's size estimate sees before any UI exists. A web view UI gets no web view on Wayland.

## OpenGL

Each pass brings its own GL backend: GLX (`x11_gl.c`) in the X11 pass, EGL on a `wl_egl_window`
(`wayland_gl.c`) in the Wayland pass. `puglSetMatchingBackendForCurrentBuild` runs inside the pass,
so a view gets its own backend's GL with the same context hints as a single-backend build (3.2 core
for the opengl3 variant). DGL and NanoVG call GL entry points from libGL directly; with glvnd those
dispatch to whichever context is current, GLX or EGL, which is what the Wayland-only build already
relies on.

## Not covered by `both` yet

- GLES: GLX cannot create an ES context, so a `both` build renders desktop GL on both backends.
  The gles2 and gles3 variants still compile, but there is no supported GLES link line for them.
- Vulkan: there is no Wayland Vulkan backend, so a Vulkan variant stops at the existing `#error`.
- Cairo and stub come along with the pass structure and pass the window tests on both backends,
  but get no coverage beyond that.
- The graphics-backend accessors (`puglSetBackend`, `puglGetBackend`, `puglGlBackend`,
  `puglStubBackend`, `puglGetProcAddress`, `puglEnterContext`, `puglLeaveContext`) are not part of
  the forwarding layer: they belong to one backend. DGL uses `puglSetMatchingBackendForCurrentBuild`.
- CLAP keeps advertising only the X11 window API. Offering `CLAP_WINDOW_API_WAYLAND` needs a way to
  create a world on the backend the host asked for, which DGL's `Application` cannot express yet.
- On a Wayland session without XWayland, a plugin UI opens as a floating Wayland window and the
  host's X11 parent is ignored.

## Tests

Two tests are built only for `both`. Both work out from the environment which backend the rule
picks (`tests/runtime-backend.hpp`); `DAF_TEST_EXPECT_BACKEND=x11|wayland|none` names the one a
program must get instead, which CI sets on every run so that a display server that did not start
fails the test rather than turn it into a check that nothing opens.

- `tests/RuntimeBackend.cpp` checks a program world and a module world against it, opens a GL
  window, checks the context is current and the drawing lands (read back from the X server on X11,
  a presented frame on Wayland), resizes it, and sends it a `PUGL_CLIENT` event. It then opens a
  window in the module world too, so that with both servers up an X11 and a Wayland window are open
  in one process. Last, it announces a compositor that cannot be used, a `WAYLAND_DISPLAY` nothing
  listens on and a `WAYLAND_SOCKET` to a stand-in compositor without `xdg_wm_base`, and checks
  that a program falls back to X11, or opens nothing when Wayland was forced.
- `tests/RuntimeBackendWindow.cpp` opens a DGL `Window` and checks the per-backend code above pugl:
  the portal parent handle, the file browser, the web view (none for a Wayland window), the
  clipboard paste loop, and following pugl's scale factor. Without a portal, only an X11 window gets
  the X11 dialog. Then a stand-in portal on a private session bus answers the dialog of either
  backend, and the window has to get the file it chose, with an `x11:` parent named for an X11
  window and none for a Wayland one. Where no `dbus-daemon` can be started, it says so and skips that.
  The paste is checked by counting what its wait loop pumps: a paste whose offer types have yet to
  arrive has to wait on the window's own backend alone. With `DAF_TEST_WAYLAND_SCALE=N`, which
  `headless-wayland.sh` also gives Weston's output, a Wayland window has to follow that output to
  scale N, and an X11 window to keep the X server's.

`tests/Wayland.cpp` and `tests/X11Focus.cpp` drive each backend's half of a `both` build directly.

`tests/headless-wayland.sh` runs a command against a headless Weston of its own, with a private
`XDG_RUNTIME_DIR`. CI uses it for Wayland alone and, under `xvfb-run`, for both servers at once:

    env -u DISPLAY LIBGL_ALWAYS_SOFTWARE=1 \
        tests/headless-wayland.sh make -C tests run DGL_BACKEND=both SKIP_UNIT_TESTS=X11Focus
    xvfb-run -a tests/headless-wayland.sh build/tests/RuntimeBackend

`build.yml` runs the `both` suite under Xvfb alone. `wayland.yml` runs it under Weston alone, runs
both runtime backend tests with both servers for each `DAF_WINDOW_BACKEND` and with EGL's vendor
libraries hidden, and runs the suite again without the Wayland libraries installed, followed by
RuntimeBackend with `WAYLAND_DISPLAY` set, where a program has to fall back to X11, or with Wayland
forced, open nothing. `cmake.yml` builds and runs `tests/runtime-backend-consumer`, an application
that consumes a `both` build through CMake.
