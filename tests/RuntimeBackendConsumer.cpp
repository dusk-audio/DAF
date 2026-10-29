/*
 * Dusk Audio Framework (DAF)
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

// The application built by tests/runtime-backend-consumer: a standalone window through DGL's public API
// that draws a few frames with OpenGL, on whichever backend the build and the environment give it.

#include "Application.hpp"
#include "OpenGL.hpp"
#include "TopLevelWidget.hpp"
#include "Window.hpp"

#include "daf/extra/Sleep.hpp"

#include <cstdio>

START_NAMESPACE_DGL

class Canvas : public TopLevelWidget
{
public:
    explicit Canvas(Window& window)
        : TopLevelWidget(window),
          frames(0) {}

    int frames;

protected:
    void onDisplay() override
    {
        glClearColor(0.25f, 0.5f, 0.75f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ++frames;
    }
};

END_NAMESPACE_DGL

int main()
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::Canvas;
    using DGL_NAMESPACE::Window;

    Application app(true);
    Window window(app);
    window.setTitle("DAF runtime backend consumer");
    window.setSize(320, 200);
    Canvas canvas(window);
    canvas.setSize(320, 200);
    window.show();

    for (int i = 0; i < 500 && canvas.frames < 3; ++i)
    {
        window.repaint();
        app.idle();
        DAF_NAMESPACE::d_msleep(10);
    }

    const bool drew = canvas.frames >= 3;
    const bool native = window.getNativeWindowHandle() != 0;

    std::printf("runtime-backend-consumer: %d frames drawn, native handle %s, portal parent '%s'\n",
                canvas.frames, native ? "set" : "unset", window.getPortalParentHandle());

    window.close();
    return drew && native ? 0 : 1;
}
