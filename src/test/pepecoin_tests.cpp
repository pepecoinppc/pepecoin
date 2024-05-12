// Copyright (c) 2015-2021 The Dogecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "arith_uint256.h"
#include "chainparams.h"
#include "pepecoin.h"
#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pepecoin_tests, TestingSetup)

/**
 * the maximum block reward at a given height for a block without fees
 */
uint64_t expectedMaxSubsidy(int height) {
    if (height < 100000) {
        return 1000000 * COIN;
    } else if (height < 145000) {//PEPE TODO Magic number
        return 500000 * COIN;
    } else if (height < 200000) {
        return 250000 * COIN;
    } else if (height < 300000) {
        return 125000 * COIN;
    } else if (height < 400000) {
        return  62500 * COIN;
    } else if (height < 500000) {
        return  31250 * COIN;
    } else if (height < 600000) {
        return  15625 * COIN;
    } else {
        return  10000 * COIN;
    }
}

/**
 * the minimum possible value for the maximum block reward at a given height
 * for a block without fees
 */
uint64_t expectedMinSubsidy(int height) {
    if (height < 100000) {
        return 0;
    } else if (height < 145000) {//PEPE TODO Magic number
        return 0;
    } else if (height < 200000) {
        return 250000 * COIN;
    } else if (height < 300000) {
        return 125000 * COIN;
    } else if (height < 400000) {
        return  62500 * COIN;
    } else if (height < 500000) {
        return  31250 * COIN;
    } else if (height < 600000) {
        return  15625 * COIN;
    } else {
        return  10000 * COIN;
    }
}

// Check the simplified rewards after block 145,000
BOOST_AUTO_TEST_CASE(subsidy_test)
{
    const CChainParams& mainParams = Params(CBaseChainParams::MAIN);
    const uint256 prevHash = uint256S("0");

    for (int nHeight = 0; nHeight < 600000; nHeight++) {
        const Consensus::Params& params = mainParams.GetConsensus(nHeight);
        CAmount nSubsidy = GetPepecoinBlockSubsidy(nHeight, params, prevHash);
        CAmount nExpectedSubsidy = (500000 >> (nHeight / 100000)) * COIN;
        BOOST_CHECK(MoneyRange(nSubsidy));
        BOOST_CHECK_EQUAL(nSubsidy, nExpectedSubsidy);
    }

    // Test reward at 600k+ is constant
    CAmount nConstantSubsidy = GetPepecoinBlockSubsidy(600000, mainParams.GetConsensus(600000), prevHash);
    BOOST_CHECK_EQUAL(nConstantSubsidy, 10000 * COIN);

    nConstantSubsidy = GetPepecoinBlockSubsidy(700000, mainParams.GetConsensus(700000), prevHash);
    BOOST_CHECK_EQUAL(nConstantSubsidy, 10000 * COIN);
}

BOOST_AUTO_TEST_CASE(get_next_work_difficulty_limit)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(0);

    CBlockIndex pindexLast;
    int64_t nLastRetargetTime = 1386474927; // Block # 1
    
    pindexLast.nHeight = 239;
    pindexLast.nTime = 1386475638; // Block #239
    pindexLast.nBits = 0x1e0ffff0;
    BOOST_CHECK_EQUAL(CalculatePepecoinNextWorkRequired(&pindexLast, nLastRetargetTime, params), 0x1e0fffff);
}

BOOST_AUTO_TEST_CASE(get_next_work_pre_digishield)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(0);
    
    CBlockIndex pindexLast;
    int64_t nLastRetargetTime = 1386942008; // Block 9359

    pindexLast.nHeight = 9599;
    pindexLast.nTime = 1386954113;
    pindexLast.nBits = 0x1c1a1206;
    BOOST_CHECK_EQUAL(CalculatePepecoinNextWorkRequired(&pindexLast, nLastRetargetTime, params), 0x1c271b09);
}

BOOST_AUTO_TEST_CASE(get_next_work_digishield)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(1000);
    
    CBlockIndex pindexLast;
    int64_t nLastRetargetTime = 1395094427;

    // First hard-fork at 145,000, which applies to block 145,001 onwards
    pindexLast.nHeight = 145000;//PEPE TODO Magic number
    pindexLast.nTime = 1395094679;
    pindexLast.nBits = 0x1b499dfd;
    BOOST_CHECK_EQUAL(CalculatePepecoinNextWorkRequired(&pindexLast, nLastRetargetTime, params), 0x1b671062);
}

