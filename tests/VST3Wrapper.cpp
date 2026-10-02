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

/* The VST3 wrapper compiled into this test, with main() as the host. Built with
 * DAF_VST3_CROSS_PLATFORM_UID: the class ID string must be the one Linux and macOS have always shown,
 * on every platform, and the factory and component must accept and return those same IDs.
 *
 * Plugin::updateStateValue() from activate() must be in the state the component saves, and, as this plugin
 * has no UI, mark the project modified through IComponentHandler2 from setActive. */

#define DAF_PLUGIN_TARGET_VST3
#define DAF_TEST_NO_DGL

#include "tests.hpp"
#include "plugin-wrappers/WrapperTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

#include <string>

// --------------------------------------------------------------------------------------------------------------------

// the class ID string as the VST3 SDK's FUID::toString prints it on this platform
static std::string sdkClassIdString(const v3_tuid tuid)
{
    char buf[40];
    std::string str;
    const uint8_t* const bytes = reinterpret_cast<const uint8_t*>(tuid);

   #if V3_COM_COMPAT
    // COM GUID: a 32-bit and two 16-bit native (little-endian) fields, then 8 bytes as they are
    uint32_t data1;
    uint16_t data2, data3;
    std::memcpy(&data1, bytes, 4);
    std::memcpy(&data2, bytes + 4, 2);
    std::memcpy(&data3, bytes + 6, 2);
    std::snprintf(buf, sizeof(buf), "%08X%04X%04X", data1, data2, data3);
    str = buf;
    for (int i = 8; i < 16; ++i)
   #else
    for (int i = 0; i < 16; ++i)
   #endif
    {
        std::snprintf(buf, sizeof(buf), "%02X", bytes[i]);
        str += buf;
    }

    return str;
}

// the string Linux and macOS show: each of the four words written out as little-endian bytes
static std::string expectedClassIdString(const uint32_t kind)
{
    const uint32_t words[4] = {
        d_cconst('D', 'P', 'F', ' '), kind, d_cconst(STRINGIFY(DAF_PLUGIN_UNIQUE_ID)), d_cconst(STRINGIFY(DAF_PLUGIN_BRAND_ID))
    };

    char buf[4];
    std::string str;
    for (int w = 0; w < 4; ++w)
        for (int b = 0; b < 4; ++b)
        {
            std::snprintf(buf, sizeof(buf), "%02X", static_cast<unsigned>((words[w] >> (8 * b)) & 0xff));
            str += buf;
        }

    return str;
}

// --------------------------------------------------------------------------------------------------------------------
// a component handler that counts IComponentHandler2::setDirty calls, and a stream that collects what is written

static int gSetDirtyCalls = 0;
static std::string gStreamData;

static v3_component_handler_cpp gHandler;
static v3_component_handler2_cpp gHandler2;
static v3_component_handler_cpp* gHandlerPtr = &gHandler;
static v3_component_handler2_cpp* gHandler2Ptr = &gHandler2;
static v3_bstream_cpp gStream;
static v3_bstream_cpp* gStreamPtr = &gStream;

static v3_result V3_API handler_query_interface(void*, const v3_tuid iid, void** const obj)
{
    if (v3_tuid_match(iid, v3_component_handler2_iid))
    {
        *obj = &gHandler2Ptr;
        return V3_OK;
    }
    if (v3_tuid_match(iid, v3_funknown_iid) || v3_tuid_match(iid, v3_component_handler_iid))
    {
        *obj = &gHandlerPtr;
        return V3_OK;
    }
    *obj = nullptr;
    return V3_NO_INTERFACE;
}

