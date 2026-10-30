// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// File drops on Wayland (dgl/src/FileDropWayland.hpp, see dgl/src/drop-targets.md), against a stand-in
// compositor on a socket pair that the client gets through WAYLAND_SOCKET. The stand-in speaks the wire
// protocol itself, file descriptors included, so the test needs no compositor and links no Wayland library:
// a DGL_BACKEND=both build must not. It offers what a world and a drag need and nothing more, plays the
// compositor's side of a drag, and writes down every request the client makes of the drag's offer.

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "dgl/src/pugl.cpp"

#ifndef HAVE_WAYLAND
# error This test requires the native Wayland backend
#endif

using namespace DGL_NAMESPACE;

static int failures = 0;

static void check(const bool ok, const char* const what)
{
    if (! ok)
    {
        ++failures;
        d_stderr2("WaylandDrop: FAILED %s", what);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// the stand-in compositor

class StandIn
{
public:
    struct Request {
        uint32_t object;
        uint16_t opcode;
        std::vector<uint32_t> words;
        std::string text;
        bool textNull;
        int fd;
    };

    int clientFd = -1;
    uint32_t dataDeviceVersion = 3;
    bool withOutput = false;

    bool start()
    {
        int fds[2];
        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
            return false;

        serverFd = fds[0];
        clientFd = fds[1];
        objects[1] = "wl_display";
        running = true;
        thread = std::thread(&StandIn::serve, this);
        return true;
    }

    void stop()
    {
        running = false;
        thread.join();
        close(serverFd);

        for (size_t i = 0; i < fds.size(); ++i)
            close(fds[i]);
        for (size_t i = 0; i < requests.size(); ++i)
            if (requests[i].fd >= 0)
                close(requests[i].fd);
    }

    uint32_t surface(const size_t index)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return index < surfaces.size() ? surfaces[index] : 0;
    }

    bool hasDataDevice()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        return dataDevice != 0;
    }

    // a new offer of the types and actions given, as the compositor introduces it ahead of an enter
    uint32_t offer(const std::vector<const char*>& types, const uint32_t actions)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const uint32_t id = nextOffer++;
        objects[id] = "wl_data_offer";
        post(dataDevice, 0, { id });
        for (size_t i = 0; i < types.size(); ++i)
            post(id, 0, string(types[i]));
        if (dataDeviceVersion >= 3)
            post(id, 1, { actions });
        return id;
    }

    void enter(const uint32_t serial, const uint32_t surfaceId, const double x, const double y, const uint32_t offerId)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        post(dataDevice, 1, { serial, surfaceId, fixed(x), fixed(y), offerId });
    }

    void motion(const uint32_t time, const double x, const double y)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        post(dataDevice, 3, { time, fixed(x), fixed(y) });
    }

    void leave()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        post(dataDevice, 2, {});
    }

    void drop()
    {
        const std::lock_guard<std::mutex> lock(mutex);
        post(dataDevice, 4, {});
    }

    void action(const uint32_t offerId, const uint32_t chosen)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        post(offerId, 2, { chosen });
    }

    void enterOutput(const uint32_t surfaceId)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        post(surfaceId, 0, { output });
    }

    // the requests made on an offer, in order
    std::vector<Request> requestsOf(const uint32_t offerId)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<Request> result;
        for (size_t i = 0; i < requests.size(); ++i)
            if (requests[i].object == offerId)
                result.push_back(requests[i]);
        return result;
    }

    // the descriptor a receive on the offer handed over, which the caller then owns, or -1
    int takeReceive(const uint32_t offerId)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < requests.size(); ++i)
        {
            if (requests[i].object == offerId && requests[i].opcode == 1 && requests[i].fd >= 0)
            {
                const int fd = requests[i].fd;
                requests[i].fd = -1;
                return fd;
            }
        }
        return -1;
    }

