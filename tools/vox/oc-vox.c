/* oc-vox: encode, decode, inspect and time OpenVoxNB files on the host.
 *
 *   oc-vox enc MODE IN.raw OUT.ovx [--ref]
 *   oc-vox dec IN.ovx OUT.raw [--ref] [--lose N] [--ber P --seed S]
 *   oc-vox info IN.ovx
 *   oc-vox bench MODE [SECONDS]
 *   oc-vox hash IN.raw
 *   oc-vox analyse MODE IN.raw            (the reference's frames as text, for tools/vox/vox_train_cb.py)
 *
 * MODE is the bitrate: 600, 800, 1000, 1200, 1400, 1600, 1800, 2400 or
 * 3200. .raw is 8 kHz mono s16le, as tools/clip/oc-clip-gen writes it; the
 * input's last partial block is dropped. .ovx is a two-byte header, the
 * magic 0xC5 then version << 4 | mode, then headerless blocks (BITSTREAM.md). The
 * production (fixed-point) codec unless --ref. --lose N drops every Nth
 * block (the decoder conceals it); --ber P flips each bit with
 * probability P (xorshift, --seed S, default 1). Errors go to stderr,
 * exit 1. */
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "oc_vox.h"
#include "oc_vox_bits.h"
#include "oc_vox_int.h"
#include "oc_vox_ref.h"

static int fail(const char *msg) { fprintf(stderr, "oc-vox: %s\n", msg); return 1; }

static int parse_mode(const char *s)
{
    int bps = atoi(s);
    for (int m = 0; m < (int)OC_VOX_MODE_COUNT; m++)
        if (oc_vox_mode_bps((oc_vox_mode_t)m) == bps) return m;
    return OC_VOX_MODE_NONE;
}

static int has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 0; i < argc; i++) if (!strcmp(argv[i], flag)) return 1;
    return 0;
}

static const char *flag_value(int argc, char **argv, const char *flag)
{
    for (int i = 0; i + 1 < argc; i++) if (!strcmp(argv[i], flag)) return argv[i + 1];
    return NULL;
}

static uint32_t xs = 1;
static uint32_t rnd(void) { xs ^= xs << 13; xs ^= xs >> 17; xs ^= xs << 5; return xs; }

#define OVX_MAGIC 0xC5u
static uint8_t header(oc_vox_mode_t m) { return (uint8_t)((OC_VOX_VERSION << 4) | (unsigned)m); }

static int cmd_enc(int argc, char **argv)
{
    int m, ref;
    FILE *in, *out;
    int16_t pcm[OC_VOX_BLOCK_SAMPLES];
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
    oc_vox_enc_t e;
    oc_vox_ref_enc_t er;
    unsigned n = 0;
    if (argc < 3) return fail("enc MODE IN.raw OUT.ovx [--ref]");
    m = parse_mode(argv[0]);
    if (m == OC_VOX_MODE_NONE) return fail("no such mode: 600, 800, 1000, 1200, 1400, 1600, 1800, 2400 or 3200");
    ref = has_flag(argc, argv, "--ref");
    in = fopen(argv[1], "rb");
    if (in == NULL) return fail("can't read the input");
    out = fopen(argv[2], "wb");
    if (out == NULL) { fclose(in); return fail("can't write the output"); }
    oc_vox_enc_init(&e, (oc_vox_mode_t)m);
    oc_vox_ref_enc_init(&er, (oc_vox_mode_t)m);
    fputc(OVX_MAGIC, out);
    fputc(header((oc_vox_mode_t)m), out);
    while (fread(pcm, sizeof(int16_t), OC_VOX_BLOCK_SAMPLES, in) == OC_VOX_BLOCK_SAMPLES) {
        if (ref) oc_vox_ref_encode(&er, pcm, block); else oc_vox_encode(&e, pcm, block);
        fwrite(block, 1, oc_vox_block_bytes((oc_vox_mode_t)m), out);
        n++;
    }
    fclose(in);
    fclose(out);
    fprintf(stderr, "oc-vox: %u blocks (%u.%02u s) of %s%s\n", n, n * 120 / 1000, (n * 120 % 1000) / 10, oc_vox_mode_name((oc_vox_mode_t)m), ref ? " (reference)" : "");
    return 0;
}

static int read_header(FILE *in, oc_vox_mode_t *m)
{
    int magic = fgetc(in), h = fgetc(in);
    if (magic != (int)OVX_MAGIC || h == EOF) return -1;
    if ((unsigned)(h >> 4) != OC_VOX_VERSION) return -2;
    if ((h & 15) >= (int)OC_VOX_MODE_COUNT) return -1;
    *m = (oc_vox_mode_t)(h & 15);
    return 0;
}

