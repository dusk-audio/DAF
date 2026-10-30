/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2024 Filipe Coelho <falktx@falktx.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any purpose with
 * or without fee is hereby granted, provided that the above copyright notice and this
 * permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD
 * TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN
 * NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "tests.hpp"
#include <functional>
#include <memory>

#define DAF_TEST_POINT_CPP
#define DAF_TEST_WINDOW_CPP
#include "dgl/src/pugl.cpp"
#include "dgl/src/Application.cpp"
#include "dgl/src/ApplicationPrivateData.cpp"
#include "dgl/src/Geometry.cpp"
#include "dgl/src/Widget.cpp"
#include "dgl/src/WidgetPrivateData.cpp"
#include "dgl/src/Window.cpp"
#include "dgl/src/WindowPrivateData.cpp"

#ifdef DGL_OPENGL
// ScopedGraphicsContext creates the renderer's drawing state, which lives with the renderers this test does
// not build; only whether the context is current matters here.
START_NAMESPACE_DGL
void Window::PrivateData::createContextIfNeeded() {}
END_NAMESPACE_DGL
#endif

#ifdef DAF_OS_MAC
using TestNativeDrag = ::PuglFileDrag;
#else
using TestNativeDrag = DGL_NAMESPACE::PuglFileDrag;
#endif

struct DragLifetimeProbe
{
    bool cancelled = false, destroyed = false, retainedDuringClose = false;
};

struct ClosingNativeDrag : TestNativeDrag
{
    DragLifetimeProbe& probe;
    std::function<void()> step;
    explicit ClosingNativeDrag(DragLifetimeProbe& state) : probe(state) {}
    ~ClosingNativeDrag() override { probe.destroyed = true; }
    void cancel() override { probe.cancelled = true; }
    bool idle() override
    {
        step();
        probe.retainedDuringClose = probe.cancelled && !probe.destroyed;
        return false;
    }
};

struct ClosingIdle : DGL_NAMESPACE::IdleCallback
{
    std::function<void()> step;
    void idleCallback() override { step(); }
};

// --------------------------------------------------------------------------------------------------------------------
// Window befriends the plugin wrappers' PluginWindow, which is how a host-provided scale factor reaches
// Window::PrivateData::setScaleFactor (see UIExporter::notifyScaleFactorChanged). This translation unit
// does not build the real one, so stand in for it with the same call.

START_NAMESPACE_DAF

class PluginWindow : public DGL_NAMESPACE::Window
{
public:
    uint scaleFactorChanges;
    double lastScaleFactor;

    PluginWindow(DGL_NAMESPACE::Application& app, const uint width, const uint height, const double scaleFactor)
        : Window(app, 0, width, height, scaleFactor, true),
          scaleFactorChanges(0),
          lastScaleFactor(0.0) {}

