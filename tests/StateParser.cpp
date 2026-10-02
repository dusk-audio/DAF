/*
 * DISTRHO Plugin Framework (DPF)
 * Copyright (C) 2012-2026 Filipe Coelho <falktx@falktx.com>
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

/* Coverage for the parser behind the VST3 setState() and CLAP stateLoad() state loaders.
 * Hosts deliver the state stream in chunks of any size, so every stream here is fed at a range of chunk sizes,
 * down to one byte at a time, and must parse identically each way. A multi-MiB value checks that loading stays
 * linear: with per-chunk reallocation it took quadratic time. */

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "daf/src/DafPluginStateParser.hpp"

#include <chrono>
#include <string>

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

struct ParseResult {
    PluginStateParser::Status status;
    std::size_t consumed; // total bytes consumed across all chunks
    bool loneEmptyKey;
    std::vector<PluginStateParser::Entry> entries;
};

static ParseResult parseInChunks(const std::string& data, const std::size_t chunkSize)
{
    PluginStateParser parser;
    ParseResult result = { PluginStateParser::kStatusNeedMoreData, 0, false, {} };

    for (std::size_t pos = 0; pos < data.size();)
    {
        const std::size_t size = std::min(chunkSize, data.size() - pos);
        std::size_t consumed = 0;
        result.status = parser.parse(data.data() + pos, size, consumed);
        result.consumed += consumed;
        pos += size;

        if (result.status != PluginStateParser::kStatusNeedMoreData)
            break;
        if (consumed != size)
        {
            result.status = PluginStateParser::kStatusInvalidSection;
            d_stderr2("consumed %zu of %zu bytes without finishing", consumed, size);
            break;
        }
    }

    result.loneEmptyKey = parser.isLoneEmptyKey();
    result.entries = parser.entries;
    return result;
}

static bool sameResult(const ParseResult& a, const ParseResult& b)
{
    if (a.status != b.status || a.consumed != b.consumed || a.loneEmptyKey != b.loneEmptyKey)
        return false;
    if (a.entries.size() != b.entries.size())
        return false;
    for (std::size_t i = 0; i < a.entries.size(); ++i)
    {
        if (a.entries[i].type != b.entries[i].type
            || a.entries[i].key != b.entries[i].key
            || a.entries[i].value != b.entries[i].value)
            return false;
    }
    return true;
}

// parse whole, then confirm every chunking gives the same answer
static bool parseAllWays(const std::string& data, ParseResult& result)
{
    result = parseInChunks(data, data.size() + 1);

    static const std::size_t chunkSizes[] = { 1, 2, 3, 5, 7, 16, 511 };
    for (std::size_t chunkSize : chunkSizes)
    {
        if (! sameResult(result, parseInChunks(data, chunkSize)))
        {
            d_stderr2("chunk size %zu changes the result", chunkSize);
            return false;
        }
    }

    return true;
}

static std::string S(const char* const str, const std::size_t size)
{
    return std::string(str, size);
}

#define STATE(lit) S(lit, sizeof(lit) - 1)

// --------------------------------------------------------------------------------------------------------------------

