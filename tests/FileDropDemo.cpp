// Copyright (C) 2026 Dusk Audio
// SPDX-License-Identifier: ISC

// A manual test for file drops (see dgl/src/drop-targets.md): a window that accepts them and prints what it
// hears, one line per callback, for end-to-end runs against a real source and compositor.
//
//   FileDropDemo [--refuse-right] [--source FILE]
//
// --refuse-right refuses drops on the right half of the window. --source also opens a window that starts a
// drag of FILE, with DGL's own startFileDrag, when it is clicked. Each window prints its native handle.

#include "dgl/Application.hpp"
#include "dgl/Cairo.hpp"
#include "dgl/StandaloneWindow.hpp"

#include <cstdio>
#include <cstring>
#include <string>

START_NAMESPACE_DGL

class DropTarget : public StandaloneWindow
{
public:
    bool refuseRight = false;

    explicit DropTarget(Application& app) : StandaloneWindow(app)
    {
        setTitle("DGL drop target");
        setSize(400, 300);
        done();
    }

protected:
    void onDisplay() override
    {
        cairo_t* const cairo = static_cast<const CairoGraphicsContext&>(getGraphicsContext()).handle;
        cairo_set_source_rgb(cairo, 0.2, 0.4, 0.3);
        cairo_paint(cairo);
    }

    bool answer(const char* const what, const FileDropEvent& ev)
    {
        const bool accept = ! refuseRight || ev.pos.getX() < getWidth() / 2;
        std::printf("%s x=%.1f y=%.1f mod=0x%x time=%u files=%u accept=%d\n", what,
                    ev.pos.getX(), ev.pos.getY(), ev.mod, ev.time, ev.numFiles, accept ? 1 : 0);
        for (uint i = 0; i < ev.numFiles; ++i)
            std::printf("  file %s\n", ev.files[i]);
        std::fflush(stdout);
        return accept;
    }

    bool onFileDragEnter(const FileDropEvent& ev) override { return answer("ENTER", ev); }
    bool onFileDragMove(const FileDropEvent& ev) override { return answer("MOVE", ev); }

    void onFileDragLeave() override
    {
        std::printf("LEAVE\n");
        std::fflush(stdout);
    }

    bool onFileDrop(const FileDropEvent& ev) override { return answer("DROP", ev); }
};

class DragSource : public StandaloneWindow
{
public:
    std::string file;

    explicit DragSource(Application& app) : StandaloneWindow(app)
    {
        setTitle("DGL drag source");
        setSize(200, 200);
        done();
    }

protected:
    void onDisplay() override
    {
        cairo_t* const cairo = static_cast<const CairoGraphicsContext&>(getGraphicsContext()).handle;
        cairo_set_source_rgb(cairo, 0.4, 0.3, 0.2);
        cairo_paint(cairo);
    }

    bool onMouse(const MouseEvent& ev) override
    {
        if (ev.press && ev.button == 1)
        {
            const bool started = startFileDrag(file.c_str());
            std::printf("SOURCE start=%d\n", started ? 1 : 0);
            std::fflush(stdout);
        }
        return true;
    }
};

END_NAMESPACE_DGL

int main(int argc, char* argv[])
{
    USE_NAMESPACE_DGL;

    bool refuseRight = false;
    const char* source = nullptr;

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--refuse-right") == 0)
            refuseRight = true;
        else if (std::strcmp(argv[i], "--source") == 0 && i + 1 < argc)
            source = argv[++i];
    }

    Application app;

    DropTarget target(app);
    target.refuseRight = refuseRight;
    target.setOffset(600, 100);
    const bool accepting = target.setAcceptingFileDrops(true);
    target.show();
    std::printf("TARGET window=0x%lx accepting=%d\n",
                static_cast<unsigned long>(target.getNativeWindowHandle()), accepting ? 1 : 0);

    DragSource* dragSource = nullptr;
    if (source != nullptr)
    {
        dragSource = new DragSource(app);
        dragSource->file = source;
        dragSource->setOffset(100, 100);
        dragSource->show();
        std::printf("SOURCE window=0x%lx\n", static_cast<unsigned long>(dragSource->getNativeWindowHandle()));
    }

    std::fflush(stdout);
    app.exec();
    delete dragSource;
    return 0;
}
