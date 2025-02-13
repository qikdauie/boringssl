/* Copyright (c) 2015, Google Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. */

#include <openssl/ssl.h>

#include <assert.h>
#include <string.h>

#include <utility>

#include <openssl/bn.h>
#include <openssl/bytestring.h>
#include <openssl/curve25519.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/kyber.h>
#include <openssl/hrss.h>
#include <openssl/mem.h>
#include <openssl/nid.h>
#include <openssl/rand.h>
#include <openssl/span.h>

#include "internal.h"
#include "../crypto/internal.h"

#include <oqs/oqs.h>

BSSL_NAMESPACE_BEGIN

namespace {

class ECKeyShare : public SSLKeyShare {
 public:
  ECKeyShare(int nid, uint16_t group_id)
      : group_(EC_GROUP_new_by_curve_name(nid)), group_id_(group_id) {}

  uint16_t GroupID() const override { return group_id_; }

  bool Generate(CBB *out) override {
    assert(!private_key_);
    // Generate a private key.
    private_key_.reset(BN_new());
    if (!group_ || !private_key_ ||
        !BN_rand_range_ex(private_key_.get(), 1,
                          EC_GROUP_get0_order(group_))) {
      return false;
    }

    // Compute the corresponding public key and serialize it.
    UniquePtr<EC_POINT> public_key(EC_POINT_new(group_));
    if (!public_key ||
        !EC_POINT_mul(group_, public_key.get(), private_key_.get(),
                      nullptr, nullptr, /*ctx=*/nullptr) ||
        !EC_POINT_point2cbb(out, group_, public_key.get(),
                            POINT_CONVERSION_UNCOMPRESSED, /*ctx=*/nullptr)) {
      return false;
    }

    return true;
  }

  bool Encap(CBB *out_ciphertext, Array<uint8_t> *out_secret,
             uint8_t *out_alert, Span<const uint8_t> peer_key) override {
    // ECDH may be fit into a KEM-like abstraction by using a second keypair's
    // public key as the ciphertext.
    *out_alert = SSL_AD_INTERNAL_ERROR;
    return Generate(out_ciphertext) && Decap(out_secret, out_alert, peer_key);
  }

  bool Decap(Array<uint8_t> *out_secret, uint8_t *out_alert,
             Span<const uint8_t> ciphertext) override {
    assert(group_);
    assert(private_key_);
    *out_alert = SSL_AD_INTERNAL_ERROR;

    UniquePtr<EC_POINT> peer_point(EC_POINT_new(group_));
    UniquePtr<EC_POINT> result(EC_POINT_new(group_));
    UniquePtr<BIGNUM> x(BN_new());
    if (!peer_point || !result || !x) {
      return false;
    }

    if (ciphertext.empty() || ciphertext[0] != POINT_CONVERSION_UNCOMPRESSED ||
        !EC_POINT_oct2point(group_, peer_point.get(), ciphertext.data(),
                            ciphertext.size(), /*ctx=*/nullptr)) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      *out_alert = SSL_AD_DECODE_ERROR;
      return false;
    }

    // Compute the x-coordinate of |peer_key| * |private_key_|.
    if (!EC_POINT_mul(group_, result.get(), NULL, peer_point.get(),
                      private_key_.get(), /*ctx=*/nullptr) ||
        !EC_POINT_get_affine_coordinates_GFp(group_, result.get(), x.get(),
                                             NULL,
                                             /*ctx=*/nullptr)) {
      return false;
    }

    // Encode the x-coordinate left-padded with zeros.
    Array<uint8_t> secret;
    if (!secret.Init((EC_GROUP_get_degree(group_) + 7) / 8) ||
        !BN_bn2bin_padded(secret.data(), secret.size(), x.get())) {
      return false;
    }

    *out_secret = std::move(secret);
    return true;
  }

  bool SerializePrivateKey(CBB *out) override {
    assert(group_);
    assert(private_key_);
    // Padding is added to avoid leaking the length.
    size_t len = BN_num_bytes(EC_GROUP_get0_order(group_));
    return BN_bn2cbb_padded(out, len, private_key_.get());
  }

  bool DeserializePrivateKey(CBS *in) override {
    assert(!private_key_);
    private_key_.reset(BN_bin2bn(CBS_data(in), CBS_len(in), nullptr));
    return private_key_ != nullptr;
  }