static v3_result V3_API no_interface(void*, const v3_tuid, void** const obj) { *obj = nullptr; return V3_NO_INTERFACE; }
static uint32_t V3_API static_ref(void*) { return 1; }
static v3_result V3_API handler_edit(void*, v3_param_id) { return V3_OK; }
static v3_result V3_API handler_perform_edit(void*, v3_param_id, double) { return V3_OK; }
static v3_result V3_API handler_restart(void*, int32_t) { return V3_OK; }
static v3_result V3_API handler2_set_dirty(void*, const v3_bool state) { if (state) ++gSetDirtyCalls; return V3_OK; }
static v3_result V3_API handler2_open_editor(void*, const char*) { return V3_OK; }
static v3_result V3_API handler2_group_edit(void*) { return V3_OK; }

static v3_result V3_API stream_read(void*, void*, int32_t, int32_t* const read)
{
    if (read != nullptr) *read = 0;
    return V3_OK;
}

static v3_result V3_API stream_write(void*, void* const buffer, const int32_t size, int32_t* const written)
{
    gStreamData.append(static_cast<const char*>(buffer), static_cast<std::size_t>(size));
    if (written != nullptr) *written = size;
    return V3_OK;
}

static v3_result V3_API stream_seek(void*, int64_t, int32_t, int64_t*) { return V3_NOT_IMPLEMENTED; }
static v3_result V3_API stream_tell(void*, int64_t*) { return V3_NOT_IMPLEMENTED; }

static void initTestObjects()
{
    gHandler.query_interface = handler_query_interface;
    gHandler.ref = gHandler.unref = static_ref;
    gHandler.comp.begin_edit = gHandler.comp.end_edit = handler_edit;
    gHandler.comp.perform_edit = handler_perform_edit;
    gHandler.comp.restart_component = handler_restart;

    gHandler2.query_interface = handler_query_interface;
    gHandler2.ref = gHandler2.unref = static_ref;
    gHandler2.comp2.set_dirty = handler2_set_dirty;
    gHandler2.comp2.request_open_editor = handler2_open_editor;
    gHandler2.comp2.start_group_edit = gHandler2.comp2.finish_group_edit = handler2_group_edit;

    gStream.query_interface = no_interface;
    gStream.ref = gStream.unref = static_ref;
    gStream.stream.read = stream_read;
    gStream.stream.write = stream_write;
    gStream.stream.seek = stream_seek;
    gStream.stream.tell = stream_tell;
}

