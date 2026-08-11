// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// AuxPoW work-reuse / parent-block-reuse tests.
//
// CAuxPow::check() pins the merge-mined block to a *single* slot of the parent
// coinbase's chain merkle tree via
//     getExpectedIndex(nNonce, nChainId, h)
// The only attacker-variable input to that slot on a *given* parent coinbase is
// nChainId, which comes from the child block's own nVersion.  fStrictChainId
// therefore does two jobs at once:
//   1. CheckAuxPowProofOfWork pins the child's chain id to nAuxpowChainId, so a
//      given (parent, nonce, height) admits exactly ONE Pepecoin block; and
//   2. CAuxPow::check refuses a parent whose chain id is ours.
// These tests demonstrate what happens on the network where fStrictChainId is
// false (testnet).

#include "auxpow.h"
#include "chainparams.h"
#include "consensus/merkle.h"
#include "pepecoin.h"
#include "primitives/block.h"
#include "script/script.h"
#include "uint256.h"
#include "arith_uint256.h"
#include "utilstrencodings.h"
#include "validation.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <set>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pepecoin_auxpow_workreuse_tests, BasicTestingSetup)

namespace {

/* Build a chain merkle tree with 2^h leaves, place `hashes[i]` at slot
   `slots[i]`, and return (root, per-leaf branch). */
struct ChainTree {
    unsigned h;
    std::vector<uint256> leaves;

    explicit ChainTree(unsigned hIn) : h(hIn), leaves(1u << hIn) {}

    void set(int slot, const uint256& v) { leaves[slot] = v; }

    /* Straight recursive merkle over `leaves` (same hashing rule as
       CAuxPow::CheckMerkleBranch: Hash(left, right)). */
    static uint256 combine(const uint256& a, const uint256& b)
    {
        return Hash(BEGIN(a), END(a), BEGIN(b), END(b));
    }

    uint256 root() const
    {
        std::vector<uint256> cur = leaves;
        while (cur.size() > 1) {
            std::vector<uint256> next;
            for (size_t i = 0; i < cur.size(); i += 2)
                next.push_back(combine(cur[i], cur[i + 1]));
            cur = next;
        }
        return cur[0];
    }

    std::vector<uint256> branch(int slot) const
    {
        std::vector<uint256> res;
        std::vector<uint256> cur = leaves;
        int idx = slot;
        while (cur.size() > 1) {
            res.push_back(cur[idx ^ 1]);
            std::vector<uint256> next;
            for (size_t i = 0; i < cur.size(); i += 2)
                next.push_back(combine(cur[i], cur[i + 1]));
            cur = next;
            idx >>= 1;
        }
        return res;
    }
};

std::vector<unsigned char> coinbaseData(const std::vector<unsigned char>& auxRoot,
                                        unsigned h, int nonce)
{
    std::vector<unsigned char> res;
    res.insert(res.end(), UBEGIN(pchMergedMiningHeader), UEND(pchMergedMiningHeader));
    res.insert(res.end(), auxRoot.begin(), auxRoot.end());
    const int size = (1 << h);
    res.insert(res.end(), UBEGIN(size), UEND(size));
    res.insert(res.end(), UBEGIN(nonce), UEND(nonce));
    return res;
}

/* Assemble one parent block committing to `tree`, and return an auxpow object
   for the leaf at `slot`. */
CAuxPow makeAuxPow(const ChainTree& tree, int slot, int nonce, int parentChainId)
{
    CBlock parent;
    parent.SetBaseVersion(2, parentChainId);

    std::vector<unsigned char> root = ToByteVector(tree.root());
    std::reverse(root.begin(), root.end()); // coinbase carries the reversed root

    CMutableTransaction mtx;
    mtx.vin.resize(1);
    mtx.vin[0].prevout.SetNull();
    mtx.vin[0].scriptSig = (CScript() << coinbaseData(root, tree.h, nonce));
    parent.vtx.clear();
    parent.vtx.push_back(MakeTransactionRef(std::move(mtx)));
    parent.hashMerkleRoot = BlockMerkleRoot(parent);

    LOCK(cs_main);
    CAuxPow res(parent.vtx[0]);
    res.InitMerkleBranch(parent, 0);
    res.vChainMerkleBranch = tree.branch(slot);
    res.nChainIndex = slot;
    res.parentBlock = parent;
    return res;
}

} // namespace

/**
 * MAINNET (fStrictChainId == true): a given parent coinbase (fixed nonce and
 * tree height) can carry at most ONE valid Pepecoin commitment, because the
 * child's chain id is forced to nAuxpowChainId and therefore the slot is
 * uniquely determined.
 */
