// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// The part of receiving file drops that every backend shares (dgl/src/FileDrop.hpp, see
// dgl/src/drop-targets.md): text/uri-list decoding, and the drag session driven through a fake backend.
// Needs no display.

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "dgl/src/pugl.hpp"

START_NAMESPACE_DGL
#include "dgl/src/FileDrop.hpp"
END_NAMESPACE_DGL

// pugl's types are in the DGL namespace, except on macOS where they are global
using namespace DGL_NAMESPACE;

static int failures = 0;

static void check(const bool ok, const char* const what)
{
    if (! ok)
    {
        ++failures;
        d_stderr2("FileDrop: FAILED %s", what);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// uri-list decoding

static bool decodes(const char* const uri, const char* const expected, const char* const host = "studio")
{
    std::string path;
    const bool ok = PuglDropFiles::decodeFileUri(uri, std::strlen(uri), host, path);
    return expected != nullptr ? ok && path == expected : ! ok;
}

static std::vector<std::string> listOf(const char* const data, const size_t size, bool* const any = nullptr)
{
    PuglDropFiles files;
    const bool usable = files.setFromUriList(data, size, "studio");
    if (any != nullptr)
        *any = usable;

    std::vector<std::string> paths;
    for (uint32_t i = 0; i < files.count(); ++i)
        paths.push_back(files.paths()[i]);
    return paths;
}

static void testDecoding()
{
    check(decodes("file:///home/u/a.wav", "/home/u/a.wav"), "empty host");
    check(decodes("file://localhost/x", "/x"), "localhost");
    check(decodes("FILE://LocalHost/x", "/x"), "scheme and localhost in any case");
    check(decodes("file://Studio/x", "/x"), "this machine's name, in any case");
    check(decodes("file://studio/x", nullptr, "other"), "another machine is ignored");
    check(decodes("file://studio/x", nullptr, nullptr), "a host with no name to compare is ignored");
    check(decodes("file://localhost:80/x", nullptr), "a port is not this machine");
    check(decodes("file:/x", "/x"), "no authority");
    check(decodes("file:///", "/"), "the root");
    check(decodes("file:x", nullptr), "relative path");
    check(decodes("file://", nullptr), "authority without path");
    check(decodes("file:", nullptr), "scheme alone");
    check(decodes("http://host/x", nullptr), "other scheme");
    check(decodes("trash:///a", nullptr), "trash");
    check(decodes("smb://server/share/a", nullptr), "smb");
    check(decodes("file:///a%20b", "/a b"), "escaped space");
    check(decodes("file:///a%2Fb%2fc", "/a/b/c"), "escaped slash, both cases");
    check(decodes("file:///a b", "/a b"), "a raw space is kept");
    check(decodes("file:///a%00b", nullptr), "escaped NUL");
    check(decodes("file:///a%G1", nullptr), "malformed escape");
    check(decodes("file:///a%4", nullptr), "truncated escape");
    check(decodes("file:///a%", nullptr), "lone percent");
    check(decodes("file:///a\x01" "b", nullptr), "raw control character");
    check(decodes("file:///C%23.mid", "/C#.mid"), "escaped hash");
    check(decodes("file:///a/C#.mid", "/a/C#.mid"), "a raw hash stays in the path");
    check(decodes("file:///a?b", "/a?b"), "a raw question mark stays in the path");
    check(decodes("file:///caf%C3%A9.wav", "/caf\xc3\xa9.wav"), "escaped UTF-8");
    check(decodes("file:///caf\xc3\xa9.wav", "/caf\xc3\xa9.wav"), "raw UTF-8");
    check(decodes("file:///%F0%9F%8E%B5.mid", "/\xf0\x9f\x8e\xb5.mid"), "four-byte UTF-8");
    check(decodes("file:///%C3", nullptr), "truncated UTF-8");
    check(decodes("file:///%C3%28", nullptr), "bad continuation byte");
    check(decodes("file:///%C0%AF", nullptr), "overlong UTF-8");
    check(decodes("file:///%E0%80%AF", nullptr), "overlong three-byte UTF-8");
    check(decodes("file:///%ED%A0%80", nullptr), "UTF-16 surrogate");
    check(decodes("file:///%F4%90%80%80", nullptr), "past U+10FFFF");
    check(decodes("file:///%FF", nullptr), "invalid lead byte");
    check(decodes("file:///latin%E9", nullptr), "a Latin-1 name is not UTF-8");

    bool any = false;
    std::vector<std::string> paths;

    {
        const char list[] = "file:///a\r\nfile:///b\r\n";
        paths = listOf(list, sizeof(list) - 1, &any);
        check(any && paths.size() == 2 && paths[0] == "/a" && paths[1] == "/b", "CRLF list");
    }
    {
        const char list[] = "file:///a\nfile:///b";
        paths = listOf(list, sizeof(list) - 1);
        check(paths.size() == 2 && paths[1] == "/b", "bare LF, no final newline");
    }
    {
        const char list[] = "# a comment\r\n\r\n  file:///a \t\r\n\r\nfile:///b\r\n";
        paths = listOf(list, sizeof(list) - 1);
        check(paths.size() == 2 && paths[0] == "/a" && paths[1] == "/b", "comments, blank lines, spaces");
    }
    {
        const char list[] = "file:///a\r\n\0file:///b\r\n";
        paths = listOf(list, sizeof(list) - 1);
        check(paths.size() == 1 && paths[0] == "/a", "stops at a NUL");
    }
    {
        const char list[] = "https://example.com/a\r\nfile:///b\r\nfile:///b\r\ntrash:///c\r\nfile:///a\r\n";
        paths = listOf(list, sizeof(list) - 1);
        check(paths.size() == 3 && paths[0] == "/b" && paths[1] == "/b" && paths[2] == "/a",
              "order and duplicates kept, other schemes skipped");
    }
    {
        const char list[] = "https://example.com/a\r\nfile://elsewhere/b\r\n";
        paths = listOf(list, sizeof(list) - 1, &any);
        check(! any && paths.empty(), "nothing usable");
    }

    paths = listOf("", 0, &any);
    check(! any && paths.empty(), "empty list");

    {
        PuglDropFiles files;
        check(! files.setFromUriList(nullptr, 4, nullptr), "no data");
        check(files.count() == 0 && files.paths() == nullptr, "an empty list has no paths");

        std::string big;
        for (int i = 0; i < 2000; ++i)
        {
            char line[64];
            std::snprintf(line, sizeof(line), "file:///tmp/clip%04d.wav\r\n", i);
            big += line;
        }
        check(files.setFromUriList(big.data(), big.size(), nullptr) && files.count() == 2000
              && std::strcmp(files.paths()[0], "/tmp/clip0000.wav") == 0
              && std::strcmp(files.paths()[1999], "/tmp/clip1999.wav") == 0,
              "a long list keeps every path valid");
    }
}

// --------------------------------------------------------------------------------------------------------------------
// the drag session, through a backend that writes down what it is asked to do

struct FakeHost : PuglDropHost
{
    std::string log;
    PuglStatus reply = PUGL_SUCCESS;
    std::function<void()> during;
    PuglDropEvent last = {};
    std::vector<std::string> lastFiles;
    std::vector<std::string> filesAfter; // the event's files, read again once the step has run
    std::vector<std::string> ballast;

    void requestData() override { log += "request;"; }
    void answer(const bool accept) override { log += accept ? "yes;" : "no;"; }
    void finish(const bool taken) override { log += taken ? "finished;" : "failed;"; }

    PuglStatus dispatch(const PuglDropEvent& event) override
    {
        switch (event.type)
        {
        case PUGL_DRAG_ENTER:  log += "ENTER;";  break;
        case PUGL_DRAG_MOTION: log += "MOTION;"; break;
        case PUGL_DRAG_LEAVE:  log += "LEAVE;";  break;
        case PUGL_DROP:        log += "DROP;";   break;
        default:                              log += "?;";      break;
        }

        last = event;
        lastFiles.clear();
        for (uint32_t i = 0; i < event.numFiles; ++i)
            lastFiles.push_back(event.files[i]);

        if (during)
        {
            const std::function<void()> step(during);
            during = nullptr;
            step();
        }

        filesAfter.clear();
        for (uint32_t i = 0; i < event.numFiles; ++i)
            filesAfter.push_back(event.files[i]);

        return reply;
    }

    std::string take()
    {
        const std::string result(log);
        log.clear();
        return result;
    }
};

static const char kList[] = "file:///a.wav\r\nfile:///b.mid\r\n";

static void testSession()
{
    // a drag that is dropped
    {
        FakeHost host;
        PuglDropSession session(host);

        session.begin(true, true);
        check(host.take().empty(), "an enter alone does nothing");
        check(session.isActive(), "the session runs from the enter");

        session.position(10, 20, 1.0, 0, 100.0);
        check(host.take() == "request;", "the first position asks for the files, and waits with its answer");
        check(session.wantsData(), "the files are wanted");

        session.position(11, 21, 1.1, PUGL_MOD_ALT, 100.1);
        check(host.take().empty(), "positions coalesce while the files are on their way");

        session.data(kList, sizeof(kList) - 1, nullptr);
        check(host.take() == "ENTER;yes;", "the files in, the view hears the enter and the source one answer");
        check(host.last.x == 11 && host.last.y == 21 && host.last.mods == PUGL_MOD_ALT && host.last.time == 1.1,
              "the enter is at the latest position");
        check(host.lastFiles.size() == 2 && host.lastFiles[0] == "/a.wav" && host.lastFiles[1] == "/b.mid",
              "the enter carries the files");
        check(! session.wantsData(), "the files are no longer wanted");

        session.position(30, 40, 1.2, 0, 100.2);
        check(host.take() == "MOTION;yes;", "a later position is a motion, answered at once");

        host.reply = PUGL_FAILURE;
        session.position(31, 41, 1.3, 0, 100.3);
        check(host.take() == "MOTION;no;", "the view can refuse at a position");

        host.reply = PUGL_SUCCESS;
        session.position(32, 42, 1.4, 0, 100.4);
        check(host.take() == "MOTION;yes;", "and accept again");

        session.drop(1.5, PUGL_MOD_ALT, 100.5);
        check(host.take() == "DROP;finished;", "the drop goes to the view and the source hears it was taken");
        check(host.last.x == 32 && host.last.mods == PUGL_MOD_ALT && host.lastFiles.size() == 2,
              "the drop is at the latest position, with the modifiers at the drop");
        check(! session.isActive(), "the drop ends the drag");

        session.leave();
        check(host.take().empty(), "no leave after a drop");
    }

    // a drag that leaves, and one the view refuses
    {
        FakeHost host;
        PuglDropSession session(host);

        session.begin(true, true);
        session.leave();
        check(host.take().empty(), "a drag the view never heard of leaves silently");

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.data(kList, sizeof(kList) - 1, nullptr);
        host.take();
        session.leave();
        check(host.take() == "LEAVE;", "a drag the view had leaves with a leave");

        host.reply = PUGL_FAILURE;
        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.data(kList, sizeof(kList) - 1, nullptr);
        check(host.take() == "request;ENTER;no;", "the view refuses the enter");
        session.drop(0, 0, 0);
        check(host.take() == "LEAVE;failed;", "a drop where the view refused fails, and the view hears a leave");
    }

    // a source without files, and files without a usable one
    {
        FakeHost host;
        PuglDropSession session(host);

        session.begin(false, true);
        session.position(1, 1, 0, 0, 0);
        check(host.take() == "no;", "no text/uri-list: refused at once, nothing asked for");
        session.drop(0, 0, 0);
        check(host.take() == "failed;", "and its drop fails");

        const char web[] = "https://example.com/a\r\n";
        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.data(web, sizeof(web) - 1, nullptr);
        check(host.take() == "request;no;", "nothing usable: refused, and the view never hears of it");
        session.position(2, 2, 0, 0, 0);
        check(host.take() == "no;", "the rest is refused");
        session.leave();
        check(host.take().empty(), "nor of its leave");

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.dataFailed();
        check(host.take() == "request;no;", "a failed transfer refuses");
    }

    // deadlines
    {
        FakeHost host;
        PuglDropSession session(host);

        session.begin(true, true);
        session.position(1, 1, 0, 0, 10.0);
        host.take();
        session.tick(10.0 + PUGL_DROP_DATA_TIMEOUT - 0.1);
        check(host.take().empty(), "nothing before the deadline");
        session.tick(10.0 + PUGL_DROP_DATA_TIMEOUT);
        check(host.take() == "no;", "the data deadline refuses the waiting position");
        check(! session.wantsData() && session.isActive(), "and the files are no longer wanted");
        session.data(kList, sizeof(kList) - 1, nullptr);
        check(host.take().empty(), "files after the deadline are ignored");
    }

    // a drop that comes before its files
    {
        FakeHost host;
        PuglDropSession session(host);

        session.begin(true, true);
        session.position(5, 6, 0, 0, 0);
        session.drop(0.5, PUGL_MOD_ALT, 1.0);
        check(host.take() == "request;", "the drop waits for its files, with no answer for the position");
        session.data(kList, sizeof(kList) - 1, nullptr);
        check(host.take() == "ENTER;DROP;finished;", "then the view hears the enter and the drop together");
        check(host.last.x == 5 && host.last.mods == PUGL_MOD_ALT, "at the last position");

        session.begin(true, true);
        session.position(5, 6, 0, 0, 0);
        session.drop(0.5, 0, 1.0);
        host.reply = PUGL_FAILURE;
        session.data(kList, sizeof(kList) - 1, nullptr);
        check(host.take() == "request;ENTER;LEAVE;failed;", "a held drop the view refuses at the enter fails");
        host.reply = PUGL_SUCCESS;

        session.begin(true, true);
        session.position(5, 6, 0, 0, 0);
        session.drop(0.5, 0, 1.0);
        host.take();
        session.tick(1.0 + PUGL_DROP_HELD_TIMEOUT);
        check(host.take() == "failed;", "a held drop whose files never come fails at its deadline");

        session.begin(true, true);
        session.position(5, 6, 0, 0, 0);
        session.drop(0.5, 0, 1.0);
        host.take();
        session.begin(true, true);
        check(host.take() == "failed;", "a new drag fails a held drop");

        session.begin(true, true);
        session.drop(0.5, 0, 1.0);
        check(host.take() == "failed;", "a drop without any position fails");
    }

    // the view stops accepting, or goes away
    {
        FakeHost host;
        PuglDropSession session(host);

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.data(kList, sizeof(kList) - 1, nullptr);
        host.take();
        session.detach(true);
        check(host.take() == "LEAVE;", "opting out ends the drag for the view with a leave");
        session.position(2, 2, 0, 0, 0);
        check(host.take() == "no;", "and refuses the rest");
        session.drop(0, 0, 0);
        check(host.take() == "failed;", "including the drop");

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        host.during = [&session] { session.detach(true); };
        session.data(kList, sizeof(kList) - 1, nullptr);
        check(host.take() == "request;ENTER;no;", "opting out inside the enter answers once, with no nested leave");
        session.leave();
        check(host.take().empty(), "and no leave later either");

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        host.during = [&session] { session.abandon(); };
        session.data(kList, sizeof(kList) - 1, nullptr);
        check(host.take() == "request;ENTER;no;", "a view destroyed inside its enter gets nothing more");
        check(! session.isActive(), "and the drag is forgotten");

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.data(kList, sizeof(kList) - 1, nullptr);
        host.take();
        host.during = [&session] { session.abandon(); };
        session.drop(0, 0, 0);
        check(host.take() == "DROP;finished;", "a view destroyed inside its drop still has its answer told");

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.drop(0, 0, 0);
        host.take();
        session.abandon();
        check(host.take() == "failed;", "a view destroyed with a drop held fails the drop");

        session.begin(true, false);
        session.position(1, 1, 0, 0, 0);
        check(host.take() == "no;", "a drag that begins detached is refused");
    }

    // a new enter while the view has a drag
    {
        FakeHost host;
        PuglDropSession session(host);

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.data(kList, sizeof(kList) - 1, nullptr);
        host.take();
        session.begin(true, true);
        check(host.take() == "LEAVE;", "a new drag leaves the one before");
        session.position(1, 1, 0, 0, 0);
        check(host.take() == "request;", "and asks for its own files");
    }

    // A view destroyed inside its enter may still read the files it was handed: they stay until the call
    // returns. Paths past the short-string size live on the heap, and strings of their size allocated right
    // after the view went take over whatever the drag freed.
    {
        static const char kLong[] = "file:///tmp/a-path-long-enough-for-the-heap.wav\r\n"
                                    "file:///tmp/another-path-long-enough-for-heap.mid\r\n";
        FakeHost host;
        PuglDropSession session(host);
        host.ballast.reserve(64);

        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        host.during = [&] {
            session.abandon();
            for (int i = 0; i < 16; ++i)
                host.ballast.push_back(std::string(40, 'x'));
        };
        session.data(kLong, sizeof(kLong) - 1, nullptr);
        check(host.filesAfter.size() == 2 && host.filesAfter[0] == "/tmp/a-path-long-enough-for-the-heap.wav"
              && host.filesAfter[1] == "/tmp/another-path-long-enough-for-heap.mid",
              "the files outlive a view destroyed inside the callback they were handed to");
    }

    // The view goes away, or stops accepting drops, inside the last callback of a drop it does not take: the
    // drag stays with the session until its end has told the source, so that the backend still knows whom to
    // tell (an X11 target forgets its source once the session is over).
    for (int optOut = 0; optOut < 2; ++optOut)
    {
        FakeHost host;
        PuglDropSession session(host);
        bool activeInside = false;
        const std::function<void()> goAway = [&] {
            if (optOut != 0)
                session.detach(true);
            else
                session.abandon();
            activeInside = session.isActive();
        };

        // inside the enter of a drop that came before its files
        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.drop(0, 0, 1.0);
        host.take();
        host.during = goAway;
        session.data(kList, sizeof(kList) - 1, nullptr);
        check(activeInside, optOut ? "opting out inside a held drop's enter leaves the drag to its end"
                                   : "a view destroyed inside a held drop's enter leaves the drag to its end");
        check(host.take() == "ENTER;failed;", "which tells the source the drop failed");
        check(! session.isActive(), "and then forgets the drag");

        // inside the leave of a drop where the view had refused
        host.reply = PUGL_FAILURE;
        session.begin(true, true);
        session.position(1, 1, 0, 0, 0);
        session.data(kList, sizeof(kList) - 1, nullptr);
        host.take();
        host.reply = PUGL_SUCCESS;
        activeInside = false;
        host.during = goAway;
        session.drop(0, 0, 0);
        check(activeInside, optOut ? "opting out inside a refused drop's leave leaves the drag to its end"
                                   : "a view destroyed inside a refused drop's leave leaves the drag to its end");
        check(host.take() == "LEAVE;failed;", "which tells the source the drop failed");
        check(! session.isActive(), "and then forgets the drag");
    }
}

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    testDecoding();
    testSession();

    if (failures != 0)
    {
        d_stderr2("FileDrop: %d checks failed", failures);
        return 1;
    }

    d_stdout("FileDrop: all checks passed");
    return 0;
}
