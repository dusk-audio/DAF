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
    bool up = false, a = false, mouse = false;
    int characters = 0;
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

    void character(unsigned int code)
    {
        CharacterInputEvent event;
        event.character = code;
        onCharacterInput(event);
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
        state.a = ImGui::IsKeyDown(ImGuiKey_A);
        state.mouse = io.MouseDown[0];
        state.characters = io.InputQueueCharacters.Size;
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
    {
        Harness view(window);
        view.frame();
        view.key('a', 0, true);
        view.character('a');
        view.loseFocus();
        const auto lost = view.frame();
        check(!lost.a && lost.characters == 0, "queued key or text input survived focus loss");
        view.character('b');
        check(view.frame().characters == 1, "text input after focus return was discarded");
    }
    for (const auto modifier : modifiers)
    {
        Harness view(window);
        view.frame();
        view.key('a', modifier, true);
        view.loseFocus();
        const auto lost = view.frame();
        check(!lost.ctrl && !lost.shift && !lost.alt && !lost.super,
              "a queued modifier returned on the first frame after focus loss");
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
        view.key('a', DGL::kModifierControl, true);
        view.loseFocus();
        const auto state = view.frame();
        check(state.mouse && !state.ctrl,
              "focus loss did not preserve a queued mouse press while clearing keyboard events");
        view.mouse(false);
        check(!view.frame().mouse, "queued mouse release did not finish the drag");
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