static int runTests()
{
    ParseResult r;

    // full stream, as the wrappers write it (the writer appends one NUL after the terminator)
    {
        const std::string data = STATE("__daf_program__\0" "3\0"
                                       "__daf_state_begin__\0" "file\0" "/tmp/a b\0" "empty\0" "\0" "__daf_state_end__\0"
                                       "__daf_parameters_begin__\0" "gain\0" "0.5\0" "__daf_parameters_end__\0"
                                       "\xfe\0");
        DAF_ASSERT_EQUAL(parseAllWays(data, r), true, "full stream parses the same in any chunking");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusTerminated, "full stream terminates");
        DAF_ASSERT_EQUAL(r.consumed, data.size() - 1, "consumed up to and including the terminator");
        DAF_ASSERT_EQUAL(r.entries.size(), 4u, "full stream entries");
        DAF_ASSERT_EQUAL(r.entries[0].type, 'i', "program type");
        DAF_ASSERT_EQUAL(r.entries[0].key, std::string("__daf_program__"), "program key");
        DAF_ASSERT_EQUAL(r.entries[0].value, std::string("3"), "program value");
        DAF_ASSERT_EQUAL(r.entries[1].type, 's', "state type");
        DAF_ASSERT_EQUAL(r.entries[1].key, std::string("file"), "state key");
        DAF_ASSERT_EQUAL(r.entries[1].value, std::string("/tmp/a b"), "state value");
        DAF_ASSERT_EQUAL(r.entries[2].key, std::string("empty"), "empty state key");
        DAF_ASSERT_EQUAL(r.entries[2].value, std::string(), "empty state value");
        DAF_ASSERT_EQUAL(r.entries[3].type, 'p', "parameter type");
        DAF_ASSERT_EQUAL(r.entries[3].key, std::string("gain"), "parameter key");
        DAF_ASSERT_EQUAL(r.entries[3].value, std::string("0.5"), "parameter value");
    }

    // a terminator alone is a valid, empty state
    {
        DAF_ASSERT_EQUAL(parseAllWays(STATE("\xfe"), r), true, "terminator alone");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusTerminated, "terminator alone terminates");
        DAF_ASSERT_EQUAL(r.entries.size(), 0u, "terminator alone has no entries");
    }

    // '\xfe' inside a token is data, also when a chunk boundary falls right before it
    {
        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_state_begin__\0" "k\xfe\0" "v\xfe\xfe\0" "__daf_state_end__\0" "\xfe"), r),
                         true, "0xfe inside tokens");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusTerminated, "0xfe inside tokens terminates");
        DAF_ASSERT_EQUAL(r.entries.size(), 1u, "0xfe inside tokens entries");
        DAF_ASSERT_EQUAL(r.entries[0].key, std::string("k\xfe"), "0xfe key");
        DAF_ASSERT_EQUAL(r.entries[0].value, std::string("v\xfe\xfe"), "0xfe value");
    }

    // section markers are only markers in key position
    {
        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_state_begin__\0" "k\0" "__daf_state_end__\0" "__daf_state_end__\0" "\xfe"), r),
                         true, "marker as value");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusTerminated, "marker as value terminates");
        DAF_ASSERT_EQUAL(r.entries.size(), 1u, "marker as value entries");
        DAF_ASSERT_EQUAL(r.entries[0].value, std::string("__daf_state_end__"), "marker as value is kept");
    }

    // the stream must not end inside a section or between a key and its value
    {
        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_state_begin__\0" "\xfe"), r), true, "open state section");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusInvalidTerminator, "open state section is invalid");

        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_parameters_begin__\0" "\xfe"), r), true, "open parameter section");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusInvalidTerminator, "open parameter section is invalid");

        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_program__\0" "\xfe"), r), true, "dangling key");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusInvalidTerminator, "dangling key is invalid");
    }

    // markers and the program out of order
    {
        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_state_end__\0" "\xfe"), r), true, "state end without begin");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusInvalidSection, "state end without begin is invalid");

        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_parameters_begin__\0" "__daf_parameters_end__\0"
                                            "__daf_state_begin__\0" "\xfe"), r), true, "states after parameters");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusInvalidSection, "states after parameters are invalid");

        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_state_begin__\0" "__daf_program__\0" "1\0" "\xfe"), r),
                         true, "program inside states");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusInvalidSection, "program inside states is invalid");
    }

    // the no-parameters, no-states stream: a single NUL and no terminator
    {
        DAF_ASSERT_EQUAL(parseAllWays(STATE("\0"), r), true, "lone empty key");
        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusNeedMoreData, "lone empty key waits for more");
        DAF_ASSERT_EQUAL(r.loneEmptyKey, true, "lone empty key is recognised");

        DAF_ASSERT_EQUAL(parseInChunks(std::string(), 1).loneEmptyKey, false, "nothing read is not a lone empty key");
        DAF_ASSERT_EQUAL(parseAllWays(STATE("__daf_state_begin__\0" "k\0"), r), true, "truncated");
        DAF_ASSERT_EQUAL(r.loneEmptyKey, false, "truncated state is not a lone empty key");
        DAF_ASSERT_EQUAL(parseAllWays(STATE("\0" "v"), r), true, "partial value");
        DAF_ASSERT_EQUAL(r.loneEmptyKey, false, "partial value is not a lone empty key");
    }

    // large value, fed in the 64 KiB chunks the wrappers use: must stay linear
    {
        const std::size_t valueSize = 64u * 1024u * 1024u;
        std::string data("__daf_state_begin__\0" "blob\0", 25);
        data.append(valueSize, 'x');
        data.append("\0" "__daf_state_end__\0" "\xfe", 20);

        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        r = parseInChunks(data, 65536);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

        DAF_ASSERT_EQUAL(r.status, PluginStateParser::kStatusTerminated, "large state terminates");
        DAF_ASSERT_EQUAL(r.entries.size(), 1u, "large state entries");
        DAF_ASSERT_EQUAL(r.entries[0].value.size(), valueSize, "large state value size");
        d_stdout("parsed a %zu MiB state value in %.3f s", valueSize / (1024u * 1024u), seconds);

        // generous bound for slow CI machines, a quadratic loader is far beyond it
        DAF_ASSERT_EQUAL(seconds < 5.0, true, "large state parses in linear time");
    }

    return 0;
}

END_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    return DAF_NAMESPACE::runTests();
}
