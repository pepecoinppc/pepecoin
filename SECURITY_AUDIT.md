# Pepecoin Adversarial Consensus Security Audit

**Audit type:** Authorized adversarial consensus / monetary-integrity review
**Scope:** Consensus-critical code paths governing money creation, transfer, and
validation. Emphasis on inflation, coinbase overpayment, value-conservation,
double-spend, UTXO/reorg accounting, AuxPoW/merged-mining validation, PoW /
difficulty, serialization ambiguity, and consensus splits.
**Method:** Static source analysis, upstream differential analysis against
Dogecoin Core, a compiled build, the existing unit + functional test suites,
new adversarial unit tests, and local regtest experiments. No public
infrastructure, mainnet, or third-party nodes were touched.

---

## Repository Commit Audited

```
$ git rev-parse HEAD
4fb5a0cd930c0df82c88292e973a7b7cfa06c4e8

$ git branch --show-current
claude/pepecoin-consensus-audit-cs8e6i

$ git status --short           # (before adding audit artifacts)
(clean working tree at audit start; artifacts added by this audit listed below)
```

Client identity: **Pepecoin Core v1.1.0.0** (`configure.ac`:
`_CLIENT_VERSION_MAJOR.MINOR.REVISION = 1.1.0`).

### Artifacts added by this audit

| Path | Purpose |
|------|---------|
| `src/test/pepecoin_consensus_tests.cpp` | Adversarial monetary-invariant unit tests (subsidy/coinbase/fees/duplicate-inputs/maturity/amount-range). |
| `src/test/pepecoin_auxpow_adversarial_tests.cpp` | Adversarial AuxPoW tests (chain index, branch length, version encoding, serialization round-trip). |
| `src/test/pepecoin_economic_attack_tests.cpp` | **Block-level inflation/double-spend/reorg attacks through the real `ProcessNewBlock`→`ConnectBlock` pipeline**, asserting supply is unchanged. |
| `src/test/pepecoin_timedata_tests.cpp` | Regression test for the network-time clamp (PEP-001). |
| `qa/rpc-tests/pepecoin_timewarp_exploit.py` | **Working end-to-end exploit for PEP-001** — raw-socket P2P client, freezes an isolated regtest node (regtest/loopback only). |
| `POC_PEP-001_timewarp.md` | Weaponized PEP-001 walkthrough: PoC, impact chain, test, and fix. |
| `qa/rpc-tests/pepecoin_timewarp_poc.py` | Earlier network-facing PoC shape for PEP-001 (regtest only). |
| `src/timedata.cpp` | **Fix applied** for PEP-001 (see finding). |
| `src/Makefile.test.include` | Registers the new unit-test files. |

Build: `./autogen.sh && ./configure --with-incompatible-bdb && make` — clean
build, `test_pepecoin` passes **275/275** suites (253 upstream + 22 new
adversarial cases). The single applied source fix (`timedata.cpp`) makes the
new regression test pass and is functionally identical to upstream Dogecoin
1.14.8+.

---

## Executive Summary

Pepecoin Core 1.1.0 is a near-verbatim rename-fork of **Dogecoin Core
1.14.x** (which is itself Bitcoin Core ~0.14/0.15 with scrypt PoW, AuxPoW
merged mining, the legacy `CCoins` UTXO model, and SegWit disabled). The
consensus-critical files differ from upstream almost exclusively by renames
(`dogecoin`→`pepecoin`, `DOGE`→`PEPE`), a handful of Pepecoin chain parameters,
and three deliberately-changed difficulty magic numbers. This makes a
**semantic differential audit against Dogecoin** the highest-signal technique,
and it is the backbone of this report.

**No inflation, coinbase-overpayment, double-spend, or value-conservation
vulnerability was found.** This conclusion was reached by attacking the **full
current money machine as it actually runs** — provenance-agnostic, i.e. *not*
excusing code because it matches upstream Bitcoin/Dogecoin, and specifically
stress-testing whether Pepecoin's unique parameters (unbounded tail emission,
`MAX_MONEY` smaller than circulating supply, 1-minute blocks, retarget-every-block
Digishield) make any latent arithmetic reachable. Every monetary invariant
traced through to an enforcing check, and the two fundamental invariants

* per transaction: `sum(outputs) ≤ sum(inputs)`, `fee = in − out ≥ 0`
* per block: `coinbase_out ≤ subsidy(height) + Σ fees`

are enforced on every consensus path examined. This was established three ways:
(1) source tracing; (2) an **empirical block-level attack campaign** that builds
adversarial blocks and feeds them through the real `ProcessNewBlock →
ConnectBlock` pipeline — 12 distinct inflation/double-spend/maturity/reorg
attacks, **all correctly rejected with supply provably unchanged**; and (3) a
**2,020-block regtest churn** whose total UTXO value equals cumulative issuance
**to the koinu**, across every halving boundary and into tail emission. See the
*Economic-Destruction Attack Campaign* section for the modular-arithmetic proof
that the one unguarded accumulator (`nFees`) can only ever *reduce* the reward,
and the quantified bound showing timestamp/difficulty manipulation cannot mint
beyond the fixed height-indexed schedule.

The audit did find **confirmed remotely-triggerable, peer-facing defects**, and
one of them **weaponizes to HIGH**:

* **PEP-001 (HIGH, weaponized; base MEDIUM)** — the network-adjusted-time clamp
  in `timedata.cpp` uses `abs64()`, which is undefined behavior on `INT64_MIN`.
  A **working end-to-end exploit** (`qa/rpc-tests/pepecoin_timewarp_exploit.py`,
  8 `version` messages from distinct IPs) drives a node’s clock offset to
  `INT64_MIN`, after which `ContextualCheckBlockHeader` rejects **every** real
  block as `time-too-new` — the node is **frozen at its tip until restart**, and
  the attack scales to a network-wide liveness halt. It stops short of CRITICAL
  because a frozen node accepts *no* chain (no mint, no split, no double-spend).
  Regression test + upstream-equivalent fix applied in this branch; the fixed
  binary resists the identical exploit. Full walkthrough: `POC_PEP-001_timewarp.md`.
* **PEP-005 (MEDIUM)** — `net_processing.cpp` is based on Dogecoin 1.14.7 and is
  missing the 1.14.8/1.14.9 peer-DoS hardening (a forced `getheaders` per INV
  entry, and `MAX_PEER_TX_ANNOUNCEMENTS` left at 100,000 vs upstream’s 5,000).

Both are network-layer / availability issues; **neither affects block or
transaction validity**. **No CRITICAL vulnerability, and no monetary/consensus
(inflation, coinbase-overpay, double-spend, value-conservation, consensus-split)
vulnerability of any severity, was found** — the weaponized finding is a
liveness/DoS escalation, deliberately not dressed up as a monetary break.

