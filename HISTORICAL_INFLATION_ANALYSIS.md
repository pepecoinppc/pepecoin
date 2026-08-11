# Historical inflation-bug transplant analysis — Pepecoin Core 1.1.0

Prompted by a set of real-world inflation/counterfeit incidents. For each, the
question is only: **does the vulnerable invariant / failure mode exist in
Pepecoin's actual code?** Classification per the audit rubric:
`NOT APPLICABLE` / `FIX PRESENT` / `POSSIBLY VULNERABLE` / `CONFIRMED`.

**Headline:** four of the six referenced classes are *architecturally
impossible* in Pepecoin — it has **no shielded pool, no zk-SNARKs, no
MimbleWimble/MWEB extension block, no cross-chain bridge, no mint function, no
smart contracts**. Issuance happens in exactly one place (the coinbase) and is
capped by one consensus condition. Verified by grep — none of `snark|groth|
bellman|shielded|sapling|sprout|mimble|mweb|pegin|pegout|bridge|wrapped-mint`
exist in `src/` (only unrelated hits: leveldb comments, P2SH wrapping). The two
that *do* map — the Bitcoin base-layer inflation CVEs — are provably enforced
against, with tests.

---

## The single issuance path (what every one of these attacks would have to break)

```
coinbase output value  ≤  GetPepecoinBlockSubsidy(height)  +  Σ real fees
```
enforced at `src/validation.cpp:1997` (`bad-cb-amount`). There is no other code
that creates PEPE. Every non-coinbase transaction is independently held to
`Σ outputs ≤ Σ inputs`, `fee ≥ 0`, and per-value `MoneyRange` at
`src/validation.cpp:1447–1457`. This is the choke point all six historical
bugs, in their own ecosystems, found a way *around*; below is why each route is
closed here.

---

## Transplant table

| # | Incident / class (resource) | Root cause in the original | Pepecoin architectural analog | Verdict | Evidence |
|---|------------------------------|----------------------------|-------------------------------|---------|----------|
| 1 | **Bitcoin value overflow, CVE-2010-5139** ("184 billion BTC") | Sum of outputs overflowed `int64`, wrapping to a small value that passed `out ≤ in` | Same `CAmount`/`int64` output summation | **FIX PRESENT** | `CheckTransaction` rejects `nValue < 0`, `nValue > MAX_MONEY`, and running-sum out of `MoneyRange` *before* any comparison (`validation.cpp:538–548`); `GetValueOut` throws on overflow (`transaction.cpp:83–93`). Tests: `getvalueout_overflow_throws`, `checktxinputs_amount_ranges`, `coinbase_may_not_overpay_subsidy`. |
| 2 | **Bitcoin duplicate-input inflation, CVE-2018-17144** (also hit forks incl. Litecoin) | The duplicate-input check was *skipped* when `CheckTransaction` ran from `CheckBlock` (perf optimization); a tx spending one coin twice double-counted its value → inflation / assert-crash | Same `CheckTransaction` / `CheckBlock` structure | **FIX PRESENT** | `CheckBlock` calls `CheckTransaction(*tx, state, true)` (`validation.cpp:2984`) and the header default is `true` (`validation.h:394`); **no** consensus path calls it with `false`. Cross-tx double-spend additionally caught by `HaveInputs` (`validation.cpp:1944`). Tests: `duplicate_inputs_rejected_in_block`, `supply_..._survives_attacks` (block double-spend), `intra_block_spend_ordering`. |
| 3 | **ZK inflation bugs** (cryptoadventure) | Soundness gap in a zero-knowledge proof system lets a forged proof mint value | **None** — Pepecoin has no zk-SNARK / proof system; all value is transparent UTXO | **NOT APPLICABLE** | grep: no `snark/groth/bellman/zk`; value is enforced by explicit arithmetic + ECDSA, not by a proof of balance. |
| 4 | **Zcash shielded-pool counterfeiting** (developmentstoday) | Flaw in the shielded-pool zk parameters (BCTV14) allowed unlimited counterfeiting of *shielded* coins where the value balance is proof-enforced, not arithmetic-enforced | **None** — Pepecoin has no shielded pool; there is no pool whose balance is enforced by anything other than per-tx `Σout ≤ Σin` | **NOT APPLICABLE** | grep: no `shielded/sapling/sprout`. Every input value is re-`MoneyRange`-checked at spend even if it entered the UTXO DB corrupted (`validation.cpp:1442`), so there is no "trusted pool balance" to break. |
| 5 | **Litecoin "85k" incident** (ambcrypto) | Litecoin-specific (MimbleWimble Extension Block / a Bitcoin-lineage base-layer bug) | Pepecoin has **no MWEB**. If the class is the base-layer CVE-2018-17144, see #2 | **NOT APPLICABLE (MWEB) / FIX PRESENT (base-layer)** | grep: no `mimble/mweb/pegin/pegout`. Pepecoin is Dogecoin-lineage without MWEB; the only shared surface is the base-layer CVEs in #1–#2, both closed. |
| 6 | **Secret Network bridge infinite-mint** (cointelegraph) | A cross-chain bridge contract minted wrapped tokens without validating the corresponding lock/deposit | **None** — Pepecoin base layer has no bridge, no wrapped-asset mint, no smart-contract mint; issuance is coinbase-only | **NOT APPLICABLE** | grep: no `bridge/mint(`; `GetPepecoinBlockSubsidy` is the sole issuance function and its output is capped at `validation.cpp:1997`. |

---

## The unifying lesson, applied

Every incident above is one pattern: **value is created or trusted in a
component that does not fully re-enforce conservation** — a proof system (3,4),
an extension block (5), a bridge mint (6), or an arithmetic check that can be
skipped/overflowed (1,2). The defense is to make conservation *unconditional* on
every path that can create or move value.

Applying that lens to Pepecoin's entire surface (done across this audit):

- **Only one path creates value** (coinbase) and it is unconditionally capped
  (`validation.cpp:1997`); there is no proof system, pool, bridge, or mint to
  bypass it.
- **Every path that moves value** re-checks `Σout ≤ Σin`, `fee ≥ 0`, and
  per-value `MoneyRange` *at spend time* — even for coins already resident in
  the UTXO DB (so a corrupted/oversized stored value cannot be spent).
- **Every value is arithmetic-enforced, never proof-trusted**, so there is no
  soundness gap to exploit.

This was checked three independent ways: source tracing, a 12-attack block-level
campaign through the real `ConnectBlock` pipeline (all rejected, supply
unchanged), and a 2,020-block regtest run whose supply equals cumulative
issuance to the koinu. See `SECURITY_AUDIT.md` → *Economic-Destruction Attack
Campaign*.

## Verdict

Of the six referenced inflation/counterfeit classes, **none is applicable-and-
open** in Pepecoin: four cannot exist (no ZK / shielded pool / MWEB / bridge),
and the two base-layer ones are **FIX PRESENT** with regression tests. **No
inflation vector was found**, and — consistent with the audit's standing rule —
none was invented to fit the hypothesis.

> If a real mainnet drain occurred, this analysis is strong evidence it was
> **not** a base-layer inflation/counterfeit bug of any of these classes. The
> remaining realistic explanations are operational (exchange/hot-wallet/RPC key
> compromise) or a **51%/rented-hashrate deep-reorg double-spend** — the latter
> being systemic to low-hashrate PoW, not a code defect. The fastest confirmation
> is to trace the actual draining transactions and check for a reorg around the
> event.
