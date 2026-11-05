/*
 * The boot legal screen with PC wording, by default.
 *
 * bonus\LEGAL.xbs holds the legal screen's text (legalbox.txt) with a
 * Microsoft / Xbox trademark paragraph. At start-up, when the overlay does
 * not have its own copy yet, this writes overlay\pc and overlay\pc_ps
 * \data\bonus\LEGAL.xbs: the player's own package with that paragraph made a
 * Windows PC one (same four lines, none longer, so the layout holds). It is
 * what tools/make_overlay.py does, here so every install has it without
 * Python; the game reads the overlay first (RECOMP_OVERLAY_DIR, settings.c).
 *
 * A package (tools/xbs.py) is a 0x40-byte header and three zlib streams:
 * binary, text, and a directory of 64-byte entries (+0x20 offset, +0x24
 * size, +0x2C stream 0x100 = text, +0x34 space). The streams are inflated
 * here; the new package's are written as stored (uncompressed) deflate
 * blocks, which any zlib reader takes. The disc's copy is only read.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MS_PARAGRAPH \
    "Microsoft, Xbox, and the Xbox logo are either registered \r\n" \
    "trademarks or trademarks of Microsoft Corporation in the \r\n" \
    "U.S. and/or other countries and are used under license \r\n" \
    "from Microsoft."
#define PC_PARAGRAPH \
    "This Windows PC version is an unofficial fan \r\n" \
    "recompilation, not affiliated with or endorsed by \r\n" \
    "Activision, Treyarch, Marvel, Columbia Pictures or \r\n" \
    "Microsoft. Windows is a trademark of Microsoft Corp."

/* ── inflate (RFC 1950 / 1951), enough for a package's streams ── */

typedef struct { const uint8_t *p, *end; uint32_t bits; int n; } Bits;
typedef struct { uint8_t *d; size_t n, cap; } Buf;

static int need(Bits *b, int k)
{
    while (b->n < k) {
        if (b->p >= b->end)
            return 0;
        b->bits |= (uint32_t)*b->p++ << b->n;
        b->n += 8;
    }
    return 1;
}

static int getbits(Bits *b, int k, uint32_t *v)
{
    if (!need(b, k))
        return 0;
    *v = b->bits & ((1u << k) - 1);
    b->bits >>= k;
    b->n -= k;
    return 1;
}

#define INFLATE_MAX (64u << 20)   /* far above any package; stops a "zip bomb" */

static int put(Buf *o, uint8_t c)
{
    if (o->n >= INFLATE_MAX)
        return 0;
    if (o->n == o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 1 << 16;
        uint8_t *d = (uint8_t *)realloc(o->d, cap);
        if (!d)
            return 0;
        o->d = d;
        o->cap = cap;
    }
    o->d[o->n++] = c;
    return 1;
}

/* A canonical Huffman code: counts per length, symbols by code. */
typedef struct { uint16_t count[16], sym[320]; } Huff;

