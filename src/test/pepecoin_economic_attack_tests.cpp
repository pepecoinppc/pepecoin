// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Economic-destruction attack suite.  Unlike pepecoin_consensus_tests.cpp
// (which checks individual invariants), this file constructs whole adversarial
// BLOCKS and feeds them through the REAL validation pipeline
// (ProcessNewBlock -> AcceptBlock -> ConnectBlock) on a regtest chain, then
// asserts that (a) the block was rejected and (b) the money supply / UTXO set
// is unchanged.  The goal is to actually mint or duplicate PEPE and prove it
// cannot be done.

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
#include "txmempool.h"
#include "utilmoneystr.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pepecoin_economic_attack_tests, TestChain240Setup)

namespace {

int TipHeight()
{
    LOCK(cs_main);
    return chainActive.Height();
}

uint256 TipHash()
{
    LOCK(cs_main);
    return chainActive.Tip()->GetBlockHash();
}

//! Sum of the entire UTXO set value — the money supply.
CAmount TotalSupplyOf(CCoinsView* view)
{
    LOCK(cs_main);
    FlushStateToDisk();
    CAmount total = 0;
    std::unique_ptr<CCoinsViewCursor> pcursor(view->Cursor());
    while (pcursor->Valid()) {
        uint256 key;
        CCoins coins;
        if (pcursor->GetKey(key) && pcursor->GetValue(coins)) {
            for (const CTxOut& out : coins.vout)
                if (!out.IsNull()) total += out.nValue;
        }
        pcursor->Next();
    }
    return total;
}

//! Re-mine `block` and submit it through the real pipeline.
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
    return TipHash() == hash;
}

CBlock BuildBlock(const std::vector<CMutableTransaction>& txns, const CScript& spk)
{
    const CChainParams& chainparams = Params();
    std::unique_ptr<CBlockTemplate> tmpl = BlockAssembler(chainparams).CreateNewBlock(spk, true);
    CBlock block = tmpl->block;
    block.vtx.resize(1);
    for (const CMutableTransaction& tx : txns)
        block.vtx.push_back(MakeTransactionRef(tx));
    unsigned int extraNonce = 0;
    {
        LOCK(cs_main);
        IncrementExtraNonce(&block, chainActive.Tip(), extraNonce);
    }
    return block;
}

void SetCoinbase(CBlock& block, const std::vector<CAmount>& values, const CScript& spk)
{
    CMutableTransaction cb(*block.vtx[0]);
    cb.vout.clear();
    for (CAmount v : values) cb.vout.push_back(CTxOut(v, spk));
    block.vtx[0] = MakeTransactionRef(std::move(cb));
}

//! Build a signed tx spending vout 0 of each `parent` for `outValue` total in
//! one output.  nInputCopies>1 duplicates the FIRST parent's outpoint.
CMutableTransaction Spend(const std::vector<CTransaction>& parents, const CKey& key,
                          const CScript& spk, CAmount outValue, int firstInputCopies = 1)
{
    CMutableTransaction tx;
    tx.nVersion = 1;
    for (int c = 0; c < firstInputCopies; c++)
        tx.vin.push_back(CTxIn(COutPoint(parents[0].GetHash(), 0)));
    for (size_t i = 1; i < parents.size(); i++)
        tx.vin.push_back(CTxIn(COutPoint(parents[i].GetHash(), 0)));
    tx.vout.resize(1);
    tx.vout[0].nValue = outValue;
    tx.vout[0].scriptPubKey = spk;
    for (size_t i = 0; i < tx.vin.size(); i++) {
        const CTransaction& p = (i < (size_t)firstInputCopies) ? parents[0]
                                : parents[i - firstInputCopies + 1];
        std::vector<unsigned char> vchSig;
        uint256 h = SignatureHash(p.vout[0].scriptPubKey, tx, i, SIGHASH_ALL, 0, SIGVERSION_BASE);
        BOOST_REQUIRE(key.Sign(h, vchSig));
        vchSig.push_back((unsigned char)SIGHASH_ALL);
        tx.vin[i].scriptSig = CScript() << vchSig;
    }
    return tx;
}

CScript CoinbaseSPK(const CKey& key)
{
    return CScript() << ToByteVector(key.GetPubKey()) << OP_CHECKSIG;
}

} // namespace

/**
 * Master economic invariant: after a burst of honest blocks and spends, and
 * after a battery of rejected inflation attempts, the supply equals exactly the
 * cumulative subsidy and never a koinu more.
 */