A small number of lower-severity observations (missing upstream diagnostic
improvements, latent-but-unreachable arithmetic) are documented, along with an
extensive **proof-of-absence** section recording the hypotheses that were
investigated and refuted, with the exact enforcing code for each.

### Findings at a glance

| ID | Severity | Confidence | Status | Title |
|----|----------|-----------|--------|-------|
| PEP-001 | **HIGH** (base MEDIUM, weaponized) | CONFIRMED (end-to-end PoC) | Fixed in this branch | `abs64(INT64_MIN)` UB → peers freeze a node's clock offset at `INT64_MIN` → node rejects **all** blocks (`time-too-new`) → persistent remote node/network liveness freeze |
| PEP-005 | MEDIUM | CONFIRMED | Open | Missing Dogecoin 1.14.8/1.14.9 P2P hardening (forced getheaders per INV entry; `MAX_PEER_TX_ANNOUNCEMENTS` 100000 vs 5000) — network DoS, non-consensus |
| PEP-002 | LOW | CONFIRMED | Open | Upstream difficulty-error “masking” fix (`c4e76a369`) not ported (diagnostic only) |
| PEP-003 | LOW/INFO | CONFIRMED | Open | `MAX_MONEY` is not a supply bound; total emission exceeds `MAX_MONEY` and `INT64_MAX` (handled where it matters) |
| PEP-006 | MEDIUM (**testnet only**) | CONFIRMED | Open (by-design relaxation) | Testnet `fStrictChainId=false` allows AuxPoW **work-reuse**: one parent-block PoW can validate up to 64 chained blocks → up to 64× hashrate amplification → cheap deep-reorg/double-spend on **testnet**. Mainnet/regtest have `fStrictChainId=true` and are **not** affected. |
| PEP-004 | INFO | CONFIRMED | Open | Dead/stale consensus code, inherited latent items, and `//PEPE TODO` magic numbers |