 private:
  UniquePtr<BIGNUM> private_key_;
  const EC_GROUP *const group_ = nullptr;
  uint16_t group_id_;
};

class X25519KeyShare : public SSLKeyShare {
 public:
  X25519KeyShare() {}

  uint16_t GroupID() const override { return SSL_GROUP_X25519; }

  bool Generate(CBB *out) override {
    uint8_t public_key[32];
    X25519_keypair(public_key, private_key_);
    return !!CBB_add_bytes(out, public_key, sizeof(public_key));
  }

  bool Encap(CBB *out_ciphertext, Array<uint8_t> *out_secret,
             uint8_t *out_alert, Span<const uint8_t> peer_key) override {
    // X25519 may be fit into a KEM-like abstraction by using a second keypair's
    // public key as the ciphertext.
    *out_alert = SSL_AD_INTERNAL_ERROR;
    return Generate(out_ciphertext) && Decap(out_secret, out_alert, peer_key);
  }

  bool Decap(Array<uint8_t> *out_secret, uint8_t *out_alert,
             Span<const uint8_t> ciphertext) override {
    *out_alert = SSL_AD_INTERNAL_ERROR;

    Array<uint8_t> secret;
    if (!secret.Init(32)) {
      return false;
    }

    if (ciphertext.size() != 32 ||  //
        !X25519(secret.data(), private_key_, ciphertext.data())) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      return false;
    }

    *out_secret = std::move(secret);
    return true;
  }

  bool SerializePrivateKey(CBB *out) override {
    return CBB_add_bytes(out, private_key_, sizeof(private_key_));
  }

  bool DeserializePrivateKey(CBS *in) override {
    if (CBS_len(in) != sizeof(private_key_) ||
        !CBS_copy_bytes(in, private_key_, sizeof(private_key_))) {
      return false;
    }
    return true;
  }

 private:
  uint8_t private_key_[32];
};

class X25519Kyber768KeyShare : public SSLKeyShare {
 public:
  X25519Kyber768KeyShare() {}

  uint16_t GroupID() const override {
    return SSL_GROUP_X25519_KYBER768_DRAFT00;
  }

  bool Generate(CBB *out) override {
    uint8_t x25519_public_key[32];
    X25519_keypair(x25519_public_key, x25519_private_key_);

    uint8_t kyber_public_key[KYBER_PUBLIC_KEY_BYTES];
    KYBER_generate_key(kyber_public_key, &kyber_private_key_);

    if (!CBB_add_bytes(out, x25519_public_key, sizeof(x25519_public_key)) ||
        !CBB_add_bytes(out, kyber_public_key, sizeof(kyber_public_key))) {
      return false;
    }

    return true;
  }

  bool Encap(CBB *out_ciphertext, Array<uint8_t> *out_secret,
             uint8_t *out_alert, Span<const uint8_t> peer_key) override {
    Array<uint8_t> secret;
    if (!secret.Init(32 + 32)) {
      return false;
    }

    uint8_t x25519_public_key[32];
    X25519_keypair(x25519_public_key, x25519_private_key_);
    KYBER_public_key peer_kyber_pub;
    CBS peer_key_cbs;
    CBS peer_x25519_cbs;
    CBS peer_kyber_cbs;
    CBS_init(&peer_key_cbs, peer_key.data(), peer_key.size());
    if (!CBS_get_bytes(&peer_key_cbs, &peer_x25519_cbs, 32) ||
        !CBS_get_bytes(&peer_key_cbs, &peer_kyber_cbs,
                       KYBER_PUBLIC_KEY_BYTES) ||
        CBS_len(&peer_key_cbs) != 0 ||
        !X25519(secret.data(), x25519_private_key_,
                CBS_data(&peer_x25519_cbs)) ||
        !KYBER_parse_public_key(&peer_kyber_pub, &peer_kyber_cbs)) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      return false;
    }

    uint8_t kyber_ciphertext[KYBER_CIPHERTEXT_BYTES];
    KYBER_encap(kyber_ciphertext, secret.data() + 32, secret.size() - 32,
                &peer_kyber_pub);

    if (!CBB_add_bytes(out_ciphertext, x25519_public_key,
                       sizeof(x25519_public_key)) ||
        !CBB_add_bytes(out_ciphertext, kyber_ciphertext,
                       sizeof(kyber_ciphertext))) {
      return false;
    }

