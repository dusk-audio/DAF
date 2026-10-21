// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC
#include "Application.hpp"
#include "Window.hpp"
#include "DearImGui.hpp"

#include <cstdio>

namespace
{
struct State
{
    bool ctrl = false, shift = false, alt = false, super = false;
    bool up = false, mouse = false;
};

class Harness : public DGL::ImGuiTopLevelWidget
{
public:
    explicit Harness(DGL::Window& window) : DGL::ImGuiTopLevelWidget(window)
    {
        setSize(320, 240);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigMacOSXBehaviors = false;
    }

    void key(unsigned int code, unsigned int modifiers, bool pressed)
    {
        KeyboardEvent event;
        event.key = code;
        event.mod = modifiers;
        event.press = pressed;
        onKeyboard(event);
    }

    void loseFocus()
    {
        FocusEvent event;
        onFocusChanged(event);
    }

    void mouse(bool pressed)
    {
        MouseEvent event;
        event.button = DGL::kMouseButtonLeft;
        event.pos = {20, 20};
        event.press = pressed;
        onMouse(event);
    }

    State frame()
    {
        onDisplay();
        return state;
    }

private:
    State state;
    void onImGuiDisplay() override
    {
        const auto& io = ImGui::GetIO();
        state.ctrl = io.KeyCtrl;
        state.shift = io.KeyShift;
        state.alt = io.KeyAlt;
        state.super = io.KeySuper;
        state.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, false);
        state.mouse = io.MouseDown[0];
    }
};
}

int main()
{
    DGL::Application app;
    DGL::Window window(app);
    window.setSize(320, 240);
    const DGL::Window::ScopedGraphicsContext context(window);
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* message)
    {
        ++checks;
        if (!ok) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
    };
    const unsigned int modifiers[] = {DGL::kModifierControl, DGL::kModifierShift,
        DGL::kModifierAlt, DGL::kModifierSuper,
        DGL::kModifierControl | DGL::kModifierShift | DGL::kModifierAlt | DGL::kModifierSuper};
    for (const auto modifier : modifiers)
    {
        Harness view(window);
        view.frame();
        view.key('a', modifier, true);
        view.loseFocus();
        view.frame();
        view.key('a', 0, false);
        view.frame();
        view.key(DGL::kKeyUp, 0, true);
        const auto state = view.frame();
        check(state.up && !state.ctrl && !state.shift && !state.alt && !state.super,
              "a queued modifier survived focus loss and its release");
    }
    for (const auto modifier : modifiers)
    {
        Harness view(window);
        view.frame();
        view.key('a', modifier, true);
        view.frame();
        view.loseFocus();
        view.key(DGL::kKeyUp, modifier, true);
        const auto state = view.frame();
        check(state.up
              && state.ctrl == ((modifier & DGL::kModifierControl) != 0)
              && state.shift == ((modifier & DGL::kModifierShift) != 0)
              && state.alt == ((modifier & DGL::kModifierAlt) != 0)
              && state.super == ((modifier & DGL::kModifierSuper) != 0),
              "a modifier still held on focus return was not restored");
    }
    {
        Harness view(window);
        view.frame();
        view.mouse(true);
        check(view.frame().mouse, "mouse press did not reach the adapter");
        view.loseFocus();
        check(view.frame().mouse, "keyboard focus loss cancelled a mouse drag");
        view.mouse(false);
        check(!view.frame().mouse, "mouse release did not finish the drag");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