**Coin-theft (spend-what-you-don't-own) is closed.** Signature/script verification
is fully enforced on the connect path (`fScriptChecks=true`, only relaxed under
assumevalid for ancestors of the assume-valid block). Empirically proven by
`src/test/pepecoin_theft_tests.cpp`: spending a victim's UTXO with no signature,
the attacker's signature, garbage bytes, a post-signing tamper, or an attacker
key against a P2PKH output are **all rejected in a block**; only the legitimately
signed control is accepted.

*Scope note:* PEP-001 and PEP-005 are network-layer (peer-facing) issues, not
consensus/monetary defects. Consistent with the audit’s severity rubric,
node-DoS issues are rated **MEDIUM**. **No CRITICAL or HIGH monetary/consensus
vulnerability was found.**

---

## Monetary Invariants — how PEPE supply is protected

PEPE is denominated in **koinu** (1 PEPE = `COIN` = 10⁸ koinu). `CAmount` is
`int64_t`. The sanity ceiling is:

```c
// src/amount.h:32
static const CAmount MAX_MONEY = 10000000000 * COIN; // 1e18 koinu
inline bool MoneyRange(const CAmount& nValue) { return (nValue >= 0 && nValue <= MAX_MONEY); }
```

Supply is created only in the coinbase output of each block. The subsidy
schedule (`src/pepecoin.cpp:127`, `GetPepecoinBlockSubsidy`) with
`fSimplifiedRewards = true` (true on **all** reachable params objects, verified
below) is:

```
subsidy(h) = (500000 * COIN) >> (h / 100000)      for h < 6*100000 = 600000
           = 10000 * COIN                          for h >= 600000   (tail emission)
```

i.e. 500,000 PEPE initial reward, halving every 100,000 blocks for six eras,
then a constant 10,000 PEPE tail forever.

### The enforcement chain (money-creation call graph)

```
ProcessNewBlock
  └─ AcceptBlock
       ├─ AcceptBlockHeader
       │    ├─ CheckBlockHeader → CheckAuxPowProofOfWork      (PoW / AuxPoW)   pepecoin.cpp:89
       │    ├─ CheckIndexAgainstCheckpoint                     validation.cpp:3002
       │    └─ ContextualCheckBlockHeader                      validation.cpp:3080
       │         ├─ legacy/auxpow activation gate
       │         ├─ nBits == GetNextWorkRequired               (difficulty)     pow.cpp:30
       │         ├─ timestamp > MedianTimePast, ≤ nAdjustedTime+2h
       │         └─ version/BIP66/BIP65 height gate
       ├─ CheckBlock                                           validation.cpp:2939
       │    ├─ merkle root + CVE-2012-2459 mutation check
       │    ├─ first-tx-is-coinbase / single-coinbase
       │    └─ for each tx: CheckTransaction(tx, state, true)  validation.cpp:527
       │         ├─ vout value < 0            → bad-txns-vout-negative
       │         ├─ vout value > MAX_MONEY    → bad-txns-vout-toolarge
       │         ├─ Σvout MoneyRange          → bad-txns-txouttotal-toolarge
       │         └─ duplicate inputs (set)    → bad-txns-inputs-duplicate   (CVE-2018-17144)
       └─ ContextualCheckBlock                                 validation.cpp:3124
            └─ BIP34 height-in-coinbase, finality, weight
  ...then ConnectTip → ConnectBlock                            validation.cpp:1783
       ├─ CheckBlock (again)
       ├─ BIP30 duplicate-txid guard                           validation.cpp:1881
       ├─ for each non-coinbase tx:
       │    ├─ view.HaveInputs(tx)                             coins.cpp:284
       │    ├─ CheckInputs → Consensus::CheckTxInputs          validation.cpp:1415
       │    │    ├─ coinbase maturity                          → bad-txns-premature-spend-of-coinbase
       │    │    ├─ per-input MoneyRange(nValueIn)             → bad-txns-inputvalues-outofrange
       │    │    ├─ nValueIn < GetValueOut                     → bad-txns-in-belowout
       │    │    ├─ nTxFee < 0                                 → bad-txns-fee-negative
       │    │    └─ MoneyRange(nFees)                          → bad-txns-fee-outofrange
       │    ├─ script verification (P2SH/DERSIG/CLTV/CSV flags)
       │    ├─ nFees += GetValueIn(tx) - GetValueOut(tx)
       │    └─ UpdateCoins → Spend inputs / add outputs        validation.cpp:1366
       └─ blockReward = nFees + GetPepecoinBlockSubsidy(height,...)   validation.cpp:1996
            └─ if coinbase.GetValueOut() > blockReward → bad-cb-amount  ← THE supply cap
```

The **single consensus condition that caps money creation** is
`validation.cpp:1997`:

```c
CAmount blockReward = nFees + GetPepecoinBlockSubsidy(pindex->nHeight, chainparams.GetConsensus(pindex->nHeight), hashPrevBlock);
if (block.vtx[0]->GetValueOut() > blockReward)
    return state.DoS(100, error("ConnectBlock(): coinbase pays too much ..."), REJECT_INVALID, "bad-cb-amount");
```

Because `nFees` is the sum of `(GetValueIn − GetValueOut)` over transactions
each already validated by `CheckTxInputs` to have `in ≥ out` and
`MoneyRange(fee)`, and `subsidy` is bounded and non-negative, the coinbase can
never legitimately claim more than the network created. This condition, plus
`CheckTransaction`’s output-range checks and `CheckTxInputs`’ input/fee-range
checks, is the whole of the supply protection. All three were exercised
adversarially (finding PEP-INV tests) and hold.

---

## Consensus Attack Surface Map

| Subsystem | Files | Pepecoin delta vs Dogecoin 1.14.9 | Verdict |
|-----------|-------|-----------------------------------|---------|
| Amounts / MoneyRange | `amount.h` | rename + copyright only | identical logic |
| Tx checks | `validation.cpp CheckTransaction` | rename only | identical |
| Input/fee checks | `validation.cpp Consensus::CheckTxInputs` | rename only | identical |
| Coinbase/subsidy | `pepecoin.cpp`, `validation.cpp ConnectBlock` | subsidy schedule constants; difficulty magic numbers `157500→1250`, `145000→1000` | see PoW analysis |
| UTXO / coins | `coins.cpp/.h`, `undo.h`, `txdb.cpp`, `compressor.cpp` | **byte-identical** | identical |
| Merkle | `consensus/merkle.cpp` | **byte-identical** | CVE-2012-2459 defense present |
| Script | `script/interpreter.cpp` | **byte-identical** | identical |
| AuxPoW | `auxpow.cpp` | **byte-identical**; `auxpow.h` rename-only | identical (upstream fixes present) |
| PoW / difficulty | `pow.cpp`, `pepecoin.cpp` | difficulty magic numbers only | see PEP-002 analysis |
| Headers / versions | `primitives/pureheader.h`, `block.h` | rename/copyright only | identical |
| Serialization | `serialize.h`, `streams.h` | **byte-identical** | identical |
| Difficulty math | `arith_uint256.cpp` | **byte-identical** | identical |
| Checkpoints | `checkpoints.cpp` | **byte-identical**; call-site reordered | see PEP-002 |
| Versionbits | `versionbits.cpp` | **byte-identical** | identical |
| Compact blocks | `blockencodings.cpp/.h` | **byte-identical** | assert-hardening (b5dec9637) present |
| Chain params | `chainparams.cpp` | heavily Pepecoin-specific | audited; consistent |
| Network time | `timedata.cpp` | **older pre-1.14.8 variant with `abs64`** | **PEP-001** |
| P2P message handling | `net_processing.cpp` | **based on Dogecoin 1.14.7** (missing 1.14.8/9 hardening) | **PEP-005** (non-consensus DoS) |
| Fees / policy | `pepecoin-fees.cpp`, `policy/policy.*` | rename only | identical logic |

---

## Dogecoin Differential Analysis

**Baseline:** Pepecoin 1.1.0’s consensus tree matches **Dogecoin Core
1.14.7 / 1.14.9** most closely (measured by minimal semantic diff across
`src/`). The following files are **byte-for-byte identical** to Dogecoin
1.14.9 and therefore inherit its consensus behavior exactly:

```
src/auxpow.cpp        src/coins.cpp          src/coins.h
src/undo.h            src/txdb.cpp           src/compressor.cpp
src/consensus/merkle.cpp   src/primitives/pureheader.cpp
src/script/interpreter.cpp  src/versionbits.cpp
src/checkpoints.cpp   src/serialize.h        src/arith_uint256.cpp
src/blockencodings.cpp/.h   src/dbwrapper.cpp
```

The **only** semantic (non-rename) deltas in consensus-critical code are:

1. **Subsidy schedule** (`pepecoin.cpp` `GetPepecoinBlockSubsidy`): Pepecoin’s
   own reward curve. Logic structure identical to Dogecoin’s; only the
   constants differ. Verified safe (see PEP-INV-8).

2. **Difficulty magic numbers** (`pow.cpp`, `pepecoin.cpp`):
   `157500 → 1250` and `145000 → 1000`. Analyzed in PEP-002 / proof-of-absence;
   benign because mainnet `nPowTargetTimespan == nPowTargetSpacing == 60` makes
   the retarget interval 1 regardless.

3. **Checkpoint-check placement** (`validation.cpp`): Pepecoin retains the
   **pre-`c4e76a369`** ordering (checkpoint check in `AcceptBlockHeader` /
   `TestBlockValidity`, not after the `nBits` check in
   `ContextualCheckBlockHeader`). Diagnostic-only; PEP-002.

4. **`timedata.cpp`**: Pepecoin ships the **pre-1.14.8** version using
   `abs64()`. Dogecoin replaced this in commit *“Avoid the use of abs64 in
   timedata”* (1.14.8). This is **PEP-001**.

5. **`net_processing.cpp`**: based on **Dogecoin 1.14.7** — the diff is 14 lines
   vs 1.14.7 but 463 lines vs 1.14.9. The 1.14.8/1.14.9 P2P hardening
   (`6e74282e2` one-getheaders-per-INV, `c9d9486c8` reduced
   `MAX_PEER_TX_ANNOUNCEMENTS`, `b961bab13`/`fd72ba453`/`e9128ec21` orphan &
   header-sync hardening) is **absent**. This is **PEP-005** (non-consensus).

**Upstream security-relevant fixes checked for presence:**

| Upstream commit | Description | Status in Pepecoin |
|-----------------|-------------|--------------------|
| `b85849d0d` | Check auxpow PoW *before* the auxpow structure | **PRESENT** (`pepecoin.cpp:118` PoW then `:121` structure; and `auxpow.cpp:105` `vin.empty()` guard) |
| `b5dec9637` | cmpctblk asserts → handled failures | **PRESENT** (`blockencodings.cpp` byte-identical to 1.14.9) |
| CVE-2012-2459 | merkle mutation | **PRESENT** (`CheckBlock` `mutated` guard, `validation.cpp:2961`) |
| CVE-2018-17144 | duplicate-input inflation | **NOT APPLICABLE / FIX PRESENT** — `CheckBlock` calls `CheckTransaction(*tx, state, true)` and the header default is `true` (`validation.h:394`); no path validates block txs with the check disabled |
| CVE-2010-5139 | output-sum overflow | **FIX PRESENT** (`CheckTransaction` per-output `MoneyRange` + running-sum `MoneyRange`) |
| *“avoid masking difficulty errors”* `c4e76a369` | 1.14.9 diagnostic | **ABSENT** → PEP-002 (diagnostic only) |
| *“avoid abs64 in timedata”* | 1.14.8 | **ABSENT** → PEP-001 |

---

## Findings

### Finding PEP-001 — `abs64(INT64_MIN)` network-time UB, weaponized to a remote node/network freeze

**Severity:** **HIGH** (base MEDIUM, escalated by working exploit) &nbsp;•&nbsp;
**Confidence:** CONFIRMED (end-to-end PoC on an isolated regtest node) &nbsp;•&nbsp;
**Status:** Fixed in this branch (upstream-equivalent patch applied)

> **Weaponization (see `POC_PEP-001_timewarp.md` for the full walkthrough).**
> 8 hostile `version` messages from distinct loopback IPs, each carrying
> `nTime = INT64_MIN + node_second`, drive the node's `getnetworkinfo.timeoffset`
> to `-9223372036854775808` (INT64_MIN). The poisoned `GetAdjustedTime()` then
> makes `ContextualCheckBlockHeader` reject **every** real-timestamp block as
> `time-too-new` — the node can neither mine nor accept the honest chain and is
> **frozen at its tip until restart** (`setKnown` caps at 200 and never evicts,
> so the poison is sticky). The same attack repeated across listening nodes
> (miners, exchanges, seeds) is a network-wide block-production/propagation halt.
> The identical exploit against the fixed binary leaves `timeoffset = 0` and the
> node keeps mining. **Honest ceiling: HIGH (liveness/DoS), not CRITICAL — a
> frozen node rejects *all* blocks and never accepts a false chain, so this
> cannot mint coins, split consensus, or double-spend.**

**File / Function / Lines:** `src/timedata.cpp` — `abs64()` (pre-fix line 41)
and `AddTimeData()` (pre-fix line 85). Reached from
`net_processing.cpp:1546` (`AddTimeData(pfrom->addr, nTimeOffset)`), where
`nTimeOffset = nTime − GetTime()` and `nTime` is the peer-supplied `version`
timestamp read at `net_processing.cpp:1416`.

**Invariant violated:** *No set of peers may move this node’s network-adjusted
time by more than `±maxtimeadjustment` (default 70 minutes).* This clamp is the
sole defense bounding peer influence over `GetAdjustedTime()`.

**Technical explanation.** The clamp was written as:

```c
static int64_t abs64(int64_t n) { return (n >= 0 ? n : -n); }
...
if (abs64(nMedian) <= std::max<int64_t>(0, GetArg("-maxtimeadjustment", DEFAULT_MAX_TIME_ADJUSTMENT)))
    nTimeOffset = nMedian;
```

`abs64(INT64_MIN)` computes `-INT64_MIN`, which overflows a signed 64-bit
integer — **undefined behavior**. In practice it evaluates back to `INT64_MIN`
(a *negative* number), so the guard `abs64(nMedian) <= max_adjustment` is
satisfied (negative ≤ positive) and the node accepts
`nTimeOffset = INT64_MIN`, wildly outside the intended `±70min` bound.

**Attacker-controlled input.** A peer’s `version` message carries an
attacker-chosen `int64_t nTime`. Setting `nTime` such that the derived offset
is `INT64_MIN` (or near it) makes that peer contribute an `INT64_MIN` sample.
`AddTimeData` de-duplicates samples **by source IP** (`static std::set<CNetAddr>
setKnown`), so an attacker needs enough *distinct-IP* inbound peers to own the
**median** of the offset filter (which seeds with one `0` sample and updates
when the sample count is odd and ≥ 5). A handful-to-dozens of distinct inbound
addresses — a routine Sybil posture — suffices to place `INT64_MIN` at the
median early, before honest samples accumulate.

**Consensus path / impact.** `GetAdjustedTime() = GetTime() + nTimeOffset`.
Driving `nTimeOffset` to `INT64_MIN` makes `GetAdjustedTime()` a garbage
(extremely negative / wrapped) value. That value is used in
`ContextualCheckBlockHeader` (`validation.cpp:3110`):

```c
if (block.GetBlockTime() > nAdjustedTime + 2 * 60 * 60)
    return state.Invalid(..., "time-too-new", ...);
```

With `nAdjustedTime` hugely negative, **every** newly received block’s
timestamp exceeds `nAdjustedTime + 2h`, so the node rejects all new blocks as
`time-too-new` and stops following the chain — a remotely-triggerable
availability / consensus-isolation DoS, and a useful primitive for eclipse
attacks. `GetAdjustedTime()` also feeds the miner’s block `nTime`.

**Why existing validation doesn’t stop it.** The clamp *is* the validation; the
`abs64` UB is precisely what defeats it. The median-filter quirk (issue #4521,
the filter stops updating past 200 samples) offers only partial, incidental
protection and does not prevent early poisoning.

**Local reproduction (deterministic).** `src/test/pepecoin_timedata_tests.cpp`
feeds four distinct addresses an `INT64_MIN` offset:

```
maxtimeadjustment = 4200
resulting nTimeOffset = -9223372036854775808          ← escaped the ±4200 clamp
GetAdjustedTime() = -9223372035068340509              ← garbage adjusted time
```

The network-facing shape is documented in
`qa/rpc-tests/pepecoin_timewarp_poc.py` (regtest only; note the loopback
`setKnown`-by-IP dedup means the *deterministic* confirmation is the unit test).

**Observed result:** the clamp is bypassed exactly as predicted; after the fix
(below) the same test yields `nTimeOffset = 0` and a sane `GetAdjustedTime()`.

**Recommended patch (applied).** Remove `abs64` and compare against the signed
bounds directly, matching upstream Dogecoin 1.14.8+:

```c
int64_t max_adjustment = std::max<int64_t>(0, GetArg("-maxtimeadjustment", DEFAULT_MAX_TIME_ADJUSTMENT));
if (nMedian >= -max_adjustment && nMedian <= max_adjustment) {
    nTimeOffset = nMedian;
}
```

(The two other `abs64` uses in the warning path were likewise replaced with the
signed-range form; `abs64` and the `boost/foreach.hpp` include were removed.
Post-fix `timedata.cpp` is functionally identical to Dogecoin 1.14.9.)

**Regression test:** `src/test/pepecoin_timedata_tests.cpp`
(`time_offset_must_stay_clamped`) — fails on the vulnerable code, passes after
the fix.

**Upstream Dogecoin comparison:** Dogecoin removed `abs64` from `timedata.cpp`
in the 1.14.8 cycle (*“Avoid the use of abs64 in timedata”* / commit
`0cc85e45f`). Pepecoin forked the pre-1.14.8 file and did not carry the fix.

---

### Finding PEP-005 — Missing Dogecoin 1.14.8/1.14.9 P2P hardening (network DoS)

**Severity:** MEDIUM &nbsp;•&nbsp; **Confidence:** CONFIRMED &nbsp;•&nbsp;
**Status:** Open &nbsp;•&nbsp; **Class:** Peer-facing DoS (non-consensus)

**Context.** Pepecoin’s `src/net_processing.cpp` is based on **Dogecoin
1.14.7** (14-line diff vs 1.14.7 vs. 463-line diff vs 1.14.9). It therefore
lacks the peer-DoS hardening that Dogecoin added in 1.14.8/1.14.9. Two concrete
gaps were confirmed by source:

**(a) Forced `getheaders` per block entry in an INV** — missing upstream
`6e74282e2` (*“Only send a getheaders for one block in an INV”*).
`src/net_processing.cpp:1769`, inside the `for (CInv& inv : vInv)` loop:

```c
// Pepecoin: We force this check, in case we're only connected to nodes that send invs
RequestHeadersFrom(pfrom, connman, pindexBestHeader, inv.hash, true);   // fforceQuery = true
```

Because `fforceQuery = true`, `RequestHeadersFrom` bypasses its own
`nPendingHeaderRequests > 0` rate-limit and pushes a full-locator `getheaders`
for **every** unknown block hash in the INV. A single crafted `inv` message can
carry up to `MAX_INV_SZ = 50,000` fabricated block hashes (all pass
`!fAlreadyHave`), eliciting up to 50,000 forced `getheaders` — a self-inflicted
outbound-bandwidth amplification. Upstream 1.14.9 issues **one** `getheaders`
per INV (using the last unknown hash), outside the loop
(`net_processing.cpp:1770` there).

**(b) `MAX_PEER_TX_ANNOUNCEMENTS` too large** — missing upstream `c9d9486c8`
(*“policy: reduce MAX_PEER_TX_ANNOUNCEMENTS”*). `src/net_processing.cpp:55`:

```c
static constexpr int32_t MAX_PEER_TX_ANNOUNCEMENTS = 2 * MAX_INV_SZ;   // = 100,000
```

Upstream 1.14.9 sets this to **5,000** (`net_processing.cpp:63`). Pepecoin
allows a single peer to hold **20×** more pending tx-announcement state
(`m_tx_announced` / `m_tx_process_time` maps in `CNodeState::TxDownloadState`),
i.e. bounded per-peer memory growth an order of magnitude above upstream’s
intended cap.

**Impact.** Remotely-triggerable bandwidth/memory pressure on a node from a
connected peer. **No consensus effect** — these paths never influence block or
transaction validity, only resource usage. Rated MEDIUM per the node-DoS rubric.

**Recommended patch.** Cherry-pick `6e74282e2` (hoist the forced `getheaders`
out of the INV loop, one per message using the last unknown hash) and
`c9d9486c8` (`MAX_PEER_TX_ANNOUNCEMENTS = 5000`). Both apply cleanly against the
1.14.7-based file. More broadly, rebasing `net_processing.cpp` onto Dogecoin
1.14.9 would pick up the full orphan-processing and header-sync hardening set
(`b961bab13`, `fd72ba453`, `e9128ec21`).

**Upstream Dogecoin comparison.** Both fixes shipped in the Dogecoin 1.14.8 →
1.14.9 window; Pepecoin forked `net_processing.cpp` before them.

---

### Finding PEP-002 — Upstream difficulty-error “masking” fix not ported (diagnostic-only)

**Severity:** LOW &nbsp;•&nbsp; **Confidence:** CONFIRMED &nbsp;•&nbsp;
**Status:** Open (no security impact; recommend porting for parity)

**File / Function / Lines:** `src/validation.cpp` —
`ContextualCheckBlockHeader` (~3100) and `AcceptBlockHeader` (~3240),
`TestBlockValidity` (~3418).

**Explanation.** Upstream Dogecoin commit `c4e76a369` (*“validation: avoid
masking of difficulty adjustment errors”*, cherry-picked from Bitcoin
`215fc33d`) moved the pre-checkpoint fork rejection to run **after** the
`nBits` difficulty check inside `ContextualCheckBlockHeader`, so that difficulty
violations on chains branching before the last checkpoint are still reported as
`bad-diffbits` rather than masked by `bad-fork-prior-to-checkpoint`. Pepecoin
retains the **older ordering** (`CheckIndexAgainstCheckpoint` invoked from
`AcceptBlockHeader`/`TestBlockValidity`, before the difficulty check).

**Why this is only LOW.** In **both** orderings the offending block is
**rejected** — no invalid block is accepted, and no consensus split results.
The only observable difference is *which* rejection reason is reported for
pre-checkpoint forks (and the `REJECT_CHECKPOINT` reject code is not surfaced).
This is a diagnostics/parity issue, not an exploitable weakness.

**Recommended patch:** port `c4e76a369` verbatim (move the checkpoint block into
`ContextualCheckBlockHeader` after the `nBits` check and drop the calls in
`AcceptBlockHeader`/`TestBlockValidity`).

---

### Finding PEP-003 — `MAX_MONEY` is a per-value sanity cap, not a supply bound

**Severity:** LOW / INFO &nbsp;•&nbsp; **Confidence:** CONFIRMED &nbsp;•&nbsp;
**Status:** Open (informational; the one place it matters is already handled)

**File / Lines:** `src/amount.h:32` (`MAX_MONEY = 1e18 koinu = 10,000,000,000
PEPE`); emission schedule `src/pepecoin.cpp:127`.

**Explanation.** Pepecoin’s emission is unbounded (500,000-PEPE initial reward,
six halvings, then a **perpetual 10,000-PEPE tail**). The cumulative supply
therefore:

* crosses `MAX_MONEY` (10 billion PEPE) at **height ≈ 20,001** (~14 days at
  1-minute blocks), and
* crosses **`INT64_MAX` koinu** (9.223e18) at **height ≈ 375,740**
  (~261 days).

So `MAX_MONEY` is roughly **10× smaller than the circulating supply** and does
**not** function as a total-supply ceiling — it is only a per-output /
per-transaction / per-fee sanity bound, which is correct and consistent with
Dogecoin’s design (Dogecoin has the same `MAX_MONEY`). This is safe for
consensus because **no consensus code sums the whole supply into a `CAmount`**;
individual amounts and per-block rewards remain far below `MAX_MONEY`.

**Where it could have bitten (and doesn’t):** `gettxoutsetinfo` /
`GetUTXOStats` sums the entire UTXO set. Pepecoin (via Dogecoin) already
accumulates this into an **`arith_uint256`**, not a `CAmount`
(`rpc/blockchain.cpp:916,947`), and formats it through a dedicated
`ValueFromAmount(const arith_uint256&)` overload
(`rpc/server.cpp:146`). So the RPC does **not** overflow. **Proof of absence
confirmed.**

**Latent (non-exploitable) note.** The block-fee accumulator in `ConnectBlock`
(`nFees += ...`, `validation.cpp:1974`) is an `int64_t` with no per-iteration
`MoneyRange` guard (identical to Bitcoin/Dogecoin). Overflowing it would
require a single block whose fees sum past `INT64_MAX` (≈ 92 billion PEPE of
fees in one block) — not reachable in practice, and even if it were, a positive
overflow *reduces* the computed `blockReward`, making the `bad-cb-amount` check
**stricter**, not weaker (no inflation). Documented for completeness in the
`block_fee_accumulator_headroom` unit test. Recommend adding a
`MoneyRange(nFees)` guard in `ConnectBlock` as defense-in-depth given the
unbounded supply.

---

### Finding PEP-004 — Stale consensus scaffolding and undocumented magic numbers

**Severity:** INFO &nbsp;•&nbsp; **Confidence:** CONFIRMED &nbsp;•&nbsp;
**Status:** Open (hygiene)

* `IsSuperMajority` (`validation.cpp:3373`) is **defined but never called** —
  BIP66/BIP65 are enforced purely by height (`validation.cpp:3116`). Dead code.
* Numerous `//PEPE TODO Magic number` markers remain around consensus heights
  (`pow.cpp:46`, `validation.cpp` maturity comment, `auxpow_tests.cpp:184`
  `GetConsensus(371337)`), and `chainparams.cpp:84` still has
  `BIP34Hash = uint256S("0x00")` with a “Replace … after mainnet launches”
  TODO. `BIP34Hash = 0x00` currently keeps BIP30 enforced for **all** blocks
  (safe); if a real hash is ever set, verify the BIP30-skip interacts correctly
  with the height-based BIP34 coinbase-height rule (it does, but the TODO
  should be closed deliberately).
* The legacy hash-derived subsidy branch in `GetPepecoinBlockSubsidy`
  (`!fSimplifiedRewards`, using `strtol` on the previous block hash +
  `generateMTRandom`) is **dead on all three networks** (proof of absence
  below) but remains compiled in. Recommend removing to shrink the
  consensus surface.

**Inherited latent items (shared verbatim with upstream Dogecoin — not
Pepecoin regressions, noted for completeness):**

* `CTxMemPool::removeForReorg` (`txmempool.cpp:556`) reads
  `coins->nHeight` **before** the `if (!coins || ...)` null-check on the next
  line — a deref-before-null-check that would crash if `AccessCoins` returned
  `NULL`. The line is **byte-identical to Dogecoin 1.14.9** (`txmempool.cpp` is
  a rename-only diff). In practice `coins` is non-NULL here (a mempool tx’s
  coinbase input is present in the post-reorg view), and the `assert(coins)` is
  debug-only (`nCheckFrequency != 0`). Recommend reordering the null-check
  before the deref regardless.
* `chainTxData` is left empty (`ChainTxData{ }`) on **mainnet**
  (`chainparams.cpp:187`) and **testnet** (`:318`), so
  `GuessVerificationProgress` / `getblockchaininfo.verificationprogress`
  effectively always reports `1.0`. Cosmetic/monitoring only; no consensus or
  IBD-correctness effect.
* `getcoincount` is registered in the RPC conversion table
  (`rpc/client.cpp:134`) under a name that does not match the registered RPC,
  making the numeric argument un-convertible from `pepecoin-cli`. Non-consensus
  usability bug.

---

## Proof of Absence — hypotheses investigated and refuted

For each promising attack idea that **failed**, the exact enforcing code is
recorded. These are as important as the positive findings.

| # | Hypothesis | Why it fails | Enforcing code |
|---|-----------|--------------|----------------|
| 1 | Coinbase overpays subsidy (`subsidy+1`, `2×`, `MAX_MONEY`, `MAX_MONEY+1`, split outputs) | Rejected `bad-cb-amount` / `bad-txns-vout-*`; confirmed by test `coinbase_may_not_overpay_subsidy` (all rejected, exact-subsidy accepted) | `validation.cpp:1997`, `CheckTransaction` 542-548 |
| 2 | Coinbase claims fees never paid, or `subsidy+fee+1` | Rejected; `nFees` counts only real `in−out`; test `coinbase_may_not_overpay_fees` | `validation.cpp:1974,1996` |
| 3 | Duplicate-input inflation (CVE-2018-17144) | `CheckTransaction(...,true)` in `CheckBlock`; default arg is `true`; test `duplicate_inputs_rejected_in_block` | `validation.cpp:551-558,2984`, `validation.h:394` |
| 4 | Input-sum overflow past `MAX_MONEY` | Per-input + running `MoneyRange`; test `checktxinputs_amount_ranges` | `validation.cpp:1441-1443` |
| 5 | Outputs > inputs / negative fee | `nValueIn < GetValueOut` and `nTxFee < 0` rejected; test asserts `bad-txns-in-belowout` | `validation.cpp:1447-1454` |
| 6 | Negative / >MAX_MONEY outputs | `bad-txns-vout-negative` / `-toolarge`; `GetValueOut` throws, but `CheckTransaction` runs first everywhere | `transaction.cpp:83-93`, `validation.cpp:542-548` |
| 7 | Immature coinbase spend, incl. across the 30→240 maturity change | Maturity keyed on spent coin’s height; test `coinbase_maturity_enforced` (regtest maturity 60 confirmed) | `validation.cpp:1431-1437` |
| 8 | `subsidy >> halvings` shift UB (`halvings ≥ 64`), negative/huge height | `h < 6*interval` guard caps `halvings ≤ 5`; boundaries tested `subsidy_schedule_boundaries` | `pepecoin.cpp:142-147` |
| 9 | Legacy random-subsidy branch becomes live | `fSimplifiedRewards == true` on every reachable params object/height on main/test/regtest; test asserts it across the full tree | `chainparams.cpp:123,128,249,256,371`; test `subsidy_schedule_boundaries` |
| 10 | `GetConsensus(negative int → uint32)` returns wrong params | Tree walk saturates to the auxpow branch for huge heights; height-0 flags (`BIP66/65/30/34`) identical across all tree nodes (all are copies); tested `consensus_param_tree_boundaries` | `chainparams.cpp:444-456` |
| 11 | BIP30 duplicate-txid can be disabled | `fEnforceBIP30 = true`, ANDed with `BIP34Hash == 0x00` test which never matches a real hash; regtest `BIP34Height=1e8` unreachable → BIP30 always on | `validation.cpp:1869-1888` |
| 12 | AuxPoW negative / out-of-range chain index | `nChainIndex != getExpectedIndex(...)` mismatch; `nIndex != 0` guard; test `auxpow_adversarial_chain_index` (INT_MIN/-1/alias all rejected) | `auxpow.cpp:84,157` |
| 13 | AuxPoW chain-branch length overflow (`1<<h`) | `vChainMerkleBranch.size() > 30` rejected before any shift; test `auxpow_adversarial_branch_length` (h=30 ok, 31/32/33/64 rejected) | `auxpow.cpp:90` |
| 14 | AuxPoW version encoding smuggles a non-auxpow/non-legacy block past the chain-ID or BIP66/65 gate | Strict chain-ID rule requires our chain id for non-legacy; `GetBaseVersion() = ver % 256` never ≥4 for low-byte <4; legacy∧auxpow impossible; test `auxpow_adversarial_version_encoding` (incl. negative nVersion) | `pepecoin.cpp:95`, `pureheader.h:88-155` |
| 15 | AuxPoW flag/payload mismatch or auxpow affecting block hash | `no auxpow on block with auxpow version` / vice-versa; block hash uses pure header only; test `auxpow_flag_and_payload_must_agree` + serialization round-trip | `pepecoin.cpp:102-116`, `block.h:36-47` |
| 16 | Truncated/oversized AuxPoW header deserialization | Round-trips exactly; every truncation throws; test `auxpow_header_serialization_roundtrip` | `block.h SerializationOp`, `validation.cpp:1181` |
| 17 | AuxPoW parent-PoW checked after untrusted structure (b85849d0d) | Pepecoin checks PoW **then** structure; `vin.empty()` guard present | `pepecoin.cpp:118-121`, `auxpow.cpp:105` |
| 18 | Difficulty magic-number cliff at height 1000 / 1250 | Mainnet `timespan==spacing==60` ⇒ retarget interval 1 in both branches, so `145000→1000` / `157500→1250` cause no behavioral cliff; regtest clamps to `powLimit` floor | `pow.cpp:48-51`, `pepecoin.cpp:41-87` |
| 19 | Negative `nActualTimespan` corrupts `arith_uint256 *= int64` | Min/max clamps make `nModulatedTimespan` positive before multiply on every network | `pepecoin.cpp:55-81` |
| 20 | Non-canonical `nBits` inflates work / passes PoW | `nBits != GetNextWorkRequired` equality rejects; `CheckProofOfWork` rejects negative/overflow/zero/`>powLimit`; `arith_uint256` byte-identical | `validation.cpp:3102`, `pow.cpp:113-130` |
| 21 | CVE-2012-2459 merkle mutation marks block permanently invalid | `mutated` flag → `bad-txns-duplicate` (not `BLOCK_FAILED`) | `validation.cpp:2952-2962` |
| 22 | `gettxoutsetinfo` supply overflow | Accumulated in `arith_uint256`, formatted via dedicated overload | `rpc/blockchain.cpp:916,947`, `rpc/server.cpp:146` |
| 23 | Compact-block peer data reaches an `assert` (b5dec9637) | Handled-failure returns present; only `assert(index<...)` is internally bounds-guarded; `blockencodings.cpp` byte-identical to 1.14.9 | `blockencodings.cpp` |
| 24 | Reorg breaks value conservation | Live regtest: connect→invalidate→reconsider returns UTXO hash + total exactly to baseline | see live experiment below |

### Live regtest UTXO-conservation experiment

```
h1000 baseline : total_amount 148166250.0  hash_serialized ab6f52...
+10 blocks     : 148266250.0
spend + 5 blks : 148316250.0  hash_serialized 0ae68e...
invalidate 1001: 148166250.0  hash_serialized ab6f52...   ← exact baseline
reconsider     : 148316250.0  hash_serialized 0ae68e...   ← exact restoration
```

Supply and the full serialized UTXO set return **bit-for-bit** to the correct
state across a disconnect/reconnect cycle — no coins created, destroyed, or
resurrected. Regtest supply at height 1000 (148,166,250 PEPE) matches the
subsidy schedule exactly.

---

## Economic-Destruction Attack Campaign

This section documents the provenance-agnostic assault on the running money
machine — the answer to *"can PEPE be minted, duplicated, or destroyed?"* — as
opposed to the differential analysis above. Nothing here was excused for
matching upstream.

### 1. Empirical block-level attacks (real `ProcessNewBlock` → `ConnectBlock`)

`src/test/pepecoin_economic_attack_tests.cpp` constructs whole adversarial
blocks, submits them through the actual acceptance pipeline on a regtest chain,
and after each attack asserts **(a)** the block was rejected, **(b)** the chain
tip is unchanged, and **(c)** the *entire UTXO set value* (summed by cursor) is
unchanged. Every one of the following was **rejected with supply intact**:

| # | Attack | Rejecting check |
|---|--------|-----------------|
| 1 | coinbase pays `subsidy + 1` | `bad-cb-amount` (`validation.cpp:1997`) |
| 2 | coinbase pays `1000 × subsidy` | `bad-cb-amount` |
| 3 | coinbase split into two outputs summing to `subsidy + 1` | `bad-cb-amount` |
| 4 | coinbase claims `MAX_MONEY` (< supply, > reward) | `bad-cb-amount` |
| 5 | non-coinbase tx with `outputs > inputs` | `bad-txns-in-belowout` (`:1447`) |
| 6 | tx with duplicate inputs (spend same coin twice in one tx) | `bad-txns-inputs-duplicate` (`:552`) |
| 7 | two txs in one block spending the same coinbase | `bad-txns-inputs-missingorspent` (`:1944`) |
| 8 | coinbase claims a fee no tx paid | `bad-cb-amount` |
| 9 | coinbase pays `subsidy + realfee + 1` | `bad-cb-amount` |
| 10 | tx spending an output created **later** in the same block | `bad-txns-inputs-missingorspent` |
| 11 | block spending its own/immature coinbase | `bad-txns-premature-spend-of-coinbase` (`:1434`) |
| 12 | **double-spend across a reorg** (spend coinbase, invalidate block, re-spend, then spend again) | second spend rejected `missingorspent` |

A matched honest control (coinbase = `subsidy + realfee`, valid in-order
intra-block chain, restored coinbase spendable exactly once post-reorg) is
**accepted**, and supply grows by **exactly the subsidy** (fees are recycled,
never created). The reorg case explicitly verifies the spent coinbase is
restored on disconnect and is then spendable **exactly once** — no duplication,
no burn.

### 2. Regtest supply-conservation churn (2,020 blocks)

A private regtest node was driven from height 0 to 2,020 with continuous
UTXO churn (self-sends every 25 blocks), crossing **every** regtest halving
(150/300/450/600/750) and entering **tail emission** (height 900+). Final
`gettxoutsetinfo.total_amount` equals the independently-computed cumulative
issuance **exactly**:

```
end height 2020   actual supply 158366250.00000000 PEPE
                  expected issuance 158366250       PEPE   →  MATCH (0 koinu drift)
```

Combined with the earlier connect→invalidate→reconsider round-trip (supply and
serialized UTXO hash return bit-for-bit to baseline), the supply invariant holds
across creation, spending, and deep reorg.

### 3. The one unguarded accumulator (`nFees`) — proof it cannot mint

`ConnectBlock` accumulates `nFees += GetValueIn(tx) − GetValueOut(tx)`
(`validation.cpp:1974`) with **no per-iteration `MoneyRange` guard**. This is the
single most suspicious spot given Pepecoin's supply can exceed `INT64_MAX` koinu.
It is nonetheless **not an inflation vector**, by a modular-arithmetic argument:

* Every addend is a per-transaction fee that `Consensus::CheckTxInputs`
  independently validates to be `≥ 0` and `≤ MAX_MONEY`, with each tx's total
  input value `≤ MAX_MONEY` and `nValueIn ≥ GetValueOut` (`validation.cpp:1442–1457`).
  So the *true* fee sum `S = Σ fees ≥ 0`.
* On two's-complement `int64`, the stored value is `S mod 2⁶⁴` reinterpreted
  signed `= S − k·2⁶⁴` for some `k ≥ 0` (because `S ≥ 0`). Therefore
  **`nFees_stored ≤ S` always.**
* Hence `blockReward = nFees_stored + subsidy ≤ S + subsidy = true_reward`, and
  the coinbase gate `coinbase_out ≤ blockReward` (`:1997`) enforces
  `coinbase_out ≤ subsidy + real_fees`. An overflow can only make `blockReward`
  **smaller** (or negative → coinbase rejected) — a miner-only *loss*, never a mint.
* Concretely: 10 txs each paying a `1e18`-koinu fee give `S = 1e19 > INT64_MAX`;
  wrapped signed this is `≈ −8.45e18`, so `blockReward` is negative and any
  positive coinbase is rejected. And any single tx large enough to matter is
  itself rejected first by `CheckInputs`, which aborts the whole block and
  discards the tainted `nFees`.

A `MoneyRange(nFees)` guard is still recommended as defense-in-depth (PEP-003),
but its absence is provably non-exploitable for inflation.

### 4. Timestamp / difficulty manipulation cannot accelerate emission

Because the subsidy is a pure function of **height** (`pepecoin.cpp:129,142`),
mining faster only front-loads the fixed per-height schedule — **total minted =
Σ subsidy(h) is invariant to block timing.** No coin is created beyond
`subsidy + fees` regardless of how blocks are spaced. Separately, the per-block
Digishield difficulty drop is hard-clamped: `nModulatedTimespan = 60 +
(nActual−60)/8` clamped to `[45, 90]` (`pepecoin.cpp:53–56,69–72`), so the
maximum target increase is `90/60 = 1.5×` (a 33.3% difficulty drop) per block,
bounded further by `MedianTimePast` and the `+2h` future-time limit. On mainnet
`fPowAllowMinDifficultyBlocks = false` in **every** params-tree node, so the
testnet/regtest min-difficulty shortcut cannot leak to mainnet.

### 5. Independent adversarial fan-out (corroboration)

A separate multi-agent attack campaign independently attacked five economic
surfaces — block conservation, UTXO double-spend/overwrite, reorg accounting,
subsidy/emission/difficulty, and amount serialization/compression — with each
finding **adversarially verified**. Result: **zero economically-destructive
findings**, with the exact enforcing code cited for **24 refuted attack
vectors** (folded into the proof-of-absence table above). Notable confirmations:
`CompressAmount`/`DecompressAmount` is an exact bijection over `[0, MAX_MONEY]`
and is on-disk only (never on the wire); `DisconnectBlock` aborts the reorg on
any undo inconsistency rather than continuing with corrupted state; and
`ApplyTxInUndo`'s three overwrite/missing/available guards plus the
`*outs != outsBlock` equality check prevent coin resurrection.

## Testing performed

* **Unit tests:** full `test_pepecoin` suite passes **275/275** (253 upstream +
  22 new adversarial cases across monetary invariants, block-level inflation /
  double-spend / reorg attacks, AuxPoW, and network time).
* **Functional tests (regtest):** `auxpow.py`, `getauxblock.py`,
  `createauxblock.py`, `p2p-fullblocktest.py`, `invalidblockrequest.py`,
  `invalidtxrequest.py`, `reindex.py`, `invalidateblock.py`, `mempool_reorg.py`,
  `txn_doublespend.py`, `txn_clone.py`, `rawtransactions.py`,
  `getchaintips.py`, `mempool_spendcoinbase.py` **pass**. (`bipdersig.py` /
  `bip65-cltv.py` “fail” identically on upstream Dogecoin — they exercise the
  old *supermajority* activation that Dogecoin/Pepecoin replaced with
  *height-based* activation; the height rule itself is verified by the passing
  C++ `bip65`/DER unit tests and by source. Not a consensus regression.)
* **Live regtest:** 1,000-block chain, subsidy-schedule verification, and the
  reorg round-trip above.

---

## Recommendations (priority order)

1. **Apply PEP-001** (done in this branch): drop `abs64` in `timedata.cpp`.
   This is the highest-value security change found.
2. **PEP-005:** cherry-pick the Dogecoin 1.14.8/1.14.9 P2P hardening
   (`6e74282e2`, `c9d9486c8`, and ideally rebase `net_processing.cpp` onto
   1.14.9). Reduces peer-facing DoS surface; no consensus risk.
3. Port `c4e76a369` (PEP-002) for upstream parity / better error reporting.
4. Add a `MoneyRange(nFees)` guard in `ConnectBlock` as defense-in-depth given
   the unbounded tail emission (PEP-003).
5. Remove dead consensus scaffolding (legacy hash-derived subsidy branch,
   `IsSuperMajority`); reorder the `removeForReorg` null-check; resolve the
   `BIP34Hash = 0x00` TODO deliberately (PEP-004).
6. Keep tracking the Dogecoin `1.14-maint` branch for future fixes; the fork is
   close enough that cherry-picks apply cleanly.

---

## Appendix — reproduction

```bash
./autogen.sh
./configure --with-incompatible-bdb --disable-bench --without-gui
make -j"$(nproc)"

# Adversarial consensus + block-level economic attacks + AuxPoW + timedata
./src/test/test_pepecoin --run_test=pepecoin_consensus_tests
./src/test/test_pepecoin --run_test=pepecoin_economic_attack_tests
./src/test/test_pepecoin --run_test=pepecoin_auxpow_adversarial_tests
./src/test/test_pepecoin --run_test=pepecoin_timedata_tests   # passes post-fix

# Full suite (275/275)
./src/test/test_pepecoin
```
