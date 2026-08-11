// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Adversarial consensus / monetary-integrity regression tests.
//
// These tests attempt to violate the two fundamental monetary invariants:
//   per transaction: sum(outputs) <= sum(inputs)
//   per block:       coinbase outputs <= subsidy(height) + sum(fees)
// and to bypass the surrounding consensus checks (maturity, duplicate inputs,
// amount ranges, subsidy schedule).

#include "chainparams.h"
#include "coins.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "miner.h"
#include "pepecoin.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "script/script.h"
#include "script/sign.h"
#include "test/test_bitcoin.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pepecoin_consensus_tests, TestChain240Setup)

namespace {

//! Re-mine and submit a block, returning true if it became the chain tip.
bool SubmitBlock(CBlock& block)
{
    const CChainParams& chainparams = Params();
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block.nNonce = 0;
    while (!CheckProofOfWork(block.GetPoWHash(), block.nBits, chainparams.GetConsensus(0)))
        ++block.nNonce;
    block.fChecked = false;

    const uint256 hash = block.GetHash();
    std::shared_ptr<const CBlock> shared_pblock = std::make_shared<const CBlock>(block);
    ProcessNewBlock(chainparams, shared_pblock, true, NULL);
    LOCK(cs_main);
    return chainActive.Tip()->GetBlockHash() == hash;
}

//! Build a candidate block on the current tip containing the given transactions.
CBlock BuildBlock(const std::vector<CMutableTransaction>& txns, const CScript& scriptPubKey)
{
    const CChainParams& chainparams = Params();
    std::unique_ptr<CBlockTemplate> tmpl = BlockAssembler(chainparams).CreateNewBlock(scriptPubKey, true);
    CBlock block = tmpl->block;
    block.vtx.resize(1);
    for (const CMutableTransaction& tx : txns)
        block.vtx.push_back(MakeTransactionRef(tx));

    // Put the block height into the coinbase (BIP34 is inactive on regtest but
    // IncrementExtraNonce keeps the coinbase well-formed and unique).
    unsigned int extraNonce = 0;
    {
        LOCK(cs_main);
        IncrementExtraNonce(&block, chainActive.Tip(), extraNonce);
    }
    return block;
}

//! Overwrite the value of the block's single coinbase output.
void SetCoinbaseValue(CBlock& block, CAmount value)
{
    CMutableTransaction cb(*block.vtx[0]);
    BOOST_REQUIRE(cb.vout.size() >= 1);
    cb.vout[0].nValue = value;
    block.vtx[0] = MakeTransactionRef(std::move(cb));
}

CAmount CoinbaseValueOf(const CBlock& block)
{
    CAmount total = 0;
    for (const CTxOut& out : block.vtx[0]->vout) total += out.nValue;
    return total;
}

//! Spend coinbaseTxns[idx] to a single output of `outValue` (fee = subsidy - outValue).
CMutableTransaction SpendCoinbase(const CTransaction& coinbase, const CKey& key,
                                  const CScript& scriptPubKey, CAmount outValue,
                                  int nInputCopies = 1)
{
    CMutableTransaction tx;
    tx.nVersion = 1;
    for (int i = 0; i < nInputCopies; i++) {
        tx.vin.push_back(CTxIn(COutPoint(coinbase.GetHash(), 0), CScript(), 0xffffffff));
    }
    tx.vout.resize(1);
    tx.vout[0].nValue = outValue;
    tx.vout[0].scriptPubKey = scriptPubKey;

    // Sign every input (all reference the same prevout in the duplicate-input case).
    for (size_t i = 0; i < tx.vin.size(); i++) {
        std::vector<unsigned char> vchSig;
        uint256 hash = SignatureHash(coinbase.vout[0].scriptPubKey, tx, i, SIGHASH_ALL, 0, SIGVERSION_BASE);
        BOOST_REQUIRE(key.Sign(hash, vchSig));
        vchSig.push_back((unsigned char)SIGHASH_ALL);
        tx.vin[i].scriptSig = CScript() << vchSig;
    }
    return tx;
}

} // anonymous namespace

