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

// Embeds a plugin's UI the way a Linux host does, by handing it an X11 parent window, and checks the
// plugin's answer. A DGL_BACKEND=both plugin UI opens on X11 when there is an X server and on Wayland when
// there is none, and a Wayland window cannot go into an X11 parent, so each format has to refuse the embed
// then rather than report one it cannot do.
//
//   plugin-embed-host embedded|refused [vst2=PATH] [vst3=PATH] [clap=PATH]
//
// With DISPLAY set the parent is a real window on that X server, otherwise a made-up id, which a plugin has
// to refuse before it uses it. Exits 1 when any format answers otherwise than expected. Linux only, built
// and run by wayland.yml's runtime-backend job.

#include "clap/entry.h"
#include "clap/ext/gui.h"
#include "clap/ext/timer-support.h"
#include "clap/factory/plugin-factory.h"
#include "travesty/component.h"
#include "travesty/edit_controller.h"
#include "travesty/factory.h"
#include "travesty/host.h"
#include "travesty/view.h"
#include "xaymar-vst2/vst.h"

#include <X11/Xlib.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

// --------------------------------------------------------------------------------------------------------------------
// VST2

static intptr_t vst2HostCallback(vst_effect*, const VST_HOST_OPCODE opcode, int32_t, int64_t, void*, float)
{
    // the host's VST version, without which a plugin refuses to load
    return opcode == VST_HOST_OPCODE_01 ? 2400 : 0;
}

static bool embedVst2(const char* const path, const uintptr_t parent)
{
    void* const lib = dlopen(path, RTLD_NOW|RTLD_LOCAL);
    if (lib == nullptr)
    {
        fprintf(stderr, "VST2: %s\n", dlerror());
        exit(2);
    }

    typedef vst_effect* (*VSTPluginMain)(vst_host_callback);
    const VSTPluginMain entry = reinterpret_cast<VSTPluginMain>(dlsym(lib, "VSTPluginMain"));
    vst_effect* const effect = entry != nullptr ? entry(vst2HostCallback) : nullptr;
    if (effect == nullptr)
    {
        fprintf(stderr, "VST2: %s has no plugin\n", path);
        exit(2);
    }

    effect->control(effect, VST_EFFECT_OPCODE_CREATE, 0, 0, nullptr, 0.0f);

    const bool embedded = effect->control(effect, VST_EFFECT_OPCODE_WINDOW_CREATE, 0, 0,
                                          reinterpret_cast<void*>(parent), 0.0f) != 0;
    if (embedded)
        effect->control(effect, VST_EFFECT_OPCODE_WINDOW_DESTROY, 0, 0, nullptr, 0.0f);

    effect->control(effect, VST_EFFECT_OPCODE_DESTROY, 0, 0, nullptr, 0.0f);
    return embedded;
}

// --------------------------------------------------------------------------------------------------------------------
// VST3
//
// The host application, plugin frame and run loop a Linux host passes, as far as attaching a view needs them.
// Each object is a pointer to its static function table, and lives as long as the process, so references are
// not counted. The host application makes no messages: a plugin that connects its view to its controller,
// which attaching a view does, reports failing to make them, and attaches the view all the same.

static v3_host_application_cpp sHostApplicationTable;
static v3_plugin_frame_cpp sPluginFrameTable;
static v3_run_loop_cpp sRunLoopTable;

static void* sHostApplication = &sHostApplicationTable;
static void* sPluginFrame = &sPluginFrameTable;
static void* sRunLoop = &sRunLoopTable;

static uint32_t V3_API v3NoRefCount(void*)
{
    return 1;
}

static v3_result V3_API hostApplicationQueryInterface(void* const self, const v3_tuid iid, void** const obj)
{
    if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_host_application_iid))
    {
        *obj = self;
        return V3_OK;
    }

    *obj = nullptr;
    return V3_NO_INTERFACE;
}

static v3_result V3_API pluginFrameQueryInterface(void* const self, const v3_tuid iid, void** const obj)
{
    if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_plugin_frame_iid))
    {
        *obj = self;
        return V3_OK;
    }

    if (v3_tuid_match(iid, v3_run_loop_iid))
    {
        *obj = &sRunLoop;
        return V3_OK;
    }

    *obj = nullptr;
    return V3_NO_INTERFACE;
}

static v3_result V3_API runLoopQueryInterface(void* const self, const v3_tuid iid, void** const obj)
{
    if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_run_loop_iid))
    {
        *obj = self;
        return V3_OK;
    }

    *obj = nullptr;
    return V3_NO_INTERFACE;
}