    *out_secret = std::move(secret);
    return true;
  }

  bool Decap(Array<uint8_t> *out_secret, uint8_t *out_alert,
             Span<const uint8_t> ciphertext) override {
    *out_alert = SSL_AD_INTERNAL_ERROR;

    Array<uint8_t> secret;
    if (!secret.Init(32 + 32)) {
      return false;
    }

    if (ciphertext.size() != 32 + KYBER_CIPHERTEXT_BYTES ||
        !X25519(secret.data(), x25519_private_key_, ciphertext.data())) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      return false;
    }

    KYBER_decap(secret.data() + 32, secret.size() - 32, ciphertext.data() + 32,
                &kyber_private_key_);
    *out_secret = std::move(secret);
    return true;
  }

 private:
  uint8_t x25519_private_key_[32];
  KYBER_private_key kyber_private_key_;
};

// Class for key-exchange using OQS supplied
// post-quantum algorithms.
class OQSKeyShare : public SSLKeyShare {
 public:
  // While oqs_meth can be determined from the group_id,
  // we pass both in as the translation from group_id to
  // oqs_meth is already done by SSLKeyShare::Create to
  // to determine if oqs_meth is enabled in liboqs and
  // and return nullptr if not. It is easier to handle
  // the error in there as opposed to in this constructor.
  OQSKeyShare(uint16_t group_id, const char *oqs_meth) : group_id_(group_id) {
    oqs_kex_ = OQS_KEM_new(oqs_meth);
  }

  uint16_t GroupID() const override { return group_id_; }

  size_t length_public_key() {
    return oqs_kex_->length_public_key;
  }

  size_t length_ciphertext() {
    return oqs_kex_->length_ciphertext;
  }

  // Client sends its public key to server
  bool Generate(CBB *out) override {
    Array<uint8_t> public_key;

    if (!public_key.Init(oqs_kex_->length_public_key) ||
        !private_key_.Init(oqs_kex_->length_secret_key)) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }
    if (OQS_KEM_keypair(oqs_kex_, public_key.data(), private_key_.data()) != OQS_SUCCESS) {
      OPENSSL_PUT_ERROR(SSL, SSL_R_PRIVATE_KEY_OPERATION_FAILED);
      return false;
    }

    if (!CBB_add_bytes(out, public_key.data(), public_key.size())) {
      return false;
    }

    return true;
  }

  // Server computes shared secret under client's public key
  // and sends a ciphertext to client
  bool Encap(CBB *out_public_key, Array<uint8_t> *out_secret,
              uint8_t *out_alert, Span<const uint8_t> peer_key) override {
    Array<uint8_t> shared_secret;
    Array<uint8_t> ciphertext;

    if (peer_key.size() != oqs_kex_->length_public_key) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      return false;
    }

    if (!shared_secret.Init(oqs_kex_->length_shared_secret) ||
        !ciphertext.Init(oqs_kex_->length_ciphertext)) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }

    if (OQS_KEM_encaps(oqs_kex_, ciphertext.data(), shared_secret.data(), peer_key.data()) != OQS_SUCCESS) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      return false;
    }

    if (!CBB_add_bytes(out_public_key, ciphertext.data(), oqs_kex_->length_ciphertext)) {
      return false;
    }

    *out_secret = std::move(shared_secret);

    return true;
  }

  // Client decapsulates the ciphertext using its
  // private key to obtain the shared secret.
  bool Decap(Array<uint8_t> *out_secret, uint8_t *out_alert,
              Span<const uint8_t> peer_key) override {
    Array<uint8_t> shared_secret;

    if (peer_key.size() != oqs_kex_->length_ciphertext) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      return false;
    }

    if (!shared_secret.Init(oqs_kex_->length_shared_secret)) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }

    if (OQS_KEM_decaps(oqs_kex_, shared_secret.data(), peer_key.data(), private_key_.data()) != OQS_SUCCESS) {
      *out_alert = SSL_AD_DECODE_ERROR;
      OPENSSL_PUT_ERROR(SSL, SSL_R_BAD_ECPOINT);
      return false;
    }

    *out_secret = std::move(shared_secret);

    return true;
  }

  ~OQSKeyShare() {
      OQS_KEM_free(oqs_kex_);
  }

 private:
  uint16_t group_id_;

  OQS_KEM *oqs_kex_;
  Array<uint8_t> private_key_;
};

