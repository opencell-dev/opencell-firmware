/* Key derivations (spec §3.2, §4.3). HMAC/HKDF inputs are byte strings;
 * TMID and call id big-endian. All return 0 on success. */
#ifndef LC_SIG_KEYS_H
#define LC_SIG_KEYS_H

#include <stddef.h>
#include <stdint.h>

int lc_sig_ct_equal(const uint8_t *a, const uint8_t *b, size_t n); /* 1 if equal, constant time */

/* Zero n bytes at p in a way the compiler must not optimise away (a
 * volatile byte loop): for key material and other secrets that must not
 * linger on the stack once they are no longer needed. */
void lc_sig_wipe(void *p, size_t n);

int lc_sig_act_tag(const uint8_t secret[16], uint32_t tmid, const uint8_t pkt[32], const uint8_t token_id[8],
                   uint8_t tag[8]);
int lc_sig_act_nak_tag(const uint8_t secret[16], uint32_t tmid, const uint8_t token_id[8], uint8_t reason,
                       uint8_t tag[8]);
int lc_sig_act_keys(const uint8_t priv[32], const uint8_t peer[32], uint32_t tmid, const uint8_t token_id[8],
                    uint8_t k[16], uint8_t opc[16]);
int lc_sig_act_confirm(const uint8_t k[16], uint32_t tmid, const uint8_t token_id[8], uint8_t out[8]);
int lc_sig_session_keys(const uint8_t ck[16], const uint8_t ik[16], const uint8_t rand[16], uint32_t tmid,
                        uint8_t k_int[16], uint8_t k_enc[16]);
int lc_sig_voice_key(const uint8_t ck[16], const uint8_t ik[16], const uint8_t rand[16], uint32_t tmid,
                     uint32_t call_id, uint8_t out[16]);

#endif
