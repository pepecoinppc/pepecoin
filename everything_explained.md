# Everything explained: how a 51% attack and a double-spend actually work (Pepecoin)

A from-scratch, detailed walkthrough of what a majority-hashrate ("51%") attack
is, exactly how it produces a double-spend, a **runnable Python demonstration on
an isolated regtest network**, and how all of this maps to Pepecoin's real code
and parameters.

> **Scope / ethics.** The demonstration only ever talks to two private regtest
> nodes it launches on `127.0.0.1`. On regtest, difficulty is trivial, so one CPU
> *is* 100% of the hashrate — which is why the attack "works" instantly there. On
> the real testnet or mainnet, the same steps require actually controlling >50%
> of real hashrate for the whole confirmation window; there is **no software
> shortcut**, and attacking public nodes is out of scope. This document is for
> understanding and defending the network.

---

## 1. The building blocks

**Blocks and the chain.** Pepecoin (like Bitcoin/Dogecoin) orders transactions
into blocks. Each block header commits to the previous block's hash, so blocks
form a chain. Each block also contains a *proof of work* (PoW): a scrypt hash of
the header that must be below a target (`nBits`). Finding one is expensive;
checking one is instant.

**The heaviest-chain rule.** When a node sees two competing chains, it follows
the one with the most cumulative *work* (roughly, the most/hardest blocks), not
the one it saw first. Code: `pindexNew->nChainWork > chainActive.Tip()->nChainWork`
drives `ActivateBestChain` in `src/validation.cpp`. If a heavier chain appears,
the node **reorganizes** (reorg): it disconnects its current blocks and connects
the heavier chain's blocks instead.

**Confirmations.** A transaction is "confirmed" once it's in a block. "6
confirmations" means 5 blocks were built on top of its block. Each confirmation
makes it exponentially harder for an attacker to build a heavier competing chain
that excludes the transaction — *unless the attacker has the majority of
hashrate*, in which case they out-build the honest chain indefinitely.

**Coins are UTXOs.** A coin is an unspent transaction output (UTXO), locked to a
script (usually "whoever can sign for public key/hash X"). To spend it you
reference it as an input and provide a valid signature. You can only spend a coin
**once** on any given chain — the node enforces this (`HaveInputs`,
`validation.cpp:1944`). The trick of a double-spend is not spending it twice on
*one* chain (impossible), but making the network *switch to a different chain*
where it was spent differently.

---

## 2. What "51%" gives you — and what it does not

A miner with >50% of hashrate can, on average, build blocks faster than everyone
else combined. That single ability yields exactly these powers:

**CAN:**
- **Double-spend their own coins** by replacing a chain that contains payment A
  with a heavier chain that contains payment B (spending the same coin).
- **Censor** transactions (never include them; orphan blocks that do).
- **Orphan competitors' blocks**, denying them rewards/fees.
- **Selfish-mine / feather-fork** variants.

**CANNOT (every full node still checks these, regardless of hashrate):**
- Create coins from nothing (coinbase capped at `subsidy + fees`,
  `validation.cpp:1997`).
- Spend coins they don't have the key for (signatures are verified —
  `src/test/pepecoin_theft_tests.cpp` proves forged/absent signatures are
  rejected).
- Put invalid transactions in a block (bad amounts, duplicate inputs, etc.).
- Reorg below the last checkpoint (height **327239** on mainnet).

So a 51% attack is a **double-spend / censorship** weapon, never a "mint money"
or "steal anyone's wallet" weapon. That distinction is the single most important
thing to get right.

---

## 3. The double-spend, step by step

The classic target is a service that hands you something irreversible after N
confirmations — an exchange crediting a deposit, or a merchant shipping goods.

