// MSBT message files (msbt.h). The format: a 0x20-byte header ("MsgStdBn", the byte-order mark FF FE, u16 0, u8
// encoding (0 UTF-8, 1 UTF-16), u8 version, u16 section count, u16 0, u32 file size, padding), then the sections, each
// a 16-byte head (its 4-character name, u32 size, 8 bytes) and its data, padded with AB to 16. LBL1: u32 bucket count,
// per bucket {u32 label count, u32 offset of its first label from the data}, each label {u8 length, the name, u32
// message index}. TXT2: u32 message count, u32 offset of each from the data, then the messages: code units of the
// encoding, where a control tag is 0E {u16 group, u16 type, u16 size, its parameters} and a closing tag 0F {u16 group,
// u16 type}, ending in a 0 unit.
#include <stdlib.h>
#include <string.h>

#include "msbt.h"

#define MAX_SECTIONS 16

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

uint64_t msbt_hash(const void *p, size_t n) {
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

typedef struct { char name[4]; size_t at, size; } Sec;   // at: the section's data

static size_t pad16(size_t n) { return (n + 15) & ~(size_t)15; }

// The sections of the MSBT p[0..n) and whether its code units are UTF-16; 0 with why.
static int sections(const uint8_t *p, size_t n, Sec *sec, int *count, int *wide, const char **why) {
    if (n < 0x20 || memcmp(p, "MsgStdBn", 8)) return *why = "not an MSBT message file", 0;
    if (p[8] != 0xff || p[9] != 0xfe) return *why = "a big-endian MSBT", 0;
    if (p[0xc] > 1) return *why = "an MSBT in an encoding other than UTF-8 and UTF-16", 0;
    if (rd32(p + 0x12) != n) return *why = "an MSBT whose size field is not its size", 0;
    unsigned c = rd16(p + 0xe);
    if (c > MAX_SECTIONS) return *why = "an MSBT with more sections than expected", 0;
    size_t o = 0x20;
    for (unsigned i = 0; i < c; i++) {
        if (n - o < 16) return *why = "an MSBT section past the end", 0;
        size_t size = rd32(p + o + 4);
        if (size > n - o - 16) return *why = "an MSBT section past the end", 0;
        memcpy(sec[i].name, p + o, 4);
        sec[i].at = o + 16, sec[i].size = size;
        o = pad16(o + 16 + size);
        if (o > n) o = n;
    }
    *count = (int)c, *wide = p[0xc] == 1;
    return 1;
}

static const Sec *section(const Sec *sec, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (!memcmp(sec[i].name, name, 4)) return &sec[i];
    return NULL;
}

// The index of the message labelled label; -1 with why.
static long label_index(const uint8_t *p, const Sec *lbl, const char *label, const char **why) {
    const uint8_t *d = p + lbl->at;
    size_t len = strlen(label);
    if (lbl->size < 4) return *why = "an MSBT label table cut short", -1;
    uint32_t buckets = rd32(d);
    if (buckets > (lbl->size - 4) / 8) return *why = "an MSBT label table cut short", -1;
    for (uint32_t b = 0; b < buckets; b++) {
        uint32_t cnt = rd32(d + 4 + 8 * b), off = rd32(d + 8 + 8 * b);
        for (uint32_t j = 0; j < cnt; j++) {
            if (off >= lbl->size || lbl->size - off < 5 || d[off] > lbl->size - off - 5)
                return *why = "an MSBT label past its table", -1;
            if (d[off] == len && !memcmp(d + off + 1, label, len)) return (long)rd32(d + off + 1 + len);
            off += 1u + d[off] + 4;
        }
    }
    return *why = "the file has no message with that label", -1;
}

typedef struct { uint8_t *p; size_t n, cap; int bad; } Buf;

static void put(Buf *b, const void *s, size_t k) {
    if (b->bad || !k) return;
    if (b->n + k > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + k) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) {
            b->bad = 1;
            return;
        }
        b->p = q, b->cap = cap;
    }
    memcpy(b->p + b->n, s, k);
    b->n += k;
}

static void put16(Buf *b, unsigned v) {
    uint8_t x[2] = {(uint8_t)v, (uint8_t)(v >> 8)};
    put(b, x, 2);
}

static void unit(Buf *b, unsigned u, int wide) {
    if (wide) put16(b, u);
    else {
        uint8_t x = (uint8_t)u;
        put(b, &x, 1);
    }
}

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// A decimal number at *s up to 0xffff; -1 when there is none.
static long number(const char **s) {
    long v = -1;
    for (; **s >= '0' && **s <= '9'; (*s)++)
        if ((v = (v < 0 ? 0 : v * 10) + (**s - '0')) > 0xffff) return -1;
    return v;
}

