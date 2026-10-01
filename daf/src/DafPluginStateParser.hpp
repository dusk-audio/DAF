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

#ifndef DAF_PLUGIN_STATE_PARSER_HPP_INCLUDED
#define DAF_PLUGIN_STATE_PARSER_HPP_INCLUDED

#include "../DafUtils.hpp"

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

/**
   Incremental parser for the state blob the VST3 and CLAP wrappers write.

   The blob is a sequence of NUL-terminated tokens, read as key/value pairs:

     [__daf_program__ \0 <n> \0]
     [__daf_state_begin__ \0 (<key> \0 <value> \0)* __daf_state_end__ \0]
     [__daf_parameters_begin__ \0 (<symbol> \0 <value> \0)* __daf_parameters_end__ \0]
     \xfe

   The section markers are single tokens, not pairs. A '\xfe' byte at the start of a token terminates the
   stream; anywhere else it is ordinary data.

   Hosts hand the stream over in chunks of whatever size they like, so a token can span any number of chunks.
   Each token is accumulated in a buffer with geometric growth and is moved into `entries` once complete,
   which keeps loading linear in the size of the state no matter how large a single value is.

   Every complete pair is recorded together with the section it was found in:
   'i' (before any section, nothing but an optional program so far), 's' (states) or 'p' (parameters).
   Applying them is up to the caller, after the whole stream was validated.
 */
class PluginStateParser
{
public:
    enum Status {
        /** All data consumed, the stream has not been terminated yet. */
        kStatusNeedMoreData,
        /** A valid terminator was found. */
        kStatusTerminated,
        /** A terminator was found at a place where the stream must not end. */
        kStatusInvalidTerminator,
        /** A section marker or the program key appeared out of order. */
        kStatusInvalidSection
    };

    struct Entry {
        char type;
        std::string key, value;
    };

    std::vector<Entry> entries;

    PluginStateParser() noexcept
        : fSection('i'),
          fFillingKey(true) {}

    /**
       Parse the next chunk of the stream.
       @a consumed receives the number of bytes used, including the terminator when one is found.
       Once a status other than kStatusNeedMoreData is returned, the parser must not be fed again.
     */
    Status parse(const char* const data, const std::size_t size, std::size_t& consumed)
    {
        std::size_t pos = 0;

        while (pos < size)
        {
            // found terminator, stop here
            if (fToken.empty() && data[pos] == '\xfe')
            {
                consumed = pos + 1;

                // Which sections the stream carries depends on the build that wrote it, so every state that
                // closes all the sections it opened is valid here: 'i' (nothing but the terminator, or just a
                // program), 'n' (program and/or states done) and 'x' (parameters done).
                // A stream stopping mid-section ('s' or 'p') or mid key/value pair is not.
                if ((fSection != 'i' && fSection != 'n' && fSection != 'x') || ! fFillingKey)
                    return kStatusInvalidTerminator;

                return kStatusTerminated;
            }

            const char* const start = data + pos;
            const char* const end = static_cast<const char*>(std::memchr(start, '\0', size - pos));

            if (end == nullptr)
            {
                // the token continues in the next chunk
                fToken.append(start, size - pos);
                pos = size;
                break;
            }

            fToken.append(start, static_cast<std::size_t>(end - start));
            pos += static_cast<std::size_t>(end - start) + 1;

            if (! tokenCompleted())
            {
                consumed = pos;
                return kStatusInvalidSection;
            }
        }

        consumed = pos;
        return kStatusNeedMoreData;
    }

    /** Current section: 'i', 'n', 's', 'p' or 'x'. See parse() for their meaning. */
    char getSection() const noexcept
    {
        return fSection;
    }

    /**
       Whether the stream so far is exactly what the wrappers write for a plugin with no parameters and no states:
       a single NUL byte, that is a lone empty key with no terminator after it.
       Pairs found before it in the initial section are allowed, as they are ignored when applying.
     */
    bool isLoneEmptyKey() const noexcept
    {
        return fSection == 'i' && ! fFillingKey && fKey.empty() && fToken.empty();
    }

private:
    char fSection;
    bool fFillingKey;
    std::string fKey;
    std::string fToken;

    bool enterSection(const char expected1, const char expected2, const char next)
    {
        DAF_SAFE_ASSERT_INT_RETURN(fSection == expected1 || fSection == expected2, fSection, false);
        fSection = next;
        fToken.clear();
        return true;
    }

    bool tokenCompleted()
    {
        if (fFillingKey)
        {
            // special keys
            if (fToken == "__daf_state_begin__")
                return enterSection('i', 'n', 's');
            if (fToken == "__daf_state_end__")
                return enterSection('s', 's', 'n');
            if (fToken == "__daf_parameters_begin__")
                return enterSection('i', 'n', 'p');
            if (fToken == "__daf_parameters_end__")
                return enterSection('p', 'p', 'x');

            // no special key, now read its value
            fKey.swap(fToken);
            fToken.clear();
            fFillingKey = false;
            return true;
        }

        fFillingKey = true;

        const bool isProgram = fKey == "__daf_program__";
        DAF_SAFE_ASSERT_INT_RETURN(!isProgram || fSection == 'i', fSection, false);

        entries.push_back(Entry());
        Entry& entry(entries.back());
        entry.type = fSection;
        entry.key.swap(fKey);
        entry.value.swap(fToken);

        fKey.clear();
        fToken.clear();

        if (isProgram)
            fSection = 'n';

        return true;
    }
};

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

#endif // DAF_PLUGIN_STATE_PARSER_HPP_INCLUDED