private:
    int serverFd = -1;
    std::thread thread;
    std::atomic<bool> running { false };
    std::mutex mutex;
    std::map<uint32_t, std::string> objects;
    std::vector<uint32_t> surfaces;
    std::vector<Request> requests;
    std::vector<int> fds; // received, not yet given to a request
    std::vector<uint8_t> input;
    uint32_t dataDevice = 0;
    uint32_t output = 0;
    uint32_t nextOffer = 0xff000000U;
    uint32_t nextSerial = 100;

    static uint32_t fixed(const double value)
    {
        return static_cast<uint32_t>(static_cast<int32_t>(value * 256.0));
    }

    static std::vector<uint32_t> string(const char* const text)
    {
        const size_t length = std::strlen(text) + 1;
        std::vector<uint32_t> words(1 + (length + 3) / 4, 0);
        words[0] = static_cast<uint32_t>(length);
        std::memcpy(words.data() + 1, text, length);
        return words;
    }

    // an event: the object, then size << 16 | opcode, then the arguments
    void post(const uint32_t object, const uint16_t opcode, const std::vector<uint32_t>& args)
    {
        std::vector<uint32_t> message;
        message.push_back(object);
        message.push_back(static_cast<uint32_t>((2 + args.size()) * 4) << 16 | opcode);
        message.insert(message.end(), args.begin(), args.end());
        ::send(serverFd, message.data(), message.size() * 4, MSG_NOSIGNAL);
    }

    void global(const uint32_t registry, const uint32_t name, const char* const interface, const uint32_t version)
    {
        std::vector<uint32_t> args;
        args.push_back(name);
        const std::vector<uint32_t> text(string(interface));
        args.insert(args.end(), text.begin(), text.end());
        args.push_back(version);
        post(registry, 0, args);
    }

    void serve()
    {
        while (running)
        {
            pollfd ready = { serverFd, POLLIN, 0 };
            if (poll(&ready, 1, 10) <= 0)
                continue;

            uint8_t chunk[4096];
            alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 28)];
            iovec io = { chunk, sizeof(chunk) };
            msghdr header = {};
            header.msg_iov = &io;
            header.msg_iovlen = 1;
            header.msg_control = control;
            header.msg_controllen = sizeof(control);

            const ssize_t length = recvmsg(serverFd, &header, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            if (length <= 0)
            {
                if (length == 0)
                    return;
                continue;
            }

            const std::lock_guard<std::mutex> lock(mutex);

            for (cmsghdr* message = CMSG_FIRSTHDR(&header); message != nullptr; message = CMSG_NXTHDR(&header, message))
            {
                if (message->cmsg_level != SOL_SOCKET || message->cmsg_type != SCM_RIGHTS)
                    continue;
                const size_t count = (message->cmsg_len - CMSG_LEN(0)) / sizeof(int);
                for (size_t i = 0; i < count; ++i)
                {
                    int fd = -1;
                    std::memcpy(&fd, CMSG_DATA(message) + i * sizeof(int), sizeof(int));
                    fds.push_back(fd);
                }
            }

            input.insert(input.end(), chunk, chunk + length);

            while (input.size() >= 8)
            {
                uint32_t head[2];
                std::memcpy(head, input.data(), sizeof(head));
                const uint32_t size = head[1] >> 16;
                if (size < 8 || input.size() < size)
                    break;

                std::vector<uint32_t> args((size - 8) / 4);
                if (! args.empty())
                    std::memcpy(args.data(), input.data() + 8, args.size() * 4);
                handle(head[0], static_cast<uint16_t>(head[1] & 0xffff), args);
                input.erase(input.begin(), input.begin() + size);
            }
        }
    }

    // a string argument at words[at]: its text, and the index past it
    static size_t readString(const std::vector<uint32_t>& words, const size_t at, std::string& text, bool& null)
    {
        const uint32_t length = at < words.size() ? words[at] : 0;
        null = length == 0;
        text.clear();
        if (length > 0 && at + 1 + (length + 3) / 4 <= words.size())
            text.assign(reinterpret_cast<const char*>(words.data() + at + 1), length - 1);
        return at + 1 + (length + 3) / 4;
    }

    void handle(const uint32_t object, const uint16_t opcode, const std::vector<uint32_t>& args)
    {
        const std::map<uint32_t, std::string>::const_iterator known = objects.find(object);
        const std::string interface = known != objects.end() ? known->second : std::string();
        const uint32_t first = args.empty() ? 0 : args[0];

        if (interface == "wl_display")
        {
            if (opcode == 0)
            {
                // sync: done, then the callback's id given back
                post(first, 0, { 0 });
                post(1, 1, { first });
            }
            else if (opcode == 1)
            {
                objects[first] = "wl_registry";
                global(first, 1, "wl_compositor", 4);
                global(first, 2, "xdg_wm_base", 1);
                global(first, 3, "wl_seat", 1);
                global(first, 4, "wl_data_device_manager", dataDeviceVersion);
                if (withOutput)
                    global(first, 5, "wl_output", 2);
            }
        }
        else if (interface == "wl_registry" && opcode == 0)
        {
            std::string name;
            bool null = false;
            const size_t next = readString(args, 1, name, null);
            const uint32_t id = next + 1 < args.size() ? args[next + 1] : 0;
            objects[id] = name;

            if (name == "wl_seat")
            {
                post(id, 0, { 0 }); // capabilities: none
            }
            else if (name == "wl_output")
            {
                output = id;
                std::vector<uint32_t> geometry = { 0, 0, 0, 0, 0 };
                const std::vector<uint32_t> empty(string(""));
                geometry.insert(geometry.end(), empty.begin(), empty.end());
                geometry.insert(geometry.end(), empty.begin(), empty.end());
                geometry.push_back(0);
                post(id, 0, geometry);
                post(id, 1, { 3, 1280, 720, 60000 });
                post(id, 3, { 2 }); // scale 2
                post(id, 2, {});    // done
            }
        }
        else if (interface == "wl_compositor")
        {
            objects[first] = opcode == 0 ? "wl_surface" : "wl_region";
            if (opcode == 0)
                surfaces.push_back(first);
        }
        else if (interface == "wl_surface" && opcode == 3)
        {
            objects[first] = "wl_callback"; // a frame callback, never answered
        }
        else if (interface == "xdg_wm_base")
        {
            if (opcode == 2)
                objects[first] = "xdg_surface";
            else if (opcode == 1)
                objects[first] = "xdg_positioner";
        }
        else if (interface == "xdg_surface" && opcode == 1)
        {
            objects[first] = "xdg_toplevel";
            post(first, 0, { 0, 0, 0 });       // xdg_toplevel.configure: any size, no states
            post(object, 0, { nextSerial++ }); // xdg_surface.configure
        }
        else if (interface == "wl_data_device_manager")
        {
            if (opcode == 1)
            {
                objects[first] = "wl_data_device";
                dataDevice = first;
            }
            else
            {
                objects[first] = "wl_data_source";
            }
        }
        else if (interface == "wl_data_offer")
        {
            Request request = { object, opcode, {}, std::string(), true, -1 };

            if (opcode == 0) // accept(serial, mime_type)
            {
                request.words.push_back(first);
                readString(args, 1, request.text, request.textNull);
            }
            else if (opcode == 1) // receive(mime_type, fd)
            {
                readString(args, 0, request.text, request.textNull);
                if (! fds.empty())
                {
                    request.fd = fds.front();
                    fds.erase(fds.begin());
                }
            }
            else if (opcode == 4) // set_actions(dnd_actions, preferred_action)
            {
                request.words = args;
            }

            requests.push_back(request);
        }
    }
};