// text (the markup msbt.h describes) as code units of the file's encoding, then the terminating 0; 0 with why.
static int encode(Buf *b, const char *text, int wide, const char **why) {
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        if (*s == '}') return *why = "the text has a '}' without its '{'", 0;
        if (*s == '{') {
            const char *q = (const char *)s + 1;
            int closing = *q == '/';
            q += closing;
            long group = number(&q), type = *q == '.' ? (q++, number(&q)) : -1;
            if (group < 0 || type < 0) return *why = "a tag in the text is not {group.type:...}", 0;
            const char *hex = q + (*q == ':'), *end = strchr(hex, '}');
            if (!end || (closing ? q != end : *q != ':') || (end - hex) % 2 || end - hex > 2 * 0xffff)
                return *why = "a tag in the text is not {group.type:hex} or {/group.type}", 0;
            unit(b, closing ? 0x0f : 0x0e, wide);
            put16(b, (unsigned)group), put16(b, (unsigned)type);
            if (!closing) {
                put16(b, (unsigned)(end - hex) / 2);
                for (const char *h = hex; h < end; h += 2) {
                    int hi = hexval(h[0]), lo = hexval(h[1]);
                    if (hi < 0 || lo < 0) return *why = "a tag's parameters in the text are not hex", 0;
                    uint8_t x = (uint8_t)(hi << 4 | lo);
                    put(b, &x, 1);
                }
            }
            s = (const unsigned char *)end + 1;
            continue;
        }
        if (!wide) {
            put(b, s++, 1);
            continue;
        }
        // UTF-8 to UTF-16
        unsigned cp, more = *s < 0x80 ? 0 : (*s & 0xe0) == 0xc0 ? 1 : (*s & 0xf0) == 0xe0 ? 2 : (*s & 0xf8) == 0xf0 ? 3 : 9;
        if (more > 3) return *why = "the text is not UTF-8", 0;
        cp = more ? *s & (0x3f >> more) : *s;
        s++;
        for (unsigned i = 0; i < more; i++, s++) {
            if ((*s & 0xc0) != 0x80) return *why = "the text is not UTF-8", 0;
            cp = cp << 6 | (*s & 0x3f);
        }
        if (cp >= 0x10000) {
            cp -= 0x10000;
            put16(b, 0xd800 | cp >> 10), put16(b, 0xdc00 | (cp & 0x3ff));
        } else put16(b, cp);
    }
    unit(b, 0, wide);
    if (b->bad) return *why = "out of memory", 0;
    return 1;
}

// The length of msg[0..n)'s leading control tags.
static size_t leading_tags(const uint8_t *msg, size_t n, int wide) {
    size_t w = wide ? 2 : 1, o = 0;
    while (n - o >= w + 6 && (wide ? rd16(msg + o) : msg[o]) == 0x0e) {
        size_t size = rd16(msg + o + w + 4);
        if (size > n - o - w - 6) break;
        o += w + 6 + size;
    }
    return o;
}

MsbtResult msbt_set_message(const uint8_t *p, size_t n, const char *label, uint64_t want, const char *text,
                            uint8_t **out, size_t *out_n, uint64_t *found, const char **why) {
    Sec sec[MAX_SECTIONS];
    int count, wide;
    *out = NULL, *found = 0;
    if (!sections(p, n, sec, &count, &wide, why)) return MSBT_FAILED;
    const Sec *lbl = section(sec, count, "LBL1"), *txt = section(sec, count, "TXT2");
    if (!lbl || !txt) return *why = "an MSBT without labels or texts", MSBT_FAILED;
    long idx = label_index(p, lbl, label, why);
    if (idx < 0) return MSBT_FAILED;
    const uint8_t *t = p + txt->at;
    uint32_t msgs = txt->size >= 4 ? rd32(t) : 0;
    if (txt->size < 4 || msgs > (txt->size - 4) / 4) return *why = "an MSBT text table cut short", MSBT_FAILED;
    if ((uint32_t)idx >= msgs) return *why = "a label of a message the file does not have", MSBT_FAILED;
    size_t first = 4 + 4 * (size_t)msgs, prev = first;
    for (uint32_t i = 0; i < msgs; i++) {   // each message runs up to the next one
        size_t o = rd32(t + 4 + 4 * i);
        if (o < prev || o > txt->size) return *why = "an MSBT whose messages are not in order", MSBT_FAILED;
        prev = o;
    }
    size_t at = rd32(t + 4 + 4 * (size_t)idx);
    size_t end = (uint32_t)idx + 1 < msgs ? rd32(t + 8 + 4 * (size_t)idx) : txt->size;
    *found = msbt_hash(t + at, end - at);
    Buf body = {0};
    if (!encode(&body, text, wide, why)) {
        free(body.p);
        return MSBT_FAILED;
    }
    if (*found != want) {   // ours already: control tags only, then the text
        size_t keep = end - at - body.n;
        int same = end - at >= body.n && !memcmp(t + end - body.n, body.p, body.n) &&
                   leading_tags(t + at, keep, wide) == keep;
        free(body.p);
        return same ? MSBT_ALREADY : MSBT_OTHER;
    }
    Buf msg = {0};
    put(&msg, t + at, leading_tags(t + at, end - at, wide));
    put(&msg, body.p, body.n);
    free(body.p);
    // The file again, the texts' section rebuilt with the new message.
    Buf f = {0};
    put(&f, p, 0x20);
    for (int i = 0; i < count; i++) {
        size_t head = f.n;
        put(&f, p + sec[i].at - 16, 16);
        if (&sec[i] != txt) put(&f, p + sec[i].at, sec[i].size);
        else {
            uint8_t x[4];
            wr32(x, msgs);
            put(&f, x, 4);
            for (uint32_t j = 0; j < msgs; j++) {
                size_t o = rd32(t + 4 + 4 * j);
                wr32(x, (uint32_t)(o + (j > (uint32_t)idx ? msg.n - (end - at) : 0)));
                put(&f, x, 4);
            }
            put(&f, t + first, at - first);
            put(&f, msg.p, msg.n);
            put(&f, t + end, txt->size - end);
        }
        if (!f.bad) wr32(f.p + head + 4, (uint32_t)(f.n - head - 16));
        static const uint8_t ab[16] = {0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab,
                                       0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab, 0xab};
        put(&f, ab, pad16(f.n) - f.n);
    }
    int bad = f.bad || msg.bad;
    free(msg.p);
    if (bad) {
        free(f.p);
        return *why = "out of memory", MSBT_FAILED;
    }
    wr32(f.p + 0x12, (uint32_t)f.n);
    *out = f.p, *out_n = f.n;
    return MSBT_SET;
}