    void installNativeDrag(const std::shared_ptr<TestNativeDrag>& drag)
    {
        pData->fileDrag = drag;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // file drops: what the Window hears of pugl's drop events

    uint dragEnters = 0, dragLeaves = 0, drops = 0;
    bool answerEnter = true, answerDrop = true;
    FileDropEvent lastDrop;
    std::function<void()> duringDrop;

    enum FileDragKind { kDragEnter, kDragMotion, kDragLeave, kDrop };

    // hand the Window a pugl drop event as a backend would, and return whether it accepted
    bool sendFileDrag(const FileDragKind kind, const double x, const double y, const uint32_t mods = 0)
    {
        USE_NAMESPACE_DGL;
        static const char* const files[] = { "/tmp/a.wav", "/tmp/b.mid" };

        PuglEvent event = {};
        event.drop.type = kind == kDragEnter  ? PUGL_DRAG_ENTER
                        : kind == kDragMotion ? PUGL_DRAG_MOTION
                        : kind == kDragLeave  ? PUGL_DRAG_LEAVE
                                              : PUGL_DROP;
        event.drop.time = 1.5;
        event.drop.x = x;
        event.drop.y = y;
        event.drop.mods = mods;
        if (kind != kDragLeave)
        {
            event.drop.numFiles = 2;
            event.drop.files = files;
        }

        // the Window may delete itself from its callback, so nothing of it is touched afterwards
        return Window::PrivateData::puglEventCallback(pData->view, &event) == PUGL_SUCCESS;
    }

    void forceAcceptingFileDrops(const bool accepting) { pData->acceptingFileDrops = accepting; }
    void setAutoScale(const double factor) { pData->autoScaling = factor != 1.0; pData->autoScaleFactor = factor; }
    void setWaitingForClipboard(const bool waiting) { pData->waitingForClipboardEvents = waiting; }
    void setModalChild(const bool modal) { pData->modal.child = modal ? pData : nullptr; }

    bool setScaleFactorFromHost(const double scaleFactor)
    {
        return pData->setScaleFactor(scaleFactor);
    }

    bool followsPuglScaleFactor() const
    {
        return pData->followsPuglScaleFactor;
    }

   #ifdef DGL_OPENGL
    bool contextIsCurrent() const
    {
        USE_NAMESPACE_DGL;
        return puglBackendIsCurrent(pData->view, false);
    }
   #endif

protected:
    void onScaleFactorChanged(const double scaleFactor) override
    {
        ++scaleFactorChanges;
        lastScaleFactor = scaleFactor;
    }

    // onFileDragMove is left to the default, which keeps the enter's answer
    bool onFileDragEnter(const FileDropEvent& ev) override
    {
        ++dragEnters;
        lastDrop = ev;
        return answerEnter;
    }

    void onFileDragLeave() override
    {
        ++dragLeaves;
    }

    bool onFileDrop(const FileDropEvent& ev) override
    {
        ++drops;
        lastDrop = ev;
        const bool answer = answerDrop;
        // a copy: the step may delete this window, and its members with it
        const std::function<void()> step(duringDrop);
        if (step)
            step();
        return answer;
    }
};

END_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::ApplicationQuitter;
    using DGL_NAMESPACE::Window;

    // Native drag modal loops can close both the Window and its Application.
    {
        DragLifetimeProbe probe;
        std::unique_ptr<Application> app(new Application(false));
        std::unique_ptr<DAF_NAMESPACE::PluginWindow> window(new DAF_NAMESPACE::PluginWindow(*app, 160, 160, 1));
        window->show();
        DAF_ASSERT_EQUAL(window->startFileDrag(nullptr), false, "null file-drag paths are rejected");
        DAF_ASSERT_EQUAL(window->startFileDrag(""), false, "empty file-drag paths are rejected");
        std::shared_ptr<ClosingNativeDrag> drag(new ClosingNativeDrag(probe));
        ClosingIdle closer;
        closer.step = [&] {
            window->close(); window.reset();
            app->removeIdleCallback(&closer); app.reset();
        };
        drag->step = [&] { app->idle(); };
        window->installNativeDrag(drag); drag.reset();
        app->addIdleCallback(&closer);
        DAF_ASSERT_EQUAL(window->startFileDrag("unused.mid"), false, "an active native drag cannot be replaced");
        app->idle();
        DAF_ASSERT_EQUAL(window.get(), nullptr, "nested native dispatch really destroyed the Window");
        DAF_ASSERT_EQUAL(app.get(), nullptr, "nested native dispatch really destroyed the Application");
        DAF_ASSERT_EQUAL(probe.cancelled, true, "Window teardown cancels its native operation");
        DAF_ASSERT_EQUAL(probe.retainedDuringClose, true, "the native operation survives until its active call returns");
        DAF_ASSERT_EQUAL(probe.destroyed, true, "the last active native call releases its operation");
    }
    {
        DragLifetimeProbe probe;
        Application app(false);
        std::unique_ptr<DAF_NAMESPACE::PluginWindow> window(new DAF_NAMESPACE::PluginWindow(app, 160, 160, 1));
        window->show();
        std::shared_ptr<ClosingNativeDrag> drag(new ClosingNativeDrag(probe));
        drag->step = [] {};
        window->installNativeDrag(drag); drag.reset(); app.idle();
        DAF_ASSERT_EQUAL(probe.destroyed, true, "completed native operations are released without closing the Window");
        DAF_ASSERT_EQUAL(probe.cancelled, false, "normal completion is not changed into cancellation");
        window->close(); window.reset();
    }

