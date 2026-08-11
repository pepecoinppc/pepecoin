#!/usr/bin/env python3
# Copyright (c) 2024 The Pepecoin Core developers
# Distributed under the MIT software license.
#
# ============================================================================
#  EDUCATIONAL 51% DOUBLE-SPEND SIMULATOR  --  ISOLATED REGTEST ONLY
# ============================================================================
# This script spins up TWO private regtest pepecoind nodes on 127.0.0.1 that it
# starts and controls itself, and walks through a textbook double-spend:
#
#   1. Attacker pays a "merchant" and lets it confirm  -> merchant ships goods.
#   2. Attacker (having "majority hashrate", trivially true on regtest) mines a
#      LONGER private chain that instead pays the same coin back to itself.
#   3. Attacker reveals the longer chain; the network follows the heaviest chain
#      and the merchant's payment is ERASED. Attacker kept the goods AND the coin.
#
# WHY THIS IS SAFE / WHY IT DOES NOT WORK ON A REAL NETWORK:
#   - regtest difficulty is trivial, so one CPU is "100% of the hashrate".
#   - On testnet/mainnet, step 2 requires actually out-mining everyone else
#     (>50% of real hashrate) for the whole confirmation window. There is no
#     software shortcut -- every node still validates every rule.
#   - This script ONLY talks to the two loopback nodes it launches. It never
#     connects to any public node. Do NOT point it at real infrastructure.
# ============================================================================

import os
import shutil
import subprocess
import sys
import time
from decimal import Decimal

# --- config -----------------------------------------------------------------
SRC = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "src"))
PEPECOIND = os.path.join(SRC, "pepecoind")
PEPECOINCLI = os.path.join(SRC, "pepecoin-cli")
WORKDIR = os.environ.get("DSIM_DIR", "/tmp/pepe_dsim")

HONEST = {"name": "honest", "rpc": 24101, "p2p": 24102}
ATTACK = {"name": "attacker", "rpc": 24111, "p2p": 24112}


def write_conf(node):
    d = os.path.join(WORKDIR, node["name"])
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, "pepecoin.conf"), "w") as f:
        f.write(
            "regtest=1\nserver=1\nlisten=1\nbind=127.0.0.1\ndiscover=0\n"
            "rpcuser=u\nrpcpassword=p\n"
            "rpcport=%d\nport=%d\nfallbackfee=0.001\n" % (node["rpc"], node["p2p"])
        )
    return d


def cli(node, *args):
    d = os.path.join(WORKDIR, node["name"])
    cmd = [PEPECOINCLI, "-datadir=%s" % d, "-rpcport=%d" % node["rpc"],
           "-rpcuser=u", "-rpcpassword=p"] + [str(a) for a in args]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        raise RuntimeError("cli %s failed: %s" % (" ".join(args), out.stderr.strip()))
    return out.stdout.strip()


def start(node):
    d = write_conf(node)
    subprocess.run([PEPECOIND, "-datadir=%s" % d, "-daemon"],
                   capture_output=True, text=True)


def wait_rpc(node, timeout=30):
    for _ in range(timeout):
        try:
            cli(node, "getblockcount")
            return
        except Exception:
            time.sleep(1)
    raise RuntimeError("%s RPC never came up" % node["name"])


def stop(node):
    try:
        cli(node, "stop")
    except Exception:
        pass


def connect(a, b):
    cli(a, "addnode", "127.0.0.1:%d" % b["p2p"], "onetry")


def disconnect(a, b):
    # Remove and drop the p2p link so the attacker can mine privately.
    try:
        cli(a, "disconnectnode", "127.0.0.1:%d" % b["p2p"])
    except Exception:
        pass
    cli(a, "setban", "127.0.0.1", "add", 600)   # keep them apart during the private mine
    cli(b, "setban", "127.0.0.1", "add", 600)


def unban(node):
    try:
        cli(node, "clearbanned")
    except Exception:
        pass


def sync_height(node):
    return int(cli(node, "getblockcount"))