// Class for key-exchange using a classical key-exchange
// algorithm in hybrid mode with OQS supplied post-quantum
// algorithms. Following https://tools.ietf.org/html/draft-ietf-tls-hybrid-design-01#section-3.2
// hybrid messages are encoded as follows:
// classical_artifact | pq_artifact
class ClassicalWithOQSKeyShare : public SSLKeyShare {
 public:
  ClassicalWithOQSKeyShare(uint16_t group_id, uint16_t classical_group_id, const char *oqs_meth) : group_id_(group_id), classical_group_id_(classical_group_id), oqs_meth_(oqs_meth) {}

  uint16_t GroupID() const override { return group_id_; }

  bool Generate(CBB *out) override {
    if (!initCheck()) {
        return false;
    }

    ScopedCBB classical_offer;
    ScopedCBB pq_offer;

    if (!CBB_init(classical_offer.get(), 0) ||
        !classical_kex_->Generate(classical_offer.get()) ||
        !CBB_flush(classical_offer.get())) {
      // classical_kex_ will set the appropriate error on failure
      return false;
    }

    if (!CBB_init(pq_offer.get(), 0) ||
        !pq_kex_->Generate(pq_offer.get()) ||
        !CBB_flush(pq_offer.get())) {
      // pq_kex_ will set the appropriate error on failure
      return false;
    }

    if (!CBB_add_bytes(out, CBB_data(classical_offer.get()), CBB_len(classical_offer.get())) ||
        !CBB_add_bytes(out, CBB_data(pq_offer.get()), CBB_len(pq_offer.get()))) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }

    return true;
  }

  bool Encap(CBB *out_public_key, Array<uint8_t> *out_secret,
              uint8_t *out_alert, Span<const uint8_t> peer_key) override {
    if (!initCheck()) {
        return false;
    }

    Array<uint8_t> out_classical_secret;
    ScopedCBB out_classical_public_key;

    Array<uint8_t> out_pq_secret;
    ScopedCBB out_pq_ciphertext;

    ScopedCBB out_secret_cbb;

    if (!CBB_init(out_classical_public_key.get(), classical_pub_size_) ||
        !classical_kex_->Encap(out_classical_public_key.get(), &out_classical_secret, out_alert, peer_key.subspan(0, classical_pub_size_)) ||
        !CBB_flush(out_classical_public_key.get())) {
      return false;
    }

    if (!CBB_init(out_pq_ciphertext.get(), 0) ||
        !pq_kex_->Encap(out_pq_ciphertext.get(), &out_pq_secret, out_alert, peer_key.subspan(classical_pub_size_, pq_kex_->length_public_key())) ||
        !CBB_flush(out_pq_ciphertext.get())) {
      return false;
    }

    if (!CBB_add_bytes(out_public_key, CBB_data(out_classical_public_key.get()), CBB_len(out_classical_public_key.get())) ||
        !CBB_add_bytes(out_public_key, CBB_data(out_pq_ciphertext.get()), CBB_len(out_pq_ciphertext.get()))) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }

    if (!CBB_init(out_secret_cbb.get(), out_classical_secret.size() + out_pq_secret.size()) ||
        !CBB_add_bytes(out_secret_cbb.get(), out_classical_secret.data(), out_classical_secret.size()) ||
        !CBB_add_bytes(out_secret_cbb.get(), out_pq_secret.data(), out_pq_secret.size()) ||
        !CBBFinishArray(out_secret_cbb.get(), out_secret)) {
      OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
      return false;
    }

    return true;
  }

  bool Decap(Array<uint8_t> *out_secret, uint8_t *out_alert,
              Span<const uint8_t> peer_key) override {
    if (!initCheck()) {
        return false;
    }

    ScopedCBB out_secret_cbb;

    Array<uint8_t> out_classical_secret;
    Array<uint8_t> out_pq_secret;

    if (!classical_kex_->Decap(&out_classical_secret, out_alert, peer_key.subspan(0, classical_pub_size_))) {
      return false;
    }

    if (!pq_kex_->Decap(&out_pq_secret, out_alert, peer_key.subspan(classical_pub_size_, pq_kex_->length_ciphertext()))) {
      return false;
    }

    if (!CBB_init(out_secret_cbb.get(), out_classical_secret.size() + out_pq_secret.size()) ||
        !CBB_add_bytes(out_secret_cbb.get(), out_classical_secret.data(), out_classical_secret.size()) ||
        !CBB_add_bytes(out_secret_cbb.get(), out_pq_secret.data(), out_pq_secret.size()) ||
        !CBBFinishArray(out_secret_cbb.get(), out_secret)) {
      return false;
    }

    return true;
  }

 private:
  uint16_t group_id_;
  uint16_t classical_group_id_;
  const char *oqs_meth_;

  UniquePtr<SSLKeyShare> classical_kex_ = nullptr;
  size_t classical_pub_size_ = 0;

  UniquePtr<OQSKeyShare> pq_kex_ = nullptr;

  bool initCheck() {
    if (!classical_kex_) {
        classical_kex_ = SSLKeyShare::Create(classical_group_id_);
        if (!classical_kex_) {
            OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
            return false;
        }
    }
    if (!pq_kex_) {
        pq_kex_ = MakeUnique<OQSKeyShare>(0, oqs_meth_); //We don't need pq_kex_->GroupID()
        if (!pq_kex_) {
            OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
            return false;
        }
    }
    if (!classical_pub_size_) {
        // TODO(oqs): This is hacky, but seems like the easiest way to go from
        // classical group ID -> classical public key size.
        UniquePtr<SSLKeyShare> tmp_kex = SSLKeyShare::Create(classical_group_id_);
        ScopedCBB tmp;
        if (!CBB_init(tmp.get(), 0) ||
            !tmp_kex->Generate(tmp.get()) ||
            !CBB_flush(tmp.get())) {
          OPENSSL_PUT_ERROR(SSL, ERR_R_MALLOC_FAILURE);
          return false;
        }
        classical_pub_size_ = CBB_len(tmp.get());
        if(!classical_pub_size_) {
            return false;
        }
    }
    return true;
  }
};

