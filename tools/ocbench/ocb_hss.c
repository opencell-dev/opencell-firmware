#define _DEFAULT_SOURCE
#include "ocb_hss.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "oc_sig_crypto.h"

int ocb_hss_lock(const char *path)
{
    char lp[600];
    if (snprintf(lp, sizeof(lp), "%s.lock", path) >= (int)sizeof(lp)) return -2;
    int fd = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return -2;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int held = errno == EWOULDBLOCK;
        close(fd);
        return held ? -1 : -2;
    }
    return fd;
}

void ocb_hss_unlock(int fd)
{
    if (fd >= 0) close(fd); /* closing the description drops the lock */
}

int ocb_hss_v1_number(const uint8_t number[OC_SIG_NUMBER_LEN])
{
    char text[OC_SIG_NUMBER_TEXT];
    oc_sig_number_to_text(number, text);
    return strlen(text) == 1 + 13;
}

static void hex_out(FILE *f, const char *key, const uint8_t *b, size_t n)
{
    fprintf(f, " %s=", key);
    for (size_t i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
}

static int hex_in(const char *s, uint8_t *b, size_t n)
{
    if (strlen(s) != n * 2u) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(s + 2u * i, "%2x", &v) != 1) return -1;
        b[i] = (uint8_t)v;
    }
    return 0;
}

/* Every key=value field of one line; unknown keys and bad values fail. */
static int parse_network(ocb_hss_t *h, char *save)
{
    int seen = 0;
    for (char *tok = strtok_r(NULL, " \t\n", &save); tok != NULL; tok = strtok_r(NULL, " \t\n", &save)) {
        char *v = strchr(tok, '=');
        if (v == NULL) return -1;
        *v++ = '\0';
        if (strcmp(tok, "key_id") == 0) h->key_id = (uint16_t)strtoul(v, NULL, 10), seen |= 1;
        else if (strcmp(tok, "sk") == 0 && hex_in(v, h->sk, 32) == 0) seen |= 2;
        else if (strcmp(tok, "pk") == 0 && hex_in(v, h->pk, 32) == 0) seen |= 4;
        else if (strcmp(tok, "mode") == 0 && (strcmp(v, "part15") == 0 || strcmp(v, "part97") == 0))
            h->mode = strcmp(v, "part15") == 0 ? OC_SIG_MODE_PART15 : OC_SIG_MODE_PART97, seen |= 8;
        else if (strcmp(tok, "period") == 0) h->period_s = (uint16_t)strtoul(v, NULL, 10), seen |= 16;
        else return -1;
    }
    h->have_network = 1;
    return seen == 31 ? 0 : -1;
}

/* 0, -1 malformed, -2 a numbering-v1 number. */
static int parse_sub(oc_sig_sub_t *s, char *save)
{
    int seen = 0;
    memset(s, 0, sizeof(*s));
    for (char *tok = strtok_r(NULL, " \t\n", &save); tok != NULL; tok = strtok_r(NULL, " \t\n", &save)) {
        char *v = strchr(tok, '=');
        if (v == NULL) return -1;
        *v++ = '\0';
        if (strcmp(tok, "number") == 0 && oc_sig_number_to_bcd(v, strlen(v), s->number) == 0) {
            if (ocb_hss_v1_number(s->number)) return -2;
            seen |= 1;
        }
        else if (strcmp(tok, "token_id") == 0 && hex_in(v, s->token_id, 8) == 0) seen |= 2;
        else if (strcmp(tok, "token_secret") == 0 && hex_in(v, s->token_secret, 16) == 0) seen |= 4;
        else if (strcmp(tok, "expiry") == 0) s->token_expiry = (uint32_t)strtoul(v, NULL, 10), seen |= 8;
        else if (strcmp(tok, "used") == 0) s->token_used = atoi(v) != 0, seen |= 16;
        else if (strcmp(tok, "tmid") == 0) s->tmid = (uint32_t)strtoul(v, NULL, 16), seen |= 32;
        else if (strcmp(tok, "activated") == 0) s->activated = atoi(v) != 0, seen |= 64;
        else if (strcmp(tok, "k") == 0 && hex_in(v, s->k, 16) == 0) seen |= 128;
        else if (strcmp(tok, "opc") == 0 && hex_in(v, s->opc, 16) == 0) seen |= 256;
        else if (strcmp(tok, "sqn") == 0 && hex_in(v, s->sqn, 6) == 0) seen |= 512;
        else return -1;
    }
    return seen == 1023 ? 0 : -1;
}

