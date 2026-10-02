/*
 * Dear ImGui for DAF
 * Copyright (C) 2021 Jean Pierre Cimalando <jp-dev@inbox.ru>
 * Copyright (C) 2021-2025 Filipe Coelho <falktx@falktx.com>
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

#include "DearImGui.hpp"
#include "Application.hpp"

#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#if defined(DGL_USE_GLES2)
# define IMGUI_IMPL_OPENGL_ES2
#elif defined(DGL_USE_GLES3)
# define IMGUI_IMPL_OPENGL_ES3
#elif defined(DGL_USE_OPENGL3)
# define IMGUI_IMPL_OPENGL_LOADER_CUSTOM
#endif

#ifdef DGL_USE_OPENGL3
# include "OpenGL.hpp"
#endif

#ifndef IMGUI_SKIP_IMPLEMENTATION
# define IMGUI_DAF_BACKEND
# include "DearImGui/imgui.cpp"
# include "DearImGui/imgui_demo.cpp"
# include "DearImGui/imgui_draw.cpp"
# include "DearImGui/imgui_tables.cpp"
# include "DearImGui/imgui_widgets.cpp"
# include "DearImGuiKnobs/imgui-knobs.cpp"
# include "DearImGuiToggle/imgui_toggle.cpp"
# include "DearImGuiToggle/imgui_toggle_palette.cpp"
# include "DearImGuiToggle/imgui_toggle_presets.cpp"
# include "DearImGuiToggle/imgui_toggle_renderer.cpp"

# if defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3) || defined(DGL_USE_OPENGL3)
#  include "DearImGui/imgui_impl_opengl3.cpp"
# else
#  include "DearImGui/imgui_impl_opengl2.cpp"
# endif
#else
# if defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3) || defined(DGL_USE_OPENGL3)
#  include "DearImGui/imgui_impl_opengl3.h"
# else
#  include "DearImGui/imgui_impl_opengl2.h"
# endif
#endif

START_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------

static const char* GetClipboardTextFn(void* const userData)
{
    TopLevelWidget* const tlw = static_cast<TopLevelWidget*>(userData);

    size_t dataSize;
    return static_cast<const char*>(tlw->getClipboard(dataSize));
}

static void SetClipboardTextFn(void* const userData, const char* const text)
{
    TopLevelWidget* const tlw = static_cast<TopLevelWidget*>(userData);
    DAF_SAFE_ASSERT_RETURN(tlw!= nullptr,);
    DAF_SAFE_ASSERT_RETURN(text != nullptr,);

    tlw->setClipboard(nullptr, text, std::strlen(text)+1);
}

// --------------------------------------------------------------------------------------------------------------------
// The ImGuiStyle members that ImGuiStyle::ScaleAllSizes scales, kept in step with it.

#define DAF_IMGUI_SCALED_STYLE_MEMBERS(X) \
    X(WindowPadding) X(WindowRounding) X(WindowMinSize) X(WindowBorderHoverPadding) X(ChildRounding) \
    X(PopupRounding) X(FramePadding) X(FrameRounding) X(ItemSpacing) X(ItemInnerSpacing) X(CellPadding) \
    X(TouchExtraPadding) X(IndentSpacing) X(ColumnsMinSpacing) X(ScrollbarSize) X(ScrollbarRounding) \
    X(GrabMinSize) X(GrabRounding) X(LogSliderDeadzone) X(ImageBorderSize) X(TabRounding) \
    X(TabCloseButtonMinWidthSelected) X(TabCloseButtonMinWidthUnselected) X(TabBarOverlineSize) \
    X(SeparatorTextPadding) X(DisplayWindowPadding) X(DisplaySafeAreaPadding) X(MouseCursorScale)

static inline bool isSameStyleSize(const float a, const float b) noexcept
{
    return a == b;
}

static inline bool isSameStyleSize(const ImVec2& a, const ImVec2& b) noexcept
{
    return a.x == b.x && a.y == b.y;
}

// whether the members ScaleAllSizes scales are the same in both styles; nothing else is compared
static bool haveSameScaledSizes(const ImGuiStyle& a, const ImGuiStyle& b) noexcept
{
   #define DAF_IMGUI_COMPARE_MEMBER(m) if (! isSameStyleSize(a.m, b.m)) return false;
    DAF_IMGUI_SCALED_STYLE_MEMBERS(DAF_IMGUI_COMPARE_MEMBER)
   #undef DAF_IMGUI_COMPARE_MEMBER
    return true;
}

// copy the members ScaleAllSizes scales from one style to another, leaving colours and the rest alone
static void copyScaledSizes(ImGuiStyle& dst, const ImGuiStyle& src) noexcept
{
   #define DAF_IMGUI_COPY_MEMBER(m) dst.m = src.m;
    DAF_IMGUI_SCALED_STYLE_MEMBERS(DAF_IMGUI_COPY_MEMBER)
   #undef DAF_IMGUI_COPY_MEMBER
}

// --------------------------------------------------------------------------------------------------------------------

template <class BaseWidget>
struct ImGuiWidget<BaseWidget>::PrivateData {
    ImGuiWidget<BaseWidget>* const self;
    ImGuiContext* context;
    double scaleFactor;
    float fontSize;
    double lastFrameTime;
    uint lastModifiers;

    // the font this widget added to the atlas itself, nullptr if none
    ImFont* ownFont;

    // The style sizes are scaled from a reference style, which holds them at referenceScaleFactor,
    // and not from whatever the last scaling left behind: ScaleAllSizes truncates, so scaling the
    // current sizes back and forth would lose a pixel here and there each time. scaledStyle is the
    // style as the last scaling left it, to tell whether the application has changed sizes since,
    // in which case its sizes become the new reference.
    ImGuiStyle referenceStyle;
    double referenceScaleFactor;
    ImGuiStyle scaledStyle;

    explicit PrivateData(ImGuiWidget<BaseWidget>* const s, const float fontSize_)
        : self(s),
          context(nullptr),
          scaleFactor(s->getTopLevelWidget()->getScaleFactor()),
          fontSize(fontSize_),
          lastFrameTime(0.0),
          lastModifiers(0),
          ownFont(nullptr),
          referenceStyle(),
          referenceScaleFactor(1.0),
          scaledStyle()
    {
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext(context);

        ImGuiIO& io(ImGui::GetIO());
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.DisplaySize.x = self->getWidth() != 0 ? self->getWidth() : 640 * scaleFactor;
        io.DisplaySize.y = self->getHeight() != 0 ? self->getHeight() : 480 * scaleFactor;
        // not needed, we handle this ourselves
        // io.DisplayFramebufferScale = ImVec2(scaleFactor, scaleFactor);
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;

        ImGuiStyle& style(ImGui::GetStyle());
        style.ScaleAllSizes(scaleFactor);
        scaledStyle = style;

        buildFonts();

        io.GetClipboardTextFn = GetClipboardTextFn;
        io.SetClipboardTextFn = SetClipboardTextFn;
        io.ClipboardUserData = s->getTopLevelWidget();

       #if defined(DGL_USE_OPENGL3) && !(defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3))
        // DGL asks for a 3.2 core context on every platform, and a core context only has to accept
        // GLSL 1.40 and 1.50; the backend's own default of 1.30 is rejected by some drivers there
        ImGui_ImplOpenGL3_Init("#version 150");
       #elif defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3) || defined(DGL_USE_OPENGL3)
        ImGui_ImplOpenGL3_Init();
       #else
        ImGui_ImplOpenGL2_Init();
       #endif
    }

    // The renderer backend's shutdown deletes its buffers, program and font texture, so the window's
    // graphics context must be current; the ImGuiWidget destructors make sure of it. Nothing else
    // does on a standalone window, and without it they leaked, and on Windows every one of those
    // entry points was reported unavailable, having been looked up for the first time with no
    // context current.
    ~PrivateData()
    {
        ImGui::SetCurrentContext(context);
       #if defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3) || defined(DGL_USE_OPENGL3)
        ImGui_ImplOpenGL3_Shutdown();
       #else
        ImGui_ImplOpenGL2_Shutdown();
       #endif
        ImGui::DestroyContext(context);
    }

    // (re)build the font atlas with only the default font, at fontSize * scaleFactor
    void buildFonts()
    {
       #ifndef DGL_NO_SHARED_RESOURCES
        using namespace daf_resources;
        ImGuiIO& io(ImGui::GetIO());
        const bool ownFontIsDefault = ownFont != nullptr && io.FontDefault == ownFont;
        ImFontConfig fc;
        fc.FontDataOwnedByAtlas = false;
        fc.OversampleH = 1;
        fc.OversampleV = 1;
        fc.PixelSnapH = true;
        io.Fonts->Clear();
        ownFont = io.Fonts->AddFontFromMemoryTTF((void*)dejavusans_ttf,
                                                 dejavusans_ttf_size,
                                                 d_roundToIntPositive(fontSize * scaleFactor),
                                                 &fc);
        io.Fonts->Build();

        // Clear() destroyed every font, so a default font set by the application is gone as well
        io.FontDefault = ownFontIsDefault ? ownFont : nullptr;
       #endif
    }

    // Whether the font atlas holds nothing but the font this widget added: a single font from a
    // single source, and it is ours. Anything the application added or merged in makes it theirs.
    bool atlasHoldsOnlyOwnFont() const
    {
       #ifndef DGL_NO_SHARED_RESOURCES
        const ImFontAtlas* const atlas = ImGui::GetIO().Fonts;
        return ownFont != nullptr
            && atlas->Fonts.Size == 1
            && atlas->Fonts[0] == ownFont
            && atlas->Sources.Size == 1
            && atlas->Sources[0].DstFont == ownFont
            && atlas->Sources[0].FontDataSize == static_cast<int>(daf_resources::dejavusans_ttf_size);
       #else
        return false;
       #endif
    }

    // Follow a scale factor change of the window, with the context current and no frame open.
    // Returns true if the font atlas was rebuilt and needs uploading again.
    bool updateScaleFactor()
    {
        const double newScaleFactor = self->getTopLevelWidget()->getScaleFactor();

        if (d_isEqual(scaleFactor, newScaleFactor) || ! (newScaleFactor > 0.0))
            return false;

        const double oldScaleFactor = scaleFactor;
        scaleFactor = newScaleFactor;

        /* Rescale only the sizes, keeping colours and every other style setting the application
           made. If the application changed sizes since the last scaling, they were made for the
           old factor and become the reference; otherwise keep scaling from the reference we have,
           so the sizes do not drift over repeated changes. */
        ImGuiStyle& style(ImGui::GetStyle());

        if (! haveSameScaledSizes(style, scaledStyle))
        {
            referenceStyle = style;
            referenceScaleFactor = oldScaleFactor;
        }

        ImGuiStyle rescaled(referenceStyle);
        rescaled.ScaleAllSizes(static_cast<float>(scaleFactor / referenceScaleFactor));
        copyScaledSizes(style, rescaled);
        scaledStyle = style;

        /* Rebuild the font atlas only when it holds nothing but our own font. Fonts the
           application added are its own: rebuilding would destroy them under the ImFont pointers
           it holds. It can rebuild them at the new factor from onImGuiPrepareFrame(). */
        if (! atlasHoldsOnlyOwnFont())
            return false;

        buildFonts();
        return true;
    }

    float getDisplayX() const noexcept;
    float getDisplayY() const noexcept;
    double getTime() const noexcept;

    inline double getTimeDelta() noexcept
    {
        const double time = getTime();
        const double delta = time - lastFrameTime;
        lastFrameTime = time;
        return delta;
    }

    void handleModifiers(ImGuiIO& io, const uint mods)
    {
        if (lastModifiers == mods)
            return;

        if ((mods & kModifierShift) != (lastModifiers & kModifierShift))
            io.AddKeyEvent(ImGuiMod_Shift, mods & kModifierShift);

        if ((mods & kModifierControl) != (lastModifiers & kModifierControl))
            io.AddKeyEvent(ImGuiMod_Ctrl, mods & kModifierControl);

        if ((mods & kModifierAlt) != (lastModifiers & kModifierAlt))
            io.AddKeyEvent(ImGuiMod_Alt, mods & kModifierAlt);

        if ((mods & kModifierSuper) != (lastModifiers & kModifierSuper))
            io.AddKeyEvent(ImGuiMod_Super, mods & kModifierSuper);

        lastModifiers = mods;
    }

    DAF_DECLARE_NON_COPYABLE(PrivateData)
};

