// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

#include "dgl/Application.hpp"
#include "dgl/OpenGL.hpp"
#include "dgl/StandaloneWindow.hpp"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <string>
#include <fcntl.h>
#include <unistd.h>

START_NAMESPACE_DGL

class ActivationWindow : public StandaloneWindow
{
public:
    explicit ActivationWindow(Application& app) : StandaloneWindow(app)
    {
        setTitle("DAF native activation");
        setSize(640, 480);
        done();
    }

protected:
    void onDisplay() override
    {
        glClearColor(0.2f, 0.4f, 0.3f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    void onFocus(const bool focused, const CrossingMode) override
    {
        std::printf("FOCUS %d\n", focused ? 1 : 0);
    }

    bool onKeyboard(const KeyboardEvent& event) override
    {
        if (event.press)
            std::printf("KEY %u\n", event.key);
        return true;
    }
};

END_NAMESPACE_DGL

int main()
{
    USE_NAMESPACE_DGL;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (std::getenv("DISPLAY") != nullptr)
        return 2;
    Application app(true, Application::kTypeAuto, Application::kWindowSystemWayland);
    ActivationWindow window(app);
    if (window.getCompletedDisplayCount() != 0)
        return 6;
    window.show();
    std::printf("WINDOW %lu PID %ld TOKEN_CLEARED %d\n",
                static_cast<unsigned long>(window.getNativeWindowHandle()), static_cast<long>(getpid()),
                std::getenv("XDG_ACTIVATION_TOKEN") == nullptr ? 1 : 0);
    if (fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == -1)
        return 3;
    std::string input;
    bool drewFrame = false;
    while (!app.isQuitting())
    {
        app.idle();
        if (!drewFrame && window.getCompletedDisplayCount() != 0)
        {
            drewFrame = true;
            std::puts("FRAME");
        }
        char buffer[1024];
        const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count == 0)
            break;
        if (count < 0 && errno != EAGAIN && errno != EINTR)
            return 5;
        if (count > 0)
            input.append(buffer, static_cast<size_t>(count));
        if (input.size() > 4096)
            return 4;
        for (size_t end; (end = input.find('\n')) != std::string::npos;)
        {
            const std::string command = input.substr(0, end);
            input.erase(0, end + 1);
            if (command.compare(0, 6, "TOKEN ") == 0)
            {
                window.focusWithActivationToken(command.c_str() + 6);
                std::puts("HANDOFF");
            }
        }
        usleep(10000);
    }
    window.close();
    return 0;
}
