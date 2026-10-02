/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2022 Filipe Coelho <falktx@falktx.com>
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

#ifndef DAF_THREAD_HPP_INCLUDED
#define DAF_THREAD_HPP_INCLUDED

#include "Mutex.hpp"
#include "Sleep.hpp"
#include "String.hpp"

#include <atomic>

#ifdef DAF_OS_LINUX
# include <sys/prctl.h>
#endif

#ifdef DAF_OS_WASM
# error Threads do not work under wasm!
#endif

START_NAMESPACE_DAF

// -----------------------------------------------------------------------
// Thread class

class Thread
{
protected:
    /*
     * Constructor.
     */
    Thread(const char* const threadName = nullptr) noexcept
        : fLock(),
          fSignal(),
          fHandleSignal(),
          fName(threadName),
         #ifdef PTW32_DLLPORT
          fHandle({nullptr, 0}),
         #else
          fHandle(0),
         #endif
          fRunToken(0),
          fLastRunToken(0),
          fShouldExit(false) {}

    /*
     * Destructor.
     */
    virtual ~Thread() /*noexcept*/
    {
        DAF_SAFE_ASSERT(! isThreadRunning());

        stopThread(-1);
    }

    /*
     * Virtual function to be implemented by the subclass.
     */
    virtual void run() = 0;

    // -------------------------------------------------------------------

public:
    /*
     * Check if the thread is running.
     */
    bool isThreadRunning() const noexcept
    {
        return fRunToken.load(std::memory_order_acquire) != 0;
    }

    /*
     * Check if the thread should exit.
     */
    bool shouldThreadExit() const noexcept
    {
        return fShouldExit.load(std::memory_order_acquire);
    }

    /*
     * Start the thread.
     */
    bool startThread(const bool withRealtimePriority = false) noexcept
    {
        // check if already running
        DAF_SAFE_ASSERT_RETURN(! isThreadRunning(), true);

        pthread_t handle;

        pthread_attr_t attr;
        pthread_attr_init(&attr);

        struct sched_param sched_param = {};

        if (withRealtimePriority)
        {
           #ifdef __MOD_DEVICES__
            int rtprio;
            const char* const srtprio = std::getenv("MOD_PLUGIN_THREAD_PRIORITY");
            if (srtprio != nullptr && (rtprio = std::atoi(srtprio)) > 0)
                sched_param.sched_priority = rtprio - 1;
            else
           #endif
            sched_param.sched_priority = 80;

           #ifndef DAF_OS_HAIKU
            if (pthread_attr_setscope(&attr, PTHREAD_SCOPE_SYSTEM)          == 0  &&
                pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0  &&
              #ifndef DAF_OS_WINDOWS
               (pthread_attr_setschedpolicy(&attr, SCHED_FIFO)              == 0  ||
                pthread_attr_setschedpolicy(&attr, SCHED_RR)                == 0) &&
              #endif
                pthread_attr_setschedparam(&attr, &sched_param)             == 0)
            {
                d_stdout("Thread setup with realtime priority successful");
            }
            else
           #endif
            {
                d_stdout("Thread setup with realtime priority failed, going with normal priority instead");
                pthread_attr_destroy(&attr);
                pthread_attr_init(&attr);
            }
        }

        const MutexLocker ml(fLock);

        fShouldExit.store(false, std::memory_order_release);

        // Mark the thread as running before it exists, so that a thread which finishes
        // straight away cannot have its "done" overwritten by a late "running" from here.
        // Every run gets its own non-zero token: an abandoned thread (see stopThread) that
        // finishes later only clears the token it was started with, never a newer run's.
        if (++fLastRunToken == 0)
            ++fLastRunToken;
        fRunToken.store(fLastRunToken, std::memory_order_release);

        bool ok = pthread_create(&handle, &attr, _entryPoint, this) == 0;
        pthread_attr_destroy(&attr);

        if (withRealtimePriority && !ok)
       {
            d_stdout("Thread with realtime priority failed on creation, going with normal priority instead");
            pthread_attr_init(&attr);
            ok = pthread_create(&handle, &attr, _entryPoint, this) == 0;
            pthread_attr_destroy(&attr);
       }

        if (! ok)
        {
            fRunToken.store(0, std::memory_order_release);
            d_safe_assert("ok", __FILE__, __LINE__);
            return false;
        }

        // publish the handle, then let the new thread proceed past its entry point;
        // the signal orders this write before anything the thread does in run().
        fHandle = handle;
        fHandleSignal.signal();

        pthread_detach(handle);

        // wait for thread to start
        fSignal.wait();
        return true;
    }