BOOST_AUTO_TEST_CASE(supply_equals_issuance_and_survives_attacks)
{
    const CScript spk = CoinbaseSPK(coinbaseKey);

    // Baseline supply == cumulative subsidy for the 240-block setup chain.
    CAmount expected = 0;
    for (int h = 1; h <= TipHeight(); h++)
        expected += GetPepecoinBlockSubsidy(h, Params().GetConsensus(h), uint256());
    BOOST_CHECK_EQUAL(TotalSupplyOf(pcoinsdbview), expected);

    const int heightBefore = TipHeight();
    const uint256 tipBefore = TipHash();
    const CAmount supplyBefore = TotalSupplyOf(pcoinsdbview);

    // ---- Attack battery: every one of these blocks must be REJECTED and must
    //      leave the tip and supply untouched. ----

    int nextHeight = heightBefore + 1;
    const CAmount subsidy = GetPepecoinBlockSubsidy(nextHeight, Params().GetConsensus(nextHeight), uint256());

    struct Attack { const char* name; std::function<CBlock()> make; };
    std::vector<Attack> attacks;

    // 1. coinbase pays subsidy + 1 (no fees available)
    attacks.push_back({"coinbase subsidy+1", [&]{
        CBlock b = BuildBlock({}, spk); SetCoinbase(b, {subsidy + 1}, spk); return b; }});
    // 2. coinbase pays 1000x subsidy
    attacks.push_back({"coinbase 1000x", [&]{
        CBlock b = BuildBlock({}, spk); SetCoinbase(b, {subsidy * 1000}, spk); return b; }});
    // 3. coinbase split into many outputs summing to subsidy+1
    attacks.push_back({"coinbase split subsidy+1", [&]{
        CBlock b = BuildBlock({}, spk);
        SetCoinbase(b, {subsidy, 1}, spk); return b; }});
    // 4. coinbase claims MAX_MONEY (which is < supply but > blockReward)
    attacks.push_back({"coinbase MAX_MONEY", [&]{
        CBlock b = BuildBlock({}, spk); SetCoinbase(b, {MAX_MONEY}, spk); return b; }});
    // 5. non-coinbase tx creating outputs > inputs (in-belowout)
    attacks.push_back({"tx out>in", [&]{
        CMutableTransaction t = Spend({coinbaseTxns[0]}, coinbaseKey, spk,
                                      coinbaseTxns[0].vout[0].nValue + COIN);
        return BuildBlock({t}, spk); }});
    // 6. tx with duplicate inputs (double-spend within tx)
    attacks.push_back({"tx dup inputs", [&]{
        CMutableTransaction t = Spend({coinbaseTxns[1]}, coinbaseKey, spk,
                                      coinbaseTxns[1].vout[0].nValue * 2 - COIN, 2);
        return BuildBlock({t}, spk); }});
    // 7. two txs in one block spending the SAME coinbase (double-spend across txs)
    attacks.push_back({"block double-spend", [&]{
        CMutableTransaction a = Spend({coinbaseTxns[2]}, coinbaseKey, spk,
                                      coinbaseTxns[2].vout[0].nValue - COIN);
        CMutableTransaction b2 = Spend({coinbaseTxns[2]}, coinbaseKey, spk,
                                       coinbaseTxns[2].vout[0].nValue - 2 * COIN);
        return BuildBlock({a, b2}, spk); }});
    // 8. coinbase claims a fee that no tx paid
    attacks.push_back({"coinbase phantom fee", [&]{
        CBlock b = BuildBlock({}, spk); SetCoinbase(b, {subsidy + 5 * COIN}, spk); return b; }});
    // 9. coinbase pays exactly subsidy + real fee + 1 (over by one koinu)
    attacks.push_back({"coinbase realfee+1", [&]{
        CAmount fee = 3 * COIN;
        CMutableTransaction t = Spend({coinbaseTxns[3]}, coinbaseKey, spk,
                                      coinbaseTxns[3].vout[0].nValue - fee);
        CBlock b = BuildBlock({t}, spk);
        SetCoinbase(b, {subsidy + fee + 1}, spk); return b; }});

    for (const auto& atk : attacks) {
        CBlock b = atk.make();
        bool accepted = SubmitBlock(b);
        BOOST_CHECK_MESSAGE(!accepted, std::string("INFLATION: attack accepted: ") + atk.name);
        BOOST_CHECK_MESSAGE(TipHash() == tipBefore,
                            std::string("tip moved after attack: ") + atk.name);
        BOOST_CHECK_MESSAGE(TotalSupplyOf(pcoinsdbview) == supplyBefore,
                            std::string("supply changed after attack: ") + atk.name);
    }

    // ---- Now an HONEST block that spends a coinbase and pays a real fee the
    //      miner collects: supply must grow by exactly the subsidy (fees are
    //      recycled, not created). ----
    {
        const CAmount fee = 7 * COIN;
        CMutableTransaction t = Spend({coinbaseTxns[4]}, coinbaseKey, spk,
                                      coinbaseTxns[4].vout[0].nValue - fee);
        CBlock b = BuildBlock({t}, spk);
        SetCoinbase(b, {subsidy + fee}, spk);
        BOOST_CHECK_MESSAGE(SubmitBlock(b), "honest subsidy+fee block was rejected");
        BOOST_CHECK_EQUAL(TotalSupplyOf(pcoinsdbview), supplyBefore + subsidy);
    }
}