int ocb_hss_load(ocb_hss_t *h, const char *path)
{
    memset(h, 0, sizeof(*h));
    FILE *f = fopen(path, "r");
    if (f == NULL) return errno == ENOENT ? 0 : -1;
    char line[512];
    int err = 0;
    unsigned ln = 0;
    while (!err && fgets(line, sizeof(line), f) != NULL) {
        char *save = NULL;
        ln++;
        char *kind = strtok_r(line, " \t\n", &save);
        if (kind == NULL || kind[0] == '#') continue;
        if (strcmp(kind, "network") == 0) {
            err = parse_network(h, save);
        } else if (strcmp(kind, "sub") == 0 && h->n < OCB_HSS_SUBS) {
            err = parse_sub(&h->subs[h->n], save);
            h->n++;
        } else {
            err = -1;
        }
    }
    fclose(f);
    if (err) {
        snprintf(h->err, sizeof(h->err), "%s:%u: %s", path, ln,
                 err == -2 ? "13-digit number (numbering v1): remove the sub lines and issue new codes"
                           : "malformed line");
    }
    return err ? -1 : 0;
}

int ocb_hss_save(const ocb_hss_t *h, const char *path)
{
    char tmp[512];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    FILE *f = fdopen(fd, "w");
    if (f == NULL) {
        close(fd);
        return -1;
    }
    fprintf(f, "# OpenCell network stand-in HSS (ocbench). Holds secrets: keep it private.\n");
    if (h->have_network) {
        fprintf(f, "network key_id=%u", h->key_id);
        hex_out(f, "sk", h->sk, 32);
        hex_out(f, "pk", h->pk, 32);
        fprintf(f, " mode=%s period=%u\n", h->mode == OC_SIG_MODE_PART97 ? "part97" : "part15", h->period_s);
    }
    for (unsigned i = 0; i < h->n; i++) {
        const oc_sig_sub_t *s = &h->subs[i];
        char num[OC_SIG_NUMBER_TEXT];
        oc_sig_number_to_text(s->number, num);
        fprintf(f, "sub number=%s", num);
        hex_out(f, "token_id", s->token_id, 8);
        hex_out(f, "token_secret", s->token_secret, 16);
        fprintf(f, " expiry=%u used=%d tmid=%08x activated=%d", s->token_expiry, s->token_used, s->tmid,
                s->activated);
        hex_out(f, "k", s->k, 16);
        hex_out(f, "opc", s->opc, 16);
        hex_out(f, "sqn", s->sqn, 6);
        fputc('\n', f);
    }
    int err = fflush(f) != 0 || fsync(fd) != 0;
    err |= fclose(f) != 0;
    if (err || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int ocb_hss_ensure_network(ocb_hss_t *h, ocb_random_fn rnd)
{
    if (h->have_network) return 0;
    rnd(h->sk, 32);
    if (oc_sig_x25519_public(h->sk, h->pk) != 0) return -1;
    h->key_id = 1;
    h->mode = OC_SIG_MODE_PART15;
    h->period_s = 1800;
    h->have_network = 1;
    return 0;
}

oc_sig_sub_t *ocb_hss_issue(ocb_hss_t *h, const uint8_t number[OC_SIG_NUMBER_LEN], uint32_t expiry,
                            ocb_random_fn rnd)
{
    oc_sig_sub_t *s = ocb_hss_by_number(h, number);
    if (s == NULL) {
        if (h->n >= OCB_HSS_SUBS) return NULL;
        s = &h->subs[h->n++];
        memset(s, 0, sizeof(*s));
        memcpy(s->number, number, OC_SIG_NUMBER_LEN);
    }
    rnd(s->token_id, 8);
    rnd(s->token_secret, 16);
    s->token_expiry = expiry;
    s->token_used = 0;
    return s;
}

oc_sig_sub_t *ocb_hss_by_token(ocb_hss_t *h, const uint8_t token_id[8])
{
    for (unsigned i = 0; i < h->n; i++) if (memcmp(h->subs[i].token_id, token_id, 8) == 0) return &h->subs[i];
    return NULL;
}

oc_sig_sub_t *ocb_hss_by_tmid(ocb_hss_t *h, uint32_t tmid)
{
    for (unsigned i = 0; i < h->n; i++) if (h->subs[i].activated && h->subs[i].tmid == tmid) return &h->subs[i];
    return NULL;
}

oc_sig_sub_t *ocb_hss_by_number(ocb_hss_t *h, const uint8_t number[OC_SIG_NUMBER_LEN])
{
    for (unsigned i = 0; i < h->n; i++) {
        if (memcmp(h->subs[i].number, number, OC_SIG_NUMBER_LEN) == 0) return &h->subs[i];
    }
    return NULL;
}

void ocb_hss_unbind(ocb_hss_t *h, uint32_t tmid)
{
    for (unsigned i = 0; i < h->n; i++) {
        if (h->subs[i].tmid == tmid) {
            h->subs[i].tmid = 0;
            h->subs[i].activated = 0;
        }
    }
}

void ocb_hss_qr(const ocb_hss_t *h, const oc_sig_sub_t *sub, oc_sig_qr_t *q)
{
    memset(q, 0, sizeof(*q));
    q->key_id = h->key_id;
    memcpy(q->pkn, h->pk, 32);
    memcpy(q->token_id, sub->token_id, 8);
    memcpy(q->token_secret, sub->token_secret, 16);
    memcpy(q->number, sub->number, OC_SIG_NUMBER_LEN);
    q->expiry = sub->token_expiry;
}