/**
 * PEP-INV-1: the coinbase may claim exactly the subsidy, and not one koinu more.
 */
BOOST_AUTO_TEST_CASE(coinbase_may_not_overpay_subsidy)
{
    CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    std::vector<CMutableTransaction> none;

    int height;
    {
        LOCK(cs_main);
        height = chainActive.Height() + 1;
    }
    const CAmount subsidy = GetPepecoinBlockSubsidy(height, Params().GetConsensus(height), uint256());
    BOOST_CHECK(MoneyRange(subsidy));

    // The assembler must not overpay itself.
    {
        CBlock b = BuildBlock(none, spk);
        BOOST_CHECK_EQUAL(CoinbaseValueOf(b), subsidy);
    }

    // subsidy + 1 koinu must be rejected.
    {
        CBlock b = BuildBlock(none, spk);
        SetCoinbaseValue(b, subsidy + 1);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "block paying subsidy+1 was ACCEPTED");
    }

    // subsidy * 2 must be rejected.
    {
        CBlock b = BuildBlock(none, spk);
        SetCoinbaseValue(b, subsidy * 2);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "block paying 2x subsidy was ACCEPTED");
    }

    // MAX_MONEY must be rejected.
    {
        CBlock b = BuildBlock(none, spk);
        SetCoinbaseValue(b, MAX_MONEY);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "block paying MAX_MONEY was ACCEPTED");
    }

    // MAX_MONEY + 1 must be rejected (bad-txns-vout-toolarge).
    {
        CBlock b = BuildBlock(none, spk);
        SetCoinbaseValue(b, MAX_MONEY + 1);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "block paying MAX_MONEY+1 was ACCEPTED");
    }

    // A negative coinbase output must be rejected (bad-txns-vout-negative).
    {
        CBlock b = BuildBlock(none, spk);
        SetCoinbaseValue(b, -1);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "block with negative coinbase was ACCEPTED");
    }

    // Two outputs, each individually in range, summing above the subsidy.
    {
        CBlock b = BuildBlock(none, spk);
        CMutableTransaction cb(*b.vtx[0]);
        cb.vout.resize(2);
        cb.vout[0].nValue = subsidy;
        cb.vout[1].nValue = 1;
        cb.vout[1].scriptPubKey = spk;
        b.vtx[0] = MakeTransactionRef(std::move(cb));
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "split coinbase paying subsidy+1 was ACCEPTED");
    }

    // Exactly the subsidy must still be accepted (the chain must not be stuck).
    {
        CBlock b = BuildBlock(none, spk);
        SetCoinbaseValue(b, subsidy);
        BOOST_CHECK_MESSAGE(SubmitBlock(b), "block paying exactly the subsidy was REJECTED");
    }
}

/**
 * PEP-INV-2: the coinbase may claim subsidy + fees actually paid, and no more.
 */
BOOST_AUTO_TEST_CASE(coinbase_may_not_overpay_fees)
{
    CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    const CAmount spent = coinbaseTxns[0].vout[0].nValue;
    const CAmount fee = 1 * COIN;
    CMutableTransaction spend = SpendCoinbase(coinbaseTxns[0], coinbaseKey, spk, spent - fee);
    std::vector<CMutableTransaction> txns{spend};

    int height;
    {
        LOCK(cs_main);
        height = chainActive.Height() + 1;
    }
    const CAmount subsidy = GetPepecoinBlockSubsidy(height, Params().GetConsensus(height), uint256());

    // subsidy + fee + 1 must be rejected.
    {
        CBlock b = BuildBlock(txns, spk);
        SetCoinbaseValue(b, subsidy + fee + 1);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "coinbase claiming fee+1 was ACCEPTED");
    }

    // Claiming a fee that was never paid must be rejected.
    {
        CBlock b = BuildBlock(std::vector<CMutableTransaction>(), spk);
        SetCoinbaseValue(b, subsidy + fee);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "coinbase claiming a nonexistent fee was ACCEPTED");
    }

    // subsidy + fee exactly must be accepted.
    {
        CBlock b = BuildBlock(txns, spk);
        SetCoinbaseValue(b, subsidy + fee);
        BOOST_CHECK_MESSAGE(SubmitBlock(b), "coinbase claiming exactly subsidy+fee was REJECTED");
    }
}

