// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Regression test for peer-controlled network time offsets.
//
// A peer's `version` message carries an unbounded int64 timestamp.  The offset
// derived from it is fed straight into AddTimeData(), whose median is clamped
// with abs64().  abs64(INT64_MIN) is undefined behaviour (negating INT64_MIN
// overflows); in practice it returns INT64_MIN, which then passes the
// "<= maxtimeadjustment" clamp and becomes the node's time offset.

#include "netaddress.h"
#include "netbase.h"
#include "timedata.h"
#include "util.h"
#include "utilstrencodings.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

#include <limits>

BOOST_FIXTURE_TEST_SUITE(pepecoin_timedata_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(time_offset_must_stay_clamped)
{
    const int64_t maxAdjustment = GetArg("-maxtimeadjustment", DEFAULT_MAX_TIME_ADJUSTMENT);
    BOOST_TEST_MESSAGE("maxtimeadjustment = " << maxAdjustment);
    BOOST_CHECK_EQUAL(GetTimeOffset(), 0);

    // Four peers on distinct addresses is enough to own the median: the filter
    // is seeded with a single 0 sample and the median is recomputed at size 5.
    for (int i = 1; i <= 4; i++) {
        CNetAddr addr;
        BOOST_REQUIRE(LookupHost(strprintf("203.0.113.%d", i).c_str(), addr, false));
        AddTimeData(addr, std::numeric_limits<int64_t>::min());
    }

    const int64_t offset = GetTimeOffset();
    BOOST_TEST_MESSAGE("resulting nTimeOffset = " << offset);

    // The whole point of the clamp is that hostile peers cannot move our clock
    // further than -maxtimeadjustment .. +maxtimeadjustment.
    BOOST_CHECK_MESSAGE(offset >= -maxAdjustment && offset <= maxAdjustment,
                        "peers moved nTimeOffset to " << offset
                        << ", outside the +/-" << maxAdjustment << " clamp");

    // And GetAdjustedTime() must remain a sane wall-clock value.
    const int64_t adjusted = GetAdjustedTime();
    BOOST_TEST_MESSAGE("GetAdjustedTime() = " << adjusted);
    BOOST_CHECK_MESSAGE(adjusted > 1600000000LL && adjusted < 4000000000LL,
                        "GetAdjustedTime() returned " << adjusted);

    // Restore the process-global time offset so the rest of the test binary is
    // unaffected: enough honest samples to take the median back to zero.
    for (int i = 1; i <= 8; i++) {
        CNetAddr addr;
        BOOST_REQUIRE(LookupHost(strprintf("198.51.100.%d", i).c_str(), addr, false));
        AddTimeData(addr, 0);
    }
    BOOST_CHECK_EQUAL(GetTimeOffset(), 0);
}

BOOST_AUTO_TEST_SUITE_END()