static void huff_build(Huff *h, const uint8_t *len, int n)
{
    uint16_t offs[16];
    int i;
    memset(h, 0, sizeof *h);
    for (i = 0; i < n; i++)
        h->count[len[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (i = 1; i < 15; i++)
        offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++)
        if (len[i])
            h->sym[offs[len[i]]++] = (uint16_t)i;
}

static int huff_decode(Bits *b, const Huff *h, int *sym)
{
    int code = 0, first = 0, index = 0, len;
    for (len = 1; len < 16; len++) {
        uint32_t bit;
        if (!getbits(b, 1, &bit))
            return 0;
        code |= (int)bit;
        if (code - first < h->count[len]) {
            *sym = h->sym[index + code - first];
            return 1;
        }
        index += h->count[len];
        first = (first + h->count[len]) << 1;
        code <<= 1;
    }
    return 0;
}

static const uint16_t LBASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t LEXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t DBASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const uint8_t DEXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(Bits *b, Buf *o, const Huff *lit, const Huff *dist)
{
    for (;;) {
        int sym;
        uint32_t e;
        if (!huff_decode(b, lit, &sym))
            return 0;
        if (sym < 256) {
            if (!put(o, (uint8_t)sym))
                return 0;
        } else if (sym == 256) {
            return 1;
        } else {
            size_t len, d, i;
            sym -= 257;
            if (sym >= 29 || !getbits(b, LEXTRA[sym], &e))
                return 0;
            len = LBASE[sym] + e;
            if (!huff_decode(b, dist, &sym) || sym >= 30 || !getbits(b, DEXTRA[sym], &e))
                return 0;
            d = DBASE[sym] + e;
            if (d > o->n)
                return 0;
            for (i = 0; i < len; i++)
                if (!put(o, o->d[o->n - d]))
                    return 0;
        }
    }
}

static int inflate_zlib(const uint8_t *src, size_t n, Buf *o)
{
    Bits b = { src + 2, src + n, 0, 0 };
    uint32_t last, type;

    if (n < 2 || (src[0] & 0x0F) != 8 || ((src[0] << 8) | src[1]) % 31)
        return 0;
    do {
        if (!getbits(&b, 1, &last) || !getbits(&b, 2, &type))
            return 0;
        if (type == 0) {                    /* stored */
            uint16_t len;
            b.bits = 0;
            b.n = 0;
            if (b.end - b.p < 4)
                return 0;
            len = (uint16_t)(b.p[0] | b.p[1] << 8);
            b.p += 4;
            if (b.end - b.p < len)
                return 0;
            while (len--)
                if (!put(o, *b.p++))
                    return 0;
        } else if (type == 1) {             /* fixed codes */
            uint8_t l[288 + 30];
            Huff lit, dist;
            int i;
            for (i = 0; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(&lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dist, l, 30);
            if (!codes(&b, o, &lit, &dist))
                return 0;
        } else if (type == 2) {             /* dynamic codes */
            static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            uint8_t l[288 + 32];
            uint32_t hlit, hdist, hclen, v;
            Huff lc, lit, dist;
            int i, nlen;
            if (!getbits(&b, 5, &hlit) || !getbits(&b, 5, &hdist) || !getbits(&b, 4, &hclen))
                return 0;
            hlit += 257; hdist += 1; hclen += 4;
            if (hlit > 286 || hdist > 30)       /* RFC 1951 limits; more would overrun l */
                return 0;
            memset(l, 0, 19);
            for (i = 0; i < (int)hclen; i++) {
                if (!getbits(&b, 3, &v))
                    return 0;
                l[order[i]] = (uint8_t)v;
            }
            huff_build(&lc, l, 19);
            nlen = (int)(hlit + hdist);
            for (i = 0; i < nlen; ) {
                int sym, rep = 0;
                uint8_t val = 0;
                if (!huff_decode(&b, &lc, &sym))
                    return 0;
                if (sym < 16) {
                    l[i++] = (uint8_t)sym;
                    continue;
                }
                if (sym == 16) {
                    if (!i || !getbits(&b, 2, &v)) return 0;
                    val = l[i - 1]; rep = 3 + (int)v;
                } else if (sym == 17) {
                    if (!getbits(&b, 3, &v)) return 0;
                    rep = 3 + (int)v;
                } else {
                    if (!getbits(&b, 7, &v)) return 0;
                    rep = 11 + (int)v;
                }
                if (i + rep > nlen)
                    return 0;
                while (rep--)
                    l[i++] = val;
            }
            huff_build(&lit, l, (int)hlit);
            huff_build(&dist, l + hlit, (int)hdist);
            if (!codes(&b, o, &lit, &dist))
                return 0;
        } else {
            return 0;
        }
    } while (!last);
    return 1;
}

/* A zlib stream of stored blocks: no compression, read by any inflater. */
static int deflate_stored(const uint8_t *src, size_t n, Buf *o)
{
    uint32_t a = 1, s = 0;
    size_t i = 0, k;

    if (!put(o, 0x78) || !put(o, 0x01))
        return 0;
    do {
        size_t len = n - i > 0xFFFF ? 0xFFFF : n - i;
        int last = i + len == n;
        if (!put(o, (uint8_t)last) || !put(o, (uint8_t)len) || !put(o, (uint8_t)(len >> 8))
                || !put(o, (uint8_t)~len) || !put(o, (uint8_t)(~len >> 8)))
            return 0;
        for (k = 0; k < len; k++)
            if (!put(o, src[i + k]))
                return 0;
        i += len;
    } while (i < n);
    for (i = 0; i < n; i++) {
        a = (a + src[i]) % 65521;
        s = (s + a) % 65521;
    }
    return put(o, (uint8_t)(s >> 8)) && put(o, (uint8_t)s) && put(o, (uint8_t)(a >> 8)) && put(o, (uint8_t)a);
}

/* ── the package ─────────────────────────────────────────────── */

static uint8_t *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *d = NULL;
    long len;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len > 0 && (d = (uint8_t *)malloc((size_t)len)) != NULL && fread(d, 1, (size_t)len, f) != (size_t)len) {
        free(d);
        d = NULL;
    }
    fclose(f);
    *n = d ? (size_t)len : 0;
    return d;
}

static void make_dirs(char *path)        /* every folder up to the file name */
{
    char *p;
    for (p = path + 3; *p; p++)
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(path, NULL);
            *p = '\\';
        }
}

