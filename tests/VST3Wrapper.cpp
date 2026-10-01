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
 * on every platform, and the factory and component must accept and return those same IDs. */

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
