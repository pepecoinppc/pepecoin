// Copyright (c) 2021 The Dogecoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/random/uniform_int.hpp>


#include "policy/policy.h"
#include "arith_uint256.h"
#include "pepecoin.h"
#include "txmempool.h"
#include "util.h"
#include "validation.h"
#include "pepecoin-fees.h"
#include "amount.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#ifdef ENABLE_WALLET

CFeeRate GetPepecoinFeeRate(int priority)
{
    switch(priority)
    {
    case MAXIMUM:
        return CFeeRate(COIN / 100 * 521); // 5.21 PEPE, but very carefully avoiding floating point maths
    case VERY_HIGH:
        return CFeeRate(CWallet::minTxFee.GetFeePerK() * 100);
    case HIGH:
        return CFeeRate(CWallet::minTxFee.GetFeePerK() * 10);
    case MODERATE:
        return CFeeRate(CWallet::minTxFee.GetFeePerK() * 5);
    case LOW:
        return CFeeRate(CWallet::minTxFee.GetFeePerK() * 2);
    case MINIMUM:
    default:
        break;
    }
    return CWallet::minTxFee;
}

const std::string GetPepecoinPriorityLabel(int priority)
{
    switch(priority)
    {
    case MAXIMUM:
        return _("Maximum");
    case VERY_HIGH:
        return _("Very High");
    case HIGH:
        return _("High");
    case MODERATE:
        return _("Moderate");                                    
    case LOW:
        return _("Low");
    case MINIMUM:
        return _("Minimum");
    default:
        break;
    }
    return _("Default");
}

#endif

CAmount GetPepecoinMinRelayFee(const CTransaction& tx, unsigned int nBytes, bool fAllowFree)
{
    {
        LOCK(mempool.cs);
        uint256 hash = tx.GetHash();
        double dPriorityDelta = 0;
        CAmount nFeeDelta = 0;
        mempool.ApplyDeltas(hash, dPriorityDelta, nFeeDelta);
        if (dPriorityDelta > 0 || nFeeDelta > 0)
            return 0;
    }

    CAmount nMinFee = ::minRelayTxFeeRate.GetFee(nBytes);
    nMinFee += GetPepecoinDustFee(tx.vout, nDustLimit);

    if (fAllowFree)
    {
        // There is a free transaction area in blocks created by most miners,
        // * If we are relaying we allow transactions up to DEFAULT_BLOCK_PRIORITY_SIZE - 1000
        //   to be considered to fall into this category. We don't want to encourage sending
        //   multiple transactions instead of one big transaction to avoid fees.
        if (nBytes < (DEFAULT_BLOCK_PRIORITY_SIZE - 1000))
            nMinFee = 0;
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MAX_MONEY;
    return nMinFee;
}

CAmount GetPepecoinDustFee(const std::vector<CTxOut> &vout, const CAmount dustLimit) {
    CAmount nFee = 0;

    // To limit dust spam, add the dust limit for each output
    // less than the (soft) dustlimit
    BOOST_FOREACH(const CTxOut& txout, vout)
        if (txout.IsDust(dustLimit))
            nFee += dustLimit;

    return nFee;
}
