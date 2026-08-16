#include "tilefinch/update.h"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#include <openssl/obj_mac.h>

static bool openssl_verify(
    void *opaque, const uint8_t public_point[65],
    const uint8_t digest[32], const uint8_t signature[64])
{
    (void) opaque;
    bool verified = false;
    EC_KEY *key = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
    ECDSA_SIG *sig = ECDSA_SIG_new();
    BIGNUM *r = BN_bin2bn(signature, 32, NULL);
    BIGNUM *s = BN_bin2bn(signature + 32, 32, NULL);
    const unsigned char *point_cursor = public_point;
    if (key != NULL && sig != NULL && r != NULL && s != NULL
        && o2i_ECPublicKey(&key, &point_cursor, 65) != NULL
        && ECDSA_SIG_set0(sig, r, s) == 1) {
        r = NULL;
        s = NULL;
        verified = ECDSA_do_verify(digest, 32, sig, key) == 1;
    }
    BN_free(r);
    BN_free(s);
    ECDSA_SIG_free(sig);
    EC_KEY_free(key);
    return verified;
}

TilefinchUpdateCrypto tilefinch_update_default_crypto(void)
{
    return (TilefinchUpdateCrypto) {.verify = openssl_verify};
}
