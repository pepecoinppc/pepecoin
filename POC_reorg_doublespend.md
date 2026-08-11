# Deep-reorg double-spend — the realistic "drain" mechanism (isolated regtest / testnet)

**What this is:** a demonstration, through Pepecoin's real
`ProcessNewBlock → ConnectBlock → reorg` pipeline, that a heavier competing
chain **reverses a confirmed payment** (double-spend). This is the systemic
mechanism behind almost every real "drain" of a low-hashrate PoW coin — and the
concrete impact of **PEP-006** (testnet AuxPoW work-reuse) and of any
51%-hashrate situation on mainnet.

**Severity framing (honest).**
- **Not a code bug / not inflation.** Every branch is internally valid; the
  protocol correctly follows the heaviest chain. No consensus rule is broken,
  no coin is minted, no signature is forged. Supply is conserved — coin `C` is
  spent exactly once on the winning chain; only the *recipient* changed.
- **The bug-bounty-relevant findings are the two things that make winning the
  race cheap or the reorg deep:**
  1. **PEP-006 (testnet, MEDIUM):** `fStrictChainId=false` → one parent PoW
     validates up to 64 chained blocks → up to **64× hashrate amplification** →
     a testnet attacker can out-mine the honest chain cheaply. Mainnet is not
     affected (`fStrictChainId=true`).
  2. **No max-reorg / rolling-checkpoint / finality rule (mainnet, informational
     → argues MEDIUM):** the only floor is the **static checkpoint at height
     327239**; above it a majority attacker can reorg **arbitrarily deep**,
     bounded only by how long they hold majority. Exchanges are protected solely
     by their confirmation count.

## Reproduction (deterministic, no network)

`src/test/pepecoin_doublespend_tests.cpp` → `reorg_reverses_confirmed_payment`:

```bash
./src/test/test_pepecoin --run_test=pepecoin_doublespend_tests
# *** No errors detected
```

Narrative the test executes on a regtest chain:

1. Attacker owns coin `C` (a mature coinbase).
2. **Branch A:** `tx1` pays the **merchant** (`C → merchant`), mined into a
   block. `OutputInUtxo(tx1)` is true — the merchant sees a confirmation and
   ships goods.
3. **Attacker's competing branch wins.** On mainnet this step needs majority
   hashrate to out-work branch A; the test forces the identical state
   transition with `InvalidateBlock` + a heavier branch B (this is exactly the
   chainstate a heavier attacker chain produces — it is not itself the bug).
4. **Branch B:** `tx2` spends the **same coin `C` back to the attacker**
   (`C → attacker`), plus an extra block so B is the heavier/active chain.
5. **Result (asserted):** `tx1` (merchant payment) is **gone from the UTXO
   set**; `tx2` (attacker) **is present**; the active tip is branch B. The
   merchant shipped goods for a payment that no longer exists. Coin `C` is spent
   exactly once (to the attacker) — supply conserved, recipient reversed.

## How this maps to a real attack

| Layer | What the attacker needs | In Pepecoin |
|-------|-------------------------|-------------|
| **Testnet** | Out-mine honest chain for a few blocks | Cheap: min-difficulty blocks allowed **and** PEP-006 64× AuxPoW work-reuse |
| **Mainnet** | Majority hashrate for N+1 blocks (N = exchange confs) | Requires real/rented scrypt+merge-mining majority; no code shortcut. **No max-reorg rule**, so depth is limited only by attacker endurance above height 327239 |
| **Force multiplier** | Reduce honest effective hashrate | **PEP-001** time-warp freeze of honest mining nodes lowers the bar (isolated-demo only; attacking live miners is out of scope) |

## Recommendations

- **Testnet:** accept that testnet is attacker-friendly by design; do not treat
  testnet reorg resistance as representative of mainnet. If testnet integrity
  matters for your bounty program, set `fStrictChainId=true` there too (closes
  PEP-006) and/or disable min-difficulty.
- **Mainnet:** the only real mitigations are operational —
  (a) **exchanges/services must require enough confirmations** for the value at
  risk (there is no protocol finality below the checkpoint);
  (b) **ship fresh, recent checkpoints** (the last is 327239 — far behind a live
  tip) or add a **rolling max-reorg-depth rule** so a deep reorg is rejected
  outright;
  (c) fix **PEP-001** so honest nodes can't be frozen as a reorg force-multiplier.
- **This is not a monetary/consensus code vulnerability.** For a bug-bounty
  submission, file PEP-006 (testnet, MEDIUM) and the missing-finality/stale-
  checkpoint hardening (MEDIUM) on their own merits; do **not** file the reorg
  double-spend itself as a "critical inflation/theft" — triage will (correctly)
  reject that, because no rule was broken and no coins were created.
