/*
 * Dear ImGui for DAF
 * Copyright (C) 2021 Jean Pierre Cimalando <jp-dev@inbox.ru>
 * Copyright (C) 2021-2024 Filipe Coelho <falktx@falktx.com>
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

#pragma once

#include "SubWidget.hpp"
#include "TopLevelWidget.hpp"
#include "StandaloneWindow.hpp"

#define IMGUI_DEFINE_MATH_OPERATORS

#include "DearImGui/imgui.h"
#include "DearImGuiKnobs/imgui-knobs.h"
#include "DearImGuiToggle/imgui_toggle.h"
#include "DearImGuiToggle/imgui_toggle_math.h"
#include "DearImGuiToggle/imgui_toggle_palette.h"
#include "DearImGuiToggle/imgui_toggle_presets.h"
#include "DearImGuiToggle/imgui_toggle_renderer.h"

#ifdef DAF_UI_HPP_INCLUDED
START_NAMESPACE_DAF
class UI;
class UIExporter;
END_NAMESPACE_DAF
#endif

START_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------

/**
   Dear ImGui Widget class.

   This class exposes the Dear ImGui drawing API inside a DGL Widget.
   The drawing function onDisplay() is implemented internally
   but a new onImGuiDisplay() needs to be overridden instead.

   This class will take care of setting up ImGui for drawing,
   and also also user input, resizes and everything in between.
 */
template <class BaseWidget>
class ImGuiWidget : public BaseWidget,
                    public IdleCallback
{
public:
   /**
      Constructor for a ImGuiSubWidget.
    */
    explicit ImGuiWidget(Widget* parentGroupWidget, float fontSize = 13.f);

   /**
      Constructor for a ImGuiTopLevelWidget.
    */
    explicit ImGuiWidget(Window& windowToMapTo, float fontSize = 13.f);

   /**
      Constructor for a ImGuiStandaloneWindow without transient parent window.
      @note StandaloneWindow::done() is called at the end of this constructor,
            so a subclass doing graphics work in its own constructor must ask for a
            context again with StandaloneWindow::reinit().
    */
    explicit ImGuiWidget(Application& app, float fontSize = 13.f);

   /**
      Constructor for a ImGuiStandaloneWindow with transient parent window.
      @note StandaloneWindow::done() is called at the end of this constructor,
            so a subclass doing graphics work in its own constructor must ask for a
            context again with StandaloneWindow::reinit().
    */
    explicit ImGuiWidget(Application& app, Window& transientParentWindow, float fontSize = 13.f);

   /**
      Destructor.
    */
    ~ImGuiWidget() override;

   /**
      Change global font size.
      This rebuilds the font atlas with only the default font at the new size, which destroys any
      font the subclass added; the subclass must add those again afterwards.
    */
    void setFontSize(float fontSize);

   /**
      True while ImGui wants text input (io.WantTextInput), that is while a text field is being edited.
      The answer comes from the last frame ImGui drew.
      @see Widget::wantsKeyboardFocus
    */
    bool wantsKeyboardFocus() override;

protected:
   /**
      New virtual onDisplay function.
      @see onDisplay
    */
    virtual void onImGuiDisplay() = 0;

   /**
      Called once per frame right before the ImGui frame begins, with the GL
      context current and no frame open. This is the one safe point to rebuild
      the font atlas at a new size (io.Fonts->Clear() / AddFont... / Build(),
      then rebuildFontTexture()): inside onImGuiDisplay the draw lists and the
      font stack already hold ImFont pointers into the old atlas.

      When the window's scale factor has changed since the last frame, this widget has already
      followed it before this is called:
      - The style sizes (those ImGuiStyle::ScaleAllSizes scales) are rescaled from the old factor
        to the new one, including sizes the subclass set; colours and every other style setting
        are left alone. Scaling starts from the sizes as the subclass last set them rather than
        from the previous result, so repeated changes do not drift.
      - The font atlas is rebuilt at the new factor only if it holds nothing but the default font
        this widget added (from the constructor or setFontSize()). As soon as the subclass adds or
        merges a font of its own, the atlas is left alone, so its ImFont pointers stay valid; it
        is then up to the subclass to rebuild its fonts here when getScaleFactor() differs from
        the factor it built them for, and to call rebuildFontTexture().
      The default implementation does nothing.
    */
    virtual void onImGuiPrepareFrame() {}

   /**
      Re-upload the font atlas to the GL backend after io.Fonts was rebuilt.
      Only valid from onImGuiPrepareFrame().
    */
    void rebuildFontTexture();

    void idleCallback() override;
    void onDisplay() override;
    bool onKeyboard(const Widget::KeyboardEvent& event) override;
    bool onCharacterInput(const Widget::CharacterInputEvent&) override;
    bool onMouse(const Widget::MouseEvent& event) override;
    bool onMotion(const Widget::MotionEvent& event) override;
    bool onScroll(const Widget::ScrollEvent& event) override;
    void onResize(const Widget::ResizeEvent& event) override;
    void onFocusChanged(const Widget::FocusEvent& event) override;

    struct PrivateData;
    PrivateData* const imData;

#ifdef DAF_UI_HPP_INCLUDED
    friend class DAF_NAMESPACE::UI;
    friend class DAF_NAMESPACE::UIExporter;
#endif

    DAF_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ImGuiWidget)
};

typedef ImGuiWidget<SubWidget> ImGuiSubWidget;
typedef ImGuiWidget<TopLevelWidget> ImGuiTopLevelWidget;
typedef ImGuiWidget<StandaloneWindow> ImGuiStandaloneWindow;

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------
// extra ImGui calls

namespace ImGui {

// --------------------------------------------------------------------------------------------------------------------
// custom ImGui LabelText implementation for right alignment

void RightAlignedLabelText(const char* label, const char* fmt, ...);
void RightAlignedLabelTextV(const char* label, const char* fmt, va_list args);

// --------------------------------------------------------------------------------------------------------------------

}
