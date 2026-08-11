// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Deep-reorg DOUBLE-SPEND demonstration (the "51% / PEP-006 work-reuse" impact
// model), run through the real ProcessNewBlock -> ConnectBlock -> reorg
// pipeline on a regtest chain.
//
// Narrative: an attacker pays a merchant (tx1, confirmed -> merchant ships).
// The attacker then gets a competing branch that omits tx1 and instead spends
// the same coin back to themselves (tx2) to become the active chain. On mainnet
// that "competing branch wins" step requires majority hashrate; here we force
// it with InvalidateBlock, which is exactly the state transition a heavier
// attacker chain produces. The point is to show the *money* effect: the
// merchant's confirmed payment is REVERSED and the coin returns to the attacker
// -- a double-spend -- WITHOUT violating any consensus rule (each branch is
// internally valid; the protocol simply follows the heavier chain).

#include "chainparams.h"
#include "coins.h"
#include "consensus/merkle.h"
#include "consensus/validation.h"
#include "key.h"
#include "miner.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "pubkey.h"
#include "script/script.h"
#include "script/sign.h"
#include "script/standard.h"
#include "test/test_bitcoin.h"
#include "txmempool.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(pepecoin_doublespend_tests, TestChain240Setup)

namespace {

uint256 TipHash() { LOCK(cs_main); return chainActive.Tip()->GetBlockHash(); }
int TipHeight() { LOCK(cs_main); return chainActive.Height(); }

bool SubmitBlock(CBlock& b)
{
    const CChainParams& cp = Params();
    b.hashMerkleRoot = BlockMerkleRoot(b);
    b.nNonce = 0;
    while (!CheckProofOfWork(b.GetPoWHash(), b.nBits, cp.GetConsensus(0))) ++b.nNonce;
    b.fChecked = false;
    const uint256 h = b.GetHash();
    ProcessNewBlock(cp, std::make_shared<const CBlock>(b), true, NULL);
    return TipHash() == h;
}

CBlock BuildBlock(const std::vector<CMutableTransaction>& txns, const CScript& spk)
{
    std::unique_ptr<CBlockTemplate> t = BlockAssembler(Params()).CreateNewBlock(spk, true);
    CBlock b = t->block;
    b.vtx.resize(1);
    for (const auto& tx : txns) b.vtx.push_back(MakeTransactionRef(tx));
    unsigned int extra = 0;
    { LOCK(cs_main); IncrementExtraNonce(&b, chainActive.Tip(), extra); }
    return b;
}

CScript P2PK(const CKey& k) { return CScript() << ToByteVector(k.GetPubKey()) << OP_CHECKSIG; }

// Spend vout 0 of `coin` (locked to `ownerScript`, owned by `ownerKey`) to `dest`.
CMutableTransaction Pay(const CTransaction& coin, const CScript& ownerScript,
                        const CKey& ownerKey, const CScript& dest, CAmount value)
{
    CMutableTransaction tx;
    tx.nVersion = 1;
    tx.vin.resize(1);
    tx.vin[0].prevout = COutPoint(coin.GetHash(), 0);
    tx.vout.resize(1);
    tx.vout[0].nValue = value;
    tx.vout[0].scriptPubKey = dest;
    std::vector<unsigned char> sig;
    uint256 h = SignatureHash(ownerScript, tx, 0, SIGHASH_ALL, 0, SIGVERSION_BASE);
    BOOST_REQUIRE(ownerKey.Sign(h, sig));
    sig.push_back((unsigned char)SIGHASH_ALL);
    tx.vin[0].scriptSig = CScript() << sig;
    return tx;
}

bool OutputInUtxo(const uint256& txid, uint32_t n)
{
    LOCK(cs_main);
    const CCoins* c = pcoinsTip->AccessCoins(txid);
    return c && c->IsAvailable(n);
}

} // namespace