static int cmd_dec(int argc, char **argv)
{
    oc_vox_mode_t m;
    FILE *in, *out;
    int ref, lose = 0, hdr;
    double ber = 0.0;
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
    int16_t pcm[OC_VOX_BLOCK_SAMPLES];
    oc_vox_dec_t d;
    oc_vox_ref_dec_t dr;
    unsigned n = 0, nb;
    const char *v;
    if (argc < 2) return fail("dec IN.ovx OUT.raw [--ref] [--lose N] [--ber P --seed S]");
    ref = has_flag(argc, argv, "--ref");
    if ((v = flag_value(argc, argv, "--lose")) != NULL) lose = atoi(v);
    if ((v = flag_value(argc, argv, "--ber")) != NULL) ber = atof(v);
    if ((v = flag_value(argc, argv, "--seed")) != NULL) xs = (uint32_t)strtoul(v, NULL, 0);
    if (xs == 0) xs = 1;
    in = fopen(argv[0], "rb");
    if (in == NULL) return fail("can't read the input");
    hdr = read_header(in, &m);
    if (hdr == -2) { fclose(in); return fail("another bitstream version"); }
    if (hdr < 0) { fclose(in); return fail("not an .ovx file"); }
    out = fopen(argv[1], "wb");
    if (out == NULL) { fclose(in); return fail("can't write the output"); }
    nb = oc_vox_block_bytes(m);
    oc_vox_dec_init(&d, m);
    oc_vox_ref_dec_init(&dr, m);
    while (fread(block, 1, nb, in) == nb) {
        const uint8_t *b = block;
        n++;
        if (lose > 0 && n % (unsigned)lose == 0) b = NULL;
        if (ber > 0.0)
            for (unsigned i = 0; i < nb * 8u; i++)
                if ((double)(rnd() % 1000000u) / 1000000.0 < ber) oc_vox_bit_set(block, i, !oc_vox_bit_get(block, i));
        if (ref) oc_vox_ref_decode(&dr, b, pcm); else oc_vox_decode(&d, b, pcm);
        fwrite(pcm, sizeof(int16_t), OC_VOX_BLOCK_SAMPLES, out);
    }
    fclose(in);
    fclose(out);
    {
        const oc_vox_dec_stats_t *st = ref ? oc_vox_ref_dec_stats(&dr) : oc_vox_dec_stats(&d);
        fprintf(stderr, "oc-vox: %u blocks of %s%s: lost %" PRIu32 ", concealed frames %" PRIu32 ", toggle gaps %" PRIu32
                        ", golay fixed %" PRIu32 " failed %" PRIu32 ", hamming fixed %" PRIu32 ", crc failed %" PRIu32 "\n",
                n, oc_vox_mode_name(m), ref ? " (reference)" : "", st->lost_blocks, st->concealed_frames, st->toggle_gaps,
                st->golay_fixed, st->golay_failed, st->hamming_fixed, st->crc_failed);
    }
    return 0;
}

static int cmd_info(int argc, char **argv)
{
    oc_vox_mode_t m;
    FILE *in;
    long size;
    int hdr;
    if (argc < 1) return fail("info IN.ovx");
    in = fopen(argv[0], "rb");
    if (in == NULL) return fail("can't read the input");
    hdr = read_header(in, &m);
    if (hdr == -2) { fclose(in); return fail("another bitstream version"); }
    if (hdr < 0) { fclose(in); return fail("not an .ovx file"); }
    fseek(in, 0, SEEK_END);
    size = ftell(in) - 2;
    fclose(in);
    {
        unsigned nb = oc_vox_block_bytes(m), blocks = (unsigned)(size / nb);
        printf("%s %u bit/s, %u bytes a block, version %u, %u blocks, %u.%02u s%s\n", oc_vox_mode_name(m), oc_vox_mode_bps(m), nb,
               OC_VOX_VERSION, blocks, blocks * 120 / 1000, (blocks * 120 % 1000) / 10, (size % nb) ? " (and a partial block)" : "");
    }
    return 0;
}

static uint64_t fnv1a_file(FILE *f)
{
    uint64_t h = 0xcbf29ce484222325ull;
    int c;
    while ((c = fgetc(f)) != EOF) { h ^= (uint8_t)c; h *= 0x100000001b3ull; }
    return h;
}