// --------------------------------------------------------------------------------------------------------------------

template <class BaseWidget>
void ImGuiWidget<BaseWidget>::setFontSize(const float fontSize)
{
    ImGui::SetCurrentContext(imData->context);

    // Built at the factor the style was last scaled for; should the window have changed scale
    // since, the next frame rebuilds both at the new one.
    imData->fontSize = fontSize;
    imData->buildFonts();
}

template <class BaseWidget>
void ImGuiWidget<BaseWidget>::rebuildFontTexture()
{
    ImGui::SetCurrentContext(imData->context);

   #if defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3) || defined(DGL_USE_OPENGL3)
    ImGui_ImplOpenGL3_DestroyFontsTexture();
    ImGui_ImplOpenGL3_CreateFontsTexture();
   #else
    ImGui_ImplOpenGL2_DestroyFontsTexture();
    ImGui_ImplOpenGL2_CreateFontsTexture();
   #endif
}

template <class BaseWidget>
void ImGuiWidget<BaseWidget>::idleCallback()
{
    BaseWidget::repaint();
}

template <class BaseWidget>
void ImGuiWidget<BaseWidget>::onDisplay()
{
   #ifdef DGL_USE_OPENGL3
    const OpenGL3GraphicsContext& gl3context
        = static_cast<const OpenGL3GraphicsContext&>(BaseWidget::getGraphicsContext());
    glUseProgram(0);
   #endif

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());

    io.DeltaTime = imData->getTimeDelta();

    // Pick up a scale factor change, from the host or the windowing system, here: the font atlas
    // can only be rebuilt with the context current and outside of a frame.
    if (imData->updateScaleFactor())
        rebuildFontTexture();

    // Before the backend's NewFrame: the backend only (re)creates its font
    // texture when it has none, so a subclass that rebuilt io.Fonts here and
    // called rebuildFontTexture() gets the new atlas uploaded for this frame.
    onImGuiPrepareFrame();

   #if defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3) || defined(DGL_USE_OPENGL3)
    ImGui_ImplOpenGL3_NewFrame();
   #else
    ImGui_ImplOpenGL2_NewFrame();
   #endif

    ImGui::NewFrame();
    onImGuiDisplay();
    ImGui::Render();

    /*
    const GraphicsContext& gc(getGraphicsContext());
    Color::fromHTML("#373").setFor(gc);
    Rectangle<double> r(0, 0, getWidth(), getHeight());
    r.draw(gc);
    */

    if (ImDrawData* const data = ImGui::GetDrawData())
    {
        data->DisplayPos.x = -imData->getDisplayX();
        data->DisplayPos.y = imData->getDisplayY();
       #if defined(DGL_USE_GLES2) || defined(DGL_USE_GLES3) || defined(DGL_USE_OPENGL3)
        ImGui_ImplOpenGL3_RenderDrawData(data);
       #else
        ImGui_ImplOpenGL2_RenderDrawData(data);
       #endif
    }

   #ifdef DGL_USE_OPENGL3
    glUseProgram(gl3context.program);
   #endif
}

