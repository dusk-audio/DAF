/*
 * Dusk Audio Framework (DAF)
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

#include "DafUI.hpp"
#include "ViewDisposePlugin.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

START_NAMESPACE_DAF

// Reads the meter straight off the instance pointer on every idle tick and every paint, as metering UIs do, with
// nothing to go on but the pointer. Exits the process, failing AUViewDispose, when its idle runs or it reads the
// plugin after the plugin was deleted: under AddressSanitizer either is a use after free, without it a silent one.
//
// DAF_TEST_AU_VIEW_SLOW (read when the UI is made) slows one callback down, so that a unit disposed on another thread
// lands inside it, with the plugin's "UI busy" parameter set:
// - "idle": each idle tick takes the pointer and sleeps, then reads through it and edits a parameter, calling the unit.
// - "resize": the first paint after the host sets the plugin's "Resize" parameter sleeps, then resizes the UI, as a
//   resize grip does. It reads nothing of the plugin after the sleep, as the plugin may be gone by then.
//
// The first paint after the plugin is gone edits a parameter, as a click into a view the host kept does: the UI must
// not call the unit then (AUViewDispose watches).
class ViewDisposeUI : public UI
{
public:
    ViewDisposeUI()
        : UI(240, 120),
          fInstance(getPluginInstancePointer()),
          fSlow(slowPlace()),
          fResized(false),
          fEditedWithoutPlugin(false),
          fPeak(0.0f),
          fEdit(0.0f) {}

protected:
    void parameterChanged(uint32_t, float) override {}

    void uiIdle() override
    {
        if (fInstance != nullptr && fInstance == deletedViewDisposePlugin().load())
            fail("the idle timer ran after the plugin was deleted");

        // taken once and used to the end of the tick, as a UI drawing a frame of meters does
        void* const instance = getPluginInstancePointer();
        if (fSlow == kSlowIdle)
            slowRead(instance);
        else
            readMeter(instance);

        repaint();
    }

    void onDisplay() override
    {
        void* const instance = getPluginInstancePointer();
        if (fSlow == kSlowResize && ! fResized && instance != nullptr
            && static_cast<ViewDisposePlugin*>(instance)->resizeRequested())
        {
            fResized = true;
            static_cast<ViewDisposePlugin*>(instance)->setUIBusy(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            setSize(getWidth() + 20, getHeight() + 20);
            return;
        }

        if (instance == nullptr && fInstance != nullptr && ! fEditedWithoutPlugin)
        {
            fEditedWithoutPlugin = true;
            editParameter(ViewDisposePlugin::kParameterEdit, true);
            setParameterValue(ViewDisposePlugin::kParameterEdit, fEdit = 1.0f - fEdit);
            editParameter(ViewDisposePlugin::kParameterEdit, false);
        }

        readMeter(instance);
    }

private:
    enum Slow { kSlowNone, kSlowIdle, kSlowResize };

    const void* const fInstance;
    const Slow fSlow;
    bool fResized;
    bool fEditedWithoutPlugin;
    float fPeak;
    float fEdit;

    void readMeter(void* const instance)
    {
        if (instance == nullptr)
            return;
        if (instance == deletedViewDisposePlugin().load())
            fail("the UI read the plugin after it was deleted");

        fPeak = static_cast<ViewDisposePlugin*>(instance)->readPeak();
    }

    void slowRead(void* const instance)
    {
        if (instance == nullptr)
            return;

        ViewDisposePlugin* const plugin = static_cast<ViewDisposePlugin*>(instance);
        plugin->setUIBusy(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        readMeter(instance);

        // a knob turned, while a dispose on another thread may be waiting for this tick to end
        editParameter(ViewDisposePlugin::kParameterEdit, true);
        setParameterValue(ViewDisposePlugin::kParameterEdit, fEdit = 1.0f - fEdit);
        editParameter(ViewDisposePlugin::kParameterEdit, false);

        plugin->setUIBusy(false);
    }

    static Slow slowPlace()
    {
        const char* const place = std::getenv("DAF_TEST_AU_VIEW_SLOW");
        if (place == nullptr)
            return kSlowNone;
        if (std::strcmp(place, "idle") == 0)
            return kSlowIdle;
        if (std::strcmp(place, "resize") == 0)
            return kSlowResize;
        fail("DAF_TEST_AU_VIEW_SLOW must be idle or resize");
        return kSlowNone;
    }

    static void fail(const char* const what)
    {
        d_stderr2("ViewDisposeUI: %s", what);
        std::_Exit(1);
    }
};

UI* createUI()
{
    return new ViewDisposeUI();
}

END_NAMESPACE_DAF