/**
 * PEP-INV-3: a transaction may not spend the same outpoint twice (CVE-2018-17144).
 */
BOOST_AUTO_TEST_CASE(duplicate_inputs_rejected_in_block)
{
    CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    const CAmount spent = coinbaseTxns[1].vout[0].nValue;

    // Two inputs, both spending the same coinbase output; outputs = 2x the value.
    CMutableTransaction dup = SpendCoinbase(coinbaseTxns[1], coinbaseKey, spk, spent * 2, 2);

    CValidationState state;
    BOOST_CHECK_MESSAGE(!CheckTransaction(CTransaction(dup), state, true),
                        "CheckTransaction accepted duplicate inputs");
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputs-duplicate");

    // And the default argument must also enable the check (CVE-2018-17144 shape).
    CValidationState state2;
    BOOST_CHECK_MESSAGE(!CheckTransaction(CTransaction(dup), state2),
                        "CheckTransaction default arg skipped the duplicate-input check");

    std::vector<CMutableTransaction> txns{dup};
    CBlock b = BuildBlock(txns, spk);
    BOOST_CHECK_MESSAGE(!SubmitBlock(b), "block with duplicate-input tx was ACCEPTED");
}

/**
 * PEP-INV-4: immature coinbases cannot be spent, and nCoinbaseMaturity is keyed
 * on the height of the coin being spent.
 */
BOOST_AUTO_TEST_CASE(coinbase_maturity_enforced)
{
    CScript spk = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;

    // The most recent coinbase is only a few blocks deep -> immature.
    const CTransaction& fresh = coinbaseTxns.back();
    CMutableTransaction spend = SpendCoinbase(fresh, coinbaseKey, spk, fresh.vout[0].nValue - COIN);

    std::vector<CMutableTransaction> txns{spend};
    CBlock b = BuildBlock(txns, spk);
    BOOST_CHECK_MESSAGE(!SubmitBlock(b), "spend of immature coinbase was ACCEPTED");

    // Sanity: the regtest maturity really is what chainparams says.
    int tip;
    {
        LOCK(cs_main);
        tip = chainActive.Height();
    }
    BOOST_CHECK_EQUAL(Params().GetConsensus(tip).nCoinbaseMaturity, 60u);
}

/**
 * PEP-INV-5: Consensus::CheckTxInputs amount-range enforcement, exercised
 * directly against a hand-built UTXO view containing adversarial values.
 */
