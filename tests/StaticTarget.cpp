/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2025 Filipe Coelho <falktx@falktx.com>
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

// API checks compile on every platform; the runtime fixture exercises shared application ownership.

#define DAF_PLUGIN_TARGET_STATIC

#include "tests.hpp"

#include "daf/src/DafUIInternal.hpp"

#include <type_traits>

static_assert(DAF_UI_USES_SIZE_REQUEST, "the static target must leave the editor's size to the host");

static_assert(std::is_same<decltype(&DAF_NAMESPACE::UIExporter::setWindowSizeFromHost),
                           void (DAF_NAMESPACE::UIExporter::*)(uint, uint)>::value,
              "a static-target host must be able to size the editor");

static_assert(std::is_same<decltype(&DAF_NAMESPACE::UIExporter::setWindowEmbeddedOffset),
                           bool (DAF_NAMESPACE::UIExporter::*)(int, int)>::value,
              "a static-target host must be able to place the embedded editor");

static_assert(std::is_same<decltype(&DAF_NAMESPACE::UIExporter::setWindowEmbeddedSurfaceGeometry),
                           bool (DAF_NAMESPACE::UIExporter::*)(int, int, uint, uint, double)>::value,
              "a static-target host must be able to supply Wayland surface coordinates");

#ifdef DAF_TEST_STATIC_TARGET_RUNTIME
#include "daf/src/DafUI.cpp"
#include "dgl/OpenGL.hpp"
#include "dgl/StandaloneWindow.hpp"

#include <chrono>
#include <memory>

START_NAMESPACE_DAF

static unsigned int uiIdleCalls = 0;
static unsigned int uiDestructions = 0;
static bool destructionContextValid = true;
static int constructionFailure = 0;