BOOST_AUTO_TEST_CASE(reorg_reverses_confirmed_payment)
{
    CKey merchant; merchant.MakeNewKey(true);
    CKey attacker; attacker.MakeNewKey(true);
    const CScript merchantSpk = GetScriptForDestination(merchant.GetPubKey().GetID());
    const CScript attackerSpk = GetScriptForDestination(attacker.GetPubKey().GetID());

    // The attacker owns coin C (a mature coinbase paying to coinbaseKey/P2PK).
    const CTransaction& C = coinbaseTxns[20];
    const CScript ownerScript = C.vout[0].scriptPubKey; // P2PK(coinbaseKey)
    const CAmount value = C.vout[0].nValue;

    const int forkHeight = TipHeight();
    const uint256 forkTip = TipHash();

    // ---- Branch A: attacker pays the merchant.  Confirmed -> merchant ships. ----
    CMutableTransaction tx1 = Pay(C, ownerScript, coinbaseKey, merchantSpk, value - COIN);
    const uint256 tx1id = CTransaction(tx1).GetHash();
    CBlock A = BuildBlock({tx1}, P2PK(coinbaseKey));
    BOOST_REQUIRE_MESSAGE(SubmitBlock(A), "branch A (payment) did not confirm");

    BOOST_CHECK_MESSAGE(OutputInUtxo(tx1id, 0),
                        "merchant payment not in UTXO set after confirmation");
    const CBlockIndex* aTip; { LOCK(cs_main); aTip = chainActive.Tip(); }
    BOOST_CHECK_EQUAL(aTip->nHeight, forkHeight + 1);
    // (Merchant sees 1 confirmation here and releases goods.)

    // ---- Attacker's competing branch wins.  On mainnet this needs majority
    //      hashrate to out-work branch A; on regtest we force the same state
    //      transition with InvalidateBlock, then build the heavier branch B. ----
    {
        CValidationState st;
        CBlockIndex* pA; { LOCK(cs_main); pA = mapBlockIndex[A.GetHash()]; }
        BOOST_REQUIRE(pA);
        InvalidateBlock(st, Params(), pA);
        ActivateBestChain(st, Params());
        LOCK(cs_main); mempool.clear();
    }
    BOOST_REQUIRE_EQUAL(TipHeight(), forkHeight);   // back to the fork point
    BOOST_REQUIRE(TipHash() == forkTip);

    // Branch B, block 1: spend the SAME coin C back to the attacker (tx2), and
    // pay the coinbase to a distinct key so B's blocks can't hash-collide with A.
    const CScript bCoinbase = P2PK(attacker);
    CMutableTransaction tx2 = Pay(C, ownerScript, coinbaseKey, attackerSpk, value - COIN);
    const uint256 tx2id = CTransaction(tx2).GetHash();
    CBlock B1 = BuildBlock({tx2}, bCoinbase);
    BOOST_REQUIRE_MESSAGE(SubmitBlock(B1), "branch B block 1 rejected");
    // Extend B so it is unambiguously the active, heavier chain.
    CBlock B2 = BuildBlock({}, bCoinbase);
    BOOST_REQUIRE(SubmitBlock(B2));

    // ---- Result: the double-spend succeeded. ----
    BOOST_CHECK_MESSAGE(!OutputInUtxo(tx1id, 0),
                        "DOUBLE-SPEND FAILED TO REVERSE: merchant payment still in UTXO");
    BOOST_CHECK_MESSAGE(OutputInUtxo(tx2id, 0),
                        "attacker's replacement output not in UTXO");
    BOOST_CHECK_MESSAGE(TipHeight() == forkHeight + 2, "branch B is not the active chain");

    // The merchant's tx1 is now not even a valid mempool tx: coin C is spent by
    // tx2 on the active chain.  The merchant shipped goods for a payment that no
    // longer exists.  No consensus rule was broken -- the heavier chain simply
    // did not include tx1.  Coin C was spent exactly once on the active chain
    // (to the attacker), so supply is conserved; only the *recipient* changed.
    {
        LOCK(cs_main);
        const CCoins* c = pcoinsTip->AccessCoins(C.GetHash());
        BOOST_CHECK_MESSAGE(!c || !c->IsAvailable(0),
                            "coin C unexpectedly still unspent after reorg");
    }
}

BOOST_AUTO_TEST_SUITE_END()