template <class BaseWidget>
bool ImGuiWidget<BaseWidget>::wantsKeyboardFocus()
{
    ImGui::SetCurrentContext(imData->context);

    return ImGui::GetIO().WantTextInput;
}

template <class BaseWidget>
bool ImGuiWidget<BaseWidget>::onKeyboard(const Widget::KeyboardEvent& event)
{
    if (BaseWidget::onKeyboard(event))
        return true;

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());
    imData->handleModifiers(io, event.mod);

    // d_stdout("onKeyboard %u %u %u", event.key, event.mod, event.keycode);

    ImGuiKey key;
    switch (event.key)
    {
    case '\t': key = ImGuiKey_Tab; break;
    case '0': key = ImGuiKey_0; break;
    case '1': key = ImGuiKey_1; break;
    case '2': key = ImGuiKey_2; break;
    case '3': key = ImGuiKey_3; break;
    case '4': key = ImGuiKey_4; break;
    case '5': key = ImGuiKey_5; break;
    case '6': key = ImGuiKey_6; break;
    case '7': key = ImGuiKey_7; break;
    case '8': key = ImGuiKey_8; break;
    case '9': key = ImGuiKey_9; break;
    case 'a': key = ImGuiKey_A; break;
    case 'b': key = ImGuiKey_B; break;
    case 'c': key = ImGuiKey_C; break;
    case 'd': key = ImGuiKey_D; break;
    case 'e': key = ImGuiKey_E; break;
    case 'f': key = ImGuiKey_F; break;
    case 'g': key = ImGuiKey_G; break;
    case 'h': key = ImGuiKey_H; break;
    case 'i': key = ImGuiKey_I; break;
    case 'j': key = ImGuiKey_J; break;
    case 'k': key = ImGuiKey_K; break;
    case 'l': key = ImGuiKey_L; break;
    case 'm': key = ImGuiKey_M; break;
    case 'n': key = ImGuiKey_N; break;
    case 'o': key = ImGuiKey_O; break;
    case 'p': key = ImGuiKey_P; break;
    case 'q': key = ImGuiKey_Q; break;
    case 'r': key = ImGuiKey_R; break;
    case 's': key = ImGuiKey_S; break;
    case 't': key = ImGuiKey_T; break;
    case 'u': key = ImGuiKey_U; break;
    case 'v': key = ImGuiKey_V; break;
    case 'w': key = ImGuiKey_W; break;
    case 'x': key = ImGuiKey_X; break;
    case 'y': key = ImGuiKey_Y; break;
    case 'z': key = ImGuiKey_Z; break;
    case '\'': key = ImGuiKey_Apostrophe; break;
    case ',': key = ImGuiKey_Comma; break;
    case '-': key = ImGuiKey_Minus; break;
    case '.': key = ImGuiKey_Period; break;
    case '/': key = ImGuiKey_Slash; break;
    case ';': key = ImGuiKey_Semicolon; break;
    case '=': key = ImGuiKey_Equal; break;
    case '[': key = ImGuiKey_LeftBracket; break;
    case '\\': key = ImGuiKey_Backslash; break;
    case ']': key = ImGuiKey_RightBracket; break;
    case '`': key = ImGuiKey_GraveAccent; break;
    case kKeyBackspace: key = ImGuiKey_Backspace; break;
    case kKeyEnter: key = ImGuiKey_Enter; break;
    case kKeyEscape: key = ImGuiKey_Escape; break;
    case kKeyDelete: key = ImGuiKey_Delete; break;
    case kKeySpace: key = ImGuiKey_Space; break;
    case kKeyF1: key = ImGuiKey_F1; break;
    case kKeyF2: key = ImGuiKey_F2; break;
    case kKeyF3: key = ImGuiKey_F3; break;
    case kKeyF4: key = ImGuiKey_F4; break;
    case kKeyF5: key = ImGuiKey_F5; break;
    case kKeyF6: key = ImGuiKey_F6; break;
    case kKeyF7: key = ImGuiKey_F7; break;
    case kKeyF8: key = ImGuiKey_F8; break;
    case kKeyF9: key = ImGuiKey_F9; break;
    case kKeyF10: key = ImGuiKey_F10; break;
    case kKeyF11: key = ImGuiKey_F11; break;
    case kKeyF12: key = ImGuiKey_F12; break;
    case kKeyPageUp: key = ImGuiKey_PageUp; break;
    case kKeyPageDown: key = ImGuiKey_PageDown; break;
    case kKeyEnd: key = ImGuiKey_End; break;
    case kKeyHome: key = ImGuiKey_Home; break;
    case kKeyLeft: key = ImGuiKey_LeftArrow; break;
    case kKeyUp: key = ImGuiKey_UpArrow; break;
    case kKeyRight: key = ImGuiKey_RightArrow; break;
    case kKeyDown: key = ImGuiKey_DownArrow; break;
    case kKeyPrintScreen: key = ImGuiKey_PrintScreen; break;
    case kKeyInsert: key = ImGuiKey_Insert; break;
    case kKeyPause: key = ImGuiKey_Pause; break;
    case kKeyMenu: key = ImGuiKey_Menu; break;
    case kKeyNumLock: key = ImGuiKey_NumLock; break;
    case kKeyScrollLock: key = ImGuiKey_ScrollLock; break;
    case kKeyCapsLock: key = ImGuiKey_CapsLock; break;
    case kKeyShiftL: key = ImGuiKey_LeftShift; break;
    case kKeyShiftR: key = ImGuiKey_RightShift; break;
    case kKeyControlL: key = ImGuiKey_LeftCtrl; break;
    case kKeyControlR: key = ImGuiKey_RightCtrl; break;
    case kKeyAltL: key = ImGuiKey_LeftAlt; break;
    case kKeyAltR: key = ImGuiKey_RightAlt; break;
    case kKeySuperL: key = ImGuiKey_LeftSuper; break;
    case kKeySuperR: key = ImGuiKey_RightSuper; break;
    case kKeyPad0: key = ImGuiKey_Keypad0; break;
    case kKeyPad1: key = ImGuiKey_Keypad1; break;
    case kKeyPad2: key = ImGuiKey_Keypad2; break;
    case kKeyPad3: key = ImGuiKey_Keypad3; break;
    case kKeyPad4: key = ImGuiKey_Keypad4; break;
    case kKeyPad5: key = ImGuiKey_Keypad5; break;
    case kKeyPad6: key = ImGuiKey_Keypad6; break;
    case kKeyPad7: key = ImGuiKey_Keypad7; break;
    case kKeyPad8: key = ImGuiKey_Keypad8; break;
    case kKeyPad9: key = ImGuiKey_Keypad9; break;
    /*
    case kKeyPadEnter: key = ImGuiKey_; break;
    case kKeyPadPageUp: key = ImGuiKey_; break;
    case kKeyPadPageDown: key = ImGuiKey_; break;
    case kKeyPadEnd: key = ImGuiKey_; break;
    case kKeyPadHome: key = ImGuiKey_; break;
    case kKeyPadLeft: key = ImGuiKey_; break;
    case kKeyPadUp: key = ImGuiKey_; break;
    case kKeyPadRight: key = ImGuiKey_; break;
    case kKeyPadDown: key = ImGuiKey_; break;
    case kKeyPadClear: key = ImGuiKey_; break;
    case kKeyPadInsert: key = ImGuiKey_; break;
    case kKeyPadDelete: key = ImGuiKey_; break;
    */
    case kKeyPadEqual: key = ImGuiKey_KeypadEqual; break;
    case kKeyPadMultiply: key = ImGuiKey_KeypadMultiply; break;
    case kKeyPadAdd: key = ImGuiKey_KeypadAdd; break;
    /*
    case kKeyPadSeparator: key = ImGuiKey_; break;
    */
    case kKeyPadSubtract: key = ImGuiKey_KeypadSubtract; break;
    case kKeyPadDecimal: key = ImGuiKey_KeypadDecimal; break;
    case kKeyPadDivide: key = ImGuiKey_KeypadDivide; break;
    /* FIXME missing ImGuiKey_KeypadEnter */
    // A key ImGui has no name for (space, for one) still belongs to a text field being edited.
    default: return io.WantTextInput;
    }

    io.AddKeyEvent(key, event.press);
    return io.WantTextInput;
}

