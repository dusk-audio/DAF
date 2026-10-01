/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2022 Filipe Coelho <falktx@falktx.com>
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

#include "WidgetPrivateData.hpp"
#include "SubWidgetPrivateData.hpp"
#include "../TopLevelWidget.hpp"

START_NAMESPACE_DGL

#define FOR_EACH_SUBWIDGET(it) \
  for (std::list<SubWidget*>::iterator it = subWidgets.begin(); it != subWidgets.end(); ++it)

#define FOR_EACH_SUBWIDGET_INV(rit) \
  for (std::list<SubWidget*>::reverse_iterator rit = subWidgets.rbegin(); rit != subWidgets.rend(); ++rit)

// -----------------------------------------------------------------------

Widget::PrivateData::PrivateData(Widget* const s, TopLevelWidget* const tlw)
    : self(s),
      topLevelWidget(tlw),
      parentWidget(nullptr),
      id(0),
      name(nullptr),
      needsScaling(false),
      visible(true),
      size(0, 0),
      subWidgets() {}

Widget::PrivateData::PrivateData(Widget* const s, Widget* const pw)
    : self(s),
      topLevelWidget(findTopLevelWidget(pw)),
      parentWidget(pw),
      id(0),
      name(nullptr),
      needsScaling(false),
      visible(true),
      size(0, 0),
      subWidgets() {}

Widget::PrivateData::~PrivateData()
{
    // detach any subwidgets that outlive us, so they do not keep pointers to freed memory.
    // a widget without a parent is either the top-level widget, whose whole subtree then loses its
    // top-level widget, or already detached, in which case its subtree is unreachable from it anyway
    FOR_EACH_SUBWIDGET(it)
    {
        SubWidget* const subwidget(*it);

        subwidget->pData->parentWidget = nullptr;
        static_cast<Widget*>(subwidget)->pData->parentWidget = nullptr;

        if (parentWidget == nullptr)
            clearTopLevelWidget(subwidget);
    }

    subWidgets.clear();
    std::free(name);
}

void Widget::PrivateData::displaySubWidgets(const uint width, const uint height, const double autoScaleFactor)
{
    if (subWidgets.size() == 0)
        return;

    for (std::list<SubWidget*>::iterator it = subWidgets.begin(); it != subWidgets.end(); ++it)
    {
        SubWidget* const subwidget(*it);

        if (subwidget->isVisible())
            subwidget->pData->display(width, height, autoScaleFactor);
    }
}

// -----------------------------------------------------------------------

bool Widget::PrivateData::giveKeyboardEventForSubWidgets(const KeyboardEvent& ev)
{
    if (! visible)
        return false;
    if (subWidgets.size() == 0)
        return false;

    FOR_EACH_SUBWIDGET_INV(rit)
    {
        SubWidget* const widget(*rit);

        if (widget->isVisible() && widget->onKeyboard(ev))
            return true;
    }

    return false;
}

bool Widget::PrivateData::giveCharacterInputEventForSubWidgets(const CharacterInputEvent& ev)
{
    if (! visible)
        return false;
    if (subWidgets.size() == 0)
        return false;

    FOR_EACH_SUBWIDGET_INV(rit)
    {
        SubWidget* const widget(*rit);

        if (widget->isVisible() && widget->onCharacterInput(ev))
            return true;
    }

    return false;
}

bool Widget::PrivateData::giveMouseEventForSubWidgets(MouseEvent& ev)
{
    if (! visible)
        return false;
    if (subWidgets.size() == 0)
        return false;

    const double x = ev.absolutePos.getX();
    const double y = ev.absolutePos.getY();

    if (SubWidget* const selfw = dynamic_cast<SubWidget*>(self))
    {
        if (selfw->pData->needsViewportScaling)
        {
            ev.absolutePos.setX(x - selfw->getAbsoluteX() + selfw->getMargin().getX());
            ev.absolutePos.setY(y - selfw->getAbsoluteY() + selfw->getMargin().getY());
        }
    }

    FOR_EACH_SUBWIDGET_INV(rit)
    {
        SubWidget* const widget(*rit);

        if (! widget->isVisible())
            continue;

        ev.pos = Point<double>(x - widget->getAbsoluteX() + widget->getMargin().getX(),
                               y - widget->getAbsoluteY() + widget->getMargin().getY());

        if (widget->onMouse(ev))
            return true;
    }

    return false;
}

bool Widget::PrivateData::giveMotionEventForSubWidgets(MotionEvent& ev)
{
    if (! visible)
        return false;
    if (subWidgets.size() == 0)
        return false;

    const double x = ev.absolutePos.getX();
    const double y = ev.absolutePos.getY();

    if (SubWidget* const selfw = dynamic_cast<SubWidget*>(self))
    {
        if (selfw->pData->needsViewportScaling)
        {
            ev.absolutePos.setX(x - selfw->getAbsoluteX() + selfw->getMargin().getX());
            ev.absolutePos.setY(y - selfw->getAbsoluteY() + selfw->getMargin().getY());
        }
    }

    FOR_EACH_SUBWIDGET_INV(rit)
    {
        SubWidget* const widget(*rit);

        if (! widget->isVisible())
            continue;

        ev.pos = Point<double>(x - widget->getAbsoluteX() + widget->getMargin().getX(),
                               y - widget->getAbsoluteY() + widget->getMargin().getY());

        if (widget->onMotion(ev))
            return true;
    }

    return false;
}

bool Widget::PrivateData::giveScrollEventForSubWidgets(ScrollEvent& ev)
{
    if (! visible)
        return false;
    if (subWidgets.size() == 0)
        return false;

    const double x = ev.absolutePos.getX();
    const double y = ev.absolutePos.getY();

    if (SubWidget* const selfw = dynamic_cast<SubWidget*>(self))
    {
        if (selfw->pData->needsViewportScaling)
        {
            ev.absolutePos.setX(x - selfw->getAbsoluteX() + selfw->getMargin().getX());
            ev.absolutePos.setY(y - selfw->getAbsoluteY() + selfw->getMargin().getY());
        }
    }

    FOR_EACH_SUBWIDGET_INV(rit)
    {
        SubWidget* const widget(*rit);

        if (! widget->isVisible())
            continue;

        ev.pos = Point<double>(x - widget->getAbsoluteX() + widget->getMargin().getX(),
                               y - widget->getAbsoluteY() + widget->getMargin().getY());

        if (widget->onScroll(ev))
            return true;
    }

    return false;
}

void Widget::PrivateData::giveFocusChangedEventForSubWidgets(const FocusEvent& ev)
{
    // No visibility check here, unlike the input events above: a widget that was hidden while
    // holding a key or a button still has that state to clear.
    FOR_EACH_SUBWIDGET(it)
    {
        SubWidget* const widget(*it);

        widget->onFocusChanged(ev);
    }
}

// -----------------------------------------------------------------------

TopLevelWidget* Widget::PrivateData::findTopLevelWidget(Widget* const pw)
{
    if (pw == nullptr)
        return nullptr;
    if (pw->pData->topLevelWidget != nullptr)
        return pw->pData->topLevelWidget;
    if (pw->pData->parentWidget != nullptr)
        return findTopLevelWidget(pw->pData->parentWidget);
    return nullptr;
}

void Widget::PrivateData::clearTopLevelWidget(Widget* const w)
{
    PrivateData* const pData(w->pData);
    pData->topLevelWidget = nullptr;

    for (std::list<SubWidget*>::iterator it = pData->subWidgets.begin(); it != pData->subWidgets.end(); ++it)
        clearTopLevelWidget(*it);
}

// -----------------------------------------------------------------------

END_NAMESPACE_DGL