constexpr NamedGroup kNamedGroups[] = {
    {NID_secp224r1, SSL_GROUP_SECP224R1, "P-224", "secp224r1"},
    {NID_X9_62_prime256v1, SSL_GROUP_SECP256R1, "P-256", "prime256v1"},
    {NID_secp384r1, SSL_GROUP_SECP384R1, "P-384", "secp384r1"},
    {NID_secp521r1, SSL_GROUP_SECP521R1, "P-521", "secp521r1"},
    {NID_X25519, SSL_GROUP_X25519, "X25519", "x25519"},
    {NID_X25519Kyber768Draft00, SSL_GROUP_X25519_KYBER768_DRAFT00,
     "X25519Kyber768Draft00", ""},
///// OQS_TEMPLATE_FRAGMENT_DEF_NAMEDGROUPS_START
    {NID_frodo640aes, SSL_GROUP_FRODO640AES, "frodo640aes", "frodo640aes"},
    {NID_p256_frodo640aes, SSL_GROUP_P256_FRODO640AES, "p256_frodo640aes", "p256_frodo640aes"},
    {NID_frodo640shake, SSL_GROUP_FRODO640SHAKE, "frodo640shake", "frodo640shake"},
    {NID_p256_frodo640shake, SSL_GROUP_P256_FRODO640SHAKE, "p256_frodo640shake", "p256_frodo640shake"},
    {NID_frodo976aes, SSL_GROUP_FRODO976AES, "frodo976aes", "frodo976aes"},
    {NID_p384_frodo976aes, SSL_GROUP_P384_FRODO976AES, "p384_frodo976aes", "p384_frodo976aes"},
    {NID_frodo976shake, SSL_GROUP_FRODO976SHAKE, "frodo976shake", "frodo976shake"},
    {NID_p384_frodo976shake, SSL_GROUP_P384_FRODO976SHAKE, "p384_frodo976shake", "p384_frodo976shake"},
    {NID_frodo1344aes, SSL_GROUP_FRODO1344AES, "frodo1344aes", "frodo1344aes"},
    {NID_p521_frodo1344aes, SSL_GROUP_P521_FRODO1344AES, "p521_frodo1344aes", "p521_frodo1344aes"},
    {NID_frodo1344shake, SSL_GROUP_FRODO1344SHAKE, "frodo1344shake", "frodo1344shake"},
    {NID_p521_frodo1344shake, SSL_GROUP_P521_FRODO1344SHAKE, "p521_frodo1344shake", "p521_frodo1344shake"},
    {NID_bikel1, SSL_GROUP_BIKEL1, "bikel1", "bikel1"},
    {NID_p256_bikel1, SSL_GROUP_P256_BIKEL1, "p256_bikel1", "p256_bikel1"},
    {NID_bikel3, SSL_GROUP_BIKEL3, "bikel3", "bikel3"},
    {NID_p384_bikel3, SSL_GROUP_P384_BIKEL3, "p384_bikel3", "p384_bikel3"},
    {NID_kyber512, SSL_GROUP_KYBER512, "kyber512", "kyber512"},
    {NID_p256_kyber512, SSL_GROUP_P256_KYBER512, "p256_kyber512", "p256_kyber512"},
    {NID_kyber768, SSL_GROUP_KYBER768, "kyber768", "kyber768"},
    {NID_p384_kyber768, SSL_GROUP_P384_KYBER768, "p384_kyber768", "p384_kyber768"},
    {NID_kyber1024, SSL_GROUP_KYBER1024, "kyber1024", "kyber1024"},
    {NID_p521_kyber1024, SSL_GROUP_P521_KYBER1024, "p521_kyber1024", "p521_kyber1024"},
    {NID_hqc128, SSL_GROUP_HQC128, "hqc128", "hqc128"},
    {NID_p256_hqc128, SSL_GROUP_P256_HQC128, "p256_hqc128", "p256_hqc128"},
    {NID_hqc192, SSL_GROUP_HQC192, "hqc192", "hqc192"},
    {NID_p384_hqc192, SSL_GROUP_P384_HQC192, "p384_hqc192", "p384_hqc192"},
    {NID_hqc256, SSL_GROUP_HQC256, "hqc256", "hqc256"},
    {NID_p521_hqc256, SSL_GROUP_P521_HQC256, "p521_hqc256", "p521_hqc256"},
///// OQS_TEMPLATE_FRAGMENT_DEF_NAMEDGROUPS_END
};

}  // namespace

