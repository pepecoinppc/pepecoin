// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// PEP-001 IMPACT proof (deterministic, no network):
//   Show that when the network-adjusted time is poisoned to INT64_MIN (the state
//   the abs64 bug produces), an UNMODIFIED node can neither PRODUCE a block nor
//   ACCEPT the honest tip -- both go through
//   ContextualCheckBlockHeader(..., GetAdjustedTime()):
//     * mining:          CreateNewBlock -> TestBlockValidity -> ContextualCheckBlockHeader
//     * accepting peers: AcceptBlockHeader              -> ContextualCheckBlockHeader
//   We call ContextualCheckBlockHeader directly with the poisoned nAdjustedTime
//   argument, on a genuinely-valid next-block header, and show it flips from
//   accepted to "time-too-new".

#include "chainparams.h"
#include "consensus/validation.h"
#include "miner.h"
#include "pubkey.h"
#include "script/script.h"
#include "test/test_bitcoin.h"
#include "timedata.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

#include <limits>

BOOST_FIXTURE_TEST_SUITE(pepecoin_timewarp_impact_tests, TestChain240Setup)

// A poisoned network-adjusted time (INT64_MIN) makes a valid next-block header
// fail the "time-too-new" gate -- the exact gate the mining path and the
// peer-acceptance path both traverse.
BOOST_AUTO_TEST_CASE(poisoned_adjusted_time_rejects_valid_block)
{
    const CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // Build a genuinely valid next block via the real miner. CreateNewBlock
    // itself runs TestBlockValidity with the REAL (un-poisoned) GetAdjustedTime,
    // so this only succeeds because the honest clock is fine.
    std::unique_ptr<CBlockTemplate> tmpl =
        BlockAssembler(Params()).CreateNewBlock(spk, true);
    BOOST_REQUIRE(tmpl);
    const CBlock& block = tmpl->block;

    CBlockIndex* prev;
    { LOCK(cs_main); prev = chainActive.Tip(); }

    const int64_t honestNow = GetAdjustedTime();
    const int64_t blockTime = block.GetBlockTime();
    BOOST_TEST_MESSAGE("block timestamp = " << blockTime
                       << " (a normal, ~current value); honest adjusted time = " << honestNow);

    // CONTROL: with the honest adjusted time, the header is accepted.
    {
        CValidationState state;
        bool ok = ContextualCheckBlockHeader(block, state, prev, honestNow);
        BOOST_CHECK_MESSAGE(ok, "valid next-block header rejected under honest clock: "
                                 << state.GetRejectReason());
    }

    // POISONED: with nAdjustedTime = INT64_MIN (what abs64(INT64_MIN) leaves in
    // nTimeOffset), the SAME valid header is rejected as time-too-new.
    {
        CValidationState state;
        bool ok = ContextualCheckBlockHeader(block, state, prev,
                                             std::numeric_limits<int64_t>::min());
        BOOST_CHECK_MESSAGE(!ok, "poisoned adjusted time still ACCEPTED the block");
        BOOST_CHECK_EQUAL(state.GetRejectReason(), "time-too-new");
    }

    // The rejection is purely from the poisoned reference time: the block's own
    // timestamp is a normal value that the honest clock accepted. Since
    //   AcceptBlockHeader (peer blocks)  and
    //   TestBlockValidity (the miner's own candidate, via CreateNewBlock)
    // both call ContextualCheckBlockHeader(..., GetAdjustedTime()), a node whose
    // GetAdjustedTime() == INT64_MIN can neither accept nor produce this block.
}

// The freeze is not specific to one block: under a poisoned reference time the
// "time-too-new" acceptance ceiling is INT64_MIN + 7200, so EVERY realistic unix
// timestamp exceeds it by ~9.2e18 and is rejected. (Arithmetic form, so it does
// not depend on nBits/median-time preconditions of a rebuilt header.)
BOOST_AUTO_TEST_CASE(poisoned_time_ceiling_excludes_all_real_timestamps)
{
    const int64_t poisoned = std::numeric_limits<int64_t>::min();
    const int64_t ceiling = poisoned + 2 * 60 * 60;   // block.GetBlockTime() must be <= this
    BOOST_TEST_MESSAGE("poisoned acceptance ceiling = " << ceiling);

    // uint32 block timestamps span [0, 4294967295]; every one is far above the
    // poisoned ceiling, so the future-time gate rejects all of them.
    for (int64_t t : {0LL, 1000000000LL, 1700000000LL, 2100000000LL, 4294967295LL}) {
        BOOST_CHECK_MESSAGE(t > ceiling,
                            "timestamp " << t << " is somehow <= the poisoned ceiling");
    }
    // (The gap is ~9.2e18 seconds; computing it here would itself overflow int64,
    // which is precisely the class of bug that created the poison in the first place.)
}

BOOST_AUTO_TEST_SUITE_END()
