/*
 * DAF - Dusk Audio Framework
 * Copyright (C) 2026 Dusk Audio
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

#include "dgl/NanoVG.hpp"

#include "daf/extra/Sleep.hpp"

#include <cstdio>

START_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------

class NanoFilledRect : public NanoSubWidget
{
public:
    NanoFilledRect(NanoTopLevelWidget* const parent, const Color c, const bool clip)
        : NanoSubWidget(parent),
          color(c),
          clipChildren(clip) {}

    NanoFilledRect(NanoSubWidget* const parent, const Color c, const bool clip)
        : NanoSubWidget(parent),
          color(c),
          clipChildren(clip) {}

protected:
    void onNanoDisplay() override
    {
        if (clipChildren)
            intersectScissor(0, 0, getWidth(), getHeight());

        beginPath();
        fillColor(color);
        rect(0, 0, getWidth(), getHeight());
        fill();
    }

private:
    const Color color;
    const bool clipChildren;
};

class NanoClipContainer : public NanoTopLevelWidget
{
public:
    NanoClipContainer(Window& win, const bool clipChildren)
        : NanoTopLevelWidget(win),
          panel(this, Color(0, 0, 255), true),
          child(&panel, Color(255, 0, 0), false)
    {
        // the panel covers the middle of the window and sets a scissor to its own bounds,
        // its child covers the whole window and must only show inside the panel when the panel
        // clips its children, and everywhere when it does not (the default)
        panel.setClipChildren(clipChildren);
        panel.setAbsolutePos(50, 50);
        panel.setSize(100, 100);
        child.setAbsolutePos(0, 0);
        child.setSize(200, 200);
    }

protected:
    void onNanoDisplay() override
    {
        beginPath();
        fillColor(Color(128, 128, 128));
        rect(0, 0, getWidth(), getHeight());
        fill();
    }

private:
    NanoFilledRect panel;
    NanoFilledRect child;
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL

// read the pixel at a position relative to the picture size, from a PPM written by Window::renderToPicture
static bool readPixel(const char* const filename, const double relX, const double relY, int rgb[3])
{
    FILE* const f = std::fopen(filename, "r");
    if (f == nullptr)
        return false;

    int width = 0, height = 0, maxval = 0;
    bool ok = std::fscanf(f, "P3 %d %d %d", &width, &height, &maxval) == 3 && width > 0 && height > 0;

    if (ok)
    {
        const int x = static_cast<int>(relX * width);
        const int y = static_cast<int>(relY * height);
        const long skip = static_cast<long>(y) * width + x;

        for (long i = 0; ok && i < skip; ++i)
            ok = std::fscanf(f, "%d %d %d", &rgb[0], &rgb[1], &rgb[2]) == 3;

        ok = ok && std::fscanf(f, "%d %d %d", &rgb[0], &rgb[1], &rgb[2]) == 3;
    }

    std::fclose(f);
    return ok;
}

// render the panel and its child, then sample the picture inside and outside the panel
static int renderAndCheck(const bool clipChildren)
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::NanoClipContainer;
    using DGL_NAMESPACE::Window;
    typedef Window::ScopedGraphicsContext ScopedGraphicsContext;

    const char* const filename = "NanoSubWidgetsClip.ppm";
    std::remove(filename);

    Application app(true);
    Window win(app);
    win.setSize(200, 200);
    ScopedPointer<NanoClipContainer> container;

    {
        const ScopedGraphicsContext sgc(win);
        container = new NanoClipContainer(win, clipChildren);
    }

    win.show();
    win.renderToPicture(filename);

    // the picture is written on the next expose, give the window system some time to deliver it
    int rgb[3] = {};
    bool rendered = false;
    for (int i = 0; i < 200 && ! rendered; ++i)
    {
        app.idle();
        d_msleep(10);

        if (FILE* const f = std::fopen(filename, "r"))
        {
            std::fclose(f);
            rendered = true;
        }
    }

    DAF_ASSERT_EQUAL(rendered, true, "window renders into a picture");

    DAF_ASSERT_EQUAL(readPixel(filename, 0.5, 0.5, rgb), true, "picture can be read");
    const bool red = rgb[0] > 200 && rgb[1] < 50 && rgb[2] < 50;
    DAF_ASSERT_EQUAL(red, true, "child draws inside its parent");

    DAF_ASSERT_EQUAL(readPixel(filename, 0.1, 0.1, rgb), true, "picture can be read");
    const bool grey = rgb[0] > 100 && rgb[0] < 160 && rgb[1] > 100 && rgb[1] < 160 && rgb[2] > 100 && rgb[2] < 160;
    const bool redOutside = rgb[0] > 200 && rgb[1] < 50 && rgb[2] < 50;

    if (clipChildren)
    {
        DAF_ASSERT_EQUAL(grey, true, "child is clipped to the scissor of its parent");
    }
    else
    {
        DAF_ASSERT_EQUAL(redOutside, true, "child is not clipped by default");
    }

    container = nullptr;
    win.close();
    app.quit();
    std::remove(filename);
    return 0;
}

int main()
{
    if (const int ret = renderAndCheck(false))
        return ret;

    return renderAndCheck(true);
}

// --------------------------------------------------------------------------------------------------------------------
