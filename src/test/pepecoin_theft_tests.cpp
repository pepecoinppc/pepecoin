// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// "Drain" attack suite: attempt to SPEND COINS THE ATTACKER DOES NOT OWN by
// bypassing signature / script verification, fed through the real
// ProcessNewBlock -> ConnectBlock pipeline on a regtest chain.  Every theft
// attempt must be rejected and must leave the UTXO set unchanged.  A single
// acceptance here would be a CRITICAL coin-theft (network-wide drain) bug.

#include "chainparams.h"
#include "coins.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "key.h"
#include "keystore.h"
#include "miner.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "script/script.h"
#include "script/sign.h"
#include "script/standard.h"
#include "test/test_bitcoin.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pepecoin_theft_tests, TestChain240Setup)

namespace {

int TipHeight() { LOCK(cs_main); return chainActive.Height(); }
uint256 TipHash() { LOCK(cs_main); return chainActive.Tip()->GetBlockHash(); }

bool SubmitBlock(CBlock& block)
{
    const CChainParams& cp = Params();
    block.hashMerkleRoot = BlockMerkleRoot(block);
    block.nNonce = 0;
    while (!CheckProofOfWork(block.GetPoWHash(), block.nBits, cp.GetConsensus(0)))
        ++block.nNonce;
    block.fChecked = false;
    const uint256 h = block.GetHash();
    std::shared_ptr<const CBlock> sb = std::make_shared<const CBlock>(block);
    ProcessNewBlock(cp, sb, true, NULL);
    return TipHash() == h;
}

CBlock BuildBlock(const std::vector<CMutableTransaction>& txns, const CScript& spk)
{
    std::unique_ptr<CBlockTemplate> tmpl = BlockAssembler(Params()).CreateNewBlock(spk, true);
    CBlock block = tmpl->block;
    block.vtx.resize(1);
    for (const auto& tx : txns) block.vtx.push_back(MakeTransactionRef(tx));
    unsigned int extraNonce = 0;
    { LOCK(cs_main); IncrementExtraNonce(&block, chainActive.Tip(), extraNonce); }
    return block;
}

CScript P2PK(const CKey& k) { return CScript() << ToByteVector(k.GetPubKey()) << OP_CHECKSIG; }

} // namespace

/**
 * The coinbases in TestChain240Setup pay to P2PK of `coinbaseKey` (victim A).
 * An attacker (key B) tries to spend victim A's coinbase without A's key.
 * Every variant must be rejected in a block; a correct signature by A is the
 * accepted control.
 */
