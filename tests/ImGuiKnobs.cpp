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

/* The vendored imgui-knobs, driven headless (no renderer, no window) with values outside the
 * knob's range, the way the text box under a knob lets a user type them in. */

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "widgets/imgui/DearImGui/imgui.cpp"
#include "widgets/imgui/DearImGui/imgui_draw.cpp"
#include "widgets/imgui/DearImGui/imgui_tables.cpp"
#include "widgets/imgui/DearImGui/imgui_widgets.cpp"
#include "widgets/imgui/DearImGuiKnobs/imgui-knobs.cpp"

#include <cmath>

// --------------------------------------------------------------------------------------------------------------------

static bool near(const float a, const float b)
{
    return std::fabs(a - b) < 1e-5f;
}

// draws one knob in a frame of its own, returns its "changed" result
static bool drawKnob(float* const value, const float vmin, const float vmax,
                     const ImGuiKnobVariant variant, const ImGuiKnobFlags flags)
{
    ImGuiIO& io(ImGui::GetIO());
    io.DisplaySize = ImVec2(400.f, 400.f);
    io.DeltaTime = 1.f / 60.f;

    ImGui::NewFrame();
    ImGui::Begin("knobs");
    const bool changed = ImGuiKnobs::Knob("knob", value, vmin, vmax, 0.f, NULL, variant, 0.f, flags);
    ImGui::End();
    ImGui::Render();

    // every vertex of the frame must be on screen
    // (a range check rather than isnan, the tests are built with -ffast-math)
    const ImDrawData* const drawData = ImGui::GetDrawData();
    for (int i = 0; i < drawData->CmdListsCount; ++i)
    {
        const ImDrawList* const list = drawData->CmdLists[i];
        for (int j = 0; j < list->VtxBuffer.Size; ++j)
        {
            const ImVec2& pos(list->VtxBuffer[j].pos);
            if (!(pos.x >= -1.f && pos.x <= 401.f && pos.y >= -1.f && pos.y <= 401.f))
            {
                d_stderr2("vertex drawn off screen: %f %f", pos.x, pos.y);
                std::abort();
            }
        }
    }

    return changed;
}

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    using ImGuiKnobs::detail::value_to_t;

    // knob position is always within [0, 1]
    {
        DAF_ASSERT_EQUAL(near(value_to_t(0.5f, 0.f, 1.f, 0), 0.5f), true, "in-range value maps linearly");
        DAF_ASSERT_EQUAL(near(value_to_t(5.f, 0.f, 1.f, 0), 1.f), true, "value above range draws at the end");
        DAF_ASSERT_EQUAL(near(value_to_t(-5.f, 0.f, 1.f, 0), 0.f), true, "value below range draws at the start");
        DAF_ASSERT_EQUAL(near(value_to_t(1.f, 1.f, 1.f, 0), 0.f), true, "empty range does not divide by zero");
        DAF_ASSERT_EQUAL(near(value_to_t(12, 0, 10, 0), 1.f), true, "integer knob above range");
    }

    // logarithmic knob position
    {
        const ImGuiKnobFlags log = ImGuiKnobFlags_Logarithmic;
        DAF_ASSERT_EQUAL(near(value_to_t(200.f, 20.f, 2000.f, log), 0.5f), true, "geometric centre is halfway");
        DAF_ASSERT_EQUAL(near(value_to_t(20000.f, 20.f, 2000.f, log), 1.f), true, "log value above range");
        DAF_ASSERT_EQUAL(near(value_to_t(-1.f, 20.f, 2000.f, log), 0.f), true, "log value below zero");
        DAF_ASSERT_EQUAL(near(value_to_t(0.5f, 0.f, 1.f, log), 0.5f), true, "log with v_min of 0 is drawn linearly");
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = NULL;
    {
        unsigned char* pixels;
        int width, height;
        ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }

    // out-of-range values are drawn on screen for every variant, and left alone without AlwaysClamp
    {
        const ImGuiKnobVariant variants[] = {
            ImGuiKnobVariant_Tick, ImGuiKnobVariant_Dot, ImGuiKnobVariant_Wiper, ImGuiKnobVariant_WiperOnly,
            ImGuiKnobVariant_WiperDot, ImGuiKnobVariant_Stepped, ImGuiKnobVariant_Space,
        };
        for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); ++i)
        {
            float value = 7.f;
            drawKnob(&value, 0.f, 1.f, variants[i], 0);
            DAF_ASSERT_EQUAL(near(value, 7.f), true, "value kept as is without AlwaysClamp");

            value = -7.f;
            drawKnob(&value, 0.f, 1.f, variants[i], ImGuiKnobFlags_Logarithmic);
            DAF_ASSERT_EQUAL(near(value, -7.f), true, "value kept as is without AlwaysClamp (log)");
        }
    }

    // AlwaysClamp brings the value back into range and reports the change
    {
        float value = 7.f;
        DAF_ASSERT_EQUAL(drawKnob(&value, 0.f, 1.f, ImGuiKnobVariant_Wiper, ImGuiKnobFlags_AlwaysClamp), true,
                         "clamping reports a change");
        DAF_ASSERT_EQUAL(near(value, 1.f), true, "value clamped to v_max");

        value = -7.f;
        drawKnob(&value, 20.f, 2000.f, ImGuiKnobVariant_Wiper, ImGuiKnobFlags_AlwaysClamp | ImGuiKnobFlags_Logarithmic);
        DAF_ASSERT_EQUAL(near(value, 20.f), true, "value clamped to v_min");

        value = 0.25f;
        DAF_ASSERT_EQUAL(drawKnob(&value, 0.f, 1.f, ImGuiKnobVariant_Wiper, ImGuiKnobFlags_AlwaysClamp), false,
                         "in-range value is not a change");
        DAF_ASSERT_EQUAL(near(value, 0.25f), true, "in-range value untouched");
    }

    ImGui::DestroyContext();
    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