static void setUpVst3Host()
{
    sHostApplicationTable.query_interface = hostApplicationQueryInterface;
    sHostApplicationTable.ref = v3NoRefCount;
    sHostApplicationTable.unref = v3NoRefCount;
    sHostApplicationTable.app.get_name = [](void*, v3_str_128 name) -> v3_result {
        const char* const hostName = "plugin-embed-host";
        for (size_t i = 0; i <= std::strlen(hostName); ++i)
            name[i] = hostName[i];
        return V3_OK;
    };
    sHostApplicationTable.app.create_instance = [](void*, v3_tuid, v3_tuid, void** const obj) -> v3_result {
        *obj = nullptr;
        return V3_NOT_IMPLEMENTED;
    };

    sPluginFrameTable.query_interface = pluginFrameQueryInterface;
    sPluginFrameTable.ref = v3NoRefCount;
    sPluginFrameTable.unref = v3NoRefCount;
    sPluginFrameTable.frame.resize_view = [](void*, v3_plugin_view**, v3_view_rect*) -> v3_result {
        return V3_OK;
    };

    sRunLoopTable.query_interface = runLoopQueryInterface;
    sRunLoopTable.ref = v3NoRefCount;
    sRunLoopTable.unref = v3NoRefCount;
    sRunLoopTable.loop.register_event_handler = [](void*, v3_event_handler**, int) -> v3_result {
        return V3_OK;
    };
    sRunLoopTable.loop.unregister_event_handler = [](void*, v3_event_handler**) -> v3_result {
        return V3_OK;
    };
    sRunLoopTable.loop.register_timer = [](void*, v3_timer_handler**, uint64_t) -> v3_result {
        return V3_OK;
    };
    sRunLoopTable.loop.unregister_timer = [](void*, v3_timer_handler**) -> v3_result {
        return V3_OK;
    };
}

static bool embedVst3(const char* const path, const uintptr_t parent)
{
    void* const lib = dlopen(path, RTLD_NOW|RTLD_LOCAL);
    if (lib == nullptr)
    {
        fprintf(stderr, "VST3: %s\n", dlerror());
        exit(2);
    }

    typedef bool (*ModuleEntry)(void*);
    typedef v3_plugin_factory** (*GetPluginFactory)();
    const ModuleEntry moduleEntry = reinterpret_cast<ModuleEntry>(dlsym(lib, "ModuleEntry"));
    const GetPluginFactory getPluginFactory = reinterpret_cast<GetPluginFactory>(dlsym(lib, "GetPluginFactory"));
    if (moduleEntry == nullptr || getPluginFactory == nullptr || ! moduleEntry(lib))
    {
        fprintf(stderr, "VST3: %s does not start as a module\n", path);
        exit(2);
    }

    v3_plugin_factory** const factory = getPluginFactory();
    v3_funknown** const host = reinterpret_cast<v3_funknown**>(&sHostApplication);

    // the component, which makes the plugin
    v3_component** component = nullptr;
    for (int32_t i = 0, count = v3_cpp_obj(factory)->num_classes(factory); i < count && component == nullptr; ++i)
    {
        v3_class_info info;
        if (v3_cpp_obj(factory)->get_class_info(factory, i, &info) == V3_OK
            && std::strcmp(info.category, "Audio Module Class") == 0)
            v3_cpp_obj(factory)->create_instance(factory, info.class_id, v3_component_iid,
                                                 reinterpret_cast<void**>(&component));
    }

    if (component == nullptr || v3_cpp_obj_initialize(component, host) != V3_OK)
    {
        fprintf(stderr, "VST3: %s has no component to create\n", path);
        exit(2);
    }

    // the edit controller, from the component itself or from a class of its own
    v3_edit_controller** controller = nullptr;
    if (v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &controller) != V3_OK)
    {
        v3_tuid controllerClassId;
        if (v3_cpp_obj(component)->get_controller_class_id(component, controllerClassId) == V3_OK)
            v3_cpp_obj(factory)->create_instance(factory, controllerClassId, v3_edit_controller_iid,
                                                 reinterpret_cast<void**>(&controller));
    }

    if (controller == nullptr || v3_cpp_obj_initialize(controller, host) != V3_OK)
    {
        fprintf(stderr, "VST3: %s has no edit controller to create\n", path);
        exit(2);
    }

    v3_plugin_view** const view = v3_cpp_obj(controller)->create_view(controller, "editor");
    if (view == nullptr)
    {
        fprintf(stderr, "VST3: %s creates no view\n", path);
        exit(2);
    }

    v3_cpp_obj(view)->set_frame(view, reinterpret_cast<v3_plugin_frame**>(&sPluginFrame));

    const bool embedded = v3_cpp_obj(view)->attached(view, reinterpret_cast<void*>(parent),
                                                     V3_VIEW_PLATFORM_TYPE_X11) == V3_OK;
    if (embedded)
        v3_cpp_obj(view)->removed(view);

    v3_cpp_obj_unref(view);
    v3_cpp_obj_terminate(controller);
    v3_cpp_obj_unref(controller);
    v3_cpp_obj_terminate(component);
    v3_cpp_obj_unref(component);
    return embedded;
}