BOOST_AUTO_TEST_CASE(get_next_work_digishield_modulated_upper)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(1000);//PEPE TODO Magic number
    
    CBlockIndex pindexLast;
    int64_t nLastRetargetTime = 1395100835;

    // Test the upper bound on modulated time using mainnet block #145,107
    pindexLast.nHeight = 145107;
    pindexLast.nTime = 1395101360;
    pindexLast.nBits = 0x1b3439cd;
    BOOST_CHECK_EQUAL(CalculatePepecoinNextWorkRequired(&pindexLast, nLastRetargetTime, params), 0x1b4e56b3);
}

BOOST_AUTO_TEST_CASE(get_next_work_digishield_modulated_lower)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(1000);//PEPE TODO Magic number
    
    CBlockIndex pindexLast;
    int64_t nLastRetargetTime = 1395380517;

    // Test the lower bound on modulated time using mainnet block #149,423
    pindexLast.nHeight = 149423;
    pindexLast.nTime = 1395380447;
    pindexLast.nBits = 0x1b446f21;
    BOOST_CHECK_EQUAL(CalculatePepecoinNextWorkRequired(&pindexLast, nLastRetargetTime, params), 0x1b335358);
}

BOOST_AUTO_TEST_CASE(get_next_work_digishield_rounding)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(1000);//PEPE TODO Magic number
    
    CBlockIndex pindexLast;
    int64_t nLastRetargetTime = 1395094679;

    // Test case for correct rounding of modulated time - this depends on
    // handling of integer division, and is not obvious from the code
    pindexLast.nHeight = 145001;
    pindexLast.nTime = 1395094727;
    pindexLast.nBits = 0x1b671062;
    BOOST_CHECK_EQUAL(CalculatePepecoinNextWorkRequired(&pindexLast, nLastRetargetTime, params), 0x1b6558a4);
}

BOOST_AUTO_TEST_CASE(hardfork_parameters)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& initialParams = Params().GetConsensus(0);

    BOOST_CHECK_EQUAL(initialParams.nPowTargetTimespan, 60);
    BOOST_CHECK_EQUAL(initialParams.fAllowLegacyBlocks, true);
    BOOST_CHECK_EQUAL(initialParams.fDigishieldDifficultyCalculation, true);

    const Consensus::Params& initialParamsEnd = Params().GetConsensus(999);
    BOOST_CHECK_EQUAL(initialParamsEnd.nPowTargetTimespan, 60);
    BOOST_CHECK_EQUAL(initialParamsEnd.fAllowLegacyBlocks, true);
    BOOST_CHECK_EQUAL(initialParamsEnd.fDigishieldDifficultyCalculation, true);

    const Consensus::Params& digishieldParams = Params().GetConsensus(1000);//PEPE TODO Magic number
    BOOST_CHECK_EQUAL(digishieldParams.nPowTargetTimespan, 60);
    BOOST_CHECK_EQUAL(digishieldParams.fAllowLegacyBlocks, true);
    BOOST_CHECK_EQUAL(digishieldParams.fDigishieldDifficultyCalculation, true);

    const Consensus::Params& digishieldParamsEnd = Params().GetConsensus(41999);
    BOOST_CHECK_EQUAL(digishieldParamsEnd.nPowTargetTimespan, 60);
    BOOST_CHECK_EQUAL(digishieldParamsEnd.fAllowLegacyBlocks, true);
    BOOST_CHECK_EQUAL(digishieldParamsEnd.fDigishieldDifficultyCalculation, true);

    const Consensus::Params& auxpowParams = Params().GetConsensus(42000);//PEPE TODO Magic number
    BOOST_CHECK_EQUAL(auxpowParams.nHeightEffective, 42000);//PEPE TODO Magic number
    BOOST_CHECK_EQUAL(auxpowParams.nPowTargetTimespan, 60);
    BOOST_CHECK_EQUAL(auxpowParams.fAllowLegacyBlocks, false);
    BOOST_CHECK_EQUAL(auxpowParams.fDigishieldDifficultyCalculation, true);

    const Consensus::Params& auxpowHighParams = Params().GetConsensus(700000); // Arbitrary point after last hard-fork
    BOOST_CHECK_EQUAL(auxpowHighParams.nPowTargetTimespan, 60);
    BOOST_CHECK_EQUAL(auxpowHighParams.fAllowLegacyBlocks, false);
    BOOST_CHECK_EQUAL(auxpowHighParams.fDigishieldDifficultyCalculation, true);
}

BOOST_AUTO_TEST_SUITE_END()