def main():
    if not (os.path.exists(PEPECOIND) and os.path.exists(PEPECOINCLI)):
        print("build pepecoind/pepecoin-cli first (make -C src)"); sys.exit(1)
    if os.path.exists(WORKDIR):
        shutil.rmtree(WORKDIR)
    os.makedirs(WORKDIR)

    print("== starting two isolated regtest nodes (loopback only) ==")
    start(HONEST); start(ATTACK)
    wait_rpc(HONEST); wait_rpc(ATTACK)

    # Bring both to a common, funded starting point, in sync.
    connect(HONEST, ATTACK)
    time.sleep(2)
    print("== attacker mines 120 blocks to fund itself (coinbases mature at 60) ==")
    attacker_addr = cli(ATTACK, "getnewaddress")
    cli(ATTACK, "generatetoaddress", 120, attacker_addr)
    time.sleep(3)
    print("   honest height=%d attacker height=%d (synced over p2p)"
          % (sync_height(HONEST), sync_height(ATTACK)))

    # The merchant lives on the honest node.
    merchant_addr = cli(HONEST, "getnewaddress")

    # The attacker picks ONE specific coin (UTXO U) it will spend two ways.
    import json
    utxos = json.loads(cli(ATTACK, "listunspent", 1))
    U = max(utxos, key=lambda u: Decimal(str(u["amount"])))
    Uval = Decimal(str(U["amount"]))
    print("   double-spend target coin U: %s:%d worth %s PEPE"
          % (U["txid"][:16], U["vout"], Uval))
    fee = Decimal("1")

    # Build BOTH conflicting transactions up front, both spending U.
    #   tx1 -> pays the merchant (the "real" payment)
    #   tx2 -> pays the same coin back to the attacker (the double-spend)
    attacker_self = cli(ATTACK, "getnewaddress")
    change = cli(ATTACK, "getnewaddress")
    ins = json.dumps([{"txid": U["txid"], "vout": U["vout"]}])
    tx1_out = json.dumps({merchant_addr: 1000, change: float(Uval - Decimal(1000) - fee)})
    tx2_out = json.dumps({attacker_self: float(Uval - fee)})
    tx1_signed = json.loads(cli(ATTACK, "signrawtransaction",
                                cli(ATTACK, "createrawtransaction", ins, tx1_out)))["hex"]
    tx2_signed = json.loads(cli(ATTACK, "signrawtransaction",
                                cli(ATTACK, "createrawtransaction", ins, tx2_out)))["hex"]

    # ---- Split the network: separate mempools so each side has ONE of the txns ----
    print("\n== partition the network (attacker mines privately from here) ==")
    disconnect(HONEST, ATTACK)

    # ---- STEP 1: the honest side confirms the merchant payment (tx1). ----
    print("\n== STEP 1: honest chain confirms tx1 (merchant paid) -> merchant ships ==")
    txid1 = cli(HONEST, "sendrawtransaction", tx1_signed)
    cli(HONEST, "generatetoaddress", 1, merchant_addr)
    merch_recv = cli(HONEST, "getreceivedbyaddress", merchant_addr, 1)
    print("   tx1 %s confirmed; merchant sees %s PEPE -> ships goods"
          % (txid1[:16], merch_recv))

    # ---- STEP 2: the attacker privately confirms tx2 and mines a LONGER chain ----
    print("\n== STEP 2: attacker privately confirms tx2 and mines a longer chain ==")
    txid2 = cli(ATTACK, "sendrawtransaction", tx2_signed)
    cli(ATTACK, "generatetoaddress", 6, attacker_self)   # 6 > honest's 1 => heavier
    print("   tx2 %s (double-spend to attacker) mined; attacker height=%d vs honest=%d"
          % (txid2[:16], sync_height(ATTACK), sync_height(HONEST)))

    # ---- STEP 3: reveal the heavier chain; the honest node reorgs to it ----
    print("\n== STEP 3: attacker reveals the longer chain; honest node reorgs ==")
    unban(HONEST); unban(ATTACK)
    connect(HONEST, ATTACK)
    time.sleep(4)
    print("   honest height=%d attacker height=%d (converged on heaviest chain)"
          % (sync_height(HONEST), sync_height(ATTACK)))

    merch_final = cli(HONEST, "getreceivedbyaddress", merchant_addr, 0)
    print("\n== RESULT ==")
    print("   merchant received (was 1000): %s PEPE" % merch_final)
    if Decimal(merch_final) == 0:
        print("   >>> DOUBLE-SPEND SUCCEEDED: the confirmed payment was erased by the")
        print("       heavier chain. The attacker kept the goods AND the coins.")
    else:
        print("   payment survived (attacker chain was not heavier).")

    print("\n== cleanup ==")
    stop(HONEST); stop(ATTACK)
    time.sleep(2)
    print("done. (isolated regtest only; nothing external was touched)")


if __name__ == "__main__":
    main()
