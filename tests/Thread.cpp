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

/* Start/stop coverage for the Thread class. The state it shares between the owner and the worker
 * is what ThreadSanitizer used to flag, so this is most useful run under TSan:
 *   make -C tests ../build/tests/Thread \
 *     BASE_OPTS="-O1 -g -fsanitize=thread" LINK_OPTS="-fsanitize=thread"
 */

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "daf/extra/Thread.hpp"

#include <atomic>

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

// loops until told to exit
class LoopingThread : public Thread
{
public:
    std::atomic<int> iterations;
    std::atomic<bool> sawOwnId;

    LoopingThread()
        : Thread("LoopingThread"),
          iterations(0),
          sawOwnId(false) {}

protected:
    void run() override
    {
        sawOwnId = pthread_equal(getThreadId(), pthread_self()) != 0;

        while (! shouldThreadExit())
        {
            ++iterations;
            d_msleep(1);
        }
    }
};

// returns immediately
class OneShotThread : public Thread
{
public:
    std::atomic<int> runs;

    OneShotThread()
        : Thread(),
          runs(0) {}

protected:
    void run() override
    {
        ++runs;
    }
};

// ignores the exit request for a while, so stopThread() gives up on it
class StubbornThread : public Thread
{
public:
    std::atomic<int> sleepMs;
    std::atomic<int> finishedRuns;

    StubbornThread()
        : Thread("StubbornThread"),
          sleepMs(0),
          finishedRuns(0) {}

protected:
    void run() override
    {
        d_msleep(static_cast<uint>(sleepMs.load()));

        // an abandoned run no longer synchronises with its owner through the Thread class,
        // so this is what tells the test it may destroy the object
        ++finishedRuns;
    }
};

// --------------------------------------------------------------------------------------------------------------------

static bool waitUntilStopped(const Thread& thread)
{
    for (int i = 0; i < 2000 && thread.isThreadRunning(); ++i)
        d_msleep(1);

    return ! thread.isThreadRunning();
}

END_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    USE_NAMESPACE_DAF;

    // repeated start/stop of a thread that runs until told otherwise
    {
        LoopingThread thread;
        DAF_ASSERT_EQUAL(thread.isThreadRunning(), false, "not running before start");

        for (int i = 0; i < 50; ++i)
        {
            DAF_ASSERT_EQUAL(thread.startThread(), true, "thread starts");
            DAF_ASSERT_EQUAL(thread.isThreadRunning(), true, "running after start");
            DAF_ASSERT_EQUAL(thread.shouldThreadExit(), false, "exit flag cleared by start");

            d_msleep(2);

            DAF_ASSERT_EQUAL(thread.stopThread(-1), true, "thread stops");
            DAF_ASSERT_EQUAL(thread.isThreadRunning(), false, "not running after stop");
            DAF_ASSERT_EQUAL(thread.shouldThreadExit(), true, "exit flag set by stop");
            DAF_ASSERT_EQUAL(thread.sawOwnId.load(), true, "getThreadId() from run() is the thread's own id");
        }

        DAF_ASSERT_NOT_EQUAL(thread.iterations.load(), 0, "thread body ran");
    }

    // a thread that finishes by itself is reported as stopped, and can be started again
    {
        OneShotThread thread;

        for (int i = 0; i < 200; ++i)
        {
            DAF_ASSERT_EQUAL(thread.startThread(), true, "one-shot thread starts");
            DAF_ASSERT_EQUAL(waitUntilStopped(thread), true, "one-shot thread finishes");
        }

        DAF_ASSERT_EQUAL(thread.runs.load(), 200, "one-shot thread ran each time");
        DAF_ASSERT_EQUAL(thread.stopThread(-1), true, "stopping a finished thread is a no-op");
    }

    // signalThreadShouldExit() alone lets the thread wind down by itself
    {
        LoopingThread thread;
        DAF_ASSERT_EQUAL(thread.startThread(), true, "thread starts");
        thread.signalThreadShouldExit();
        DAF_ASSERT_EQUAL(thread.shouldThreadExit(), true, "exit flag set");
        DAF_ASSERT_EQUAL(waitUntilStopped(thread), true, "thread exits after the signal");
    }

    // a thread abandoned by stopThread() must not clear the state of the run that replaced it
    // (the "assertion failure" printed here is stopThread() reporting that it gave up, as expected)
    {
        StubbornThread thread;

        thread.sleepMs = 200;
        DAF_ASSERT_EQUAL(thread.startThread(), true, "stubborn thread starts");
        DAF_ASSERT_EQUAL(thread.stopThread(10), false, "stopThread() gives up on it");
        DAF_ASSERT_EQUAL(thread.isThreadRunning(), false, "abandoned thread is no longer tracked");

        thread.sleepMs = 600;
        DAF_ASSERT_EQUAL(thread.startThread(), true, "restart after giving up");

        // the abandoned run finishes in the meantime
        d_msleep(400);
        DAF_ASSERT_EQUAL(thread.isThreadRunning(), true, "new run still reported as running");

        DAF_ASSERT_EQUAL(waitUntilStopped(thread), true, "new run finishes");
        DAF_ASSERT_EQUAL(thread.finishedRuns.load(), 2, "both runs finished");
    }

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