BOOST_AUTO_TEST_CASE(cannot_spend_without_valid_signature)
{
    const CScript spk = P2PK(coinbaseKey);           // attacker's own payout dest
    CKey attacker; attacker.MakeNewKey(true);        // key B, NOT the coin owner

    const CTransaction& victimCoin = coinbaseTxns[0]; // P2PK to victim A, mature
    const CAmount value = victimCoin.vout[0].nValue;

    auto buildSpend = [&](std::function<void(CMutableTransaction&)> forgeSig) {
        CMutableTransaction tx;
        tx.nVersion = 1;
        tx.vin.resize(1);
        tx.vin[0].prevout = COutPoint(victimCoin.GetHash(), 0);
        tx.vout.resize(1);
        tx.vout[0].nValue = value - COIN;
        tx.vout[0].scriptPubKey = spk;
        forgeSig(tx);
        return tx;
    };

    const uint256 tip0 = TipHash();

    // 1. No signature at all (empty scriptSig).
    {
        CMutableTransaction tx = buildSpend([](CMutableTransaction&){});
        CBlock b = BuildBlock({tx}, spk);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "THEFT: empty-scriptSig spend accepted");
        BOOST_CHECK(TipHash() == tip0);
    }

    // 2. Signature by the ATTACKER's key over the correct sighash (B signs, but
    //    the output locks to A's pubkey -> CHECKSIG must fail).
    {
        CMutableTransaction tx = buildSpend([&](CMutableTransaction& t){
            std::vector<unsigned char> sig;
            uint256 h = SignatureHash(victimCoin.vout[0].scriptPubKey, t, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
            BOOST_REQUIRE(attacker.Sign(h, sig));
            sig.push_back((unsigned char)SIGHASH_ALL);
            t.vin[0].scriptSig = CScript() << sig;
        });
        CBlock b = BuildBlock({tx}, spk);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "THEFT: attacker-signed spend of victim coin accepted");
        BOOST_CHECK(TipHash() == tip0);
    }

    // 3. Garbage bytes in place of a signature.
    {
        CMutableTransaction tx = buildSpend([](CMutableTransaction& t){
            t.vin[0].scriptSig = CScript() << std::vector<unsigned char>(72, 0x01);
        });
        CBlock b = BuildBlock({tx}, spk);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "THEFT: garbage-scriptSig spend accepted");
        BOOST_CHECK(TipHash() == tip0);
    }

    // 4. Valid signature by A but over a DIFFERENT (tampered) transaction: sign
    //    the tx, then change an output value after signing.
    {
        CMutableTransaction tx = buildSpend([&](CMutableTransaction& t){
            std::vector<unsigned char> sig;
            uint256 h = SignatureHash(victimCoin.vout[0].scriptPubKey, t, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
            BOOST_REQUIRE(coinbaseKey.Sign(h, sig));
            sig.push_back((unsigned char)SIGHASH_ALL);
            t.vin[0].scriptSig = CScript() << sig;
        });
        tx.vout[0].nValue = value;   // tamper AFTER signing -> sighash mismatch
        CBlock b = BuildBlock({tx}, spk);
        BOOST_CHECK_MESSAGE(!SubmitBlock(b), "THEFT: post-signing tamper accepted");
        BOOST_CHECK(TipHash() == tip0);
    }

    // 5. CONTROL: a correct signature by the real owner A must be accepted.
    {
        CMutableTransaction tx = buildSpend([&](CMutableTransaction& t){
            std::vector<unsigned char> sig;
            uint256 h = SignatureHash(victimCoin.vout[0].scriptPubKey, t, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
            BOOST_REQUIRE(coinbaseKey.Sign(h, sig));
            sig.push_back((unsigned char)SIGHASH_ALL);
            t.vin[0].scriptSig = CScript() << sig;
        });
        CBlock b = BuildBlock({tx}, spk);
        BOOST_CHECK_MESSAGE(SubmitBlock(b), "legitimately-signed spend was rejected");
        BOOST_CHECK(TipHash() != tip0);
    }
}

/**
 * P2PKH theft: pay to HASH160(pubkey A); attacker tries to redeem with their
 * own pubkey/sig.  Must fail on the pubkey-hash comparison and the signature.
 */
BOOST_AUTO_TEST_CASE(cannot_redeem_p2pkh_of_another_key)
{
    CKey victim; victim.MakeNewKey(true);
    CKey attacker; attacker.MakeNewKey(true);
    const CScript payout = P2PK(coinbaseKey);

    // Fund a P2PKH(victim) output by spending a coinbase we own.
    const CTransaction& src = coinbaseTxns[1];
    CScript p2pkhVictim = GetScriptForDestination(victim.GetPubKey().GetID());
    CMutableTransaction fund;
    fund.nVersion = 1;
    fund.vin.resize(1);
    fund.vin[0].prevout = COutPoint(src.GetHash(), 0);
    fund.vout.resize(1);
    fund.vout[0].nValue = src.vout[0].nValue - COIN;
    fund.vout[0].scriptPubKey = p2pkhVictim;
    {
        std::vector<unsigned char> sig;
        uint256 h = SignatureHash(src.vout[0].scriptPubKey, fund, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
        BOOST_REQUIRE(coinbaseKey.Sign(h, sig));
        sig.push_back((unsigned char)SIGHASH_ALL);
        fund.vin[0].scriptSig = CScript() << sig;
    }
    CBlock fb = BuildBlock({fund}, payout);
    BOOST_REQUIRE_MESSAGE(SubmitBlock(fb), "funding P2PKH(victim) failed");
    const CTransaction fundTx(fund);
    const uint256 tipFunded = TipHash();

    // Attacker tries to spend the P2PKH(victim) output with their own key.
    CMutableTransaction steal;
    steal.nVersion = 1;
    steal.vin.resize(1);
    steal.vin[0].prevout = COutPoint(fundTx.GetHash(), 0);
    steal.vout.resize(1);
    steal.vout[0].nValue = fundTx.vout[0].nValue - COIN;
    steal.vout[0].scriptPubKey = payout;
    {
        std::vector<unsigned char> sig;
        uint256 h = SignatureHash(p2pkhVictim, steal, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
        BOOST_REQUIRE(attacker.Sign(h, sig));
        sig.push_back((unsigned char)SIGHASH_ALL);
        steal.vin[0].scriptSig = CScript() << sig << ToByteVector(attacker.GetPubKey());
    }
    CBlock sb = BuildBlock({steal}, payout);
    BOOST_CHECK_MESSAGE(!SubmitBlock(sb), "THEFT: P2PKH(victim) redeemed with attacker key");
    BOOST_CHECK(TipHash() == tipFunded);

    // CONTROL: the real victim can redeem it.
    CMutableTransaction ok;
    ok.nVersion = 1;
    ok.vin.resize(1);
    ok.vin[0].prevout = COutPoint(fundTx.GetHash(), 0);
    ok.vout.resize(1);
    ok.vout[0].nValue = fundTx.vout[0].nValue - COIN;
    ok.vout[0].scriptPubKey = payout;
    {
        std::vector<unsigned char> sig;
        uint256 h = SignatureHash(p2pkhVictim, ok, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
        BOOST_REQUIRE(victim.Sign(h, sig));
        sig.push_back((unsigned char)SIGHASH_ALL);
        ok.vin[0].scriptSig = CScript() << sig << ToByteVector(victim.GetPubKey());
    }
    CBlock okb = BuildBlock({ok}, payout);
    BOOST_CHECK_MESSAGE(SubmitBlock(okb), "victim could not redeem their own P2PKH");
}

BOOST_AUTO_TEST_SUITE_END()