/**
 * Intra-block ordering: a tx may spend an output created earlier in the same
 * block, but NOT one created later, and NOT the block's own coinbase (immature).
 */
BOOST_AUTO_TEST_CASE(intra_block_spend_ordering)
{
    const CScript spk = CoinbaseSPK(coinbaseKey);
    const uint256 tipBefore = TipHash();
    const CAmount supplyBefore = TotalSupplyOf(pcoinsdbview);

    // A: parent spends coinbaseTxns[5]; child spends parent's output. Ordered
    //    [parent, child] this is a VALID chain and should be accepted.
    {
        const CAmount v = coinbaseTxns[5].vout[0].nValue;
        CMutableTransaction parent = Spend({coinbaseTxns[5]}, coinbaseKey, spk, v - COIN);
        CTransaction parentTx(parent);
        CMutableTransaction child = Spend({parentTx}, coinbaseKey, spk, v - 2 * COIN);
        CBlock b = BuildBlock({parent, child}, spk);
        // Coinbase may collect the 2 PEPE of fees.
        int h = TipHeight() + 1;
        CAmount subsidy = GetPepecoinBlockSubsidy(h, Params().GetConsensus(h), uint256());
        SetCoinbase(b, {subsidy + 2 * COIN}, spk);
        BOOST_CHECK_MESSAGE(SubmitBlock(b), "valid in-order intra-block chain rejected");
    }

    const uint256 tipAfterValid = TipHash();
    const CAmount supplyAfterValid = TotalSupplyOf(pcoinsdbview);

    // B: child BEFORE parent — spending an output that doesn't exist yet. Must
    //    be rejected and leave state unchanged.
    {
        const CAmount v = coinbaseTxns[6].vout[0].nValue;
        CMutableTransaction parent = Spend({coinbaseTxns[6]}, coinbaseKey, spk, v - COIN);
        CTransaction parentTx(parent);
        CMutableTransaction child = Spend({parentTx}, coinbaseKey, spk, v - 2 * COIN);
        CBlock b = BuildBlock({child, parent}, spk); // wrong order
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "out-of-order intra-block spend accepted");
        BOOST_CHECK(TipHash() == tipAfterValid);
        BOOST_CHECK_EQUAL(TotalSupplyOf(pcoinsdbview), supplyAfterValid);
    }

    BOOST_CHECK(tipBefore != tipAfterValid); // sanity: the valid block did land
}

/**
 * A block that tries to spend its own coinbase (or any coinbase younger than
 * nCoinbaseMaturity) must be rejected — reward maturity cannot be bypassed.
 */
BOOST_AUTO_TEST_CASE(no_premature_coinbase_spend_in_block)
{
    const CScript spk = CoinbaseSPK(coinbaseKey);
    const uint256 tipBefore = TipHash();
    const CAmount supplyBefore = TotalSupplyOf(pcoinsdbview);

    // Spend the most-recent (immature) coinbase.
    const CTransaction& fresh = coinbaseTxns.back();
    CMutableTransaction t = Spend({fresh}, coinbaseKey, spk, fresh.vout[0].nValue - COIN);
    CBlock b = BuildBlock({t}, spk);
    BOOST_CHECK_MESSAGE(!SubmitBlock(b), "premature coinbase spend accepted in a block");
    BOOST_CHECK(TipHash() == tipBefore);
    BOOST_CHECK_EQUAL(TotalSupplyOf(pcoinsdbview), supplyBefore);
}

/**
 * Reorg conservation: mine two competing branches; a coinbase spent only on the
 * losing branch must be fully restored (spendable, not duplicated) after the
 * reorg, and the supply must equal issuance for the winning branch.
 */