template <class BaseWidget>
bool ImGuiWidget<BaseWidget>::onCharacterInput(const Widget::CharacterInputEvent& event)
{
    if (BaseWidget::onCharacterInput(event))
        return true;

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());
    imData->handleModifiers(io, event.mod);

    // d_stdout("input %u %u %lu '%s'", event.keycode, event.character, std::strlen(event.string), event.string);

    io.AddInputCharacter(event.character);
    return io.WantCaptureKeyboard;
}

template <class BaseWidget>
bool ImGuiWidget<BaseWidget>::onMouse(const Widget::MouseEvent& event)
{
    if (BaseWidget::onMouse(event))
        return true;

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());
    imData->handleModifiers(io, event.mod);

    ImGuiMouseButton button;
    switch (event.button)
    {
    case kMouseButtonLeft: button = ImGuiMouseButton_Left; break;
    case kMouseButtonRight: button = ImGuiMouseButton_Right; break;
    case kMouseButtonMiddle: button = ImGuiMouseButton_Middle; break;
    default: return false;
    }

    io.AddMouseButtonEvent(button, event.press);
    return io.WantCaptureMouse;
}

template <class BaseWidget>
bool ImGuiWidget<BaseWidget>::onMotion(const Widget::MotionEvent& event)
{
    if (BaseWidget::onMotion(event))
        return true;

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());
    imData->handleModifiers(io, event.mod);

    io.AddMousePosEvent(event.pos.getX(), event.pos.getY());
    return false;
}

