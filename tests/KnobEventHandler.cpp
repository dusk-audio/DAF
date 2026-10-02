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

#include "dgl/EventHandlers.hpp"
#include "dgl/SubWidget.hpp"
#include "dgl/TopLevelWidget.hpp"
#include "dgl/Window.hpp"

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::KnobEventHandler;
    using DGL_NAMESPACE::SubWidget;
    using DGL_NAMESPACE::TopLevelWidget;
    using DGL_NAMESPACE::Widget;
    using DGL_NAMESPACE::Window;

    Application app(true);
    Window win(app);
    TopLevelWidget tlw(win);
    SubWidget widget(&tlw);
    widget.setSize(100, 100);

    // dragging a log-scale knob up must sweep its whole range, never moving backwards
    {
        KnobEventHandler knob(&widget);
        knob.setRange(1.0f, 100.0f);
        knob.setUsingLogScale(true);
        knob.setValue(1.0f);

        Widget::MouseEvent press;
        press.button = 1;
        press.press = true;
        press.pos = DGL_NAMESPACE::Point<double>(50, 50);
        press.time = 1;
        DAF_ASSERT_EQUAL(knob.mouseEvent(press), true, "knob takes the press");

        float last = knob.getValue();

        for (int y = 49; y >= -250; --y)
        {
            Widget::MotionEvent motion;
            motion.pos = DGL_NAMESPACE::Point<double>(50, y);
            knob.motionEvent(motion);

            const float value = knob.getValue();
            DAF_ASSERT_EQUAL(value < last, false, "log knob moves monotonically while dragged up");
            last = value;
        }

        DAF_ASSERT_SAFE_EQUAL(knob.getValue(), 100.0f, "log knob reaches its maximum");
        DAF_ASSERT_SAFE_EQUAL(knob.getNormalizedValue(), 1.0f, "log knob reaches its maximum");
    }

    // scrolling a stepped knob must not round it out of its range
    {
        KnobEventHandler knob(&widget);
        knob.setRange(0.5f, 1.9f);
        knob.setStep(0.4f);
        knob.setValue(0.6f);

        Widget::ScrollEvent scroll;
        scroll.pos = DGL_NAMESPACE::Point<double>(50, 50);

        scroll.delta = DGL_NAMESPACE::Point<double>(0, -1);
        DAF_ASSERT_EQUAL(knob.scrollEvent(scroll), true, "knob takes the scroll");
        DAF_ASSERT_EQUAL(knob.getValue() < 0.5f, false, "scrolling down stays above the minimum");

        knob.setValue(1.8f);
        scroll.delta = DGL_NAMESPACE::Point<double>(0, 1);
        DAF_ASSERT_EQUAL(knob.scrollEvent(scroll), true, "knob takes the scroll");
        DAF_ASSERT_EQUAL(knob.getValue() > 1.9f, false, "scrolling up stays below the maximum");
    }

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