    // file drops: what a Window hears of pugl's drop events, whatever the backend (each backend's own
    // protocol has a test of its own)
    {
        typedef DAF_NAMESPACE::PluginWindow W;
        Application app(true);
        W win(app, 200, 200, 1.0);

        DAF_ASSERT_EQUAL(win.isAcceptingFileDrops(), false, "a window takes no drops until it asks");
        DAF_ASSERT_EQUAL(win.setAcceptingFileDrops(false), true, "turning drops off always succeeds");
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragEnter, 10, 20), false, "a window that did not ask refuses");
        DAF_ASSERT_EQUAL(win.dragEnters, 0u, "and is not asked");

        win.forceAcceptingFileDrops(true);
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragEnter, 10, 20, 4 /* alt */), true, "the enter's answer is the window's");
        DAF_ASSERT_EQUAL(win.dragEnters, 1u, "the window hears the enter");
        DAF_ASSERT_EQUAL(win.lastDrop.pos.getX(), 10.0, "at the position given");
        DAF_ASSERT_EQUAL(win.lastDrop.pos.getY(), 20.0, "at the position given");
        DAF_ASSERT_EQUAL(win.lastDrop.mod, 4u, "with the modifiers");
        DAF_ASSERT_EQUAL(win.lastDrop.time, 1500u, "with the time in milliseconds");
        DAF_ASSERT_EQUAL(win.lastDrop.numFiles, 2u, "with the files");
        DAF_ASSERT_EQUAL(std::strcmp(win.lastDrop.files[1], "/tmp/b.mid"), 0, "with the files");
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragMotion, 11, 21), true, "a motion keeps the enter's answer by default");

        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragLeave, 0, 0), true, "a leave is taken");
        DAF_ASSERT_EQUAL(win.dragLeaves, 1u, "the window hears the leave");
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragLeave, 0, 0), true, "a second leave is taken");
        DAF_ASSERT_EQUAL(win.dragLeaves, 1u, "but only one reaches the window");
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDrop, 0, 0), false, "a drop with no enter before it is refused");
        DAF_ASSERT_EQUAL(win.drops, 0u, "without reaching the window");

        win.answerEnter = false;
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragMotion, 12, 22), false, "a motion with no enter is the enter");
        DAF_ASSERT_EQUAL(win.dragEnters, 2u, "the window hears it as one");
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragMotion, 13, 23), false, "a refusal is kept by default too");

        win.answerEnter = true;
        win.sendFileDrag(W::kDragLeave, 0, 0);
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragEnter, 30, 40), true, "a new drag is accepted");
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDrop, 31, 41, 4), true, "the drop's answer is the window's");
        DAF_ASSERT_EQUAL(win.drops, 1u, "the window hears the drop");
        DAF_ASSERT_EQUAL(win.lastDrop.pos.getX(), 31.0, "at the position given");
        DAF_ASSERT_EQUAL(win.lastDrop.mod, 4u, "with the modifiers at the drop");
        win.sendFileDrag(W::kDragLeave, 0, 0);
        DAF_ASSERT_EQUAL(win.dragLeaves, 2u, "no leave after a drop");

        win.answerDrop = false;
        win.sendFileDrag(W::kDragEnter, 30, 40);
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDrop, 31, 41), false, "a drop the window does not take fails");
        win.answerDrop = true;

        // auto-scaling: mouse events reach top-level widgets divided by the auto-scale factor, and so do drops
        win.setAutoScale(2.0);
        win.sendFileDrag(W::kDragEnter, 100, 50);
        DAF_ASSERT_EQUAL(win.lastDrop.pos.getX(), 50.0, "auto-scaling applies to the position");
        DAF_ASSERT_EQUAL(win.lastDrop.pos.getY(), 25.0, "auto-scaling applies to the position");
        win.sendFileDrag(W::kDragLeave, 0, 0);
        win.setAutoScale(1.0);

        // a modal child has the input, as it has the mouse
        const uint enters = win.dragEnters;
        win.setModalChild(true);
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragEnter, 1, 1), false, "refused under a modal child");
        DAF_ASSERT_EQUAL(win.dragEnters, enters, "without asking the window");
        win.setModalChild(false);

        // getClipboard pumps events while it waits: drags are refused then, a leave still arrives
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragEnter, 1, 1), true, "accepted again");
        win.setWaitingForClipboard(true);
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragMotion, 2, 2), false, "refused while the clipboard is awaited");
        const uint leaves = win.dragLeaves;
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDrop, 2, 2), false, "a drop is refused then");
        DAF_ASSERT_EQUAL(win.dragLeaves, leaves + 1, "and ends the drag for the window with a leave");
        win.setWaitingForClipboard(false);

        win.forceAcceptingFileDrops(false);
        DAF_ASSERT_EQUAL(win.sendFileDrag(W::kDragEnter, 1, 1), false, "a window that stopped accepting refuses");
    }

    // a window may delete itself from inside its drop
    {
        typedef DAF_NAMESPACE::PluginWindow W;
        Application app(true);
        std::unique_ptr<W> win(new W(app, 200, 200, 1.0));
        win->forceAcceptingFileDrops(true);
        win->sendFileDrag(W::kDragEnter, 5, 5);
        W* const raw = win.get();
        raw->duringDrop = [&win] { win->close(); win.reset(); };
        DAF_ASSERT_EQUAL(raw->sendFileDrag(W::kDrop, 5, 5), true, "the answer of a window that deleted itself stands");
        DAF_ASSERT_EQUAL(win.get(), nullptr, "the window really went");
    }

    // creating and destroying simple window
    {
        Application app(true);
        Window win(app);
    }

    // creating and destroying simple window, with a delay
    {
        Application app(true);
        ApplicationQuitter appQuitter(app);
        Window win(app);
        app.exec();
    }

    // showing and closing simple window, MUST be visible on screen
    {
        Application app(true);
        ApplicationQuitter appQuitter(app);
        Window win(app);
        win.show();
        app.exec();
    }

    // auto-scaling must apply the scale factor exactly once, whatever size the window starts at
    {
        Application app(true);

        // as a plugin UI does it: the window is created at the already-scaled size, then
        // auto-scaling is switched on with the unscaled design size as the minimum
        Window win(app, 0, 360, 360, 1.8, true);
        win.setGeometryConstraints(200, 200, true, true, true);
        DAF_ASSERT_EQUAL(win.getWidth(), 360u, "pre-scaled window keeps its size");
        DAF_ASSERT_EQUAL(win.getHeight(), 360u, "pre-scaled window keeps its size");

        // and a second call leaves it alone rather than scaling again
        win.setGeometryConstraints(200, 200, true, true, true);
        DAF_ASSERT_EQUAL(win.getWidth(), 360u, "repeated call does not scale again");
        DAF_ASSERT_EQUAL(win.getHeight(), 360u, "repeated call does not scale again");
    }

    // a window still at its unscaled design size grows to the scaled minimum instead
    {
        Application app(true);

        Window win(app, 0, 200, 200, 1.8, true);
        win.setGeometryConstraints(200, 200, true, true, true);
        DAF_ASSERT_EQUAL(win.getWidth(), 360u, "unscaled window grows to the scaled minimum");
        DAF_ASSERT_EQUAL(win.getHeight(), 360u, "unscaled window grows to the scaled minimum");
    }

    // a scale factor change from the host is what getScaleFactor() reports afterwards, and notifies once
    {
        Application app(true);

        DAF_NAMESPACE::PluginWindow win(app, 200, 200, 1.0);
        DAF_ASSERT_EQUAL(win.setScaleFactorFromHost(2.0), true, "new scale factor is taken");
        DAF_ASSERT_EQUAL(win.getScaleFactor(), 2.0, "getScaleFactor follows the host");
        DAF_ASSERT_EQUAL(win.scaleFactorChanges, 1u, "onScaleFactorChanged is called");
        DAF_ASSERT_EQUAL(win.lastScaleFactor, 2.0, "onScaleFactorChanged gets the new factor");

        // the same factor again is not a change
        DAF_ASSERT_EQUAL(win.setScaleFactorFromHost(2.0), false, "unchanged scale factor is ignored");
        DAF_ASSERT_EQUAL(win.scaleFactorChanges, 1u, "no notification for an unchanged factor");

        // a window that does not auto-scale leaves its size to the application
        DAF_ASSERT_EQUAL(win.getWidth(), 200u, "non auto-scaling window keeps its size");
        DAF_ASSERT_EQUAL(win.getHeight(), 200u, "non auto-scaling window keeps its size");
    }

    // an auto-scaling window follows a host scale factor change, applying the new factor exactly once
    {
        Application app(true);

        DAF_NAMESPACE::PluginWindow win(app, 360, 360, 1.8);
        win.setGeometryConstraints(200, 200, true, true, true);
        DAF_ASSERT_EQUAL(win.getWidth(), 360u, "pre-scaled window keeps its size");

        win.setScaleFactorFromHost(1.0);
        DAF_ASSERT_EQUAL(win.getWidth(), 200u, "auto-scaling window shrinks to the new factor");
        DAF_ASSERT_EQUAL(win.getHeight(), 200u, "auto-scaling window shrinks to the new factor");

        win.setScaleFactorFromHost(2.5);
        DAF_ASSERT_EQUAL(win.getWidth(), 500u, "auto-scaling window grows to the new factor");
        DAF_ASSERT_EQUAL(win.getHeight(), 500u, "auto-scaling window grows to the new factor");
        DAF_ASSERT_EQUAL(win.scaleFactorChanges, 2u, "each change is notified once");
    }

    // a window that follows pugl's scale factor stops doing so once the host sets one, so a later
    // configure event (Wayland) does not put pugl's factor back
    {
        Application app(true);

        DAF_NAMESPACE::PluginWindow win(app, 200, 200, 0.0);
        DAF_ASSERT_EQUAL(win.followsPuglScaleFactor(), true, "window without a scale factor follows pugl");

        win.setScaleFactorFromHost(win.getScaleFactor() * 2.0);
        DAF_ASSERT_EQUAL(win.followsPuglScaleFactor(), false, "host scale factor stops following pugl");
    }

    // Window sizes are in pixels whatever the backing scale: a size set is the size reported. On macOS pugl
    // converts them to points and back, and when the two directions read the scale from different screens
    // the size came back halved or doubled (360 -> 180), so they must agree, and the factor pugl reports
    // for a window must be a usable one, never 0.
    {
        Application app(true);

        DAF_NAMESPACE::PluginWindow win(app, 300, 300, 0.0);
        DAF_ASSERT_EQUAL(win.getScaleFactor() > 0.0, true, "pugl reports a usable scale factor");
        DAF_ASSERT_EQUAL(win.getWidth(), 300u, "window reports the pixel size it was created with");
        DAF_ASSERT_EQUAL(win.getHeight(), 300u, "window reports the pixel size it was created with");

        win.setSize(422, 254);
        DAF_ASSERT_EQUAL(win.getWidth(), 422u, "window reports the pixel size it was given");
        DAF_ASSERT_EQUAL(win.getHeight(), 254u, "window reports the pixel size it was given");
    }

   #ifdef DGL_OPENGL
    // A graphics context scope opened while the context is current leaves it current: widget destructors
    // open one to delete their GL objects, and when the plugin wrapper has entered the context for the
    // whole UI teardown, everything deleted after them still needs it.
    {
        Application app(true);

        DAF_NAMESPACE::PluginWindow win(app, 200, 200, 1.0);
        {
            const Window::ScopedGraphicsContext outer(win);
            DAF_ASSERT_EQUAL(win.contextIsCurrent(), true, "a scope makes the context current");
            {
                const Window::ScopedGraphicsContext inner(win);
                DAF_ASSERT_EQUAL(win.contextIsCurrent(), true, "a nested scope keeps the context current");
            }
            DAF_ASSERT_EQUAL(win.contextIsCurrent(), true, "a nested scope leaves the context to the outer one");
        }
        DAF_ASSERT_EQUAL(win.contextIsCurrent(), false, "the outer scope leaves the context it entered");
    }
   #endif

    // TODO

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