// --------------------------------------------------------------------------------------------------------------------
// the client: views whose drop events are written down

struct Heard {
    std::string log;
    double x = -1.0, y = -1.0;
    std::vector<std::string> files;
    PuglStatus answer = PUGL_SUCCESS;
    std::function<void()> during;
};

static Heard heard;

static PuglStatus onEvent(PuglView*, const PuglEvent* const event)
{
    const char* name = nullptr;
    switch (event->type)
    {
    case PUGL_DRAG_ENTER:  name = "ENTER;";  break;
    case PUGL_DRAG_MOTION: name = "MOTION;"; break;
    case PUGL_DRAG_LEAVE:  name = "LEAVE;";  break;
    case PUGL_DROP:        name = "DROP;";   break;
    default:               return PUGL_SUCCESS;
    }

    heard.log += name;
    heard.x = event->drop.x;
    heard.y = event->drop.y;
    heard.files.clear();
    for (uint32_t i = 0; i < event->drop.numFiles; ++i)
        heard.files.push_back(event->drop.files[i]);

    if (heard.during)
    {
        const std::function<void()> step(heard.during);
        heard.during = nullptr;
        step();
    }

    return heard.answer;
}

static std::string take()
{
    const std::string result(heard.log);
    heard.log.clear();
    return result;
}

