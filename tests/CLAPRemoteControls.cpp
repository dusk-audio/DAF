/*
 * Dusk Audio Framework (DAF)
 * Copyright (C) 2026 Dusk Audio
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

/* Plugin::initRemoteControlPage() through the CLAP wrapper compiled into this test, with main() as the host. The
 * plugin (plugin-remote-controls/, DAF_PLUGIN_WANT_REMOTE_CONTROLS) lays its parameters out on three pages.
 *
 * - The remote controls extension is there under its id and under the draft id, as the same interface.
 * - Each instance asks the plugin for each page once, when it is created, up to and including the first index it has
 *   no page for, and never again. (The entry's own instance, which describes the plugin, asks too; the counts here
 *   are what creating one more instance adds.)
 * - count() is the number of pages, and get() gives each one: section and page names, the page index as its id, the
 *   parameter index as each control's param id in the plugin's order, CLAP_INVALID_ID for an empty control, and not
 *   a preset page. A control naming an output parameter or a missing one comes out empty.
 * - get() past the last page fails and leaves the page alone.
 *
 * CLAPWrapper checks that a plugin without the macro offers no such extension. */

#define DAF_PLUGIN_TARGET_CLAP
#define DAF_TEST_NO_DGL

#include "tests.hpp"
#include "plugin-remote-controls/RemoteControlsTestPlugin.hpp"
#include "daf/DafPluginMain.cpp"

#include <cstring>

// --------------------------------------------------------------------------------------------------------------------

static const void* CLAP_ABI host_get_extension(const clap_host_t*, const char*) { return nullptr; }
static void CLAP_ABI host_request(const clap_host_t*) {}

static bool samePage(const clap_remote_controls_page_t& page, const char* const section, const clap_id pageId,
                     const char* const name, const clap_id (&ids)[CLAP_REMOTE_CONTROLS_COUNT])
{
    if (std::strcmp(page.section_name, section) != 0 || page.page_id != pageId
        || std::strcmp(page.page_name, name) != 0 || page.is_for_preset)
        return false;

    for (uint32_t i = 0; i < CLAP_REMOTE_CONTROLS_COUNT; ++i)
        if (page.param_ids[i] != ids[i])
            return false;

    return true;
}

int main()
{
    USE_NAMESPACE_DAF;

    const clap_host_t host = {
        CLAP_VERSION, nullptr, "DAF tests", "DAF", "", "1.0",
        host_get_extension, host_request, host_request, host_request
    };

    DAF_ASSERT_EQUAL(clap_entry.init(""), true, "clap_entry.init must succeed");

    const clap_plugin_factory_t* const factory =
        static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
    DAF_ASSERT_NOT_EQUAL(factory, nullptr, "the plugin factory must be available");

    uint32_t before[kRemoteControlsTestCountedIndexes];
    for (uint32_t i = 0; i < kRemoteControlsTestCountedIndexes; ++i)
        before[i] = gPageInits[i].load();
    const uint32_t beforePast = gPageInitsPast.load();

    const clap_plugin_t* const plugin = factory->create_plugin(factory, &host, DAF_PLUGIN_CLAP_ID);
    DAF_ASSERT_NOT_EQUAL(plugin, nullptr, "create_plugin must succeed");
    DAF_ASSERT_EQUAL(plugin->init(plugin), true, "plugin init must succeed");

    // each page asked for once by the new instance, up to the first missing one
    for (uint32_t i = 0; i <= kRemoteControlsTestPages; ++i)
        DAF_ASSERT_EQUAL(gPageInits[i].load() - before[i], 1u,
                         "each page up to the first missing one must be asked for once");
    for (uint32_t i = kRemoteControlsTestPages + 1; i < kRemoteControlsTestCountedIndexes; ++i)
        DAF_ASSERT_EQUAL(gPageInits[i].load() - before[i], 0u, "no page past the first missing one may be asked for");
    DAF_ASSERT_EQUAL(gPageInitsPast.load() - beforePast, 0u, "no page past the first missing one may be asked for");

    const clap_plugin_remote_controls_t* const remote =
        static_cast<const clap_plugin_remote_controls_t*>(plugin->get_extension(plugin, CLAP_EXT_REMOTE_CONTROLS));
    DAF_ASSERT_NOT_EQUAL(remote, nullptr, "the remote controls extension must be available");
    DAF_ASSERT_EQUAL(plugin->get_extension(plugin, CLAP_EXT_REMOTE_CONTROLS_COMPAT) == remote, true,
                     "the draft id must give the same remote controls extension");

    // a control's param id is the parameter's id in the params extension
    const clap_plugin_params_t* const params =
        static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    DAF_ASSERT_NOT_EQUAL(params, nullptr, "the params extension must be available");
    clap_param_info_t info = {};
    DAF_ASSERT_EQUAL(params->get_info(plugin, 2, &info) && info.id == 2 && std::strcmp(info.name, "p2") == 0, true,
                     "parameter 2 must have param id 2, the id a page names it by");

    DAF_ASSERT_EQUAL(remote->count(plugin), kRemoteControlsTestPages, "count() must be the number of pages");

    // pages filled over garbage, so every field has to be written
    clap_remote_controls_page_t page;

    std::memset(&page, 0x5a, sizeof(page));
    const clap_id page0[CLAP_REMOTE_CONTROLS_COUNT] = {
        2, 0, CLAP_INVALID_ID, 4, 1, CLAP_INVALID_ID, CLAP_INVALID_ID, 3
    };
    DAF_ASSERT_EQUAL(remote->get(plugin, 0, &page), true, "get() of page 0 must succeed");
    DAF_ASSERT_EQUAL(samePage(page, "Main", 0, "Performance", page0), true,
                     "page 0 must keep its names, its controls in order and its empty controls");

    std::memset(&page, 0x5a, sizeof(page));
    const clap_id page1[CLAP_REMOTE_CONTROLS_COUNT] = {
        CLAP_INVALID_ID, CLAP_INVALID_ID, 0, CLAP_INVALID_ID,
        CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID
    };
    DAF_ASSERT_EQUAL(remote->get(plugin, 1, &page), true, "get() of page 1 must succeed");
    DAF_ASSERT_EQUAL(samePage(page, "Main", 1, "Checks", page1), true,
                     "a control naming an output or a missing parameter must come out empty");

    std::memset(&page, 0x5a, sizeof(page));
    const clap_id page2[CLAP_REMOTE_CONTROLS_COUNT] = {
        3, CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID,
        CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID, CLAP_INVALID_ID
    };
    DAF_ASSERT_EQUAL(remote->get(plugin, 2, &page), true, "get() of page 2 must succeed");
    DAF_ASSERT_EQUAL(samePage(page, "Other", 2, "", page2), true, "an unnamed page must come out with an empty name");

    std::memset(&page, 0x5a, sizeof(page));
    DAF_ASSERT_EQUAL(remote->get(plugin, kRemoteControlsTestPages, &page), false,
                     "get() past the last page must fail");
    DAF_ASSERT_EQUAL(static_cast<unsigned char>(page.page_name[0]), 0x5au,
                     "a failed get() must leave the page alone");
    DAF_ASSERT_EQUAL(remote->get(plugin, UINT32_MAX, &page), false, "get() of index UINT32_MAX must fail");

    // asking again asks the plugin nothing
    for (uint32_t i = 0; i <= kRemoteControlsTestPages; ++i)
        DAF_ASSERT_EQUAL(gPageInits[i].load() - before[i], 1u, "count() and get() must not ask the plugin again");

    plugin->destroy(plugin);
    clap_entry.deinit();

    d_stdout("CLAP remote controls tests passed");
    return 0;
}
