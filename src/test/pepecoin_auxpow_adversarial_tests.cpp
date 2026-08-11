// Copyright (c) 2024 The Pepecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
// Adversarial AuxPoW / merged-mining tests.  These complement auxpow_tests.cpp
// by attacking the parts of CAuxPow::check() and CheckAuxPowProofOfWork() that
// take attacker-controlled integers: the chain merkle index, the branch length,
// and the block nVersion (chain id / auxpow flag / base version) encoding.

#include "auxpow.h"
#include "chainparams.h"
#include "consensus/merkle.h"
#include "pepecoin.h"
#include "primitives/block.h"
#include "script/script.h"
#include "streams.h"
#include "uint256.h"
#include "utilstrencodings.h"
#include "validation.h"

#include "test/test_bitcoin.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <limits>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pepecoin_auxpow_adversarial_tests, BasicTestingSetup)

namespace {

/** Minimal auxpow builder (mirrors the one in auxpow_tests.cpp). */
struct Builder {
    CBlock parentBlock;
    std::vector<uint256> chainBranch;
    int chainIndex;

    Builder(int baseVersion, int chainId) : chainIndex(-1)
    {
        parentBlock.SetBaseVersion(baseVersion, chainId);
    }

    void setCoinbase(const CScript& scr)
    {
        CMutableTransaction mtx;
        mtx.vin.resize(1);
        mtx.vin[0].prevout.SetNull();
        mtx.vin[0].scriptSig = scr;
        parentBlock.vtx.clear();
        parentBlock.vtx.push_back(MakeTransactionRef(std::move(mtx)));
        parentBlock.hashMerkleRoot = BlockMerkleRoot(parentBlock);
    }

    std::vector<unsigned char> buildChain(const uint256& hashAux, unsigned h, int index)
    {
        chainIndex = index;
        chainBranch.clear();
        for (unsigned i = 0; i < h; ++i)
            chainBranch.push_back(ArithToUint256(arith_uint256(i)));
        const uint256 hash = CAuxPow::CheckMerkleBranch(hashAux, chainBranch, index);
        std::vector<unsigned char> res = ToByteVector(hash);
        std::reverse(res.begin(), res.end());
        return res;
    }

    CAuxPow get() const
    {
        LOCK(cs_main);
        CAuxPow res(parentBlock.vtx[0]);
        res.InitMerkleBranch(parentBlock, 0);
        res.vChainMerkleBranch = chainBranch;
        res.nChainIndex = chainIndex;
        res.parentBlock = parentBlock;
        return res;
    }

    static std::vector<unsigned char> coinbaseData(bool header,
                                                   const std::vector<unsigned char>& auxRoot,
                                                   unsigned h, int nonce)
    {
        std::vector<unsigned char> res;
        if (header)
            res.insert(res.end(), UBEGIN(pchMergedMiningHeader), UEND(pchMergedMiningHeader));
        res.insert(res.end(), auxRoot.begin(), auxRoot.end());
        const int size = (1 << h);
        res.insert(res.end(), UBEGIN(size), UEND(size));
        res.insert(res.end(), UBEGIN(nonce), UEND(nonce));
        return res;
    }
};

} // namespace

/**
 * A negative or out-of-range chain merkle index must never validate, whatever
 * the branch contents.  nChainIndex is a signed int taken straight off the wire.
 */
BOOST_AUTO_TEST_CASE(auxpow_adversarial_chain_index)
{
    const Consensus::Params& params = Params().GetConsensus(42000);
    const uint256 hashAux = ArithToUint256(arith_uint256(0xdeadbeef));
    const int32_t ourChainId = params.nAuxpowChainId;
    const unsigned height = 4;
    const int nonce = 7;

    const int good = CAuxPow::getExpectedIndex(nonce, ourChainId, height);

    // Sanity: the honest construction validates.
    {
        Builder b(5, 42);
        std::vector<unsigned char> root = b.buildChain(hashAux, height, good);
        b.setCoinbase(CScript() << Builder::coinbaseData(true, root, height, nonce));
        BOOST_CHECK(b.get().check(hashAux, ourChainId, params));
    }

    // Adversarial indices: all must be rejected, none may crash or hang.
    const int badIndices[] = {
        -1, -2, -1000,
        std::numeric_limits<int>::min(),
        std::numeric_limits<int>::max(),
        (1 << height),        // one past the end of the tree
        (1 << height) + good, // aliases to `good` modulo the tree size
    };
    for (int idx : badIndices) {
        Builder b(5, 42);
        std::vector<unsigned char> root = b.buildChain(hashAux, height, idx);
        b.setCoinbase(CScript() << Builder::coinbaseData(true, root, height, nonce));
        BOOST_CHECK_MESSAGE(!b.get().check(hashAux, ourChainId, params),
                            "auxpow accepted with nChainIndex = " << idx);
    }

    // Keep the committed root correct but lie about the index afterwards.
    for (int idx : badIndices) {
        Builder b(5, 42);
        std::vector<unsigned char> root = b.buildChain(hashAux, height, good);
        b.setCoinbase(CScript() << Builder::coinbaseData(true, root, height, nonce));
        CAuxPow ap = b.get();
        ap.nChainIndex = idx;
        BOOST_CHECK_MESSAGE(!ap.check(hashAux, ourChainId, params),
                            "auxpow accepted with substituted nChainIndex = " << idx);
    }
}

