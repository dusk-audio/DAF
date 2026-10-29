# Runtime windowing backend (`DGL_BACKEND=both`)

By default DGL on Linux is built for exactly one windowing system: X11 whenever its development
files are installed, the native Wayland backend only where they are not. `DGL_BACKEND=both` builds
both into one binary, which picks X11 or Wayland when it opens its first window. One Linux binary
then runs natively on a Wayland desktop and on X11 where there is no compositor.

It is an explicit opt-in. `auto` still means X11 whenever X11 is installed, and an X11-only or
Wayland-only build compiles to exactly the symbols it would without this support.

## Build switch

| | Make | CMake |
|---|---|---|
| Values | `DGL_BACKEND=auto` (default) or `both` | `-DDGL_BACKEND=auto` (default), `x11`, `wayland` or `both` |
| Requires | every X11 and every Wayland development file | same |
| Defines | `HAVE_X11`, `HAVE_WAYLAND`, `DGL_RUNTIME_BACKEND` | same, on `dgl-system-libs-definitions` |
| Links | X11 libraries, `gl x11`, `-ldl` | X11 libraries, `OPENGL_gl_LIBRARY`, `CMAKE_DL_LIBS` |

`make features` reports `DGL_RUNTIME_BACKEND: Yes` for such a build. `both` is Linux only, and has no
GLES or Vulkan flavour (see the end of this note). It needs every Wayland development file to build,
but links none of the Wayland libraries, see below.

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

A call inside a pass to a public function that the same pass only defines further down (one case
today: `puglShow` in `x11.c` calling `puglObscureRegion`) finds the DGL-scope forwarder through the
base class and reaches the same function one step later.

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

DGL code that differs per backend tests `DGL_VIEW_IS_X11(view)` or `DGL_VIEW_IS_WAYLAND(view)`
inside `#ifdef DGL_USING_X11` or `#ifdef DGL_USING_WAYLAND`. A `both` build defines both
`DGL_USING_*` macros and answers at runtime (`puglViewIsX11`, `puglWorldIsX11`); a single-backend
build answers with a constant, so the test folds away there. The places that need it:

- `WindowPrivateData.cpp`: following pugl's scale factor on configure (Wayland), the clipboard
  wait loops, the X11 window type, forwarding unused keys to an X11 parent, and treating a failed
  Wayland offer as final.
- `Window.cpp`: `getPortalParentHandle` answers `x11:` or `wayland:` by the view's backend.
- The file browser and web view of a `both` build are the X11 ones. A Wayland view hands the file
  browser no parent, so the portal centres the dialog as it does in a Wayland-only build, and gets
  no web view, which a Wayland-only build cannot create either.
- `DafUI.cpp`: a plugin UI that finds no X server reports an unknown desktop scale factor (0), as
  the Wayland-only build does, since its world will open on Wayland.

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

`tests/RuntimeBackend.cpp` (built only for `both`) works out from the environment which backend the
rule picks, checks a program world and a module world against it, opens a GL window, checks the
context is current and the drawing lands (read back from the X server on X11, a presented frame on
Wayland), resizes it, and sends it a `PUGL_CLIENT` event. `tests/Wayland.cpp` and
`tests/X11Focus.cpp` drive each backend's half of a `both` build directly.

`tests/headless-wayland.sh` runs a command against a headless Weston of its own, with a private
`XDG_RUNTIME_DIR`. CI uses it for Wayland alone and, under `xvfb-run`, for both servers at once:

    env -u DISPLAY LIBGL_ALWAYS_SOFTWARE=1 \
        tests/headless-wayland.sh make -C tests run DGL_BACKEND=both SKIP_UNIT_TESTS=X11Focus
    xvfb-run -a tests/headless-wayland.sh build/tests/RuntimeBackend

`build.yml` runs the `both` suite under Xvfb alone, `wayland.yml` under Weston alone and
RuntimeBackend with both servers and each `DAF_WINDOW_BACKEND`, and `cmake.yml` builds and runs
`tests/runtime-backend-consumer`, an application that consumes a `both` build through CMake.