// whether the state the component saves has the status state at this value
static bool savedStateHasStatus(v3_component_cpp** const component, const char* const value)
{
    gStreamData.clear();
    if ((*component)->comp.get_state(component, (v3_bstream**)&gStreamPtr) != V3_OK)
        return false;
    return gStreamData.find(std::string(kWrapperTestStatusKey) + '\0' + value + '\0') != std::string::npos;
}

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    USE_NAMESPACE_DAF;

   #if defined(DAF_OS_WINDOWS)
    DAF_ASSERT_EQUAL(InitDll(), true, "InitDll must succeed");
   #elif defined(DAF_OS_MAC)
    DAF_ASSERT_EQUAL(bundleEntry(nullptr), true, "bundleEntry must succeed");
   #else
    DAF_ASSERT_EQUAL(ModuleEntry(nullptr), true, "ModuleEntry must succeed");
   #endif

    v3_plugin_factory_cpp** const factory = (v3_plugin_factory_cpp**)GetPluginFactory();
    DAF_ASSERT_NOT_EQUAL(factory, nullptr, "GetPluginFactory must return the factory");

    const int32_t numClasses = (*factory)->v1.num_classes(factory);
    DAF_ASSERT_NOT_EQUAL(numClasses, 0, "the factory must list the plugin");

    v3_class_info info = {};
    DAF_ASSERT_EQUAL((*factory)->v1.get_class_info(factory, 0, &info), V3_OK, "get_class_info must succeed");
    DAF_ASSERT_EQUAL(sdkClassIdString(info.class_id), expectedClassIdString(d_cconst('c', 'l', 'a', 's')),
                     "the component class ID string must match Linux and macOS");

    // the ID the factory lists is the one it must create from
    v3_component_cpp** component = nullptr;
    DAF_ASSERT_EQUAL((*factory)->v1.create_instance(factory, info.class_id, v3_component_iid, (void**)&component),
                     V3_OK, "create_instance must accept the listed class ID");
    DAF_ASSERT_NOT_EQUAL(component, nullptr, "create_instance must return the component");

    // and the controller ID the component names is the one the factory lists
    v3_tuid controllerId = {};
    DAF_ASSERT_EQUAL((*component)->comp.get_controller_class_id(component, controllerId), V3_OK,
                     "get_controller_class_id must succeed");
    DAF_ASSERT_EQUAL(sdkClassIdString(controllerId), expectedClassIdString(d_cconst('c', 't', 'r', 'l')),
                     "the controller class ID string must match Linux and macOS");

    if (numClasses > 1)
    {
        v3_class_info ctrlInfo = {};
        DAF_ASSERT_EQUAL((*factory)->v1.get_class_info(factory, 1, &ctrlInfo), V3_OK, "get_class_info must succeed");
        DAF_ASSERT_EQUAL(std::memcmp(ctrlInfo.class_id, controllerId, sizeof(v3_tuid)), 0,
                         "the factory must list the controller ID the component names");
    }

    // Plugin::updateStateValue() from activate()
    initTestObjects();
    DAF_ASSERT_EQUAL(v3_cpp_obj_initialize(component, nullptr), V3_OK, "component initialize must succeed");

    v3_edit_controller_cpp** controller = nullptr;
    DAF_ASSERT_EQUAL(v3_cpp_obj_query_interface(component, v3_edit_controller_iid, &controller), V3_OK,
                     "the component must also be the edit controller");
    DAF_ASSERT_EQUAL((*controller)->ctrl.set_component_handler(controller, (v3_component_handler**)&gHandlerPtr),
                     V3_OK, "set_component_handler must succeed");

    DAF_ASSERT_EQUAL(gUpdateStateFromConstructor, false, "updateStateValue must fail from the plugin constructor");
    DAF_ASSERT_EQUAL(gUpdateStateFromInitState, false, "updateStateValue must fail from initState");

    DAF_ASSERT_EQUAL((*component)->comp.set_active(component, true), V3_OK, "set_active must succeed");
    DAF_ASSERT_EQUAL(gUpdateStateFromActivate, true, "updateStateValue must succeed from activate");
    DAF_ASSERT_EQUAL(gUpdateStateUnknownKey, false, "updateStateValue must fail for an unknown key");
    DAF_ASSERT_EQUAL(gSetDirtyCalls, 1, "an update must mark the project modified");
    DAF_ASSERT_EQUAL(savedStateHasStatus(component, "active-1"), true, "a saved state must have the update");

    DAF_ASSERT_EQUAL((*component)->comp.set_active(component, false), V3_OK, "set_active must succeed");
    DAF_ASSERT_EQUAL(gSetDirtyCalls, 1, "nothing new must not mark the project modified");
    DAF_ASSERT_EQUAL((*component)->comp.set_active(component, true), V3_OK, "set_active must succeed");
    DAF_ASSERT_EQUAL(gSetDirtyCalls, 2, "an update must mark the project modified");
    DAF_ASSERT_EQUAL(savedStateHasStatus(component, "active-2"), true, "a saved state must have the update");
    DAF_ASSERT_EQUAL((*component)->comp.set_active(component, false), V3_OK, "set_active must succeed");

    (*controller)->ctrl.set_component_handler(controller, nullptr);
    v3_cpp_obj_unref(controller);
    (*component)->base.terminate(component);

    (*component)->unref(component);
    (*factory)->unref(factory);

   #if defined(DAF_OS_WINDOWS)
    ExitDll();
   #elif defined(DAF_OS_MAC)
    bundleExit();
   #else
    ModuleExit();
   #endif

    d_stdout("VST3 wrapper tests passed");
    return 0;
}