static bool pump(PuglWorld* const world, const std::function<bool()>& done, const int ms = 2000)
{
    for (int waited = 0; waited < ms; waited += 5)
    {
        puglUpdate(world, 0.005);
        if (done())
            return true;
    }
    return false;
}

static void settle(PuglWorld* const world, const int ms = 100)
{
    pump(world, [] { return false; }, ms);
}

static PuglView* makeView(PuglWorld* const world, const bool accept)
{
    PuglView* const view = puglNewView(world);
    puglSetMatchingBackendForCurrentBuild(view);
    puglSetSizeHint(view, PUGL_DEFAULT_SIZE, 200, 100);
    puglSetEventFunc(view, onEvent);
    if (puglRealize(view) != PUGL_SUCCESS)
        return nullptr;
    if (accept && puglSetAcceptsFileDrops(view, true) != PUGL_SUCCESS)
        return nullptr;
    return view;
}

static std::string describe(const std::vector<StandIn::Request>& requests)
{
    std::string result;
    for (size_t i = 0; i < requests.size(); ++i)
    {
        const StandIn::Request& r = requests[i];
        switch (r.opcode)
        {
        case 0: result += r.textNull ? "accept(null);" : "accept(" + r.text + ");"; break;
        case 1: result += "receive(" + r.text + ");"; break;
        case 2: result += "destroy;"; break;
        case 3: result += "finish;"; break;
        case 4: result += "set_actions(" + std::to_string(r.words.size() > 0 ? r.words[0] : 0) + ","
                       + std::to_string(r.words.size() > 1 ? r.words[1] : 0) + ");"; break;
        default: result += "?;"; break;
        }
    }
    return result;
}

static bool supply(StandIn& compositor, const uint32_t offer, const char* const list)
{
    const int fd = compositor.takeReceive(offer);
    if (fd < 0)
        return false;
    const ssize_t written = write(fd, list, std::strlen(list));
    close(fd);
    return written == static_cast<ssize_t>(std::strlen(list));
}

static int openDescriptors()
{
    int count = 0;
    if (DIR* const dir = opendir("/proc/self/fd"))
    {
        while (readdir(dir) != nullptr)
            ++count;
        closedir(dir);
    }
    return count;
}

static PuglWorld* openWorld(StandIn& compositor)
{
    char socket[16];
    std::snprintf(socket, sizeof(socket), "%d", compositor.clientFd);
    setenv("WAYLAND_SOCKET", socket, 1);
   #ifdef DGL_RUNTIME_BACKEND
    return puglNewWorldOnSystem(PUGL_PROGRAM, 0, false);
   #else
    return puglNewWorld(PUGL_PROGRAM, 0);
   #endif
}

static const char kList[] = "file:///tmp/a.wav\r\nfile:///tmp/b.mid\r\n";
static const uint32_t kCopy = 1, kMove = 2;

// --------------------------------------------------------------------------------------------------------------------

