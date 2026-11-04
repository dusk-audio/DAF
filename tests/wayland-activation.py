#!/usr/bin/env python3
"""Native activation against private headless Mutter, with GTK 4 launcher tokens.

Run: python3 tests/wayland-activation.py run build/tests/WaylandActivation <log-directory>
Requires Mutter, dbus-run-session and Python GObject with GTK 4. No live desktop is used.
"""

import contextlib
import os
from pathlib import Path
import signal
import shlex
import subprocess
import sys
import time


def wait_for(predicate, description, seconds=15):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(0.05)
    raise RuntimeError(description)


def lines(path):
    return path.read_text().splitlines() if path.exists() else []


def last_state(path, prefix):
    states = [line for line in lines(path) if line.startswith(prefix)]
    return states[-1] if states else None


def stop(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def cover(probe, token_file):
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gio, Gtk

    def activate(app):
        window = Gtk.ApplicationWindow(application=app, title="DAF activation launcher")
        window.set_decorated(False)
        window.set_child(Gtk.Label(label="Launch or activate DAF"))
        window.connect("notify::is-active", lambda w, _p: print("ACTIVE %d" % w.is_active(), flush=True))
        click = Gtk.GestureClick()

        def pressed(*_args):
            info = Gio.AppInfo.create_from_commandline(shlex.quote(probe), "DAF activation probe", Gio.AppInfoCreateFlags.NONE)
            context = window.get_display().get_app_launch_context()
            token = context.get_startup_notify_id(info, [])
            if not token:
                raise RuntimeError("GTK did not obtain an activation token")
            token_file.write_text(token)
            print("TOKEN", flush=True)

        click.connect("pressed", pressed)
        window.add_controller(click)
        window.maximize()
        window.present()
        print("MAPPED", flush=True)

    app = Gtk.Application(application_id="audio.dusk.daf.ActivationLauncher",
                          flags=Gio.ApplicationFlags.NON_UNIQUE)
    app.connect("activate", activate)
    app.run([sys.argv[0]])


class Desktop:
    def __init__(self):
        from gi.repository import Gio, GLib
        self.Gio, self.GLib = Gio, GLib
        self.bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
        self.path = "/org/gnome/Mutter/RemoteDesktop"
        self.interface = "org.gnome.Mutter.RemoteDesktop"
        self.path = self.call("CreateSession", reply="(o)").unpack()[0]
        self.interface += ".Session"
        self.call("Start")
        self.move(1, 0)
        self.tap(0xFFE1)

    def call(self, method, signature=None, args=(), reply=None):
        return self.bus.call_sync("org.gnome.Mutter.RemoteDesktop", self.path, self.interface, method,
                                  self.GLib.Variant(signature, args) if signature else None,
                                  self.GLib.VariantType(reply) if reply else None,
                                  self.Gio.DBusCallFlags.NONE, 5000, None)

    def move(self, x, y):
        self.call("NotifyPointerMotionRelative", "(dd)", (float(x), float(y)))

    def click(self, x, y):
        self.move(-10000, -10000)
        time.sleep(0.05)
        while x > 0 or y > 0:
            dx, dy = min(x, 100), min(y, 100)
            self.move(dx, dy)
            x, y = x - dx, y - dy
            time.sleep(0.005)
        self.call("NotifyPointerButton", "(ib)", (272, True))
        time.sleep(0.05)
        self.call("NotifyPointerButton", "(ib)", (272, False))

    def key(self, key, pressed):
        self.call("NotifyKeyboardKeysym", "(ub)", (key, pressed))

    def tap(self, key, modifier=None):
        if modifier:
            self.key(modifier, True)
        self.key(key, True)
        time.sleep(0.05)
        self.key(key, False)
        if modifier:
            self.key(modifier, False)


def drive(probe, work):
    from gi.repository import Gio
    if os.environ.get("XDG_RUNTIME_DIR") != str(work / "runtime"):
        raise RuntimeError("drive requires the private session created by run")
    desktop = Desktop()
    env = dict(os.environ, DAF_WINDOW_BACKEND="wayland", GDK_BACKEND="wayland",
               GTK_A11Y="none", WAYLAND_DEBUG="client")
    env.pop("DISPLAY", None)
    probe_log = work / "probe.log"
    probe_process = None
    with contextlib.ExitStack() as resources:
        for index, case in enumerate(("launch", "covered", "minimized")):
            token_file = work / (case + ".token")
            cover_log = work / (case + ".log")
            output = resources.enter_context(cover_log.open("w"))
            wire = resources.enter_context((work / (case + ".wire")).open("w"))
            launcher = subprocess.Popen([sys.executable, "-I", __file__, "cover", probe, str(token_file)],
                                        env=env, stdout=output, stderr=wire)
            resources.callback(stop, launcher)
            wait_for(lambda: "MAPPED" in lines(cover_log), "launcher did not map")
            time.sleep(0.5)
            if case == "minimized":
                # Establish focus before invoking Mutter's configured minimize shortcut.
                desktop.click(50, 50)
                wait_for(lambda: token_file.exists(), "baseline launcher token missing")
                probe_process.stdin.write("TOKEN " + token_file.read_text() + "\n")
                probe_process.stdin.flush()
                wait_for(lambda: last_state(probe_log, "FOCUS ") == "FOCUS 1"
                         and last_state(cover_log, "ACTIVE ") == "ACTIVE 0", "baseline activation failed")
                keys = Gio.Settings.new("org.gnome.desktop.wm.keybindings").get_strv("minimize")
                if "<Super>h" not in keys:
                    raise RuntimeError("private Mutter minimize shortcut differs from Super+h")
                desktop.tap(ord("h"), 0xFFEB)
                wait_for(lambda: last_state(probe_log, "FOCUS ") == "FOCUS 0", "minimize did not remove focus")
                print("MINIMIZED through Mutter shortcut", flush=True)
            desktop.click(50, 50)
            wait_for(lambda: last_state(cover_log, "ACTIVE ") == "ACTIVE 1", "launcher did not take focus")
            if probe_process:
                wait_for(lambda: last_state(probe_log, "FOCUS ") == "FOCUS 0", "probe did not lose focus")
            # Start the actual case with a fresh click and token after proving the cover has focus.
            token_file.unlink(missing_ok=True)
            desktop.click(960, 540)
            wait_for(lambda: token_file.exists(), "launcher token missing")
            token = token_file.read_text()
            if not token:
                raise RuntimeError("empty launcher token")
            if probe_process is None:
                probe_process = subprocess.Popen([probe], env=dict(env, XDG_ACTIVATION_TOKEN=token),
                    stdin=subprocess.PIPE, text=True,
                    stdout=resources.enter_context(probe_log.open("w")),
                    stderr=resources.enter_context((work / "probe.wire").open("w")))
                resources.callback(stop, probe_process)
            else:
                probe_process.stdin.write("TOKEN " + token + "\n")
                probe_process.stdin.flush()
            wait_for(lambda: last_state(probe_log, "FOCUS ") == "FOCUS 1"
                     and last_state(cover_log, "ACTIVE ") == "ACTIVE 0", case + " did not transfer focus")
            nonce = ord("a") + index
            desktop.tap(nonce)
            wait_for(lambda: "KEY %d" % nonce in lines(probe_log), case + " did not deliver the nonce key")
            if probe_process.poll() is not None:
                raise RuntimeError("probe exited during " + case)
            print("PASS %s: native focus and keyboard delivery, pid %d" % (case, probe_process.pid), flush=True)
            stop(launcher)
        observations = lines(probe_log)
        windows = [line for line in observations if line.startswith("WINDOW ")]
        if len(windows) != 1 or not windows[0].endswith("TOKEN_CLEARED 1") or "FRAME" not in observations:
            raise RuntimeError("probe did not retain one presented window or leaked its launch token")
        if "xdg_activation_v1" not in (work / "probe.wire").read_text():
            raise RuntimeError("native activation protocol was not used")
        probe_process.stdin.close()
        if probe_process.wait(timeout=5) != 0:
            raise RuntimeError("probe failed to shut down")


def run(probe, work):
    work.mkdir(parents=True, exist_ok=False)
    runtime, home = work / "runtime", work / "home"
    runtime.mkdir(mode=0o700)
    home.mkdir()
    env = dict(PATH="/usr/local/bin:/usr/bin:/bin", HOME=str(home), USER=os.environ.get("USER", ""),
               LANG="C.UTF-8", XDG_RUNTIME_DIR=str(runtime), XDG_CONFIG_HOME=str(home / ".config"),
               XDG_DATA_HOME=str(home / ".local/share"), XDG_CACHE_HOME=str(home / ".cache"),
               XDG_STATE_HOME=str(home / ".local/state"), XDG_SESSION_TYPE="wayland",
               XDG_CURRENT_DESKTOP="GNOME", GSETTINGS_BACKEND="memory", LIBGL_ALWAYS_SOFTWARE="1")
    command = ["dbus-run-session", "--", "mutter", "--wayland", "--headless", "--virtual-monitor", "1920x1080",
               "--wayland-display", "daf-activation", "--", sys.executable, "-I", __file__, "drive", probe, str(work)]
    try:
        with (work / "session.log").open("w") as output:
            result = subprocess.run(command, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=100)
        print((work / "session.log").read_text())
        return 0 if result.returncode == 0 and (work / "passed").exists() else 1
    finally:
        # Session services may outlive Mutter; match only our fresh private runtime directory.
        marker = ("XDG_RUNTIME_DIR=" + str(runtime)).encode()
        for sig in (signal.SIGTERM, signal.SIGKILL):
            for entry in Path("/proc").iterdir():
                if not entry.name.isdecimal():
                    continue
                try:
                    if entry.stat().st_uid == os.getuid() and marker in (entry / "environ").read_bytes().split(b"\0"):
                        os.kill(int(entry.name), sig)
                except OSError:
                    pass
            time.sleep(0.5)


if __name__ == "__main__":
    mode, binary, destination = sys.argv[1:]
    if mode == "cover":
        cover(binary, Path(destination))
    elif mode == "drive":
        drive(binary, Path(destination))
        (Path(destination) / "passed").write_text("native activation passed\n")
    elif mode == "run":
        sys.exit(run(str(Path(binary).resolve()), Path(destination).resolve()))
    else:
        raise SystemExit("expected run, drive, or cover")