template <class BaseWidget>
bool ImGuiWidget<BaseWidget>::onScroll(const Widget::ScrollEvent& event)
{
    if (BaseWidget::onScroll(event))
        return true;

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());
    imData->handleModifiers(io, event.mod);

    io.AddMouseWheelEvent(event.delta.getX(), event.delta.getY());
    return io.WantCaptureMouse;
}

template <class BaseWidget>
void ImGuiWidget<BaseWidget>::onResize(const Widget::ResizeEvent& event)
{
    BaseWidget::onResize(event);

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());
    io.DisplaySize.x = event.size.getWidth();
    io.DisplaySize.y = event.size.getHeight();
}

template <class BaseWidget>
void ImGuiWidget<BaseWidget>::onFocusChanged(const Widget::FocusEvent& event)
{
    BaseWidget::onFocusChanged(event);

    // A grab is a temporary loan of the focus, as a menu or a pointer grab does, and the window
    // gets it back when the grab ends, so only a real focus change is acted on.
    if (event.mode != kCrossingNormal)
        return;

    // Gaining focus needs nothing: the key state was already cleared on the way out, and the
    // modifier cache below makes the next key event resend whatever is still held.
    if (event.focus)
        return;

    ImGui::SetCurrentContext(imData->context);

    ImGuiIO& io(ImGui::GetIO());

    // Clear the KEYBOARD state only, and deliberately not through io.AddFocusEvent(false).
    //
    // What focus loss must fix is a key or modifier that was held when the window lost focus and
    // released somewhere else: ImGui never sees the release, so it believes the key is still down
    // and the next Ctrl-click or Shift-drag behaves as though a modifier were held.
    //
    // AddFocusEvent(false) does fix that, but it also makes ImGui call ClearInputMouse() at the
    // end of the frame, which drops a held mouse button. A plugin editor embedded in a DAW does
    // not own keyboard focus the way a standalone window does, so focus-out arrives during
    // ordinary use, and a knob drag then dies the moment the user starts moving the pointer. That
    // is not theoretical: it shipped, and every DAF plugin's knobs stopped turning
    // (dusk-audio/plugins#233 follow-up). An earlier attempt tried to dodge it by filtering on the
    // crossing mode, but kCrossingNormal is precisely the pointer-driven case that occurs mid-drag.
    //
    // ClearInputKeys() skips mouse keys by design (it does the ImGui::IsMouseKey(key) continue),
    // so it clears exactly the half that needs clearing and leaves a gesture in progress alone.
    io.ClearInputKeys();

    // The cache of what ImGui was last told goes with it, otherwise a modifier still held when
    // the focus comes back is never resent.
    imData->lastModifiers = 0;
}

