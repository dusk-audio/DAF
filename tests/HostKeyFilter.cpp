/*
 * DAF - Dusk Audio Framework
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

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include "dgl/src/HostKeyFilter.hpp"

// --------------------------------------------------------------------------------------------------------------------

int main()
{
    using DGL_NAMESPACE::HostKeyFilter;

    bool declined = false;

    // nothing offered: native keys and characters pass
    {
        HostKeyFilter f;
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 100, declined), false, "a key nobody offered passes");
        DAF_ASSERT_EQUAL(declined, false, "and is not reported declined, the UI decides about it");
        DAF_ASSERT_EQUAL(f.dropNativeText(), false, "its character passes");
        DAF_ASSERT_EQUAL(f.dropNativeKey(false, 110, declined), false, "its release passes");
    }

    // a printable key the host offered, declined and then dispatched natively: both copies drop
    {
        HostKeyFilter f;
        f.keyOffered(true, 200, true);
        f.keyUsed(false);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 200, declined), true, "the native copy of the offered press drops");
        DAF_ASSERT_EQUAL(declined, true, "the UI declined it, so it goes on to the host");
        DAF_ASSERT_EQUAL(f.dropNativeText(), true, "and so does its character, delivered already");
        DAF_ASSERT_EQUAL(f.dropNativeText(), false, "only one character drops");

        f.keyOffered(false, 260, false);
        DAF_ASSERT_EQUAL(f.dropNativeKey(false, 260, declined), true, "the native copy of the offered release drops");
        DAF_ASSERT_EQUAL(f.dropNativeText(), false, "a release drops no character");
    }

    // a key the format call gave no character (Enter, Ctrl+key): the native character is kept
    {
        HostKeyFilter f;
        f.keyOffered(true, 300, false);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 300, declined), true, "the native copy drops");
        DAF_ASSERT_EQUAL(f.dropNativeText(), false, "its character is the only one the UI gets, so it passes");
    }

    // a key the UI used drops without going to the host
    {
        HostKeyFilter f;
        f.keyOffered(true, 350, true);
        f.keyUsed(true);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 350, declined), true, "the native copy of a used key drops");
        DAF_ASSERT_EQUAL(declined, false, "and stays away from the host");
        DAF_ASSERT_EQUAL(f.dropNativeText(), true, "its character drops too");

        f.keyOffered(true, 360, true);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 360, declined), true, "a new offer starts as not used");
        DAF_ASSERT_EQUAL(declined, true, "so an unanswered offer counts as declined");
    }

    // a record matches once only
    {
        HostKeyFilter f;
        f.keyOffered(true, 400, true);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 400, declined), true, "the first copy drops");
        DAF_ASSERT_EQUAL(f.dropNativeText(), true, "with its character");
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 400, declined), false, "a second native key at that time passes");
    }

    // a different native key does not match, and clears the record
    {
        HostKeyFilter f;
        f.keyOffered(true, 500, true);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 510, declined), false, "a later key passes");
        DAF_ASSERT_EQUAL(f.dropNativeText(), false, "its character passes");
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 500, declined), false, "the stale record no longer matches");

        f.keyOffered(true, 600, true);
        DAF_ASSERT_EQUAL(f.dropNativeKey(false, 600, declined), false, "a release does not match an offered press");
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 600, declined), false, "and the record went with it");
    }

    // a host that took the key does not dispatch it, and the next key is unaffected
    {
        HostKeyFilter f;
        f.keyOffered(true, 700, true);
        f.keyOffered(false, 720, false);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 800, declined), false, "an unrelated native key passes");
        DAF_ASSERT_EQUAL(f.dropNativeText(), false, "with its character");
    }

    // a new offer cancels a character still waiting to drop
    {
        HostKeyFilter f;
        f.keyOffered(true, 900, true);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 900, declined), true, "the native copy drops");
        f.keyOffered(true, 950, true);
        DAF_ASSERT_EQUAL(f.dropNativeText(), false, "the next offer starts afresh");
    }

    // message times wrap around, the filter only compares them
    {
        HostKeyFilter f;
        f.keyOffered(true, 0xffffffffU, true);
        DAF_ASSERT_EQUAL(f.dropNativeKey(true, 0xffffffffU, declined), true, "a time at the top of the range matches");
    }

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