// --------------------------------------------------------------------------------------------------------------------
// CLAP
//
// The host extensions an embedded CLAP UI needs, doing nothing. Timers never fire.

static const void* CLAP_ABI clapHostGetExtension(const clap_host_t*, const char* const id)
{
    static const clap_host_gui_t gui = {
        [](const clap_host_t*) {},
        [](const clap_host_t*, uint32_t, uint32_t) { return true; },
        [](const clap_host_t*) { return true; },
        [](const clap_host_t*) { return true; },
        [](const clap_host_t*, bool) {},
    };
    static const clap_host_timer_support_t timerSupport = {
        [](const clap_host_t*, uint32_t, clap_id* const timerId) { *timerId = 1; return true; },
        [](const clap_host_t*, clap_id) { return true; },
    };

    if (std::strcmp(id, CLAP_EXT_GUI) == 0)
        return &gui;
    if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0)
        return &timerSupport;
    return nullptr;
}

static bool embedClap(const char* const path, const uintptr_t parent)
{
    void* const lib = dlopen(path, RTLD_NOW|RTLD_LOCAL);
    if (lib == nullptr)
    {
        fprintf(stderr, "CLAP: %s\n", dlerror());
        exit(2);
    }

    const clap_plugin_entry_t* const entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    if (entry == nullptr || ! entry->init(path))
    {
        fprintf(stderr, "CLAP: %s does not start as a module\n", path);
        exit(2);
    }

    static const clap_host_t host = {
        CLAP_VERSION, nullptr, "plugin-embed-host", "Dusk Audio", "", "1.0",
        clapHostGetExtension,
        [](const clap_host_t*) {},
        [](const clap_host_t*) {},
        [](const clap_host_t*) {},
    };

    const clap_plugin_factory_t* const factory
        = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_plugin_descriptor_t* const descriptor
        = factory != nullptr ? factory->get_plugin_descriptor(factory, 0) : nullptr;
    const clap_plugin_t* const plugin
        = descriptor != nullptr ? factory->create_plugin(factory, &host, descriptor->id) : nullptr;
    if (plugin == nullptr || ! plugin->init(plugin))
    {
        fprintf(stderr, "CLAP: %s has no plugin to create\n", path);
        exit(2);
    }

    const clap_plugin_gui_t* const gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    if (gui == nullptr || ! gui->create(plugin, CLAP_WINDOW_API_X11, false))
    {
        fprintf(stderr, "CLAP: %s offers no embedded X11 UI\n", path);
        exit(2);
    }

    clap_window_t window;
    window.api = CLAP_WINDOW_API_X11;
    window.x11 = parent;

    const bool embedded = gui->set_parent(plugin, &window);

    gui->destroy(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    return embedded;
}

// --------------------------------------------------------------------------------------------------------------------

int main(const int argc, char* argv[])
{
    const bool expectEmbedded = argc > 1 && std::strcmp(argv[1], "embedded") == 0;
    if (argc < 3 || (! expectEmbedded && std::strcmp(argv[1], "refused") != 0))
    {
        fprintf(stderr, "usage: %s embedded|refused [vst2=PATH] [vst3=PATH] [clap=PATH]\n", argv[0]);
        return 2;
    }

    Display* display = nullptr;
    uintptr_t parent = 0x2a00001;
    if (std::getenv("DISPLAY") != nullptr)
    {
        display = XOpenDisplay(nullptr);
        if (display == nullptr)
        {
            fprintf(stderr, "cannot open the X server in DISPLAY\n");
            return 2;
        }
        parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 640, 480, 0, 0, 0);
        XSync(display, False);
    }

    setUpVst3Host();

    int status = 0;
    for (int i = 2; i < argc; ++i)
    {
        const char* const format = argv[i];
        const char* const path = std::strchr(format, '=');
        if (path == nullptr)
        {
            fprintf(stderr, "'%s' is not FORMAT=PATH\n", format);
            return 2;
        }

        bool embedded;
        /**/ if (std::strncmp(format, "vst2=", 5) == 0)
            embedded = embedVst2(path + 1, parent);
        else if (std::strncmp(format, "vst3=", 5) == 0)
            embedded = embedVst3(path + 1, parent);
        else if (std::strncmp(format, "clap=", 5) == 0)
            embedded = embedClap(path + 1, parent);
        else
        {
            fprintf(stderr, "unknown format in '%s'\n", format);
            return 2;
        }

        const bool ok = embedded == expectEmbedded;
        printf("plugin-embed-host: %.4s %s the X11 parent%s\n",
               format, embedded ? "embedded in" : "refused", ok ? "" : ", which it should not have");
        if (! ok)
            status = 1;
    }

    if (display != nullptr)
    {
        XDestroyWindow(display, parent);
        XCloseDisplay(display);
    }

    return status;
}
