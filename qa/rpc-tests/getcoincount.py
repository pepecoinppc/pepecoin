#!/usr/bin/env python3
# Copyright (c) 2024 The Pepecoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *


class GetCoinCountTest(BitcoinTestFramework):

    def __init__(self):
        super().__init__()
        self.setup_clean_chain = True
        self.num_nodes = 1

    def setup_network(self, split=False):
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir)
        self.is_network_split=False

    def run_test(self):
        print("Mining blocks...")
        node = self.nodes[0]
        node.generate(1)

        result = node.getcoincount()
        assert_equal(result, 1 * 500000)

        result = node.getcoincount(100001)
        assert_equal(result, (100000 * 500000) + (1 * 250000))

        result = node.getcoincount(200001)
        assert_equal(result, (100000 * 500000) + (100000 * 250000) + (1 * 125000))

        result = node.getcoincount(300001)
        assert_equal(result, (100000 * 500000) + (100000 * 250000) + (100000 * 125000) + (1 * 62500))

        result = node.getcoincount(400001)
        assert_equal(result, (100000 * 500000) + (100000 * 250000) + (100000 * 125000) + (100000 * 62500) + (1 * 31250))

        result = node.getcoincount(500001)
        assert_equal(result, (100000 * 500000) + (100000 * 250000) + (100000 * 125000)
                           + (100000 *  62500) + (100000 *  31250) + (1      *  15625))

        result = node.getcoincount(600001)
        assert_equal(result, (100000 * 500000) + (100000 * 250000) + (100000 * 125000)
                           + (100000 *  62500) + (100000 *  31250) + (100000 *  15625) + (1 * 10000))

        result = node.getcoincount(632824)
        assert_equal(result, (100000 * 500000) + (100000 * 250000) + (100000 * 125000)
                           + (100000 *  62500) + (100000 *  31250) + (100000 *  15625) + (32824 * 10000))

if __name__ == '__main__':
    GetCoinCountTest().main()