// --------------------------------------------------------------------------------------------------------------------
// The primary template declares one constructor per base (SubWidget, TopLevelWidget, StandaloneWindow), but each
// base only has a definition for its own. MSVC warns (C4661) about the other constructors at every explicit
// instantiation below; they are intentionally left undefined, so silence it for this block only.

#ifdef _MSC_VER
# pragma warning(push)
# pragma warning(disable:4661)
#endif

// --------------------------------------------------------------------------------------------------------------------
// ImGuiSubWidget

template <>
float ImGuiWidget<SubWidget>::PrivateData::getDisplayX() const noexcept
{
    return self->getAbsoluteX();
}

template <>
float ImGuiWidget<SubWidget>::PrivateData::getDisplayY() const noexcept
{
    return self->getWindow().getHeight() - self->getAbsoluteY() - self->getHeight();
}

template <>
double ImGuiWidget<SubWidget>::PrivateData::getTime() const noexcept
{
    return self->getApp().getTime();
}

template <>
ImGuiWidget<SubWidget>::ImGuiWidget(Widget* const parent, const float fontSize)
    : SubWidget(parent),
      imData(new PrivateData(this, fontSize))
{
    getWindow().addIdleCallback(this, 1000 / 60); // 60 fps
}

template <>
ImGuiWidget<SubWidget>::~ImGuiWidget()
{
    getWindow().removeIdleCallback(this);

    // the renderer backend deletes its GL objects on shutdown, see PrivateData::~PrivateData
    const Window::ScopedGraphicsContext sgc(getWindow());
    delete imData;
}