BOOST_AUTO_TEST_CASE(auxpow_mainnet_one_commitment_per_parent)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(42000);
    BOOST_CHECK(params.fStrictChainId);

    const unsigned h = 4;
    const int nonce = 0x1337;
    const int32_t ourId = params.nAuxpowChainId;
    const int slot = CAuxPow::getExpectedIndex(nonce, ourId, h);

    const uint256 hashA = ArithToUint256(arith_uint256(0xAAAA));
    const uint256 hashB = ArithToUint256(arith_uint256(0xBBBB));

    ChainTree tree(h);
    tree.set(slot, hashA);
    // Try to smuggle a second Pepecoin block into a different slot.
    tree.set((slot + 1) % (1 << h), hashB);

    BOOST_CHECK(makeAuxPow(tree, slot, nonce, 98).check(hashA, ourId, params));
    BOOST_CHECK_MESSAGE(!makeAuxPow(tree, (slot + 1) % (1 << h), nonce, 98)
                            .check(hashB, ourId, params),
                        "second Pepecoin block validated against the same parent coinbase");

    // Every other chain id is refused up-front by CheckAuxPowProofOfWork, so
    // there is no way to reach a second slot at all.
    for (int32_t id = 0; id < 8; ++id) {
        if (id == ourId) continue;
        CBlockHeader hdr;
        hdr.SetBaseVersion(4, id);
        hdr.SetAuxpowFlag(true);
        BOOST_CHECK(hdr.GetChainId() != params.nAuxpowChainId);
    }
}

/**
 * MAINNET: the parent block may not carry our own chain id.  This is the check
 * that stops a Pepecoin block from being reused as its own auxpow parent.
 */
BOOST_AUTO_TEST_CASE(auxpow_mainnet_parent_may_not_be_pepecoin)
{
    SelectParams(CBaseChainParams::MAIN);
    const Consensus::Params& params = Params().GetConsensus(42000);
    const unsigned h = 2;
    const int nonce = 5;
    const int32_t ourId = params.nAuxpowChainId;
    const int slot = CAuxPow::getExpectedIndex(nonce, ourId, h);
    const uint256 hashAux = ArithToUint256(arith_uint256(0xC0FFEE));

    ChainTree tree(h);
    tree.set(slot, hashAux);

    BOOST_CHECK(makeAuxPow(tree, slot, nonce, 98).check(hashAux, ourId, params));
    BOOST_CHECK_MESSAGE(!makeAuxPow(tree, slot, nonce, ourId).check(hashAux, ourId, params),
                        "parent block with our own chain id was accepted on mainnet");
}

/**
 * TESTNET (fStrictChainId == false): BOTH guards are disabled.  A single parent
 * coinbase can commit to many distinct Pepecoin blocks (one per declared chain
 * id), and the parent may itself be a Pepecoin block.  One parent proof-of-work
 * therefore validates an arbitrary number of distinct testnet blocks.
 */
BOOST_AUTO_TEST_CASE(auxpow_testnet_strictchainid_off_allows_work_reuse)
{
    SelectParams(CBaseChainParams::TESTNET);
    const Consensus::Params& params = Params().GetConsensus(42000);
    BOOST_CHECK_MESSAGE(!params.fStrictChainId,
                        "testnet unexpectedly has fStrictChainId set");

    const unsigned h = 6;              // 64 slots
    const int nonce = 0x2A;
    const int nSlots = 1 << h;

    // Pick one distinct chain id per slot.
    std::vector<int32_t> idForSlot(nSlots, -1);
    for (int32_t id = 1; id < 20000; ++id) {
        const int s = CAuxPow::getExpectedIndex(nonce, id, h);
        if (idForSlot[s] == -1) idForSlot[s] = id;
    }

    ChainTree tree(h);
    std::vector<uint256> childHash(nSlots);
    for (int s = 0; s < nSlots; ++s) {
        childHash[s] = ArithToUint256(arith_uint256(0x1000 + s));
        tree.set(s, childHash[s]);
    }

    // Parent is itself a Pepecoin-chain-id block: allowed, because
    // fStrictChainId is false.
    const int32_t parentChainId = params.nAuxpowChainId;

    size_t nAccepted = 0;
    for (int s = 0; s < nSlots; ++s) {
        if (idForSlot[s] == -1) continue;
        const CAuxPow ap = makeAuxPow(tree, s, nonce, parentChainId);
        if (ap.check(childHash[s], idForSlot[s], params))
            ++nAccepted;
    }

    BOOST_TEST_MESSAGE("testnet: " << nAccepted << " distinct blocks validated by ONE parent coinbase");
    BOOST_CHECK_MESSAGE(nAccepted > 1,
                        "expected multiple distinct blocks to share one parent PoW on testnet");
    BOOST_CHECK_EQUAL(nAccepted, (size_t)nSlots);

    SelectParams(CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_SUITE_END()
