# Majority-hashrate (51%+) attack surface — Pepecoin Core 1.1.0

Grounded in the actual chainparams and PoW code. This enumerates what a
majority miner **can** and **cannot** do, and the Pepecoin-specific factors that
lower the hashrate required or deepen the impact. **None of these is a novel
consensus code bug** — they are systemic PoW properties plus by-design testnet
relaxations — but several are legitimate hardening findings.

> Scope note: this is analysis + isolated-regtest demonstration only. The
> engagement forbids attacking the public testnet/mainnet or real miners; the
> chain "freeze honest nodes → gain relative majority → reorg" is only ever run
> against private/loopback nodes here.

---

## A. What a 51%+ attacker CAN do (no special Pepecoin defense)

| Capability | Mechanism | Demonstrated |
|-----------|-----------|--------------|
| **Double-spend** | Mine a heavier branch that omits/replaces a confirmed tx | `pepecoin_doublespend_tests.cpp` (payment reversed) |
| **Arbitrarily deep reorg** | No max-reorg / rolling-checkpoint / finality rule; only floor is checkpoint **327239** | `POC_reorg_doublespend.md`; grep confirms no finality rule |
| **Transaction censorship** | Refuse to include target txs; orphan honest blocks that do | trivial (omit from block) |
| **Reward/fee denial** | Orphan competitors → they lose subsidy+fees | inherent |
| **Selfish mining / feather-forking** | Withhold blocks / threaten orphaning | inherent (>~25–33%) |

## B. What a 51%+ attacker CANNOT do (hard bounds — verified)

- **Cannot inflate** — coinbase capped at `subsidy+fees` (`validation.cpp:1997`); every full node rejects a too-large coinbase regardless of hashrate.
- **Cannot steal coins** — no valid signature, no spend (`pepecoin_theft_tests.cpp`).
- **Cannot create invalid blocks** — bad merkle/sig/amount/duplicate-input blocks are rejected by every node.
- **Cannot reorg below the last checkpoint** (`CheckIndexAgainstCheckpoint`, height 327239).
- **Cannot force IBD nodes onto a low-work chain** — `nMinimumChainWork` gate.

So a majority attack is a **double-spend / censorship** tool, never a mint/theft tool. This is the ceiling; do not file it as inflation/theft.

## C. Pepecoin-specific factors that LOWER the bar or DEEPEN the impact

### C1. Merge-mining makes cheap majority the #1 systemic risk (mainnet + testnet)
Pepecoin is AuxPoW merge-mined (`nAuxpowChainId=0x3f`, auxpow from height 42000).
A scrypt miner secures Pepecoin **for free** as a side-effect of mining its
parent chain. Pepecoin's real security is therefore only the *fraction of scrypt
hashrate that opts in* — not the whole scrypt network. **A single large scrypt
pool that isn't already merge-mining Pepecoin could point spare/parent hashrate
at it and exceed 51% at near-zero marginal cost.** For a small merge-mined coin
this is the dominant, realistic "drain" vector and needs no software bug.
*Mitigation is economic/operational (attract more merge-miners; exchanges raise
confirmations), not a code fix.*

### C2. Testnet mining is essentially FREE → majority is costless there
Testnet sets `fPowAllowMinDifficultyBlocks=true`. `AllowDigishieldMinDifficulty
ForBlock` (`pepecoin.cpp:26`) resets difficulty to `powLimit` (the easiest
target) whenever a block's timestamp is `> 2 × nPowTargetSpacing` (120 s) after
the previous one. So an attacker mines testnet blocks at **minimum difficulty**
by spacing timestamps ≥120 s apart — trivial CPU work. Combined with **PEP-006**
(testnet `fStrictChainId=false` → one parent PoW validates up to 64 chained
blocks, 64× amplification), a single machine can out-mine and reorg the public
testnet at will. **This is by design (testnet mirrors Dogecoin testnet) and is
not representative of mainnet**, where `fPowAllowMinDifficultyBlocks=false` and
`fStrictChainId=true` close both.

### C3. No finality → deep reorgs (mainnet)
There is no rolling checkpoint or max-reorg-depth rule; the last static
checkpoint is **327239**, far behind a live tip. A sustained majority can reorg
from the tip down to just above 327239. Exchanges are protected **only** by
their confirmation count. *Hardening: ship recent checkpoints and/or add a
max-reorg-depth rule.*

### C4. PEP-001 time-warp as a hashrate force-multiplier
Freezing honest **mining** nodes (PEP-001, `time-too-new` freeze) removes their
work from the honest chain, lowering the hashrate an attacker needs for a
majority reorg. It does **not** grant majority by itself. *Fixing PEP-001
removes this multiplier.*

## D. Timestamp / difficulty manipulation (classic "time-warp") — bounded on mainnet

Digishield retargets **every block** with an amplitude filter and a hard clamp
(`pepecoin.cpp:53–72`), for the 60 s target:

```
nModulatedTimespan = 60 + (nActual − 60)/8        (amplitude filter)
clamp to [45, 90]                                 (= [60−60/4, 60+60/2])
target_next = target * nModulatedTimespan / 60
```

So the **per-block** target move is bounded to `[45/60, 90/60] = [0.75×, 1.5×]`,
i.e. at most a **33% difficulty drop per block**. To keep dropping difficulty an
attacker must keep pushing timestamps forward, but each must exceed
`GetMedianTimePast` and stay `≤ GetAdjustedTime + 2h`, so the forward budget is
capped (~2h) — the classic time-warp is **resisted** on mainnet. (On testnet C2
defeats this entirely by dropping straight to `powLimit`.)

*Note:* this is exactly why **PEP-001 matters even for difficulty** — the `+2h`
future bound uses `GetAdjustedTime()`. PEP-001 corrupts that clock, but toward
`INT64_MIN` (rejects all blocks) — it cannot be used to *widen* the future
window (the clamp only mis-handles `INT64_MIN`, a negative value), so it does
not enable extra difficulty-grinding, only the freeze.

## E. Bug-bounty framing (honest)

| Item | Fileable? | Severity |
|------|-----------|----------|
| 51% double-spend itself | **No** — no rule broken, follows heaviest chain | — |
| Cheap merge-mined majority (C1) | Design/economic, usually **out of scope** for code bounties | informational |
| Testnet free-majority + PEP-006 (C2) | **Yes** (testnet integrity) | MEDIUM |
| No finality / stale checkpoint (C3) | **Yes** (hardening) | MEDIUM |
| PEP-001 freeze (C4, and on its own) | **Yes**, with working PoC | **HIGH** |

The strongest, credible submissions are **PEP-001 (HIGH)** and the
**PEP-006 / missing-finality hardening (MEDIUM)**. A majority attack is the
*impact model* that motivates them, not a vulnerability to file on its own.
