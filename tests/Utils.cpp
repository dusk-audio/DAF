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

/* Helpers from daf/DafUtils.hpp. The tests build with -ffast-math, as plugins do, which is the case d_isFinite
 * exists for: there std::isfinite is folded to true. The values are parsed at run time so that the compiler
 * cannot see them either. */

#define DAF_TEST_NO_DGL
#include "tests.hpp"

#include <cstdlib>

START_NAMESPACE_DAF

// --------------------------------------------------------------------------------------------------------------------

static int runTests()
{
    const char* const notFinite[] = { "nan", "-nan", "inf", "-inf" };
    for (const char* const text : notFinite)
    {
        DAF_ASSERT_EQUAL(d_isFinite(std::strtod(text, nullptr)), false, text);
        DAF_ASSERT_EQUAL(d_isFinite(std::strtof(text, nullptr)), false, text);
    }

    // finite, zero, subnormal and largest values included
    const char* const finiteDouble[] = { "0", "-0", "1.5", "-1e-310", "4.9e-324", "1.7976931348623157e308" };
    for (const char* const text : finiteDouble)
        DAF_ASSERT_EQUAL(d_isFinite(std::strtod(text, nullptr)), true, text);

    const char* const finiteFloat[] = { "0", "-0", "1.5", "-1e-40", "1.4e-45", "3.4028234e38" };
    for (const char* const text : finiteFloat)
        DAF_ASSERT_EQUAL(d_isFinite(std::strtof(text, nullptr)), true, text);

    // text beyond the range of float parses as infinity
    DAF_ASSERT_EQUAL(d_isFinite(std::strtof("1e39", nullptr)), false, "1e39 as a float");

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------

END_NAMESPACE_DAF

int main()
{
    return DAF_NAMESPACE::runTests();
}