    /*
     * Stop the thread.
     * In the 'timeOutMilliseconds':
     * = 0 -> no wait
     * > 0 -> wait timeout value
     * < 0 -> wait forever
     */
    bool stopThread(const int timeOutMilliseconds) noexcept
    {
        const MutexLocker ml(fLock);

        if (isThreadRunning())
        {
            signalThreadShouldExit();

            if (timeOutMilliseconds != 0)
            {
                // Wait for the thread to stop
                int timeOutCheck = (timeOutMilliseconds == 1 || timeOutMilliseconds == -1) ? timeOutMilliseconds : timeOutMilliseconds/2;

                for (; isThreadRunning();)
                {
                    d_msleep(2);

                    if (timeOutCheck < 0)
                        continue;

                    if (timeOutCheck > 0)
                        timeOutCheck -= 1;
                    else
                        break;
                }
            }

            if (isThreadRunning())
            {
                // should never happen!
                d_stderr2("assertion failure: \"! isThreadRunning()\" in file %s, line %i", __FILE__, __LINE__);

                // give up on the thread: it is already detached, so just stop tracking it.
                // Clearing the token (rather than letting the thread do it) allows a restart,
                // and the thread's own exit will leave a newer run's token alone.
                fRunToken.store(0, std::memory_order_release);
                return false;
            }
        }

        return true;
    }

    /*
     * Tell the thread to stop as soon as possible.
     */
    void signalThreadShouldExit() noexcept
    {
        fShouldExit.store(true, std::memory_order_release);
    }

    // -------------------------------------------------------------------

    /*
     * Returns the name of the thread.
     * This is the name that gets set in the constructor.
     */
    const String& getThreadName() const noexcept
    {
        return fName;
    }

    /*
     * Returns the Id/handle of the thread, or a null handle if the thread is not running.
     * The handle is written only by startThread(), so this must not race with a startThread() call.
     */
    pthread_t getThreadId() const noexcept
    {
        if (isThreadRunning())
            return fHandle;

       #ifdef PTW32_DLLPORT
        const pthread_t nullHandle = {nullptr, 0};
        return nullHandle;
       #else
        return 0;
       #endif
    }

    /*
     * Changes the name of the caller thread.
     */
    static void setCurrentThreadName(const char* const name) noexcept
    {
        DAF_SAFE_ASSERT_RETURN(name != nullptr && name[0] != '\0',);

       #ifdef DAF_OS_LINUX
        prctl(PR_SET_NAME, name, 0, 0, 0);
       #endif
       #if defined(__GLIBC__) && (__GLIBC__ * 1000 + __GLIBC_MINOR__) >= 2012 && !defined(DAF_OS_GNU_HURD)
        pthread_setname_np(pthread_self(), name);
       #endif
       #ifdef DAF_OS_MAC
        // macOS only provides the single-argument form, which always renames the calling thread
        pthread_setname_np(name);
       #endif
       #ifdef DAF_OS_WINDOWS
        // SetThreadDescription requires Windows 10 1607 or later,
        // resolve it dynamically so that older systems simply get no thread name.
        typedef HRESULT(WINAPI* PFN_SetThreadDescription)(HANDLE, PCWSTR);

       #if defined(__GNUC__) && (__GNUC__ >= 9)
       # pragma GCC diagnostic push
       # pragma GCC diagnostic ignored "-Wcast-function-type"
       #endif
        static const PFN_SetThreadDescription setThreadDescription = (PFN_SetThreadDescription)
            GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetThreadDescription");
       #if defined(__GNUC__) && (__GNUC__ >= 9)
       # pragma GCC diagnostic pop
       #endif

        if (setThreadDescription != nullptr)
        {
            // thread names are short by convention, a fixed buffer is plenty
            WCHAR wname[128];

            if (MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, static_cast<int>(ARRAY_SIZE(wname))) > 0)
                setThreadDescription(GetCurrentThread(), wname);
        }
       #endif
    }

    // -------------------------------------------------------------------

private:
    Mutex                 fLock;          // Thread lock, serialises startThread() and stopThread()
    Signal                fSignal;        // Thread start wait signal
    Signal                fHandleSignal;  // Handle published, the new thread may proceed
    const String          fName;          // Thread name
    pthread_t             fHandle;        // Handle for this thread, written by startThread() only
    std::atomic<uint32_t> fRunToken;      // non-zero while running, unique per run
    uint32_t              fLastRunToken;  // last token handed out, guarded by fLock
    std::atomic<bool>     fShouldExit;    // true if thread should exit

    /*
     * Thread entry point.
     */
    void _runEntryPoint() noexcept
    {
        // wait until startThread() has stored our handle
        fHandleSignal.wait();

        // startThread() holds fLock until we report ready, so this is our own run's token
        uint32_t runToken = fRunToken.load(std::memory_order_acquire);

        if (fName.isNotEmpty())
            setCurrentThreadName(fName);

        // report ready
        fSignal.signal();

        try {
            run();
        } catch(...) {}

        // done. Only clear our own token, stopThread() may have given up on us and started anew.
        // This must be the last access to *this, the owner may destroy us as soon as it sees it.
        fRunToken.compare_exchange_strong(runToken, 0, std::memory_order_acq_rel, std::memory_order_relaxed);
    }

    /*
     * Thread entry point.
     */
    static void* _entryPoint(void* userData) noexcept
    {
        static_cast<Thread*>(userData)->_runEntryPoint();
        return nullptr;
    }

    DAF_DECLARE_NON_COPYABLE(Thread)
};

// -----------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_THREAD_HPP_INCLUDED