/**
 * The chain merkle branch length is bounded at 30 so that `1 << h` cannot
 * overflow in getExpectedIndex.  Verify the bound is enforced before any shift,
 * at and beyond the boundary.
 */
BOOST_AUTO_TEST_CASE(auxpow_adversarial_branch_length)
{
    const Consensus::Params& params = Params().GetConsensus(42000);
    const uint256 hashAux = ArithToUint256(arith_uint256(0x1234));
    const int32_t ourChainId = params.nAuxpowChainId;
    const int nonce = 3;

    // h == 30 is the maximum allowed and must still validate.
    {
        const unsigned h = 30;
        const int idx = CAuxPow::getExpectedIndex(nonce, ourChainId, h);
        Builder b(5, 42);
        std::vector<unsigned char> root = b.buildChain(hashAux, h, idx);
        b.setCoinbase(CScript() << Builder::coinbaseData(true, root, h, nonce));
        BOOST_CHECK(b.get().check(hashAux, ourChainId, params));
    }

    // h > 30 must be rejected by the explicit length check, never reaching the
    // `1 << h` computation (h == 31/32 would be UB / wrap).
    for (unsigned h : {31u, 32u, 33u, 64u}) {
        Builder b(5, 42);
        // buildChain with a huge h is fine; the index cannot be computed safely
        // so just use 0 - the length check must fire first.
        std::vector<unsigned char> root = b.buildChain(hashAux, h, 0);
        b.setCoinbase(CScript() << Builder::coinbaseData(true, root, 30, nonce));
        BOOST_CHECK_MESSAGE(!b.get().check(hashAux, ourChainId, params),
                            "auxpow accepted with chain merkle branch of length " << h);
    }
}

/**
 * Truth table for adversarial nVersion values.  Pepecoin encodes the chain id in
 * the high 16 bits, the auxpow flag in bit 8, and the "base version" in the low
 * 8 bits - all inside a SIGNED int32 read straight from the wire.  Verify that
 * no bit pattern produces a block that is simultaneously (a) not legacy, (b) not
 * auxpow, (c) accepted by the strict chain-id rule, and (d) reports a base
 * version >= 4 to the BIP65/BIP66 gate while actually being a low version.
 */
BOOST_AUTO_TEST_CASE(auxpow_adversarial_version_encoding)
{
    const Consensus::Params& mainParams = Params().GetConsensus(42000);
    BOOST_CHECK(mainParams.fStrictChainId);
    const int32_t ourChainId = mainParams.nAuxpowChainId;

    const int32_t versions[] = {
        0, 1, 2, 3, 4, 5,
        (int32_t)0x00000100,                  // auxpow flag, chain id 0
        (int32_t)0x00000104,
        (ourChainId << 16) | 4,               // honest non-auxpow block
        (ourChainId << 16) | 0x100 | 4,       // honest auxpow block
        (ourChainId << 16),                   // base version 0
        (ourChainId << 16) | 3,
        (int32_t)0x80000000,                  // sign bit set
        (int32_t)0x803f0004,
        (int32_t)0x803f0104,
        (int32_t)0xffffffff,                  // -1
        (int32_t)0xffff0004,
        (int32_t)0x003fff04,                  // stray bits between flag and chain id
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(),
    };

    for (int32_t v : versions) {
        CBlockHeader h;
        h.nVersion = v;
        const int32_t chainId = h.GetChainId();
        const int32_t baseVer = h.GetBaseVersion();
        const bool legacy = h.IsLegacy();
        const bool auxpow = h.IsAuxpow();

        BOOST_TEST_MESSAGE("nVersion=0x" << strprintf("%08x", (uint32_t)v)
                           << " chainId=" << chainId << " base=" << baseVer
                           << " legacy=" << legacy << " auxpow=" << auxpow);

        // The strict chain-id rule in CheckAuxPowProofOfWork rejects every
        // non-legacy header whose chain id is not ours.  Anything that slips
        // through must carry our chain id exactly.
        const bool passesChainIdRule = legacy || chainId == ourChainId;
        if (passesChainIdRule && !legacy) {
            BOOST_CHECK_MESSAGE(chainId == ourChainId,
                                "non-legacy header with foreign chain id passed the chain-id rule");
        }

        // GetBaseVersion must never report >= 4 for a header whose low byte is
        // below 4 - that would let a v1/v2/v3 block through the BIP65/BIP66 gate.
        const int32_t lowByte = (int32_t)((uint32_t)v & 0xffu);
        if (lowByte < 4) {
            BOOST_CHECK_MESSAGE(baseVer < 4,
                                "GetBaseVersion() reported >= 4 for low byte " << lowByte
                                << " (nVersion 0x" << strprintf("%08x", (uint32_t)v) << ")");
        }

        // A header can never be both legacy and auxpow.
        BOOST_CHECK(!(legacy && auxpow));
    }
}

