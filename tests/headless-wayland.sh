#!/bin/bash
# Runs a command against a headless Weston of its own, the way CI runs the Wayland side of the tests:
#
#   env -u DISPLAY LIBGL_ALWAYS_SOFTWARE=1 tests/headless-wayland.sh make -C tests run
#
# Weston gets a private XDG_RUNTIME_DIR, so the command sees only this compositor, through
# WAYLAND_DISPLAY. DISPLAY is passed through as it is: unset it for a Wayland-only run, or keep it,
# under xvfb-run say, to give the command an X server as well. Weston draws with pixman and needs no
# GPU; GL clients need Mesa's software rasterizer, which LIBGL_ALWAYS_SOFTWARE=1 selects.
set -eu

runtime=$(mktemp -d)
chmod 700 "$runtime"
weston_pid=

# shellcheck disable=SC2329 # run by the EXIT trap below
cleanup() {
    if [ -n "$weston_pid" ]; then
        kill "$weston_pid" 2>/dev/null || true
        wait "$weston_pid" 2>/dev/null || true
    fi
    rm -rf "$runtime"
}
trap cleanup EXIT

XDG_RUNTIME_DIR=$runtime weston --backend=headless --renderer=pixman --socket=daf-headless \
    --width=1280 --height=720 --idle-time=0 > "$runtime/weston.log" 2>&1 &
weston_pid=$!

for _ in $(seq 100); do
    [ -S "$runtime/daf-headless" ] && break
    kill -0 "$weston_pid" 2>/dev/null || break
    sleep 0.1
done

if [ ! -S "$runtime/daf-headless" ]; then
    echo "headless-wayland.sh: weston did not start" >&2
    cat "$runtime/weston.log" >&2
    exit 1
fi

status=0
XDG_RUNTIME_DIR=$runtime WAYLAND_DISPLAY=daf-headless "$@" || status=$?

if [ "$status" != 0 ]; then
    echo "headless-wayland.sh: '$*' exited $status, weston's log:" >&2
    tail -n 30 "$runtime/weston.log" >&2
fi

exit "$status"