static void testVersion3()
{
    StandIn compositor;
    compositor.dataDeviceVersion = 3;
    compositor.withOutput = true;
    check(compositor.start(), "the stand-in starts");

    PuglWorld* const world = openWorld(compositor);
    check(world != nullptr, "a Wayland world opens on the stand-in");
    if (world == nullptr)
        return compositor.stop();
    check(pump(world, [&] { return compositor.hasDataDevice(); }), "the world gets its data device");

    PuglView* const view = makeView(world, true);
    PuglView* const plain = makeView(world, false);
    check(view != nullptr && plain != nullptr, "two views, one accepting drops");
    check(pump(world, [&] { return compositor.surface(1) != 0; }), "both have surfaces");
    const uint32_t surface = compositor.surface(0), plainSurface = compositor.surface(1);

    // a drag, dropped
    uint32_t offer = compositor.offer({ "text/plain", "text/uri-list" }, kCopy | kMove);
    compositor.enter(10, surface, 12.0, 34.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() >= 3; }), "the enter asks for the files");
    check(describe(compositor.requestsOf(offer)) == "set_actions(1,1);accept(null);receive(text/uri-list);",
          "copy is the only action offered, and nothing is accepted before the files are in");
    check(take().empty(), "the view hears nothing before the files are in");
    compositor.action(offer, kCopy);
    check(supply(compositor, offer, kList), "the source writes the list");
    check(pump(world, [&] { return ! heard.log.empty(); }), "the view hears of the drag");
    check(take() == "ENTER;", "as an enter");
    check(heard.x == 12.0 && heard.y == 34.0, "at the enter's position");
    check(heard.files.size() == 2 && heard.files[1] == "/tmp/b.mid", "with the files");
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "the view's answer goes out");
    check(compositor.requestsOf(offer).back().text == "text/uri-list", "accepting text/uri-list");

    compositor.motion(1000, 50.0, 60.0);
    check(pump(world, [&] { return ! heard.log.empty(); }), "a motion reaches the view");
    check(take() == "MOTION;" && heard.x == 50.0, "at its position");
    check(compositor.requestsOf(offer).size() == 4, "an unchanged answer is not repeated");

    compositor.drop();
    compositor.leave();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 6; }), "the drop ends the offer");
    check(take() == "DROP;", "the view hears the drop, and no leave");
    check(describe(compositor.requestsOf(offer)) ==
          "set_actions(1,1);accept(null);receive(text/uri-list);accept(text/uri-list);finish;destroy;",
          "a taken drop is finished, then destroyed");

    // the view refuses: nothing accepted, no finish
    heard.answer = PUGL_FAILURE;
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(11, surface, 5.0, 5.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() >= 3; }), "a second drag asks for its files");
    supply(compositor, offer, kList);
    check(pump(world, [&] { return ! heard.log.empty(); }), "the view hears it");
    check(take() == "ENTER;", "and refuses it");
    compositor.leave();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "its offer ends");
    check(take() == "LEAVE;", "with a leave");
    check(describe(compositor.requestsOf(offer)) == "set_actions(1,1);accept(null);receive(text/uri-list);destroy;",
          "never accepted, never finished");
    heard.answer = PUGL_SUCCESS;

    // a source that can only move is refused without asking for anything
    offer = compositor.offer({ "text/uri-list" }, kMove);
    compositor.enter(12, surface, 5.0, 5.0, offer);
    settle(world);
    compositor.leave();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 2; }), "a move-only drag ends");
    check(describe(compositor.requestsOf(offer)) == "accept(null);destroy;", "with no actions, no receive");
    check(take().empty(), "and the view never hears of it");

    // no text/uri-list
    offer = compositor.offer({ "text/plain" }, kCopy);
    compositor.enter(13, surface, 5.0, 5.0, offer);
    settle(world);
    compositor.drop();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 2; }), "a drag without files ends at its drop");
    check(describe(compositor.requestsOf(offer)) == "accept(null);destroy;", "refused");
    check(take().empty(), "unheard");

    // the drag leaves while the list is on its way, and another follows at once
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(14, surface, 5.0, 5.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "a drag that will leave asks for its files");
    const int late = compositor.takeReceive(offer);
    compositor.leave();
    const uint32_t next = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(15, surface, 7.0, 8.0, next);
    check(pump(world, [&] { return compositor.requestsOf(next).size() == 3; }), "the next drag asks for its files");
    if (late >= 0)
    {
        const ssize_t written = write(late, kList, sizeof(kList) - 1);
        (void)written;
        close(late);
    }
    settle(world);
    check(take().empty(), "the drag that left never reaches the view");
    check(describe(compositor.requestsOf(offer)) == "set_actions(1,1);accept(null);receive(text/uri-list);destroy;",
          "and its offer was destroyed at the leave");
    supply(compositor, next, kList);
    check(pump(world, [&] { return ! heard.log.empty(); }), "the next drag reaches the view");
    check(take() == "ENTER;" && heard.x == 7.0, "at its own position");

    // the source dies halfway through its list: the compositor ends the drag
    compositor.leave();
    pump(world, [&] { return ! heard.log.empty(); });
    take();
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(16, surface, 5.0, 5.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "a drag whose source dies asks for its files");
    const int half = compositor.takeReceive(offer);
    if (half >= 0)
    {
        const ssize_t written = write(half, "file:///tmp/half", 16);
        (void)written;
    }
    compositor.leave();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "its offer ends");
    check(take().empty(), "and its half list never reaches the view");
    if (half >= 0)
        close(half);

    // a view that does not accept drops gets what every view got before
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(17, plainSurface, 5.0, 5.0, offer);
    settle(world);
    compositor.leave();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 2; }), "a drag over a plain view ends");
    check(describe(compositor.requestsOf(offer)) == "accept(null);destroy;", "declined as before");

    // the view goes away mid-drag: nothing more reaches it, and the offer is still destroyed at the leave
    PuglView* const doomed = makeView(world, true);
    check(pump(world, [&] { return compositor.surface(2) != 0; }), "a third view");
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(18, compositor.surface(2), 5.0, 5.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "it asks for the files");
    supply(compositor, offer, kList);
    check(pump(world, [&] { return ! heard.log.empty(); }), "and hears the enter");
    take();
    puglFreeView(doomed);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 5; }), "its accept is taken back");
    check(compositor.requestsOf(offer).back().textNull, "with a NULL accept");
    compositor.motion(2000, 9.0, 9.0);
    compositor.drop();
    compositor.leave();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 6; }), "the drag over the gone view ends");
    check(compositor.requestsOf(offer).back().opcode == 2, "with its offer destroyed and nothing finished");
    check(take().empty(), "and nothing reached the freed view");

    // the view deletes itself inside its drop: the drop is still finished
    PuglView* selfish = makeView(world, true);
    check(pump(world, [&] { return compositor.surface(3) != 0; }), "a fourth view");
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(19, compositor.surface(3), 5.0, 5.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "it asks for the files");
    compositor.action(offer, kCopy);
    supply(compositor, offer, kList);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "and accepts them");
    take();
    heard.during = [&selfish] { puglFreeView(selfish); selfish = nullptr; };
    compositor.drop();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() >= 6; }), "the drop ends");
    check(selfish == nullptr, "with the view gone");
    check(describe(compositor.requestsOf(offer)).find("finish;destroy;") != std::string::npos,
          "and still finished with the view's answer");
    compositor.leave();
    take();

    // A view that deletes itself inside the last callback of a drop it does not take: the offer is still
    // destroyed, and never finished. First inside the enter of a drop the compositor sent before the files.
    PuglView* doomedEarly = makeView(world, true);
    check(pump(world, [&] { return compositor.surface(4) != 0; }), "a fifth view");
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(23, compositor.surface(4), 5.0, 5.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "a drop that will come early asks for its files");
    compositor.drop();
    settle(world, 50);
    heard.during = [&doomedEarly] { puglFreeView(doomedEarly); doomedEarly = nullptr; };
    supply(compositor, offer, kList);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "the held drop ends");
    check(doomedEarly == nullptr && take() == "ENTER;", "with the view gone inside its enter");
    check(describe(compositor.requestsOf(offer)) == "set_actions(1,1);accept(null);receive(text/uri-list);destroy;",
          "destroying the offer, unfinished");
    compositor.leave();

    // then inside the leave of a drop where the view refused
    PuglView* doomedLate = makeView(world, true);
    check(pump(world, [&] { return compositor.surface(5) != 0; }), "a sixth view");
    heard.answer = PUGL_FAILURE;
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(24, compositor.surface(5), 5.0, 5.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "a drop that will be refused asks for its files");
    supply(compositor, offer, kList);
    check(pump(world, [&] { return take() == "ENTER;"; }), "and is refused at its enter");
    heard.answer = PUGL_SUCCESS;
    heard.during = [&doomedLate] { puglFreeView(doomedLate); doomedLate = nullptr; };
    compositor.drop();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "the refused drop ends");
    check(doomedLate == nullptr && take() == "LEAVE;", "with the view gone inside its leave");
    check(describe(compositor.requestsOf(offer)) == "set_actions(1,1);accept(null);receive(text/uri-list);destroy;",
          "destroying that offer, unfinished too");
    compositor.leave();

    // the output's scale: positions are in buffer pixels, as the pointer's are
    compositor.enterOutput(surface);
    settle(world);
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(20, surface, 10.0, 20.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "a drag on a scaled view asks for its files");
    supply(compositor, offer, kList);
    check(pump(world, [&] { return ! heard.log.empty(); }), "and is heard");
    check(take() == "ENTER;" && heard.x == 20.0 && heard.y == 40.0, "at twice the surface position");
    compositor.leave();
    pump(world, [&] { return ! heard.log.empty(); });
    take();

    // opting out mid-drag
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    compositor.enter(21, surface, 1.0, 1.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "a last drag asks for its files");
    supply(compositor, offer, kList);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "and is accepted");
    take();
    puglSetAcceptsFileDrops(view, false);
    check(take() == "LEAVE;", "opting out ends it for the view with a leave");
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 5; }), "and takes the accept back");
    check(compositor.requestsOf(offer).back().textNull, "with a NULL accept");
    compositor.leave();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 6; }), "the offer is destroyed at the leave");

    // a drag left in flight when the world goes is cleaned up with it
    offer = compositor.offer({ "text/uri-list" }, kCopy);
    check(puglSetAcceptsFileDrops(view, true) == PUGL_SUCCESS, "opting in again");
    compositor.enter(22, surface, 1.0, 1.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "a drag in flight at teardown");

    puglFreeView(view);
    puglFreeView(plain);
    puglFreeWorld(world);
    compositor.stop();
}

