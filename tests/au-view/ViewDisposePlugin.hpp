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

#pragma once

#include "DafPlugin.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>

START_NAMESPACE_DAF

// The plugin last deleted, which its UI must never read again (see ViewDisposeUI.cpp). AUViewDispose has one unit at
// a time, and a new plugin made at the same address clears it.
inline std::atomic<const void*>& deletedViewDisposePlugin() noexcept
{
    static std::atomic<const void*> deleted(nullptr);
    return deleted;
}

// A stereo pass-through with a peak meter, which the UI reads straight off the instance pointer. Two parameters are
// outputs the host reads: "UI reads" counts those reads, so the host can tell the UI ran before it disposes the unit,
// and "UI busy" is set while the UI is in a slow callback, so that the host can dispose the unit from another thread
// right then. "Edit" is an input the UI changes from its slow idle tick, calling into the unit meanwhile, and the host
// sets "Resize" to have the UI resize itself in its next paint.
class ViewDisposePlugin : public Plugin
{
public:
    enum Parameters { kParameterUIReads, kParameterUIBusy, kParameterEdit, kParameterResize, kParameterCount };

    ViewDisposePlugin() : Plugin(kParameterCount, 0, 0)
    {
        const void* self = this;
        deletedViewDisposePlugin().compare_exchange_strong(self, nullptr);
    }

    ~ViewDisposePlugin() override
    {
        deletedViewDisposePlugin().store(this);
    }

    // the UI's read of the meter
    float readPeak() noexcept
    {
        fReads.fetch_add(1, std::memory_order_relaxed);
        return fPeak.load(std::memory_order_relaxed);
    }

    void setUIBusy(const bool busy) noexcept
    {
        fUIBusy.store(busy, std::memory_order_relaxed);
    }

    bool resizeRequested() const noexcept
    {
        return fResize.load(std::memory_order_relaxed) > 0.5f;
    }

protected:
    const char* getLabel() const override { return "ViewDisposeTest"; }
    const char* getMaker() const override { return "DAF"; }
    const char* getLicense() const override { return "ISC"; }
    uint32_t getVersion() const override { return d_version(1, 0, 0); }

    void initParameter(const uint32_t index, Parameter& parameter) override
    {
        parameter.ranges.min = 0.0f;
        parameter.ranges.def = 0.0f;
        switch (index)
        {
        case kParameterUIReads:
            parameter.hints = kParameterIsOutput | kParameterIsInteger;
            parameter.name = "UI reads";
            parameter.symbol = "ui_reads";
            parameter.ranges.max = 16777216.0f;
            break;
        case kParameterUIBusy:
            parameter.hints = kParameterIsOutput | kParameterIsBoolean;
            parameter.name = "UI busy";
            parameter.symbol = "ui_busy";
            parameter.ranges.max = 1.0f;
            break;
        case kParameterEdit:
            parameter.hints = kParameterIsAutomatable;
            parameter.name = "Edit";
            parameter.symbol = "edit";
            parameter.ranges.max = 1.0f;
            break;
        default:
            parameter.hints = kParameterIsAutomatable | kParameterIsBoolean;
            parameter.name = "Resize";
            parameter.symbol = "resize";
            parameter.ranges.max = 1.0f;
            break;
        }
    }

    float getParameterValue(const uint32_t index) const override
    {
        switch (index)
        {
        case kParameterUIReads:
            return static_cast<float>(fReads.load(std::memory_order_relaxed));
        case kParameterUIBusy:
            return fUIBusy.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        case kParameterEdit:
            return fEdit.load(std::memory_order_relaxed);
        default:
            return fResize.load(std::memory_order_relaxed);
        }
    }

    void setParameterValue(const uint32_t index, const float value) override
    {
        if (index == kParameterEdit)
            fEdit.store(value, std::memory_order_relaxed);
        else if (index == kParameterResize)
            fResize.store(value, std::memory_order_relaxed);
    }

    void run(const float** const inputs, float** const outputs, const uint32_t frames) override
    {
        float peak = 0.0f;
        for (uint32_t c = 0; c < 2; ++c)
        {
            for (uint32_t i = 0; i < frames; ++i)
            {
                const float sample = inputs[c][i];
                outputs[c][i] = sample;
                peak = std::max(peak, std::abs(sample));
            }
        }
        fPeak.store(peak, std::memory_order_relaxed);
    }

private:
    std::atomic<float> fPeak{0.0f};
    std::atomic<uint32_t> fReads{0};
    std::atomic<bool> fUIBusy{false};
    std::atomic<float> fEdit{0.0f};
    std::atomic<float> fResize{0.0f};
};

END_NAMESPACE_DAF
