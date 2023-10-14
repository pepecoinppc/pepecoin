// Copyright (c) 2009-2014 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "uritests.h"

#include "guiutil.h"
#include "walletmodel.h"

#include <QUrl>

void URITests::uriTests()
{
    SendCoinsRecipient rv;
    QUrl uri;
    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?req-dontexist="));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?dontexist="));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?label=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS"));
    QVERIFY(rv.label == QString("Wikipedia Example Address"));
    QVERIFY(rv.amount == 0);

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?amount=0.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100000);

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?amount=1.001"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS"));
    QVERIFY(rv.label == QString());
    QVERIFY(rv.amount == 100100000);

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?amount=100&label=Wikipedia Example"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS"));
    QVERIFY(rv.amount == 10000000000LL);
    QVERIFY(rv.label == QString("Wikipedia Example"));

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?message=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));
    QVERIFY(rv.address == QString("PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS"));
    QVERIFY(rv.label == QString());

    QVERIFY(GUIUtil::parseBitcoinURI("pepecoin://PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?message=Wikipedia Example Address", &rv));
    QVERIFY(rv.address == QString("PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS"));
    QVERIFY(rv.label == QString());

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?req-message=Wikipedia Example Address"));
    QVERIFY(GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?amount=1,000&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));

    uri.setUrl(QString("pepecoin:PvN5ZkKHqSbexjvKjuTpBTQJERL3qSHrhS?amount=1,000.0&label=Wikipedia Example"));
    QVERIFY(!GUIUtil::parseBitcoinURI(uri, &rv));
}