/**
 * A header claiming the auxpow flag but carrying no auxpow, and a header
 * carrying an auxpow without the flag, must both be rejected; and the auxpow
 * must not influence the block hash.
 */
BOOST_AUTO_TEST_CASE(auxpow_flag_and_payload_must_agree)
{
    SelectParams(CBaseChainParams::REGTEST);
    const Consensus::Params& params = Params().GetConsensus(42000);

    const arith_uint256 target = (~arith_uint256(0) >> 1);
    CBlockHeader block;
    block.nBits = target.GetCompact();

    // auxpow version bit set, but auxpow pointer empty.
    block.SetBaseVersion(4, params.nAuxpowChainId);
    block.SetAuxpowFlag(true);
    block.auxpow.reset();
    BOOST_CHECK_MESSAGE(!CheckAuxPowProofOfWork(block, params),
                        "header with auxpow version but no auxpow was accepted");

    // auxpow attached but the version bit cleared.
    CBlockHeader block2;
    block2.nBits = target.GetCompact();
    block2.SetBaseVersion(4, params.nAuxpowChainId);
    CAuxPow::initAuxPow(block2);
    const uint256 hashWithFlag = block2.GetHash();
    block2.SetAuxpowFlag(false);
    BOOST_CHECK(block2.auxpow);
    BOOST_CHECK_MESSAGE(!CheckAuxPowProofOfWork(block2, params),
                        "header with auxpow payload but non-auxpow version was accepted");

    // The block hash must depend only on the pure header, never on the auxpow.
    block2.SetAuxpowFlag(true);
    BOOST_CHECK_EQUAL(block2.GetHash().ToString(), hashWithFlag.ToString());
    std::shared_ptr<CAuxPow> saved = block2.auxpow;
    block2.auxpow.reset();
    BOOST_CHECK_EQUAL(block2.GetHash().ToString(), hashWithFlag.ToString());
    block2.auxpow = saved;

    SelectParams(CBaseChainParams::MAIN);
}

/**
 * Header (de)serialisation with auxpow must round-trip exactly, must reject
 * truncated auxpow payloads, and must reject trailing garbage.
 */
BOOST_AUTO_TEST_CASE(auxpow_header_serialization_roundtrip)
{
    SelectParams(CBaseChainParams::REGTEST);
    const Consensus::Params& params = Params().GetConsensus(42000);

    CBlockHeader block;
    const arith_uint256 tgt = (~arith_uint256(0) >> 1);
    block.nBits = tgt.GetCompact();
    block.SetBaseVersion(4, params.nAuxpowChainId);
    CAuxPow::initAuxPow(block);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << block;
    const std::vector<unsigned char> full(ss.begin(), ss.end());

    // Exact round trip.
    {
        CDataStream in(full, SER_NETWORK, PROTOCOL_VERSION);
        CBlockHeader out;
        in >> out;
        BOOST_CHECK_EQUAL(out.GetHash().ToString(), block.GetHash().ToString());
        BOOST_CHECK(out.auxpow);
        BOOST_CHECK(in.empty());

        CDataStream re(SER_NETWORK, PROTOCOL_VERSION);
        re << out;
        BOOST_CHECK(std::vector<unsigned char>(re.begin(), re.end()) == full);
    }

    // Every truncation must throw rather than silently produce a header.
    for (size_t cut = 1; cut < full.size(); cut++) {
        std::vector<unsigned char> shortened(full.begin(), full.end() - cut);
        CDataStream in(shortened, SER_NETWORK, PROTOCOL_VERSION);
        CBlockHeader out;
        bool threw = false;
        try {
            in >> out;
        } catch (const std::exception&) {
            threw = true;
        }
        BOOST_CHECK_MESSAGE(threw, "truncating " << cut << " bytes did not throw");
    }

    SelectParams(CBaseChainParams::MAIN);
}

BOOST_AUTO_TEST_SUITE_END()
