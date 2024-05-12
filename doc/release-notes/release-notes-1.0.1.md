## Pepecoin Core 1.0.1 Release Notes

This is a new minor release that activates AuxPow at block 42,000. This will cause a hard fork after the first AuxPow block is mined.

***All users on Pepecoin 1.0.0 must upgrade to 1.0.1***

#### Technical Changes

* Set AuxPow activation height to `42,000`
* Change `CLIENT_NAME` from `satoshi` to `pepetoshi`
* Increment protocol version from `70015` to `70016` for auxpow acceleration
* Reduce the amount of blockchain space recommended during installation
* Add `getcoincount` RPC call to report current number of coins in circulation

#### Design Changes

* Add `ribbits` in place of `satoshi` in GUI tooltips
