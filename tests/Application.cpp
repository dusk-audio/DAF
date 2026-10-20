/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2024 Filipe Coelho <falktx@falktx.com>
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
#include <functional>
#include <memory>

#define DAF_TEST_APPLICATION_CPP
#include "dgl/src/pugl.cpp"
#include "dgl/src/Application.cpp"
#include "dgl/src/ApplicationPrivateData.cpp"

START_NAMESPACE_DGL

// --------------------------------------------------------------------------------------------------------------------

struct IdleCallbackCounter : IdleCallback
{
    int counter;

    IdleCallbackCounter()
        : counter(0) {}

    void idleCallback() override
    {
        ++counter;
    }
};

struct MutatingIdleCallback : IdleCallback
{
    std::function<void()> action;
    void idleCallback() override { action(); }
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DGL

int main()
{
    using DGL_NAMESPACE::Application;
    using DGL_NAMESPACE::ApplicationQuitter;
    using DGL_NAMESPACE::IdleCallbackCounter;

    // regular usage
    {
        Application app(true);
        IdleCallbackCounter idleCounter;
        app.addIdleCallback(&idleCounter);
        DAF_ASSERT_EQUAL(app.isQuitting(), false, "app MUST NOT be set as quitting during init");
        DAF_ASSERT_EQUAL(idleCounter.counter, 0, "app MUST NOT have triggered idle callbacks yet");
        app.idle();
        DAF_ASSERT_EQUAL(app.isQuitting(), false, "app MUST NOT be set as quitting after idle()");
        DAF_ASSERT_EQUAL(idleCounter.counter, 1, "app MUST have triggered 1 idle callback");
        app.idle();
        DAF_ASSERT_EQUAL(idleCounter.counter, 2, "app MUST have triggered 2 idle callbacks");
        app.quit();
        DAF_ASSERT_EQUAL(app.isQuitting(), true, "app MUST be set as quitting after quit()");
        DAF_ASSERT_EQUAL(idleCounter.counter, 2, "app MUST have triggered only 2 idle callbacks in its lifetime");
    }

    // A UI can unregister and delete its Window while handling an idle call.
    // Exercise the public Application path, not only the registry in isolation.
    {
        Application app(false);
        std::unique_ptr<DGL_NAMESPACE::MutatingIdleCallback> self(new DGL_NAMESPACE::MutatingIdleCallback);
        IdleCallbackCounter survivor;
        unsigned calls = 0;
        self->action = [&] { ++calls; app.removeIdleCallback(self.get()); self.reset(); };
        app.addIdleCallback(self.get()); app.addIdleCallback(&survivor);
        app.idle(); app.idle();
        DAF_ASSERT_EQUAL(calls, 1u, "a registered callback can unregister and delete itself");
        DAF_ASSERT_EQUAL(survivor.counter, 2, "self-deletion preserves later public callbacks");
        app.removeIdleCallback(&survivor);
    }

    // Embedded UIs can also destroy their Application from a native callback.
    {
        std::unique_ptr<Application> app(new Application(false));
        DGL_NAMESPACE::MutatingIdleCallback closer;
        IdleCallbackCounter survivor;
        closer.action = [&] { app.reset(); };
        app->addIdleCallback(&closer); app->addIdleCallback(&survivor);
        app->idle();
        DAF_ASSERT_EQUAL(app.get(), nullptr, "the Application really was destroyed during idle");
        DAF_ASSERT_EQUAL(survivor.counter, 0, "destroying the Application cancels pending callbacks");
    }

    // standalone exec, must not block forever due to quit() called from another thread
    {
        Application app(true);
        ApplicationQuitter appQuitter(app);
        IdleCallbackCounter idleCounter;
        app.addIdleCallback(&idleCounter);
        app.exec();
        DAF_ASSERT_EQUAL(appQuitter.isThreadRunning(), false, "app quit triggered because we told it so");
        DAF_ASSERT_NOT_EQUAL(idleCounter.counter, 0, "app idle callbacks MUST have been triggered");
    }

    // standalone exec, but with 0 as timeout
    {
        Application app(true);
        ApplicationQuitter appQuitter(app);
        IdleCallbackCounter idleCounter;
        app.addIdleCallback(&idleCounter);
        app.exec(0);
        DAF_ASSERT_EQUAL(appQuitter.isThreadRunning(), false, "app quit triggered because we told it so");
        DAF_ASSERT_NOT_EQUAL(idleCounter.counter, 0, "app idle callbacks MUST have been triggered");
    }

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