template class ImGuiWidget<SubWidget>;

// --------------------------------------------------------------------------------------------------------------------
// ImGuiTopLevelWidget

template <>
float ImGuiWidget<TopLevelWidget>::PrivateData::getDisplayX() const noexcept
{
    return 0.0f;
}

template <>
float ImGuiWidget<TopLevelWidget>::PrivateData::getDisplayY() const noexcept
{
    return 0.0f;
}

template <>
double ImGuiWidget<TopLevelWidget>::PrivateData::getTime() const noexcept
{
    return self->getApp().getTime();
}

template <>
ImGuiWidget<TopLevelWidget>::ImGuiWidget(Window& windowToMapTo, const float fontSize)
    : TopLevelWidget(windowToMapTo),
      imData(new PrivateData(this, fontSize))
{
    addIdleCallback(this, 1000 / 60); // 60 fps
}

template <>
ImGuiWidget<TopLevelWidget>::~ImGuiWidget()
{
    removeIdleCallback(this);

    // the renderer backend deletes its GL objects on shutdown, see PrivateData::~PrivateData
    const Window::ScopedGraphicsContext sgc(getWindow());
    delete imData;
}

template class ImGuiWidget<TopLevelWidget>;

// --------------------------------------------------------------------------------------------------------------------
// ImGuiStandaloneWindow

