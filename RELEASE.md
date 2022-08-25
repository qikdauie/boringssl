OQS-BoringSSL snapshot 2022-08
==============================

About
-----

The **Open Quantum Safe (OQS) project** has the goal of developing and prototyping quantum-resistant cryptography.  More information on OQS can be found on our website: https://openquantumsafe.org/ and on Github at https://github.com/open-quantum-safe/.

**liboqs** is an open source C library for quantum-resistant cryptographic algorithms.

**open-quantum-safe/boringssl** is an integration of liboqs into (a fork of) BoringSSL.  The goal of this integration is to provide easy prototyping of quantum-resistant cryptography in TLS 1.3.  The integration should not be considered "production quality".

Release notes
=============

This is the 2022-08 snapshot release of OQS-BoringSSL, released on August 24, 2022. This release is intended to be used with liboqs version 0.7.2.

What's New
----------

This is the sixth snapshot release of OQS-BoringSSL.  It is based on BoringSSL commit [6191cc95a1ef9a7b0a3f79ac23cbbbba85698c0f](https://github.com/google/boringssl/commit/6191cc95a1ef9a7b0a3f79ac23cbbbba85698c0f).

- Upstream update
- Removal of Rainbow level 1 and SIKE/SIDH

Previous release notes
----------------------

- [OQS-BoringSSL snapshot 2021-08](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2021-08) aligned with liboqs 0.7.0 (August 11, 2021)
- [OQS-BoringSSL snapshot 2021-03](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2021-03) aligned with liboqs 0.5.0 (March 26, 2021)
- [OQS-BoringSSL snapshot 2020-08](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2020-08) aligned with liboqs 0.4.0 (August 11, 2020)
- [OQS-BoringSSL snapshot 2020-07](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2020-07) aligned with liboqs 0.3.0 (July 10, 2020)

---

Detailed changelog
------------------

* Update BoringSSL by @xvzcf in https://github.com/open-quantum-safe/boringssl/pull/80
* remove rainbowI by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/83
* remove SIKE/SIDH by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/84

**Full Changelog**: https://github.com/open-quantum-safe/boringssl/compare/OQS-BoringSSL-snapshot-2022-01...OQS-BoringSSL-snapshot-2022-08