Span<const NamedGroup> NamedGroups() {
  return MakeConstSpan(kNamedGroups, OPENSSL_ARRAY_SIZE(kNamedGroups));
}

UniquePtr<SSLKeyShare> SSLKeyShare::Create(uint16_t group_id) {
  switch (group_id) {
    case SSL_GROUP_SECP224R1:
      return MakeUnique<ECKeyShare>(NID_secp224r1, SSL_GROUP_SECP224R1);
    case SSL_GROUP_SECP256R1:
      return MakeUnique<ECKeyShare>(NID_X9_62_prime256v1, SSL_GROUP_SECP256R1);
    case SSL_GROUP_SECP384R1:
      return MakeUnique<ECKeyShare>(NID_secp384r1, SSL_GROUP_SECP384R1);
    case SSL_GROUP_SECP521R1:
      return MakeUnique<ECKeyShare>(NID_secp521r1, SSL_GROUP_SECP521R1);
    case SSL_GROUP_X25519:
      return MakeUnique<X25519KeyShare>();
    case SSL_GROUP_X25519_KYBER768_DRAFT00:
      return MakeUnique<X25519Kyber768KeyShare>();
///// OQS_TEMPLATE_FRAGMENT_HANDLE_GROUP_IDS_START
    case SSL_GROUP_FRODO640AES:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_FRODO640AES, OQS_KEM_alg_frodokem_640_aes);
    case SSL_GROUP_P256_FRODO640AES:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P256_FRODO640AES, SSL_GROUP_SECP256R1, OQS_KEM_alg_frodokem_640_aes);
    case SSL_GROUP_FRODO640SHAKE:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_FRODO640SHAKE, OQS_KEM_alg_frodokem_640_shake);
    case SSL_GROUP_P256_FRODO640SHAKE:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P256_FRODO640SHAKE, SSL_GROUP_SECP256R1, OQS_KEM_alg_frodokem_640_shake);
    case SSL_GROUP_FRODO976AES:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_FRODO976AES, OQS_KEM_alg_frodokem_976_aes);
    case SSL_GROUP_P384_FRODO976AES:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P384_FRODO976AES, SSL_GROUP_SECP384R1, OQS_KEM_alg_frodokem_976_aes);
    case SSL_GROUP_FRODO976SHAKE:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_FRODO976SHAKE, OQS_KEM_alg_frodokem_976_shake);
    case SSL_GROUP_P384_FRODO976SHAKE:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P384_FRODO976SHAKE, SSL_GROUP_SECP384R1, OQS_KEM_alg_frodokem_976_shake);
    case SSL_GROUP_FRODO1344AES:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_FRODO1344AES, OQS_KEM_alg_frodokem_1344_aes);
    case SSL_GROUP_P521_FRODO1344AES:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P521_FRODO1344AES, SSL_GROUP_SECP521R1, OQS_KEM_alg_frodokem_1344_aes);
    case SSL_GROUP_FRODO1344SHAKE:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_FRODO1344SHAKE, OQS_KEM_alg_frodokem_1344_shake);
    case SSL_GROUP_P521_FRODO1344SHAKE:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P521_FRODO1344SHAKE, SSL_GROUP_SECP521R1, OQS_KEM_alg_frodokem_1344_shake);
    case SSL_GROUP_BIKEL1:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_BIKEL1, OQS_KEM_alg_bike_l1);
    case SSL_GROUP_P256_BIKEL1:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P256_BIKEL1, SSL_GROUP_SECP256R1, OQS_KEM_alg_bike_l1);
    case SSL_GROUP_BIKEL3:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_BIKEL3, OQS_KEM_alg_bike_l3);
    case SSL_GROUP_P384_BIKEL3:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P384_BIKEL3, SSL_GROUP_SECP384R1, OQS_KEM_alg_bike_l3);
    case SSL_GROUP_KYBER512:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_KYBER512, OQS_KEM_alg_kyber_512);
    case SSL_GROUP_P256_KYBER512:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P256_KYBER512, SSL_GROUP_SECP256R1, OQS_KEM_alg_kyber_512);
    case SSL_GROUP_KYBER768:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_KYBER768, OQS_KEM_alg_kyber_768);
    case SSL_GROUP_P384_KYBER768:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P384_KYBER768, SSL_GROUP_SECP384R1, OQS_KEM_alg_kyber_768);
    case SSL_GROUP_KYBER1024:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_KYBER1024, OQS_KEM_alg_kyber_1024);
    case SSL_GROUP_P521_KYBER1024:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P521_KYBER1024, SSL_GROUP_SECP521R1, OQS_KEM_alg_kyber_1024);
    case SSL_GROUP_HQC128:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_HQC128, OQS_KEM_alg_hqc_128);
    case SSL_GROUP_P256_HQC128:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P256_HQC128, SSL_GROUP_SECP256R1, OQS_KEM_alg_hqc_128);
    case SSL_GROUP_HQC192:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_HQC192, OQS_KEM_alg_hqc_192);
    case SSL_GROUP_P384_HQC192:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P384_HQC192, SSL_GROUP_SECP384R1, OQS_KEM_alg_hqc_192);
    case SSL_GROUP_HQC256:
      return MakeUnique<OQSKeyShare>(SSL_GROUP_HQC256, OQS_KEM_alg_hqc_256);
    case SSL_GROUP_P521_HQC256:
      return MakeUnique<ClassicalWithOQSKeyShare>(SSL_GROUP_P521_HQC256, SSL_GROUP_SECP521R1, OQS_KEM_alg_hqc_256);