/* The rebuilt package, or NULL. */
static Buf *build(const uint8_t *d, size_t n)
{
    uint32_t h[16];
    Buf s[3] = { { 0 } }, z[3] = { { 0 } }, *out = NULL;
    size_t pl = strlen(MS_PARAGRAPH), ql = strlen(PC_PARAGRAPH), at, i;
    int k, found = 0;

    if (n < 0x40)
        return NULL;
    memcpy(h, d, sizeof h);
    /* streams: 0 at h[4] (h[12] packed), 1 at h[8] (h[13]), 2 at h[0] (h[11]) */
    if ((size_t)h[4] + h[12] > n || (size_t)h[8] + h[13] > n || (size_t)h[0] + h[11] > n
            || !inflate_zlib(d + h[4], h[12], &s[0]) || !inflate_zlib(d + h[8], h[13], &s[1])
            || !inflate_zlib(d + h[0], h[11], &s[2])
            || s[0].n != h[5] || s[1].n != h[9] || s[2].n != 64u * h[2])
        goto done;
    /* The text entry holding the paragraph: its new text goes at the end of
     * the text stream, 0x20-aligned, as xbs.py's put() does. */
    for (i = 0; i < s[2].n; i += 64) {
        uint8_t *e = s[2].d + i;
        uint32_t off, size, space;
        uint8_t *t, *m;
        memcpy(&off, e + 0x20, 4);
        memcpy(&size, e + 0x24, 4);
        if (*(uint32_t *)(e + 0x2C) != 0x100 || (size_t)off + size > s[1].n || size < pl)
            continue;
        t = s[1].d + off;
        for (m = t; m + pl <= t + size && memcmp(m, MS_PARAGRAPH, pl); m++)
            ;
        if (m + pl > t + size)
            continue;
        {
            size_t before = (size_t)(m - t), after = size - before - pl, newsize = before + ql + after;
            uint8_t *copy = (uint8_t *)malloc(size);
            if (!copy)
                goto done;
            memcpy(copy, t, size);
            while (s[1].n % 0x20)
                if (!put(&s[1], 0)) { free(copy); goto done; }
            at = s[1].n;
            for (k = 0; k < (int)before; k++) put(&s[1], copy[k]);
            for (k = 0; k < (int)ql; k++) put(&s[1], (uint8_t)PC_PARAGRAPH[k]);
            for (k = 0; k < (int)after; k++) put(&s[1], copy[before + pl + k]);
            space = (uint32_t)((newsize + 15) & ~(size_t)15);
            while (s[1].n < at + space)
                put(&s[1], 0);
            free(copy);
            off = (uint32_t)at;
            size = (uint32_t)newsize;
            memcpy(e + 0x20, &off, 4);
            memcpy(e + 0x24, &size, 4);
            memcpy(e + 0x34, &space, 4);
            found = 1;
        }
    }
    if (!found)
        goto done;
    for (k = 0; k < 3; k++)
        if (!deflate_stored(s[k].d, s[k].n, &z[k]))
            goto done;
    if (!(out = (Buf *)calloc(1, sizeof *out)))
        goto done;
    for (k = 0; k < 0x40; k++)
        put(out, 0);
    {
        static const int order[3] = { 0, 1, 2 };     /* stream 0, then 1, then the directory */
        uint32_t pos[3];
        for (k = 0; k < 3; k++) {
            int st = order[k];
            size_t j;
            while (out->n % 0x20)
                put(out, 0);
            pos[st] = (uint32_t)out->n;
            for (j = 0; j < z[st].n; j++)
                put(out, z[st].d[j]);
        }
        h[4] = pos[0]; h[8] = pos[1]; h[0] = pos[2];
        h[5] = (uint32_t)s[0].n; h[9] = (uint32_t)s[1].n; h[2] = (uint32_t)(s[2].n / 64);
        h[12] = (uint32_t)z[0].n; h[13] = (uint32_t)z[1].n; h[11] = (uint32_t)z[2].n;
        h[6] = (uint32_t)((out->n + 3) & ~(size_t)3);
        while (out->n < h[6] + 0x20)
            put(out, 0);
        memcpy(out->d, h, sizeof h);
    }
done:
    for (k = 0; k < 3; k++) {
        free(s[k].d);
        free(z[k].d);
    }
    return out;
}

/* Called at start-up, after settings.c: writes whichever overlay copies are
 * missing. game_dir is the player's copy (main.c). */
void legal_pc_ensure(const char *game_dir)
{
    static const char *const variants[] = { "pc", "pc_ps" };
    char exe[MAX_PATH], src[MAX_PATH], dst[MAX_PATH], *slash;
    uint8_t *d = NULL;
    size_t n = 0;
    Buf *out = NULL;
    int v;

    GetModuleFileNameA(NULL, exe, MAX_PATH);
    if ((slash = strrchr(exe, '\\')) != NULL)
        slash[1] = 0;
    for (v = 0; v < 2; v++) {
        FILE *f;
        _snprintf(dst, sizeof dst - 1, "%soverlay\\%s\\data\\bonus\\LEGAL.xbs", exe, variants[v]);
        dst[sizeof dst - 1] = 0;
        if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES)
            continue;
        if (!out) {
            _snprintf(src, sizeof src - 1, "%s\\data\\bonus\\LEGAL.xbs", game_dir);
            src[sizeof src - 1] = 0;
            if (!(d = read_file(src, &n)) || !(out = build(d, n))) {
                fprintf(stderr, "  [LEGAL] could not make the PC legal screen from %s\n", src);
                break;
            }
        }
        make_dirs(dst);
        if ((f = fopen(dst, "wb")) != NULL) {
            fwrite(out->d, 1, out->n, f);
            fclose(f);
            fprintf(stderr, "  [LEGAL] PC legal screen -> %s\n", dst);
        }
    }
    free(d);
    if (out) {
        free(out->d);
        free(out);
    }
}