BOOST_AUTO_TEST_CASE(checktxinputs_amount_ranges)
{
    const CChainParams& params = Params();
    CCoinsView dummy;
    CCoinsViewCache view(&dummy);

    // Craft a funding "transaction" with two MAX_MONEY outputs directly in the view.
    CMutableTransaction funding;
    funding.vin.resize(1);
    funding.vin[0].prevout.SetNull();
    funding.vin[0].scriptSig = CScript() << OP_1 << OP_1;
    funding.vout.resize(2);
    funding.vout[0].nValue = MAX_MONEY;
    funding.vout[1].nValue = MAX_MONEY;
    const uint256 fundHash = CTransaction(funding).GetHash();
    {
        CCoinsModifier c = view.ModifyNewCoins(fundHash, false);
        c->FromTx(CTransaction(funding), 1);
        c->fCoinBase = false;
    }

    // Spending both MAX_MONEY outputs overflows nValueIn past MAX_MONEY.
    CMutableTransaction tx;
    tx.vin.resize(2);
    tx.vin[0].prevout = COutPoint(fundHash, 0);
    tx.vin[1].prevout = COutPoint(fundHash, 1);
    tx.vout.resize(1);
    tx.vout[0].nValue = MAX_MONEY;

    CValidationState state;
    BOOST_CHECK_MESSAGE(!Consensus::CheckTxInputs(params, CTransaction(tx), state, view, 1000),
                        "CheckTxInputs accepted inputs summing above MAX_MONEY");
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-inputvalues-outofrange");

    // Spending a single MAX_MONEY output for MAX_MONEY is legal (fee 0).
    CMutableTransaction ok;
    ok.vin.resize(1);
    ok.vin[0].prevout = COutPoint(fundHash, 0);
    ok.vout.resize(1);
    ok.vout[0].nValue = MAX_MONEY;
    CValidationState state2;
    BOOST_CHECK(Consensus::CheckTxInputs(params, CTransaction(ok), state2, view, 1000));

    // Outputs above inputs must be rejected.  Fund a small coin so that the
    // output total stays inside MoneyRange while exceeding the input value.
    CMutableTransaction small;
    small.vin.resize(1);
    small.vin[0].prevout.SetNull();
    small.vin[0].scriptSig = CScript() << OP_1 << OP_1;
    small.vout.resize(1);
    small.vout[0].nValue = 1000 * COIN;
    const uint256 smallHash = CTransaction(small).GetHash();
    {
        CCoinsModifier c = view.ModifyNewCoins(smallHash, false);
        c->FromTx(CTransaction(small), 1);
        c->fCoinBase = false;
    }

    CMutableTransaction over;
    over.vin.resize(1);
    over.vin[0].prevout = COutPoint(smallHash, 0);
    over.vout.resize(1);
    over.vout[0].nValue = 1001 * COIN;
    CValidationState state3;
    BOOST_CHECK_MESSAGE(!Consensus::CheckTxInputs(params, CTransaction(over), state3, view, 1000),
                        "CheckTxInputs accepted outputs above inputs");
    BOOST_CHECK_EQUAL(state3.GetRejectReason(), "bad-txns-in-belowout");

    // Consensus::CheckTxInputs calls tx.GetValueOut() unguarded, so a transaction
    // whose outputs are out of range makes it THROW rather than return false.
    // Every consensus caller must therefore run CheckTransaction first.
    CMutableTransaction throws;
    throws.vin.resize(1);
    throws.vin[0].prevout = COutPoint(fundHash, 0);
    throws.vout.resize(2);
    throws.vout[0].nValue = MAX_MONEY;
    throws.vout[1].nValue = MAX_MONEY;
    CValidationState state4;
    BOOST_CHECK_THROW(Consensus::CheckTxInputs(params, CTransaction(throws), state4, view, 1000),
                      std::runtime_error);
}

/**
 * PEP-INV-6: CTransaction::GetValueOut throws rather than returning a wrapped
 * value.  Document exactly which inputs make it throw, because ConnectBlock
 * calls it on the coinbase outside of any try/catch.
 */
BOOST_AUTO_TEST_CASE(getvalueout_overflow_throws)
{
    CMutableTransaction tx;
    tx.vin.resize(1);
    tx.vout.resize(2);
    tx.vout[0].nValue = MAX_MONEY;
    tx.vout[1].nValue = MAX_MONEY;
    BOOST_CHECK_THROW(CTransaction(tx).GetValueOut(), std::runtime_error);

    CMutableTransaction neg;
    neg.vin.resize(1);
    neg.vout.resize(1);
    neg.vout[0].nValue = -1;
    BOOST_CHECK_THROW(CTransaction(neg).GetValueOut(), std::runtime_error);

    // CheckTransaction must reject both before any consensus code calls GetValueOut.
    CValidationState s1, s2;
    BOOST_CHECK(!CheckTransaction(CTransaction(tx), s1));
    BOOST_CHECK_EQUAL(s1.GetRejectReason(), "bad-txns-txouttotal-toolarge");
    BOOST_CHECK(!CheckTransaction(CTransaction(neg), s2));
    BOOST_CHECK_EQUAL(s2.GetRejectReason(), "bad-txns-vout-negative");
}

