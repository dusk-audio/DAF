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
};

END_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::ApplicationQuitter;
    using DGL_NAMESPACE::Window;

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