static void testVersion1()
{
    StandIn compositor;
    compositor.dataDeviceVersion = 1;
    check(compositor.start(), "the stand-in starts again");

    PuglWorld* const world = openWorld(compositor);
    check(world != nullptr, "a Wayland world opens on a version 1 data device");
    if (world == nullptr)
        return compositor.stop();
    check(pump(world, [&] { return compositor.hasDataDevice(); }), "with its data device");

    PuglView* const view = makeView(world, true);
    check(view != nullptr && pump(world, [&] { return compositor.surface(0) != 0; }), "a view that accepts drops");

    const uint32_t offer = compositor.offer({ "text/uri-list" }, 0);
    compositor.enter(30, compositor.surface(0), 3.0, 4.0, offer);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 2; }), "the enter asks for the files");
    check(describe(compositor.requestsOf(offer)) == "accept(null);receive(text/uri-list);",
          "below version 3 there are no actions to set");
    supply(compositor, offer, kList);
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 3; }), "the view accepts");
    take();
    compositor.drop();
    check(pump(world, [&] { return compositor.requestsOf(offer).size() == 4; }), "the drop ends the offer");
    check(take() == "DROP;", "the view hears the drop");
    check(describe(compositor.requestsOf(offer)) == "accept(null);receive(text/uri-list);accept(text/uri-list);destroy;",
          "and below version 3 there is no finish, only the destroy");

    puglFreeView(view);
    puglFreeWorld(world);
    compositor.stop();
}

int main()
{
    // the stand-in writes into pipes whose reader may have gone
    std::signal(SIGPIPE, SIG_IGN);
    unsetenv("WAYLAND_DISPLAY");

    const int before = openDescriptors();

    testVersion3();
    testVersion1();

    check(openDescriptors() == before, "no descriptor is left open");

    if (failures != 0)
    {
        d_stderr2("WaylandDrop: %d checks failed", failures);
        return 1;
    }

    d_stdout("WaylandDrop: all checks passed");
    return 0;
}