```
   Common chain up to block H (attacker owns coin C, worth 500,000)
   ────────────────────────────●  H
                                │
        (attacker splits the network / mines privately)
                                │
   Public "honest" chain:       │        Attacker's private chain:
   H → [ tx1: C → MERCHANT ]     │        H → [ tx2: C → ATTACKER ]
        block H+1  (1 conf)      │             block H+1'
        MERCHANT SHIPS GOODS     │             → H+2' → H+3' → H+4' → H+5' → H+6'
                                 │             (6 blocks: heavier than honest's 1)
                                 ▼
        Attacker publishes the private chain.
        Every node follows the HEAVIER chain → reorg to the attacker's branch.
   ────────────────────────────●───●───●───●───●───●  H+6'
        tx1 (the merchant's payment) is GONE. tx2 (attacker keeps C) wins.
        Merchant shipped goods for a payment that no longer exists.
```

Both branches are **individually valid** — the attacker breaks no rule. Coin C
is spent exactly once on the winning chain (to the attacker). Supply is
conserved; only the *recipient* changed. The merchant's mistake was trusting 1
confirmation against an attacker who could produce more.

**Why more confirmations help:** to reverse an N-confirmation payment the
attacker must privately out-build the honest chain by N+1 blocks. With <50%
hashrate the probability of ever catching up shrinks exponentially in N (this is
the calculation in Satoshi's whitepaper §11). With >50% it approaches certainty
regardless of N — which is why "51%" is the magic threshold.

---

## 4. Runnable demonstration (isolated regtest)

`qa/rpc-tests/doublespend_regtest_sim.py` performs exactly the diagram above
against two private regtest nodes. The core logic:

```python
# 1. Attacker builds TWO conflicting transactions that spend the SAME coin U:
#      tx1 -> pays the merchant      tx2 -> pays the same coin back to attacker
ins      = [{"txid": U["txid"], "vout": U["vout"]}]
tx1      = sign(create_raw(ins, {merchant_addr: 1000, change: Uval-1000-fee}))
tx2      = sign(create_raw(ins, {attacker_self:  Uval-fee}))

# 2. Partition the network so the two sides have separate mempools.
disconnect(HONEST, ATTACK)

# 3. Honest side confirms tx1 -> the merchant sees the payment and ships.
honest.sendrawtransaction(tx1); honest.generatetoaddress(1, merchant_addr)

# 4. Attacker privately confirms tx2 and mines a LONGER chain (6 > 1).
attacker.sendrawtransaction(tx2); attacker.generatetoaddress(6, attacker_self)

# 5. Reveal the heavier chain: the honest node reorgs to it.
connect(HONEST, ATTACK)

# 6. Result: the merchant's confirmed payment is gone.
assert honest.getreceivedbyaddress(merchant_addr, 0) == 0   # DOUBLE-SPEND
```

Run it:

```bash
make -C src pepecoind pepecoin-cli          # build once
DSIM_DIR=/root/pepe_dsim python3 qa/rpc-tests/doublespend_regtest_sim.py
```

**Observed output (real run):**

```
== attacker mines 120 blocks to fund itself (coinbases mature at 60) ==
   double-spend target coin U: e9ecb17fe31e1c74:0 worth 500000.0 PEPE
== STEP 1: honest chain confirms tx1 (merchant paid) -> merchant ships ==
   tx1 c44e93148a42c03f confirmed; merchant sees 1000.00000000 PEPE -> ships goods
== STEP 2: attacker privately confirms tx2 and mines a longer chain ==
   tx2 d6e85c787cd276ac (double-spend to attacker) mined; attacker height=126 vs honest=121
== STEP 3: attacker reveals the longer chain; honest node reorgs ==
   honest height=126 attacker height=126 (converged on heaviest chain)
== RESULT ==
   merchant received (was 1000): 0.00000000 PEPE
   >>> DOUBLE-SPEND SUCCEEDED: the confirmed payment was erased by the heavier chain.
```

The same thing is proven deterministically at the C++/consensus level in
`src/test/pepecoin_doublespend_tests.cpp` (`reorg_reverses_confirmed_payment`),
which drives it straight through `ProcessNewBlock → ConnectBlock → reorg` and
asserts the merchant's output leaves the UTXO set while the attacker's replacement
enters it.

### Why "6 blocks vs 1" instantly wins here but not on a real network
On regtest, `generatetoaddress` mints blocks with no real work (difficulty is
`powLimit`). The attacker "mining 6 blocks" is free, so it trivially has the
heavier chain. On mainnet those 6 blocks are ~6 minutes of the *entire network's*
scrypt hashrate; to produce them faster than the honest network you must own more
than half of it for the whole window. The **mechanism** is identical; only the
**cost** differs. That cost is the only thing protecting a real chain.

---

## 5. What makes this cheap or deep on Pepecoin specifically

These are the factors that lower the hashrate needed or deepen the damage
(details and code refs in `MAJORITY_ATTACK_ANALYSIS.md`):

1. **Merge-mining (biggest real risk).** Pepecoin is AuxPoW merge-mined
   (`nAuxpowChainId=0x3f`). A scrypt miner secures Pepecoin *for free* as a
   by-product of mining its parent chain, so Pepecoin's real security is only the
   fraction of scrypt hashrate that opts in. A large scrypt pool could point spare
   parent hashrate at Pepecoin and exceed 51% at near-zero marginal cost — **no
   bug required.** For a small merge-mined coin this is the most likely real-world
   drain.
2. **No finality / no max-reorg rule.** There is no rolling checkpoint; the only
   floor is the static checkpoint at height **327239**. A sustained majority can
   reorg arbitrarily deep above it. Exchanges are protected *only* by their
   confirmation count.
3. **Testnet is essentially free to dominate.** Testnet allows minimum-difficulty
   blocks (drop to `powLimit` after a 120s gap) and `fStrictChainId=false` enables
   AuxPoW **work-reuse** (one parent PoW validates up to 64 chained blocks —
   `src/test/pepecoin_auxpow_workreuse_tests.cpp`). A single machine can out-mine
   and reorg the testnet. **By design; mainnet closes both.**
4. **PEP-001 time-warp as a force-multiplier.** Freezing honest *mining* nodes
   (the `abs64(INT64_MIN)` clock bug, `POC_PEP-001_timewarp.md`) removes their
   work from the honest chain, lowering the majority threshold. Fixing PEP-001
   removes this lever.

---

## 6. Defenses

- **Require enough confirmations for the value at risk.** This is the primary,
  non-optional defense — there is no protocol finality below the checkpoint. The
  deeper the confirmation requirement, the more sustained hashrate an attacker
  needs.
- **Ship recent checkpoints** and/or add a **max-reorg-depth rule** so a
  suspiciously deep reorg is rejected outright.
- **Attract more merge-miners** so Pepecoin's opted-in hashrate is a large,
  hard-to-exceed fraction of the parent chain's.
- **Fix PEP-001** (the clock freeze force-multiplier) and **PEP-006** (testnet
  work-reuse, if testnet integrity matters).
- Monitor for reorgs (`-alertnotify`, chain-tip monitoring) so an in-progress
  attack is detected before large withdrawals settle.

---

## 7. One-paragraph summary

A 51% attacker cannot mint or steal coins; it can only **replace recent history
with a heavier chain of its own**, which lets it **double-spend its own
transactions** and censor others. A double-spend is: pay a merchant, let it
confirm and ship, then reveal a longer privately-mined chain that spends the same
coin back to yourself — the payment vanishes because every node follows the
heaviest chain. The isolated-regtest Python demo shows this end-to-end
(merchant's balance goes 1000 → 0); on a real network the identical steps cost
majority hashrate for the whole confirmation window, which — for a small
merge-mined coin like Pepecoin with no finality rule and a stale checkpoint — is
cheaper than it should be, and is best mitigated with deeper confirmations,
fresher checkpoints, and more merge-miners.