/**
 * PEP-INV-7: the block-fee accumulator in ConnectBlock has no MoneyRange guard.
 * This test documents the arithmetic head-room rather than asserting a bug: it
 * fails if a future change makes the accumulator reachable with fewer than the
 * number of transactions a block can hold.
 */
BOOST_AUTO_TEST_CASE(block_fee_accumulator_headroom)
{
    // Consensus::CheckTxInputs bounds a single transaction's fee by MAX_MONEY.
    const CAmount maxFeePerTx = MAX_MONEY;

    // int64 overflow of the ConnectBlock accumulator needs this many such txs:
    const int64_t txsToOverflow = (std::numeric_limits<int64_t>::max() / maxFeePerTx) + 1;

    // A 1 MB block can hold far more transactions than that, so the accumulator
    // is bounded only by how much money exists, not by any consensus rule.
    const int64_t minTxSerializedSize = 61; // 4 ver + 1 vin count + 36 outpoint + 1 script + 4 seq + 1 vout count + 8 value + 1 spk + 4 locktime
    const int64_t maxTxsPerBlock = MAX_BLOCK_BASE_SIZE / minTxSerializedSize;

    BOOST_TEST_MESSAGE("txs needed to overflow nFees: " << txsToOverflow
                       << ", max txs per block: " << maxTxsPerBlock);
    BOOST_CHECK_MESSAGE(txsToOverflow < maxTxsPerBlock,
                        "block size no longer bounds the fee accumulator - re-audit");

    // Total emission after the six halvings, in koinu.  Computed in 128 bits
    // because it does NOT fit in the int64_t that CAmount is.
    const int64_t interval = 100000;
    __int128 emission = 0;
    for (int era = 0; era < 6; era++)
        emission += (__int128)interval * ((500000 * COIN) >> era);
    const double emissionD = (double)emission;
    BOOST_TEST_MESSAGE("emission after 6 halvings (koinu): " << emissionD
                       << " MAX_MONEY: " << MAX_MONEY
                       << " INT64_MAX: " << std::numeric_limits<int64_t>::max());

    // The eventual money supply exceeds MAX_MONEY by a wide margin ...
    BOOST_CHECK(emission > (__int128)MAX_MONEY);
    // ... and, notably, also exceeds INT64_MAX, so any code that sums the whole
    // supply into a CAmount overflows.  Consensus never does this, but
    // gettxoutsetinfo does.
    BOOST_CHECK(emission > (__int128)std::numeric_limits<int64_t>::max());
}

/**
 * PEP-INV-8: subsidy schedule boundaries and shift safety.
 */