///// OQS_TEMPLATE_FRAGMENT_HANDLE_GROUP_IDS_END
    default:
      return nullptr;
  }
}

bool ssl_nid_to_group_id(uint16_t *out_group_id, int nid) {
  for (const auto &group : kNamedGroups) {
    if (group.nid == nid) {
      *out_group_id = group.group_id;
      return true;
    }
  }
  return false;
}

bool ssl_name_to_group_id(uint16_t *out_group_id, const char *name, size_t len) {
  for (const auto &group : kNamedGroups) {
    if (len == strlen(group.name) &&
        !strncmp(group.name, name, len)) {
      *out_group_id = group.group_id;
      return true;
    }
    if (strlen(group.alias) > 0 && len == strlen(group.alias) &&
        !strncmp(group.alias, name, len)) {
      *out_group_id = group.group_id;
      return true;
    }
  }
  return false;
}

int ssl_group_id_to_nid(uint16_t group_id) {
  for (const auto &group : kNamedGroups) {
    if (group.group_id == group_id) {
      return group.nid;
    }
  }
  return NID_undef;
}

BSSL_NAMESPACE_END

using namespace bssl;

const char* SSL_get_group_name(uint16_t group_id) {
  for (const auto &group : kNamedGroups) {
    if (group.group_id == group_id) {
      return group.name;
    }
  }
  return nullptr;
}

size_t SSL_get_all_group_names(const char **out, size_t max_out) {
  return GetAllNames(out, max_out, Span<const char *>(), &NamedGroup::name,
                     MakeConstSpan(kNamedGroups));
}