static int cmd_hash(int argc, char **argv)
{
    FILE *in;
    if (argc < 1) return fail("hash IN.raw");
    in = fopen(argv[0], "rb");
    if (in == NULL) return fail("can't read the input");
    printf("%016" PRIx64 "\n", fnv1a_file(in));
    fclose(in);
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* A speech-shaped test signal: a pulse train at 140 Hz through nothing
 * fancy, plus noise, so both the harmonic and the noise paths work. */
static void test_signal(int16_t *pcm, unsigned n)
{
    uint32_t r = 7;
    double ph = 0;
    for (unsigned i = 0; i < n; i++) {
        double v = 0;
        r ^= r << 13; r ^= r >> 17; r ^= r << 5;
        ph += 140.0 / 8000.0;
        if (ph >= 1.0) ph -= 1.0;
        for (int h = 1; h <= 12; h++) v += 3000.0 / h * sin(6.283185307 * h * ph);
        v += ((int)(r % 2001u) - 1000) * 0.5;
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
}

static int cmd_bench(int argc, char **argv)
{
    int m;
    unsigned seconds = 10, blocks, b;
    static int16_t pcm[8000 * 60];
    uint8_t block[OC_VOX_BLOCK_MAX_BYTES];
    oc_vox_enc_t e;
    oc_vox_dec_t d;
    oc_vox_ref_enc_t er;
    oc_vox_ref_dec_t dr;
    double t0, te, td, ter, tdr;
    if (argc < 1) return fail("bench MODE [SECONDS]");
    m = parse_mode(argv[0]);
    if (m == OC_VOX_MODE_NONE) return fail("no such mode");
    if (argc > 1) seconds = (unsigned)atoi(argv[1]);
    if (seconds < 1 || seconds > 60) return fail("1 to 60 seconds");
    blocks = seconds * 8000u / OC_VOX_BLOCK_SAMPLES;
    test_signal(pcm, blocks * OC_VOX_BLOCK_SAMPLES);
    oc_vox_enc_init(&e, (oc_vox_mode_t)m);
    oc_vox_dec_init(&d, (oc_vox_mode_t)m);
    oc_vox_ref_enc_init(&er, (oc_vox_mode_t)m);
    oc_vox_ref_dec_init(&dr, (oc_vox_mode_t)m);
    t0 = now_s();
    for (b = 0; b < blocks; b++) oc_vox_encode(&e, pcm + 960 * b, block);
    te = now_s() - t0;
    t0 = now_s();
    for (b = 0; b < blocks; b++) oc_vox_decode(&d, block, pcm + 960 * b);
    td = now_s() - t0;
    test_signal(pcm, blocks * OC_VOX_BLOCK_SAMPLES);
    t0 = now_s();
    for (b = 0; b < blocks; b++) oc_vox_ref_encode(&er, pcm + 960 * b, block);
    ter = now_s() - t0;
    t0 = now_s();
    for (b = 0; b < blocks; b++) oc_vox_ref_decode(&dr, block, pcm + 960 * b);
    tdr = now_s() - t0;
    printf("%s, %u s of audio on this host:\n", oc_vox_mode_name((oc_vox_mode_t)m), seconds);
    printf("  production  encode %6.1fx realtime (%.2f ms a block), decode %6.1fx realtime (%.2f ms a block)\n",
           seconds / te, 1000.0 * te / blocks, seconds / td, 1000.0 * td / blocks);
    printf("  reference   encode %6.1fx realtime (%.2f ms a block), decode %6.1fx realtime (%.2f ms a block)\n",
           seconds / ter, 1000.0 * ter / blocks, seconds / tdr, 1000.0 * tdr / blocks);
    return 0;
}

static int cmd_analyse(int argc, char **argv)
{
    int m;
    FILE *in;
    int16_t pcm[OC_VOX_BLOCK_SAMPLES];
    oc_vox_pframe_t fr[OC_VOX_FRAMES];
    oc_vox_ref_enc_t er;
    if (argc < 2) return fail("analyse MODE IN.raw");
    m = parse_mode(argv[0]);
    if (m == OC_VOX_MODE_NONE) return fail("no such mode");
    in = fopen(argv[1], "rb");
    if (in == NULL) return fail("can't read the input");
    oc_vox_ref_enc_init(&er, (oc_vox_mode_t)m);
    printf("# e p v vmix emid s0..s11 (oc_vox_int.h units), one 20 ms frame a line\n");
    while (fread(pcm, sizeof(int16_t), OC_VOX_BLOCK_SAMPLES, in) == OC_VOX_BLOCK_SAMPLES) {
        oc_vox_ref_analyse_block(&er, pcm, fr);
        for (unsigned f = 0; f < OC_VOX_FRAMES; f++) {
            printf("%u %u %u %u %d", fr[f].e, fr[f].p, fr[f].v, fr[f].vmix, fr[f].emid);
            for (unsigned b = 0; b < OC_VOX_BANDS; b++) printf(" %d", fr[f].s[b]);
            printf("\n");
        }
    }
    fclose(in);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) return fail("enc | dec | info | bench | hash | analyse (see the top of oc-vox.c)");
    if (!strcmp(argv[1], "enc")) return cmd_enc(argc - 2, argv + 2);
    if (!strcmp(argv[1], "dec")) return cmd_dec(argc - 2, argv + 2);
    if (!strcmp(argv[1], "info")) return cmd_info(argc - 2, argv + 2);
    if (!strcmp(argv[1], "bench")) return cmd_bench(argc - 2, argv + 2);
    if (!strcmp(argv[1], "hash")) return cmd_hash(argc - 2, argv + 2);
    if (!strcmp(argv[1], "analyse")) return cmd_analyse(argc - 2, argv + 2);
    return fail("unknown command");
}