template <>
float ImGuiWidget<StandaloneWindow>::PrivateData::getDisplayX() const noexcept
{
    return 0.0f;
}

template <>
float ImGuiWidget<StandaloneWindow>::PrivateData::getDisplayY() const noexcept
{
    return 0.0f;
}

template <>
double ImGuiWidget<StandaloneWindow>::PrivateData::getTime() const noexcept
{
    return self->getApp().getTime();
}

template <>
ImGuiWidget<StandaloneWindow>::ImGuiWidget(Application& app, const float fontSize)
    : StandaloneWindow(app),
      imData(new PrivateData(this, fontSize))
{
    Window::addIdleCallback(this, 1000 / 60); // 60 fps
    done();
}

template <>
ImGuiWidget<StandaloneWindow>::ImGuiWidget(Application& app, Window& transientParentWindow, const float fontSize)
    : StandaloneWindow(app, transientParentWindow),
      imData(new PrivateData(this, fontSize))
{
    Window::addIdleCallback(this, 1000 / 60); // 60 fps
    done();
}

template <>
ImGuiWidget<StandaloneWindow>::~ImGuiWidget()
{
    Window::removeIdleCallback(this);

    // the renderer backend deletes its GL objects on shutdown, see PrivateData::~PrivateData
    const Window::ScopedGraphicsContext sgc(static_cast<Window&>(*this));
    delete imData;
}

template class ImGuiWidget<StandaloneWindow>;

#ifdef _MSC_VER
# pragma warning(pop)
#endif

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------
// extra ImGui calls

namespace ImGui {

// --------------------------------------------------------------------------------------------------------------------

void RightAlignedLabelText(const char* label, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    RightAlignedLabelTextV(label, fmt, args);
    va_end(args);
}

void RightAlignedLabelTextV(const char* label, const char* fmt, va_list args)
{
    ImGuiWindow* window = GetCurrentWindow();
    if (window->SkipItems)
        return;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const float w = CalcItemWidth();

    const char* value_text_begin, *value_text_end;
    ImFormatStringToTempBufferV(&value_text_begin, &value_text_end, fmt, args);
    const ImVec2 value_size = CalcTextSize(value_text_begin, value_text_end, false);
    const ImVec2 label_size = CalcTextSize(label, NULL, true);

    const ImVec2 pos = window->DC.CursorPos;
    const ImRect value_bb(pos, pos + ImVec2(w, value_size.y + style.FramePadding.y * 2));
    const ImRect total_bb(pos, pos + ImVec2(w + (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f), ImMax(value_size.y, label_size.y) + style.FramePadding.y * 2));
    ItemSize(total_bb, style.FramePadding.y);
    if (!ItemAdd(total_bb, 0))
        return;

    // Render
    RenderTextClipped(value_bb.Max - value_size - style.FramePadding, value_bb.Max, value_text_begin, value_text_end, &value_size, ImVec2(0.0f, 0.0f));
    RenderText(ImVec2(value_bb.Max.x + style.ItemInnerSpacing.x, value_bb.Min.y + style.FramePadding.y), label);
}

// --------------------------------------------------------------------------------------------------------------------

}