BOOST_AUTO_TEST_CASE(reorg_preserves_conservation)
{
    const CScript spk = CoinbaseSPK(coinbaseKey);

    const CAmount supply0 = TotalSupplyOf(pcoinsdbview);
    const int h0 = TipHeight();

    // Branch A: one block that spends a mature coinbase.
    const CAmount v = coinbaseTxns[10].vout[0].nValue;
    CMutableTransaction spendA = Spend({coinbaseTxns[10]}, coinbaseKey, spk, v - COIN);
    CBlock a1 = BuildBlock({spendA}, spk);
    int hA = TipHeight() + 1;
    CAmount subA = GetPepecoinBlockSubsidy(hA, Params().GetConsensus(hA), uint256());
    SetCoinbase(a1, {subA + COIN}, spk);
    BOOST_REQUIRE(SubmitBlock(a1));
    const CAmount supplyA = TotalSupplyOf(pcoinsdbview);
    BOOST_CHECK_EQUAL(supplyA, supply0 + subA); // subsidy only; fee recycled

    // The coinbase output spent on branch A must now be gone.
    {
        LOCK(cs_main);
        BOOST_CHECK(!pcoinsTip->AccessCoins(coinbaseTxns[10].GetHash())
                    || !pcoinsTip->AccessCoins(coinbaseTxns[10].GetHash())->IsAvailable(0));
    }

    // Invalidate a1 to force it off the chain (disconnect), which must undo the
    // spend of coinbaseTxns[10].  Clear the mempool afterwards so the resurrected
    // spendA is not re-mined into an identical (now-invalid) block.
    {
        CValidationState state;
        CBlockIndex* pidx = nullptr;
        {
            LOCK(cs_main);
            pidx = mapBlockIndex[a1.GetHash()];
        }
        BOOST_REQUIRE(pidx);
        InvalidateBlock(state, Params(), pidx);
        ActivateBestChain(state, Params());
        LOCK(cs_main);
        mempool.clear();
    }

    // After invalidating a1, the spend is undone: coinbaseTxns[10] must be
    // spendable again, and supply back to the pre-a1 value.
    BOOST_CHECK_EQUAL(TotalSupplyOf(pcoinsdbview), supply0);
    BOOST_CHECK_EQUAL(TipHeight(), h0);
    {
        LOCK(cs_main);
        const CCoins* c = pcoinsTip->AccessCoins(coinbaseTxns[10].GetHash());
        BOOST_CHECK_MESSAGE(c && c->IsAvailable(0),
                            "coinbase not restored after reorg (would be a burn); "
                            "or worse, still spendable+spent = double-spend");
    }

    // Re-mine on a DISTINCT coinbase branch (OP_TRUE) so the new blocks cannot
    // hash-collide with the invalidated a1.  Two honest empty blocks: supply
    // grows by exactly their subsidies.
    const CScript spk2 = CScript() << OP_TRUE;
    CBlock b1 = BuildBlock({}, spk2);
    BOOST_REQUIRE(SubmitBlock(b1));
    CBlock b2 = BuildBlock({}, spk2);
    BOOST_REQUIRE(SubmitBlock(b2));

    CAmount expected = supply0;
    for (int hh = h0 + 1; hh <= TipHeight(); hh++)
        expected += GetPepecoinBlockSubsidy(hh, Params().GetConsensus(hh), uint256());
    BOOST_CHECK_EQUAL(TotalSupplyOf(pcoinsdbview), expected);

    // Finally, spend coinbaseTxns[10] again on the new branch: it must still be
    // worth exactly its original value (not doubled by the reorg).
    CMutableTransaction spend2 = Spend({coinbaseTxns[10]}, coinbaseKey, spk, v - COIN);
    CBlock b3 = BuildBlock({spend2}, spk2);
    int hB = TipHeight() + 1;
    CAmount subB = GetPepecoinBlockSubsidy(hB, Params().GetConsensus(hB), uint256());
    SetCoinbase(b3, {subB + COIN}, spk2);
    BOOST_CHECK_MESSAGE(SubmitBlock(b3), "restored coinbase not spendable post-reorg");
    // Spending it a SECOND time in the next block must fail (already spent).
    CMutableTransaction spend3 = Spend({coinbaseTxns[10]}, coinbaseKey, spk, v - COIN);
    CBlock b4 = BuildBlock({spend3}, spk2);
    BOOST_CHECK_MESSAGE(!SubmitBlock(b4), "DOUBLE SPEND: coinbase spent twice across reorg");
}

BOOST_AUTO_TEST_SUITE_END()