BOOST_AUTO_TEST_CASE(subsidy_schedule_boundaries)
{
    const CChainParams& main = Params(CBaseChainParams::MAIN);
    const uint256 prevHash = uint256S("0x1234567890abcdef1234567890abcdef1234567890abcdef1234567890abcdef");

    struct { int height; CAmount expected; } cases[] = {
        {0,       500000 * COIN},
        {1,       500000 * COIN},
        {99999,   500000 * COIN},
        {100000,  250000 * COIN},
        {199999,  250000 * COIN},
        {200000,  125000 * COIN},
        {499999,   31250 * COIN},
        {500000,   15625 * COIN},
        {599999,   15625 * COIN},
        {600000,   10000 * COIN},
        {600001,   10000 * COIN},
        {100000000, 10000 * COIN},
    };
    for (const auto& c : cases) {
        CAmount s = GetPepecoinBlockSubsidy(c.height, main.GetConsensus(c.height), prevHash);
        BOOST_CHECK_EQUAL(s, c.expected);
        BOOST_CHECK(MoneyRange(s));
    }

    // Every reachable consensus params object must use simplified rewards, or the
    // dead legacy branch (strtol + mersenne twister on the previous block hash)
    // becomes live consensus code.
    for (const std::string& net : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET, CBaseChainParams::REGTEST}) {
        const CChainParams& p = Params(net);
        for (int h : {0, 1, 9, 10, 11, 19, 20, 21, 149, 150, 999, 1000, 1001, 1249, 1250,
                      41999, 42000, 42001, 600000, 100000000}) {
            BOOST_CHECK_MESSAGE(p.GetConsensus(h).fSimplifiedRewards,
                                "fSimplifiedRewards false on " << net << " at height " << h);
        }
    }
}

/**
 * PEP-INV-9: the height-indexed consensus parameter tree resolves correctly at
 * every branch boundary on every network.
 */
BOOST_AUTO_TEST_CASE(consensus_param_tree_boundaries)
{
    const CChainParams& main = Params(CBaseChainParams::MAIN);
    BOOST_CHECK_EQUAL(main.GetConsensus(0).nHeightEffective, 0u);
    BOOST_CHECK_EQUAL(main.GetConsensus(999).nHeightEffective, 0u);
    BOOST_CHECK_EQUAL(main.GetConsensus(1000).nHeightEffective, 1000u);
    BOOST_CHECK_EQUAL(main.GetConsensus(41999).nHeightEffective, 1000u);
    BOOST_CHECK_EQUAL(main.GetConsensus(42000).nHeightEffective, 42000u);
    BOOST_CHECK_EQUAL(main.GetConsensus(1000000).nHeightEffective, 42000u);

    // Coinbase maturity really does change at 1000 on mainnet.
    BOOST_CHECK_EQUAL(main.GetConsensus(999).nCoinbaseMaturity, 30u);
    BOOST_CHECK_EQUAL(main.GetConsensus(1000).nCoinbaseMaturity, 240u);

    // Legacy blocks are allowed below the auxpow height and banned at/above it.
    BOOST_CHECK(main.GetConsensus(41999).fAllowLegacyBlocks);
    BOOST_CHECK(!main.GetConsensus(42000).fAllowLegacyBlocks);

    // A negative height passed as int truncates to a huge uint32_t: document what
    // the tree returns so callers that do this are known.
    const Consensus::Params& negative = main.GetConsensus((uint32_t)(-1));
    BOOST_CHECK_EQUAL(negative.nHeightEffective, 42000u);

    const CChainParams& reg = Params(CBaseChainParams::REGTEST);
    BOOST_CHECK_EQUAL(reg.GetConsensus(9).nHeightEffective, 0u);
    BOOST_CHECK_EQUAL(reg.GetConsensus(10).nHeightEffective, 10u);
    BOOST_CHECK_EQUAL(reg.GetConsensus(19).nHeightEffective, 10u);
    BOOST_CHECK_EQUAL(reg.GetConsensus(20).nHeightEffective, 20u);

    // Regtest never assigns fPowAllowDigishieldMinDifficultyBlocks; because the
    // CChainParams instances have static storage duration it is zero-initialised
    // rather than indeterminate.  Pin that down so a refactor to a heap/stack
    // instance cannot silently make consensus depend on uninitialised memory.
    BOOST_CHECK(!reg.GetConsensus(0).fPowAllowDigishieldMinDifficultyBlocks);
    BOOST_CHECK(!reg.GetConsensus(10).fPowAllowDigishieldMinDifficultyBlocks);
    BOOST_CHECK(!reg.GetConsensus(20).fPowAllowDigishieldMinDifficultyBlocks);
}

BOOST_AUTO_TEST_SUITE_END()
