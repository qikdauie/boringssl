OQS-BoringSSL snapshot 2022-01
==============================

About
-----

The **Open Quantum Safe (OQS) project** has the goal of developing and prototyping quantum-resistant cryptography.  More information on OQS can be found on our website: https://openquantumsafe.org/ and on Github at https://github.com/open-quantum-safe/.

**liboqs** is an open source C library for quantum-resistant cryptographic algorithms.

**open-quantum-safe/boringssl** is an integration of liboqs into (a fork of) BoringSSL.  The goal of this integration is to provide easy prototyping of quantum-resistant cryptography in TLS 1.3.  The integration should not be considered "production quality".

Release notes
=============

This is the 2022-01 snapshot release of OQS-BoringSSL, released on January 6, 2022. This release is intended to be used with liboqs version 0.7.1.

What's New
----------

This is the fifth snapshot release of OQS-BoringSSL.  It is based on BoringSSL commit [519c2986c73c23461b130ad19b93fd7d081353d5](https://github.com/google/boringssl/commit/519c2986c73c23461b130ad19b93fd7d081353d5).

- Update to BoringSSL commit 519c2986c73c23461b130ad19b93fd7d081353d5.
- Add NTRU and NTRU Prime Level 5 KEMs.

Previous release notes
----------------------

- [OQS-BoringSSL snapshot 2021-08](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2021-08) aligned with liboqs 0.7.0 (August 11, 2021)
- [OQS-BoringSSL snapshot 2021-03](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2021-03) aligned with liboqs 0.5.0 (March 26, 2021)
- [OQS-BoringSSL snapshot 2020-08](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2020-08) aligned with liboqs 0.4.0 (August 11, 2020)
- [OQS-BoringSSL snapshot 2020-07](https://github.com/open-quantum-safe/boringssl/releases/tag/OQS-BoringSSL-snapshot-2020-07) aligned with liboqs 0.3.0 (July 10, 2020)

---

Detailed changelog
------------------

* Update README.md by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/69
* Upgrade to upstream 519c2986c73c23461b130ad19b93fd7d081353d5  (Chromium 92 0 4515 107) by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/68
* further README update [skip ci] by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/70
* Search for liboqs in appropriate location on Windows by @dstebila in https://github.com/open-quantum-safe/boringssl/pull/76
* added s/ntrup1277 by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/75
* s/ntrup761 hybrid code point bump by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/78
* adding ntru_hps40961229 by @baentsch in https://github.com/open-quantum-safe/boringssl/pull/79


**Full Changelog**: https://github.com/open-quantum-safe/boringssl/compare/OQS-BoringSSL-snapshot-2021-08...OQS-BoringSSL-snapshot-2022-01