class TestUI : public UI
{
public:
    TestUI() : UI(160, 120)
    {
        if (constructionFailure == 1)
            throw 7;
    }
    ~TestUI() override
    {
        ++uiDestructions;
        destructionContextValid = destructionContextValid && glGetString(GL_VERSION) != nullptr;
    }

protected:
    void parameterChanged(uint32_t, float) override { getWindow().close(); }
    void uiIdle() override { ++uiIdleCalls; }
    void onDisplay() override
    {
        glClearColor(0.1f, 0.4f, 0.6f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
};

UI* createUI() { return constructionFailure == 2 ? nullptr : new TestUI; }

END_NAMESPACE_DAF

class HostWindow : public DGL_NAMESPACE::StandaloneWindow
{
public:
    explicit HostWindow(DGL_NAMESPACE::Application& app) : StandaloneWindow(app)
    {
        setSize(640, 480);
        done();
        show();
    }

protected:
    void onDisplay() override
    {
        glClearColor(0.3f, 0.3f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
};

class IdleCounter : public DGL_NAMESPACE::IdleCallback
{
public:
    unsigned int calls = 0;
    void idleCallback() override { ++calls; }
};

static std::unique_ptr<DAF_NAMESPACE::UIExporter> makeEditor(DGL_NAMESPACE::Application* const app,
                                                           const uintptr_t parent = 0)
{
    return std::unique_ptr<DAF_NAMESPACE::UIExporter>(new DAF_NAMESPACE::UIExporter(
        nullptr, parent, 48000.0, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, 1.0, DGL_NAMESPACE::Application::kTypeAuto, 0, 0xffffffff,
        "static-editor", DGL_NAMESPACE::Application::kWindowSystemAuto, app));
}

template<class Predicate>
static bool pumpUntil(DGL_NAMESPACE::Application& app, HostWindow& host, Predicate ready)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do
    {
        host.repaint();
        app.idle();
        if (ready())
            return true;
        DAF_NAMESPACE::d_msleep(5);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

int main()
{
    using DGL_NAMESPACE::Application;
    Application app(true);
    app.setClassName("static-host");
    HostWindow host(app);
    DAF_ASSERT_NOT_EQUAL(host.getNativeWindowHandle(), 0, "host window must open");
    IdleCounter idle;
    app.addIdleCallback(&idle);
    auto first = makeEditor(&app);
    auto second = makeEditor(&app);
    first->setWindowVisible(true);
    second->setWindowVisible(true);
    DAF_ASSERT_EQUAL(std::strcmp(app.getClassName(), "static-host"), 0, "editor must preserve host class name");
    const unsigned int beforeIdle = idle.calls;
    const unsigned int beforeUIIdle = DAF_NAMESPACE::uiIdleCalls;
    DAF_ASSERT_EQUAL(first->plugin_idle(), true, "first editor is open");
    DAF_ASSERT_EQUAL(second->plugin_idle(), true, "second editor is open");
    DAF_ASSERT_EQUAL(idle.calls, beforeIdle, "editor idle must not dispatch the shared application");
    DAF_ASSERT_EQUAL(DAF_NAMESPACE::uiIdleCalls, beforeUIIdle + 2, "editor-local idle must still run");
    DAF_ASSERT_EQUAL(pumpUntil(app, host, [&] {
        return first->getCompletedDisplayCount() > 0 && second->getCompletedDisplayCount() > 0;
    }), true, "host pump must draw both editors");
    DAF_ASSERT_NOT_EQUAL(idle.calls, beforeIdle, "host pump must run shared idle callbacks");
    for (int failure = 1; failure <= 2; ++failure)
    {
        DAF_NAMESPACE::constructionFailure = failure;
        bool failed = false;
        const DGL_NAMESPACE::Window::ScopedGraphicsContext context(host);
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        try
        {
            auto rejected = makeEditor(&app, host.getNativeWindowHandle());
        }
        catch (int code)
        {
            failed = failure == 1 && code == 7;
        }
        catch (const std::runtime_error&)
        {
            failed = failure == 2;
        }
        if (app.getWindowSystem() == Application::kWindowSystemWayland)
        {
            GLint binding = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
            DAF_ASSERT_EQUAL(static_cast<GLuint>(binding), texture, "failure must restore the host graphics context");
        }
        glDeleteTextures(1, &texture);
        DAF_NAMESPACE::constructionFailure = 0;
        DAF_ASSERT_EQUAL(failed, true, "UI construction failure must reach the host");
    }
    first->quit();
    DAF_ASSERT_EQUAL(first->plugin_idle(), false, "quit must stop this editor");
    first.reset();
    DAF_ASSERT_EQUAL(app.isQuitting(), false, "editor destruction must not quit the host");
    DAF_ASSERT_EQUAL(host.isVisible(), true, "host must remain visible");
    DAF_ASSERT_EQUAL(second->isVisible(), true, "sibling must remain visible");
    const uint64_t beforeDraw = second->getCompletedDisplayCount();
    second->repaint();
    DAF_ASSERT_EQUAL(pumpUntil(app, host, [&] {
        return second->getCompletedDisplayCount() > beforeDraw;
    }), true, "sibling must draw after editor destruction");

    auto embedded = makeEditor(&app, host.getNativeWindowHandle());
    if (app.getWindowSystem() == Application::kWindowSystemWayland)
        DAF_ASSERT_EQUAL(embedded->setWindowEmbeddedSurfaceGeometry(10, 20, 160, 120, 1.0), true,
                         "embedded editor must accept surface geometry");
    DAF_ASSERT_EQUAL(pumpUntil(app, host, [&] {
        return embedded->getCompletedDisplayCount() > 0;
    }), true, "embedded editor must draw through the host pump");
    embedded->quit();
    DAF_ASSERT_EQUAL(embedded->plugin_idle(), false, "quit must stop the embedded editor");
    embedded.reset();
    second->parameterChanged(0, 0.0f);
    DAF_ASSERT_EQUAL(second->plugin_idle(), false, "window close must stop only its editor");
    DAF_ASSERT_EQUAL(app.isQuitting(), false, "window close must leave the host alive");
    DAF_ASSERT_EQUAL(second->setWindowVisible(true), true, "a closed editor can be shown again");
    DAF_ASSERT_EQUAL(second->plugin_idle(), true, "reopened editor must resume idle");
    second.reset();
    DAF_ASSERT_EQUAL(app.isQuitting(), false, "all editor deletions must leave the host alive");
    DAF_ASSERT_EQUAL(DAF_NAMESPACE::uiDestructions, 3, "each editor must be destroyed once");
    DAF_ASSERT_EQUAL(DAF_NAMESPACE::destructionContextValid, true, "UI destruction requires its graphics context");
    app.removeIdleCallback(&idle);
    host.close();
    DAF_ASSERT_EQUAL(app.isQuitting(), true, "failed UI construction must not leave a registered window");

    auto owned = makeEditor(nullptr);
    owned->setWindowVisible(true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (owned->getCompletedDisplayCount() == 0 && std::chrono::steady_clock::now() < deadline)
    {
        owned->plugin_idle();
        DAF_NAMESPACE::d_msleep(5);
    }
    DAF_ASSERT_NOT_EQUAL(owned->getCompletedDisplayCount(), 0, "owned editor must pump its own application");
    owned->quit();
    DAF_ASSERT_EQUAL(owned->plugin_idle(), false, "owned editor quit behavior must be preserved");
    return 0;
}
#endif
