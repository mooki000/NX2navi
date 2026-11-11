// AINB logic graphs (ainb.h), version 0x408. The layout, as the game's 5036 graphs have it:
//   header (0x74): "AIB ", the version, the file name, the counts of commands, nodes, query nodes and attachments, and
//     the offsets of the sections below (0x50 repeats the jump table's, 0x54 and 0x58 are 0);
//   commands (0x18 each), nodes (0x3c each: u16 type (0 application-defined), u16 index, u16 attachment count, u8 flags
//     (bit 0 a query, bit 1 a module call), the type name, its 32-bit murmur3 hash, the node's parameter block, its EXB
//     function count, its multi-input count, its first attachment index, u16 first entry and count in the query array,
//     a GUID);
//   the blackboard (a header of seven {u16 count, u16 first entry, u16 offset of the values, u16} for string, int, u32,
//     float, bool, vec3 and pointer, the entries {u32 name and flags, u32 note}, the values, the file references);
//   each node's parameter block, in node order: 6 {u32 first, u32 count} into the property arrays, 6 {u32 first input,
//     count, first output, count} into the input and output arrays (int, bool, float, string, vec3, pointer), 10 {u8
//     count, u8 first} plug kinds (2: children, 3: jumps), the offsets of the plugs, then the plugs (u32 node, u32 name,
//     and more for some node types);
//   the attachment index array and the attachments (0x10 each, in either order), each attachment's parameters (0x64:
//     six offsets at its end all point where they end);
//   the property table (6 offsets, then the arrays: name, flags, value), the input/output table (12 offsets, then
//     inputs {name, s16 source node or -100 - first multi-input, s16 its output or the multi-input count, flags, value}
//     and outputs {name, bit 31: used}; pointer entries carry a class name too);
//   multi-inputs (8 each), jumps, the query array (u32 each: a query node's rank among the query nodes), the expression
//     binary, module calls, external actions, file hashes, the replacement table, a 4-byte section, the enum relocation
//     array (empty in this game) and the string pool, which every name and string value is an offset into.
// Only the header, the nodes' and attachments' parameter offsets, the plug offsets, the attachment parameters' end
// offsets and the two tables' array offsets hold positions in the file; the other sections are moved as they are.
// An edit changes values in place and appends: nodes, their parameter blocks, entries of the typed arrays, query lists,
// strings. The writer lays the sections out again in the order read.
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ainb.h"

#define HEADER 0x74
#define CMD 0x18
#define ELEM 0x3c
#define BLOCK 0xa4             // a parameter block before its plug offsets
#define ATT 0x10
#define ATT_ENDS 0x34          // where an attachment's parameters hold their six end offsets
#define LINKED_FLAGS 0         // an input connected to an output
#define OWN_VALUE 0x00800000u  // an input holding a value of its own
#define DEFAULT_VALUE 0x01000000u  // ... equal to the node type's default
#define BOUND 0x80000000u      // a parameter bound to a blackboard entry (index in the low 16 bits)
#define USED 0x80000000u       // an output something reads

enum { T_INT, T_BOOL, T_FLOAT, T_STRING, T_VEC3, T_PTR, NTYPES };
static const unsigned PROP_SIZE[NTYPES] = {12, 12, 12, 12, 20, 12};
static const unsigned IN_SIZE[NTYPES] = {16, 16, 16, 16, 24, 20};
static const unsigned OUT_SIZE[NTYPES] = {4, 4, 4, 4, 4, 8};
static const char *const TYPE_NAME[NTYPES] = {"int", "bool", "float", "string", "vec3", "pointer"};
static const int BB_TYPE[NTYPES] = {1, 4, 3, 0, 5, 6};   // the blackboard header's slot of each value type

enum {
    S_HEADER, S_CMDS, S_ELEMS, S_BB, S_PARAM, S_NEWPARAM, S_ATT, S_ATTID, S_ATTPARAM, S_PROP, S_IO, S_MULTI, S_JUMP,
    S_QUERY, S_EXB, S_MOD, S_ACTION, S_LINK, S_REPL, S_X6C, S_ENUM, S_STR,
};

typedef struct { uint8_t kind; uint32_t index; size_t at, size; } Sec;
typedef struct { uint8_t *p; size_t n, cap; } Buf;

struct Ainb {
    uint8_t *b;                // the file as read, values edited in place
    size_t n;
    uint32_t ncmd, nel, nq, natt;
    int nsec;
    Sec *sec;
    size_t prop_at[NTYPES], in_at[NTYPES], out_at[NTYPES], pool_at, pool_n, query_at;
    uint32_t prop_n[NTYPES], in_n[NTYPES], out_n[NTYPES], multi_n, query_n;
    int *att_sec;              // the section of each attachment's parameters
    // appended by edits
    Buf elems, params, prop[NTYPES], in[NTYPES], out[NTYPES], multi, query, str;
    uint32_t add_nel, add_nq;
    size_t *param_off;         // where each added node's block is in params
    int nnames;
    char *names[64];
    int named[64];
};

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static int16_t rds16(const uint8_t *p) { return (int16_t)rd16(p); }
static void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }

static int buf_put(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 256;
        while (cap < b->n + n) cap *= 2;
        uint8_t *q = realloc(b->p, cap);
        if (!q) return 0;
        b->p = q, b->cap = cap;
    }
    if (p) memcpy(b->p + b->n, p, n);
    else memset(b->p + b->n, 0, n);
    b->n += n;
    return 1;
}

static void say(char *why, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void say(char *why, size_t n, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (n) vsnprintf(why, n, fmt, ap);
    va_end(ap);
}

// ---- reading

static int sec_find(const Ainb *g, int kind, uint32_t index) {
    for (int s = 0; s < g->nsec; s++)
        if (g->sec[s].kind == kind && g->sec[s].index == index) return s;
    return -1;
}

static const Sec *sec_of(const Ainb *g, int kind) {
    int s = sec_find(g, kind, 0);
    return s < 0 ? NULL : &g->sec[s];
}

static size_t elems_at(const Ainb *g) { return HEADER + (size_t)CMD * g->ncmd; }
static uint32_t nodes(const Ainb *g) { return g->nel + g->add_nel; }

static uint8_t *elem(const Ainb *g, uint32_t i) {
    return i < g->nel ? g->b + elems_at(g) + (size_t)ELEM * i : g->elems.p + (size_t)ELEM * (i - g->nel);
}

static uint8_t *block(const Ainb *g, uint32_t i) {
    return i < g->nel ? g->b + rd32(elem(g, i) + 0x14) : g->params.p + g->param_off[i - g->nel];
}

static uint32_t count_of(const Buf *b, unsigned size, uint32_t n) { return n + (uint32_t)(b->n / size); }

static uint8_t *prop_entry(const Ainb *g, int k, uint32_t j) {
    return j < g->prop_n[k] ? g->b + g->prop_at[k] + (size_t)j * PROP_SIZE[k]
                            : g->prop[k].p + (size_t)(j - g->prop_n[k]) * PROP_SIZE[k];
}
static uint8_t *in_entry(const Ainb *g, int k, uint32_t j) {
    return j < g->in_n[k] ? g->b + g->in_at[k] + (size_t)j * IN_SIZE[k]
                          : g->in[k].p + (size_t)(j - g->in_n[k]) * IN_SIZE[k];
}
static uint8_t *out_entry(const Ainb *g, int k, uint32_t j) {
    return j < g->out_n[k] ? g->b + g->out_at[k] + (size_t)j * OUT_SIZE[k]
                           : g->out[k].p + (size_t)(j - g->out_n[k]) * OUT_SIZE[k];
}
static uint8_t *multi_entry(const Ainb *g, uint32_t j) {
    return j < g->multi_n ? g->b + sec_of(g, S_MULTI)->at + 8 * (size_t)j : g->multi.p + 8 * (size_t)(j - g->multi_n);
}
static uint8_t *query_entry(const Ainb *g, uint32_t j) {
    return j < g->query_n ? g->b + g->query_at + 4 * (size_t)j : g->query.p + 4 * (size_t)(j - g->query_n);
}

// The string at offset o of the pool (the pool as read, then the strings edits added); NULL past its end.
static const char *str_at(const Ainb *g, uint32_t o) {
    if (o < g->pool_n) {
        const uint8_t *s = g->b + g->pool_at + o;
        return memchr(s, 0, g->pool_n - o) ? (const char *)s : NULL;
    }
    o -= (uint32_t)g->pool_n;
    return o < g->str.n ? (const char *)g->str.p + o : NULL;
}

// The pool offset of s: a string the pool has (whole, not the tail of another), else one added at its end.
static int intern(Ainb *g, const char *s, uint32_t *out) {
    size_t len = strlen(s);
    const uint8_t *p = g->b + g->pool_at;
    for (size_t o = 0; o < g->pool_n;) {
        const uint8_t *z = memchr(p + o, 0, g->pool_n - o);
        if (!z) break;
        size_t l = (size_t)(z - (p + o));
        if (l == len && !memcmp(p + o, s, len)) return *out = (uint32_t)o, 1;
        o += l + 1;
    }
    for (size_t o = 0; o < g->str.n;) {
        size_t l = strlen((const char *)g->str.p + o);
        if (l == len && !memcmp(g->str.p + o, s, len)) return *out = (uint32_t)(g->pool_n + o), 1;
        o += l + 1;
    }
    *out = (uint32_t)(g->pool_n + g->str.n);
    return buf_put(&g->str, s, len + 1);
}

static int add_sec(Ainb *g, int cap, int kind, uint32_t index, size_t at) {
    if (g->nsec >= cap) return 0;
    g->sec[g->nsec++] = (Sec){(uint8_t)kind, index, at, 0};
    return 1;
}

Ainb *ainb_read(const uint8_t *p, size_t n, const char **why) {
    *why = "not an AINB graph";
    if (n < HEADER || memcmp(p, "AIB ", 4)) return NULL;
    if (rd32(p + 4) != 0x408) return *why = "an AINB graph of a version other than 0x408", NULL;
    Ainb *g = calloc(1, sizeof *g);
    if (!g || !(g->b = malloc(n))) {
        free(g);
        return *why = "out of memory", NULL;
    }
    memcpy(g->b, p, n);
    g->n = n;
    const uint8_t *b = g->b;
    g->ncmd = rd32(b + 0xc), g->nel = rd32(b + 0x10), g->nq = rd32(b + 0x14), g->natt = rd32(b + 0x18);
    *why = "a damaged AINB graph (counts past its end)";
    if (g->ncmd > n / CMD || g->nel > n / ELEM || g->natt > n / ATT || elems_at(g) + (size_t)ELEM * g->nel > n) goto bad;
    *why = "a damaged AINB graph (a section offset past its end)";
    static const int at_fields[] = {0x20, 0x24, 0x28, 0x2c, 0x30, 0x34, 0x38, 0x3c, 0x40, 0x48, 0x4c, 0x5c, 0x68, 0x6c,
                                    0x70};
    for (size_t i = 0; i < sizeof at_fields / sizeof *at_fields; i++)
        if (rd32(b + at_fields[i]) < HEADER || rd32(b + at_fields[i]) > n) goto bad;
    if (rd32(b + 0x44) && (rd32(b + 0x44) < HEADER || rd32(b + 0x44) > n)) goto bad;
    *why = "an AINB graph whose header is not laid out as this reader knows";
    if (rd32(b + 0x50) != rd32(b + 0x30) || rd32(b + 0x54) || rd32(b + 0x58)) goto bad;
    if (rd32(b + 0x28) + 4 > n || rd32(b + rd32(b + 0x28))) {
        *why = "an AINB graph with enum relocations, which this reader does not move";
        goto bad;
    }
    // the sections in file order
    int cap = 24 + (int)g->nel + (int)g->natt;
    g->sec = calloc((size_t)cap, sizeof *g->sec);
    g->att_sec = calloc(g->natt ? g->natt : 1, sizeof *g->att_sec);
    if (!g->sec || !g->att_sec) {
        *why = "out of memory";
        goto bad;
    }
    add_sec(g, cap, S_HEADER, 0, 0);
    add_sec(g, cap, S_CMDS, 0, HEADER);
    add_sec(g, cap, S_ELEMS, 0, elems_at(g));
    add_sec(g, cap, S_BB, 0, rd32(b + 0x20));
    for (uint32_t i = 0; i < g->nel; i++) add_sec(g, cap, S_PARAM, i, rd32(elem(g, i) + 0x14));
    size_t att = rd32(b + 0x3c), attid = rd32(b + 0x40);
    int attid_first = attid < att || (attid == att && g->natt);
    add_sec(g, cap, S_NEWPARAM, 0, attid_first ? attid : att);   // where the blocks of added nodes go
    if (attid_first) add_sec(g, cap, S_ATTID, 0, attid);
    add_sec(g, cap, S_ATT, 0, att);
    if (!attid_first) add_sec(g, cap, S_ATTID, 0, attid);
    *why = "a damaged AINB graph (an attachment past its end)";
    if (att + (size_t)ATT * g->natt > n) goto bad;
    // attachment parameters, in the order they lie
    int first_att = g->nsec;
    for (uint32_t j = 0; j < g->natt; j++) {
        size_t at = rd32(b + att + ATT * j + 4);
        int s = g->nsec;
        while (s > first_att && g->sec[s - 1].at > at) s--;
        memmove(&g->sec[s + 1], &g->sec[s], sizeof *g->sec * (size_t)(g->nsec - s));
        g->sec[s] = (Sec){S_ATTPARAM, j, at, 0};
        g->nsec++;
    }
    static const struct { int kind, field; } tail[] = {
        {S_PROP, 0x2c}, {S_IO, 0x34}, {S_MULTI, 0x38}, {S_JUMP, 0x30}, {S_QUERY, 0x4c}, {S_EXB, 0x44}, {S_MOD, 0x5c},
        {S_ACTION, 0x68}, {S_LINK, 0x70}, {S_REPL, 0x48}, {S_X6C, 0x6c}, {S_ENUM, 0x28}, {S_STR, 0x24},
    };
    for (size_t i = 0; i < sizeof tail / sizeof *tail; i++)
        if (tail[i].kind != S_EXB || rd32(b + 0x44)) add_sec(g, cap, tail[i].kind, 0, rd32(b + tail[i].field));
    *why = "an AINB graph whose sections are not in the order this reader knows";
    for (int s = 0; s < g->nsec; s++) {
        size_t end = s + 1 < g->nsec ? g->sec[s + 1].at : n;
        if (end < g->sec[s].at) goto bad;
        g->sec[s].size = end - g->sec[s].at;
    }
    for (int s = 0; s < g->nsec; s++)
        if (g->sec[s].kind == S_ATTPARAM) g->att_sec[g->sec[s].index] = s;
    if (sec_of(g, S_ELEMS)->size != (size_t)ELEM * g->nel || sec_of(g, S_ATT)->size != (size_t)ATT * g->natt ||
        sec_of(g, S_NEWPARAM)->size)
        goto bad;
    for (uint32_t j = 0; j < g->natt; j++) {   // the six offsets at the end of an attachment's parameters
        const Sec *a = &g->sec[g->att_sec[j]];
        if (a->size < ATT_ENDS + 48) goto bad;
        for (int k = 0; k < 6; k++)
            if (rd32(b + a->at + ATT_ENDS + 8 * k + 4) != a->at + a->size) goto bad;
    }
    // the typed arrays
    *why = "a damaged AINB graph (its property or input/output table)";
    const Sec *pr = sec_of(g, S_PROP), *io = sec_of(g, S_IO);
    if (pr->size < 0x18 || io->size < 0x30) goto bad;
    size_t at = pr->at + 0x18;
    for (int k = 0; k < NTYPES; k++) {
        size_t s0 = rd32(b + pr->at + 4 * k), s1 = k + 1 < NTYPES ? rd32(b + pr->at + 4 * k + 4) : pr->at + pr->size;
        if (s0 != at || s1 < s0 || (s1 - s0) % PROP_SIZE[k]) goto bad;
        g->prop_at[k] = s0, g->prop_n[k] = (uint32_t)((s1 - s0) / PROP_SIZE[k]), at = s1;
    }
    if (at != pr->at + pr->size) goto bad;
    at = io->at + 0x30;
    for (int m = 0; m < 2 * NTYPES; m++) {
        size_t s0 = rd32(b + io->at + 4 * m), s1 = m + 1 < 2 * NTYPES ? rd32(b + io->at + 4 * m + 4) : io->at + io->size;
        unsigned size = m % 2 ? OUT_SIZE[m / 2] : IN_SIZE[m / 2];
        if (s0 != at || s1 < s0 || (s1 - s0) % size) goto bad;
        if (m % 2) g->out_at[m / 2] = s0, g->out_n[m / 2] = (uint32_t)((s1 - s0) / size);
        else g->in_at[m / 2] = s0, g->in_n[m / 2] = (uint32_t)((s1 - s0) / size);
        at = s1;
    }
    if (at != io->at + io->size) goto bad;
    const Sec *mu = sec_of(g, S_MULTI), *qu = sec_of(g, S_QUERY), *st = sec_of(g, S_STR);
    if (mu->size % 8 || qu->size % 4) goto bad;
    g->multi_n = (uint32_t)(mu->size / 8), g->query_at = qu->at, g->query_n = (uint32_t)(qu->size / 4);
    g->pool_at = st->at, g->pool_n = st->size;
    *why = "a damaged AINB graph (a name past its string pool)";
    if (!str_at(g, rd32(b + 8)) || !str_at(g, rd32(b + 0x60))) goto bad;
    for (uint32_t i = 0; i < g->ncmd; i++)
        if (!str_at(g, rd32(b + HEADER + CMD * i))) goto bad;
    for (uint32_t j = 0; j < g->natt; j++)
        if (!str_at(g, rd32(b + att + ATT * j))) goto bad;
    // the nodes and their parameter blocks
    *why = "a damaged AINB graph (a node's parameters)";
    uint32_t nq = 0;
    for (uint32_t i = 0; i < g->nel; i++) {
        const uint8_t *e = elem(g, i);
        if (rd16(e + 2) != i || !str_at(g, rd32(e + 8))) goto bad;
        nq += e[6] & 1;
        const Sec *s = &g->sec[sec_find(g, S_PARAM, i)];
        if (s->size < BLOCK) goto bad;
        const uint8_t *bl = b + s->at;
        unsigned plugs = 0;
        for (int k = 0; k < 10; k++) plugs += bl[0x90 + 2 * k];
        if (s->size < BLOCK + 4 * (size_t)plugs) goto bad;
        for (int k = 0; k < 10; k++)
            if (bl[0x90 + 2 * k] && bl[0x90 + 2 * k + 1] + bl[0x90 + 2 * k] > (int)plugs) goto bad;
        for (unsigned j = 0; j < plugs; j++) {
            size_t q = rd32(bl + BLOCK + 4 * j);
            if (q < s->at + BLOCK + 4 * (size_t)plugs || q + 8 > s->at + s->size) goto bad;
        }
        for (int k = 0; k < NTYPES; k++) {
            uint32_t pb = rd32(bl + 8 * k), pc = rd32(bl + 8 * k + 4);
            uint32_t ib = rd32(bl + 0x30 + 16 * k), ic = rd32(bl + 0x34 + 16 * k);
            uint32_t ob = rd32(bl + 0x38 + 16 * k), oc = rd32(bl + 0x3c + 16 * k);
            if ((pc && (pb > g->prop_n[k] || pc > g->prop_n[k] - pb)) ||
                (ic && (ib > g->in_n[k] || ic > g->in_n[k] - ib)) || (oc && (ob > g->out_n[k] || oc > g->out_n[k] - ob)))
                goto bad;
            for (uint32_t j = 0; j < pc; j++) {   // names and string values within the pool
                const uint8_t *x = prop_entry(g, k, pb + j);
                if (!str_at(g, rd32(x)) || (k == T_STRING && !str_at(g, rd32(x + 8)))) goto bad;
            }
            for (uint32_t j = 0; j < oc; j++)
                if (!str_at(g, rd32(out_entry(g, k, ob + j)) & ~USED)) goto bad;
            for (uint32_t j = 0; j < ic; j++) {
                const uint8_t *x = in_entry(g, k, ib + j) + (k == T_PTR ? 8 : 4);
                if (!str_at(g, rd32(x - (k == T_PTR ? 8 : 4))) || (k == T_STRING && !(rd32(x + 4) & BOUND) &&
                                                                  !str_at(g, rd32(x + 8))))
                    goto bad;
                int src = rds16(x), cnt = rds16(x + 2);
                if (src >= 0 && (uint32_t)src >= g->nel) goto bad;
                if (src <= -100 && (cnt < 0 || (uint32_t)(-100 - src) + (uint32_t)cnt > g->multi_n)) goto bad;
            }
        }
        uint32_t qb = rd16(e + 0x24), qc = rd16(e + 0x26);
        if (qc && qb + qc > g->query_n) goto bad;
    }
    if (nq != g->nq) goto bad;
    return g;
bad:
    ainb_free(g);
    return NULL;
}

void ainb_free(Ainb *g) {
    if (!g) return;
    free(g->b);
    free(g->sec);
    free(g->att_sec);
    free(g->elems.p);
    free(g->params.p);
    for (int k = 0; k < NTYPES; k++) free(g->prop[k].p), free(g->in[k].p), free(g->out[k].p);
    free(g->multi.p);
    free(g->query.p);
    free(g->str.p);
    free(g->param_off);
    for (int i = 0; i < g->nnames; i++) free(g->names[i]);
    free(g);
}

// ---- writing

static size_t new_size(const Ainb *g, const Sec *s) {
    size_t n = s->size;
    switch (s->kind) {
    case S_ELEMS: return n + (size_t)ELEM * g->add_nel;
    case S_NEWPARAM: return g->params.n;
    case S_PROP:
        for (int k = 0; k < NTYPES; k++) n += g->prop[k].n;
        return n;
    case S_IO:
        for (int k = 0; k < NTYPES; k++) n += g->in[k].n + g->out[k].n;
        return n;
    case S_MULTI: return n + g->multi.n;
    case S_QUERY: return n + g->query.n;
    case S_STR: return n + g->str.n;
    }
    return n;
}

uint8_t *ainb_write(const Ainb *g, size_t *out_n) {
    size_t *at = malloc(sizeof *at * (size_t)g->nsec), total = 0;
    if (!at) return NULL;
    for (int s = 0; s < g->nsec; s++) at[s] = total, total += new_size(g, &g->sec[s]);
    uint8_t *o = malloc(total ? total : 1);
    if (!o) {
        free(at);
        return NULL;
    }
    const uint8_t *b = g->b;
    for (int s = 0; s < g->nsec; s++) {
        const Sec *x = &g->sec[s];
        uint8_t *w = o + at[s];
        switch (x->kind) {
        case S_HEADER: {
            memcpy(w, b, HEADER);
            wr32(w + 0x10, g->nel + g->add_nel);
            wr32(w + 0x14, g->nq + g->add_nq);
            static const struct { int field, kind; } f[] = {
                {0x20, S_BB}, {0x24, S_STR}, {0x28, S_ENUM}, {0x2c, S_PROP}, {0x30, S_JUMP}, {0x34, S_IO},
                {0x38, S_MULTI}, {0x3c, S_ATT}, {0x40, S_ATTID}, {0x44, S_EXB}, {0x48, S_REPL}, {0x4c, S_QUERY},
                {0x50, S_JUMP}, {0x5c, S_MOD}, {0x68, S_ACTION}, {0x6c, S_X6C}, {0x70, S_LINK},
            };
            for (size_t i = 0; i < sizeof f / sizeof *f; i++) {
                int t = sec_find(g, f[i].kind, 0);
                wr32(w + f[i].field, t < 0 ? 0 : (uint32_t)at[t]);
            }
            break;
        }
        case S_ELEMS: {
            memcpy(w, b + x->at, x->size);
            if (g->add_nel) memcpy(w + x->size, g->elems.p, (size_t)ELEM * g->add_nel);
            size_t np = at[sec_find(g, S_NEWPARAM, 0)];
            for (uint32_t i = 0; i < g->nel + g->add_nel; i++)
                wr32(w + (size_t)ELEM * i + 0x14,
                     (uint32_t)(i < g->nel ? at[sec_find(g, S_PARAM, i)] : np + g->param_off[i - g->nel]));
            break;
        }
        case S_PARAM: {
            memcpy(w, b + x->at, x->size);
            unsigned plugs = 0;
            for (int k = 0; k < 10; k++) plugs += w[0x90 + 2 * k];
            for (unsigned j = 0; j < plugs; j++)
                wr32(w + BLOCK + 4 * j, (uint32_t)(at[s] + rd32(w + BLOCK + 4 * j) - x->at));
            break;
        }
        case S_NEWPARAM:
            if (g->params.n) memcpy(w, g->params.p, g->params.n);
            break;
        case S_ATT:
            memcpy(w, b + x->at, x->size);
            for (uint32_t j = 0; j < g->natt; j++) wr32(w + ATT * j + 4, (uint32_t)at[g->att_sec[j]]);
            break;
        case S_ATTPARAM:
            memcpy(w, b + x->at, x->size);
            for (int k = 0; k < 6; k++) wr32(w + ATT_ENDS + 8 * k + 4, (uint32_t)(at[s] + x->size));
            break;
        case S_PROP: {
            uint8_t *q = w + 0x18;
            for (int k = 0; k < NTYPES; k++) {
                wr32(w + 4 * k, (uint32_t)(at[s] + (size_t)(q - w)));
                memcpy(q, b + g->prop_at[k], (size_t)g->prop_n[k] * PROP_SIZE[k]);
                q += (size_t)g->prop_n[k] * PROP_SIZE[k];
                if (g->prop[k].n) memcpy(q, g->prop[k].p, g->prop[k].n);
                q += g->prop[k].n;
            }
            break;
        }
        case S_IO: {
            uint8_t *q = w + 0x30;
            for (int k = 0; k < NTYPES; k++) {
                wr32(w + 8 * k, (uint32_t)(at[s] + (size_t)(q - w)));
                memcpy(q, b + g->in_at[k], (size_t)g->in_n[k] * IN_SIZE[k]);
                q += (size_t)g->in_n[k] * IN_SIZE[k];
                if (g->in[k].n) memcpy(q, g->in[k].p, g->in[k].n);
                q += g->in[k].n;
                wr32(w + 8 * k + 4, (uint32_t)(at[s] + (size_t)(q - w)));
                memcpy(q, b + g->out_at[k], (size_t)g->out_n[k] * OUT_SIZE[k]);
                q += (size_t)g->out_n[k] * OUT_SIZE[k];
                if (g->out[k].n) memcpy(q, g->out[k].p, g->out[k].n);
                q += g->out[k].n;
            }
            break;
        }
        case S_MULTI:
        case S_QUERY:
        case S_STR: {
            const Buf *add = x->kind == S_MULTI ? &g->multi : x->kind == S_QUERY ? &g->query : &g->str;
            memcpy(w, b + x->at, x->size);
            if (add->n) memcpy(w + x->size, add->p, add->n);
            break;
        }
        default: memcpy(w, b + x->at, x->size);
        }
    }
    free(at);
    *out_n = total;
    return o;
}

// ---- nodes, names, parameters

int ainb_nodes(const Ainb *g) { return (int)nodes(g); }

const char *ainb_type(const Ainb *g, int i) {
    static const struct { unsigned t; const char *name; } builtin[] = {
        {1, "S32Selector"}, {2, "Sequential"}, {3, "Simultaneous"}, {4, "F32Selector"}, {5, "StringSelector"},
        {6, "RandomSelector"}, {7, "BoolSelector"}, {8, "Fork"}, {9, "Join"}, {10, "Alert"}, {20, "Expression"},
        {100, "ModuleIF_Input_S32"}, {101, "ModuleIF_Input_F32"}, {102, "ModuleIF_Input_Vec3f"},
        {103, "ModuleIF_Input_String"}, {104, "ModuleIF_Input_Bool"}, {105, "ModuleIF_Input_Ptr"},
        {200, "ModuleIF_Output_S32"}, {201, "ModuleIF_Output_F32"}, {202, "ModuleIF_Output_Vec3f"},
        {203, "ModuleIF_Output_String"}, {204, "ModuleIF_Output_Bool"}, {205, "ModuleIF_Output_Ptr"},
        {300, "ModuleIF_Child"}, {400, "StateEnd"}, {500, "SplitTiming"},
    };
    if (i < 0 || (uint32_t)i >= nodes(g)) return "?";
    const uint8_t *e = elem(g, (uint32_t)i);
    unsigned t = rd16(e);
    if (!t) {
        const char *s = str_at(g, rd32(e + 8));
        return s ? s : "?";
    }
    for (size_t k = 0; k < sizeof builtin / sizeof *builtin; k++)
        if (builtin[k].t == t) return builtin[k].name;
    return "?";
}

int ainb_name(Ainb *g, const char *name, int i) {
    for (int k = 0; k < g->nnames; k++)
        if (!strcmp(g->names[k], name)) return g->named[k] = i, 1;
    if (g->nnames >= (int)(sizeof g->names / sizeof *g->names) || !(g->names[g->nnames] = strdup(name))) return 0;
    g->named[g->nnames++] = i;
    return 1;
}

int ainb_named(const Ainb *g, const char *name) {
    for (int k = 0; k < g->nnames; k++)
        if (!strcmp(g->names[k], name)) return g->named[k];
    return -1;
}

typedef struct {
    int input;        // 1: an input, 0: a property
    int k;            // value type
    uint8_t *p;       // the entry
} Param;

static uint8_t *value_of(const Param *x) {
    if (x->k == T_PTR) return x->input ? x->p + 16 : NULL;
    return x->p + (x->input ? 12 : 8);
}
static uint8_t *flags_of(const Param *x) {
    if (x->k == T_PTR) return x->p + (x->input ? 12 : 8);
    return x->p + (x->input ? 8 : 4);
}
// an input's source node and its output (or -100 - first multi-input and their count)
static uint8_t *source_of(const Param *x) { return x->p + (x->k == T_PTR ? 8 : 4); }

// Property or input `name` of node i; 0 when it has none or both.
static int param_of(const Ainb *g, uint32_t i, const char *name, Param *out) {
    const uint8_t *bl = block(g, i);
    int found = 0;
    for (int k = 0; k < NTYPES; k++) {
        uint32_t pb = rd32(bl + 8 * k), pc = rd32(bl + 8 * k + 4);
        for (uint32_t j = 0; j < pc; j++) {
            uint8_t *p = prop_entry(g, k, pb + j);
            const char *s = str_at(g, rd32(p));
            if (s && !strcmp(s, name)) *out = (Param){0, k, p}, found++;
        }
        uint32_t ib = rd32(bl + 0x30 + 16 * k), ic = rd32(bl + 0x34 + 16 * k);
        for (uint32_t j = 0; j < ic; j++) {
            uint8_t *p = in_entry(g, k, ib + j);
            const char *s = str_at(g, rd32(p));
            if (s && !strcmp(s, name)) *out = (Param){1, k, p}, found++;
        }
    }
    return found == 1;
}

// Output `name` of node i among those of type k: its entry and its index among them; NULL when none.
static uint8_t *output_of(const Ainb *g, uint32_t i, int k, const char *name, int *idx) {
    const uint8_t *bl = block(g, i);
    uint32_t ob = rd32(bl + 0x38 + 16 * k), oc = rd32(bl + 0x3c + 16 * k);
    for (uint32_t j = 0; j < oc; j++) {
        uint8_t *p = out_entry(g, k, ob + j);
        const char *s = str_at(g, rd32(p) & ~USED);
        if (s && !strcmp(s, name)) return *idx = (int)j, p;
    }
    return NULL;
}

static const char *output_name(const Ainb *g, uint32_t i, int k, int idx) {
    const uint8_t *bl = block(g, i);
    uint32_t ob = rd32(bl + 0x38 + 16 * k), oc = rd32(bl + 0x3c + 16 * k);
    if (idx < 0 || (uint32_t)idx >= oc) return "?";
    const char *s = str_at(g, rd32(out_entry(g, k, ob + (uint32_t)idx)) & ~USED);
    return s ? s : "?";
}

// The blackboard entry a parameter is bound to (flags), "?" when it cannot be told.
static const char *bb_name(const Ainb *g, int k, uint32_t flags) {
    const Sec *bb = sec_of(g, S_BB);
    if ((flags & 0x7fff0000u) || bb->size < 0x38) return "?";
    const uint8_t *h = g->b + bb->at + 8 * BB_TYPE[k];
    uint32_t idx = (flags & 0xffff) + rd16(h + 2), total = 0;
    for (int t = 0; t < 7; t++) total += rd16(g->b + bb->at + 8 * t);
    if ((flags & 0xffff) >= rd16(h) || idx >= total || 0x38 + 8 * (size_t)(idx + 1) > bb->size) return "?";
    const char *s = str_at(g, rd32(g->b + bb->at + 0x38 + 8 * idx) & 0x3fffff);
    return s ? s : "?";
}

static int same_float(double a, double b) { return fabs(a - b) <= 1e-5 * (fabs(b) > 1 ? fabs(b) : 1); }

// text as a value of type k: 1 when it reads as one (v gets the bytes; a string's text in *s).
static int parse_value(int k, const char *text, uint8_t v[12], const char **s) {
    char *end;
    switch (k) {
    case T_INT: {
        long x = strtol(text, &end, 0);
        if (end == text || *end || x < INT32_MIN || x > INT32_MAX) return 0;
        wr32(v, (uint32_t)(int32_t)x);
        return 1;
    }
    case T_BOOL:
        if (!strcmp(text, "true") || !strcmp(text, "1")) return wr32(v, 1), 1;
        if (!strcmp(text, "false") || !strcmp(text, "0")) return wr32(v, 0), 1;
        return 0;
    case T_FLOAT: {
        float f = strtof(text, &end);
        if (end == text || *end) return 0;
        memcpy(v, &f, 4);
        return 1;
    }
    case T_STRING: *s = text; return 1;
    case T_VEC3:
        for (int c = 0; c < 3; c++) {
            float f = strtof(text, &end);
            if (end == text || (c < 2 ? *end != ',' : *end != 0)) return 0;
            memcpy(v + 4 * c, &f, 4);
            text = end + 1;
        }
        return 1;
    }
    return 0;
}

// Whether the value at v (of type k) is text's value.
static int value_is(const Ainb *g, int k, const uint8_t *v, const char *text) {
    uint8_t w[12];
    const char *s = NULL;
    if (!parse_value(k, text, w, &s)) return 0;
    switch (k) {
    case T_INT: return rd32(v) == rd32(w);
    case T_BOOL: return !rd32(v) == !rd32(w);
    case T_FLOAT:
    case T_VEC3:
        for (int c = 0; c < (k == T_VEC3 ? 3 : 1); c++) {
            float a, b;
            memcpy(&a, v + 4 * c, 4), memcpy(&b, w + 4 * c, 4);
            if (!same_float(a, b)) return 0;
        }
        return 1;
    case T_STRING: {
        const char *t = str_at(g, rd32(v));
        return t && !strcmp(t, s);
    }
    }
    return 0;
}

static const char *show_value(const Ainb *g, int k, const uint8_t *v, char *out, size_t n) {
    float f[3];
    switch (k) {
    case T_INT: snprintf(out, n, "%d", (int32_t)rd32(v)); break;
    case T_BOOL: snprintf(out, n, "%s", rd32(v) ? "true" : "false"); break;
    case T_FLOAT: memcpy(f, v, 4), snprintf(out, n, "%g", f[0]); break;
    case T_STRING: {
        const char *s = str_at(g, rd32(v));
        snprintf(out, n, "'%s'", s ? s : "?");
        break;
    }
    case T_VEC3: memcpy(f, v, 12), snprintf(out, n, "%g,%g,%g", f[0], f[1], f[2]); break;
    default: snprintf(out, n, "a pointer");
    }
    return out;
}

// What a parameter holds, as text: "@name.Out" / "#3.Out", "bb:Name", "several outputs", "=value".
static const char *show_param(const Ainb *g, const Param *x, char *out, size_t n) {
    uint32_t fl = rd32(flags_of(x));
    if (x->input) {
        int src = rds16(source_of(x)), idx = rds16(source_of(x) + 2);
        if (src >= 0) {
            for (int k = 0; k < g->nnames; k++)
                if (g->named[k] == src) {
                    snprintf(out, n, "@%s.%s", g->names[k], output_name(g, (uint32_t)src, x->k, idx));
                    return out;
                }
            snprintf(out, n, "#%d.%s", src, output_name(g, (uint32_t)src, x->k, idx));
            return out;
        }
        if (src <= -100) {   // "#2.Bool,#5.Bool"
            size_t o = 0;
            out[0] = 0;
            for (int q = 0; q < idx && o + 1 < n; q++) {
                const uint8_t *m = multi_entry(g, (uint32_t)(-100 - src + q));
                int s = rds16(m);
                o += (size_t)snprintf(out + o, n - o, "%s#%d.%s", q ? "," : "", s,
                                      s >= 0 && (uint32_t)s < nodes(g) ? output_name(g, (uint32_t)s, x->k, rds16(m + 2)) : "?");
                if (o >= n) o = n - 1;
            }
            return out;
        }
    }
    if (fl & BOUND) return snprintf(out, n, "bb:%s", bb_name(g, x->k, fl)), out;
    if (!value_of(x)) return snprintf(out, n, "a pointer"), out;
    char v[96];
    snprintf(out, n, "=%s", show_value(g, x->k, value_of(x), v, sizeof v));
    return out;
}

// "@name.Out" or "#3.Out": the node and the output's name (into out); -1 when it names no node.
static int parse_ref(const Ainb *g, const char *ref, char *outname, size_t n) {
    const char *dot = strchr(ref, '.');
    if (!dot || (size_t)(dot - ref) >= 64 || strlen(dot + 1) >= n) return -1;
    char head[64];
    memcpy(head, ref, (size_t)(dot - ref));
    head[dot - ref] = 0;
    strcpy(outname, dot + 1);
    if (head[0] == '@') return ainb_named(g, head + 1);
    if (head[0] == '#') {
        char *end;
        long i = strtol(head + 1, &end, 10);
        return *end || end == head + 1 || i < 0 || i >= (long)nodes(g) ? -1 : (int)i;
    }
    return -1;
}

// Whether parameter x holds what `want` says ("@name.Out", "#3.Out", "bb:Name", "=value").
static int holds(const Ainb *g, const Param *x, const char *want) {
    uint32_t fl = rd32(flags_of(x));
    int src = x->input ? rds16(source_of(x)) : -1, idx = x->input ? rds16(source_of(x) + 2) : 0;
    if (want[0] == '@' || want[0] == '#') {
        char out[64];
        int i = parse_ref(g, want, out, sizeof out);
        return i >= 0 && src == i && !strcmp(output_name(g, (uint32_t)i, x->k, idx), out);
    }
    if (!strncmp(want, "bb:", 3)) return src < 0 && src > -100 && (fl & BOUND) && !strcmp(bb_name(g, x->k, fl), want + 3);
    if (want[0] == '=') return src < 0 && src > -100 && !(fl & BOUND) && value_of(x) && value_is(g, x->k, value_of(x), want + 1);
    return 0;
}

static int type_is(const Ainb *g, uint32_t i, const char *type) { return !strcmp(ainb_type(g, (int)i), type); }

// Whether node i meets every item of match (find's grammar).
static int matches(const Ainb *g, uint32_t i, const char *match) {
    char item[256];
    while (*match) {
        size_t len = strcspn(match, ";");
        if (len >= sizeof item) return 0;
        memcpy(item, match, len);
        item[len] = 0;
        match += len + (match[len] == ';');
        if (!item[0]) continue;
        if (item[0] == '#') {
            char *end;
            long k = strtol(item + 1, &end, 10);
            if (*end || k != (long)i) return 0;
            continue;
        }
        char *arrow = strstr(item, "<-"), *eq = strchr(item, '=');
        Param x;
        if (arrow) {
            *arrow = 0;
            if (!param_of(g, i, item, &x) || !holds(g, &x, arrow + 2)) return 0;
        } else if (eq) {
            *eq = 0;
            char want[256];
            snprintf(want, sizeof want, "=%s", eq + 1);
            if (!param_of(g, i, item, &x) || !holds(g, &x, want)) return 0;
        } else
            return 0;
    }
    return 1;
}

int ainb_find(const Ainb *g, const char *type, const char *match, char *why, size_t why_n) {
    int found = -1, count = 0, typed = 0;
    for (uint32_t i = 0; i < nodes(g); i++) {
        if (!type_is(g, i, type)) continue;
        typed++;
        if (matches(g, i, match ? match : "")) found = (int)i, count++;
    }
    if (!typed) return say(why, why_n, "the graph has no %s node", type), -1;
    if (!count) return say(why, why_n, "no %s node (of %d) has %s", type, typed, match), -1;
    if (count > 1) return say(why, why_n, "%d %s nodes have %s", count, type, match), -1;
    say(why, why_n, "node #%d (%s)", found, type);
    return found;
}

int ainb_set(Ainb *g, int i, const char *param, const char *was, const char *now, char *why, size_t why_n) {
    Param x;
    char have[128];
    if (i < 0 || (uint32_t)i >= nodes(g) || !param_of(g, (uint32_t)i, param, &x))
        return say(why, why_n, "node #%d (%s) has no parameter %s", i, ainb_type(g, i), param), 0;
    show_param(g, &x, have, sizeof have);
    if (have[0] != '=')
        return say(why, why_n, "%s of node #%d (%s) holds %s, not a value of its own", param, i, ainb_type(g, i), have), 0;
    uint8_t v[12];
    const char *s = NULL;
    if (!parse_value(x.k, now, v, &s))
        return say(why, why_n, "%s of node #%d holds values of type %s; \"%s\" is not one", param, i, TYPE_NAME[x.k],
                   now), 0;
    if (value_is(g, x.k, value_of(&x), now) && !value_is(g, x.k, value_of(&x), was))
        return say(why, why_n, "%s of node #%d (%s) is %s already", param, i, ainb_type(g, i), now), 2;
    if (!value_is(g, x.k, value_of(&x), was))
        return say(why, why_n, "%s of node #%d (%s) holds %s, not %s", param, i, ainb_type(g, i), have + 1, was), 0;
    if (x.k == T_STRING) {
        uint32_t o;
        if (!intern(g, s, &o)) return say(why, why_n, "out of memory"), 0;
        wr32(v, o);
    }
    memcpy(value_of(&x), v, x.k == T_VEC3 ? 12 : 4);
    wr32(flags_of(&x), rd32(flags_of(&x)) & ~DEFAULT_VALUE);
    return say(why, why_n, "%s of node #%d (%s): %s -> %s%s%s", param, i, ainb_type(g, i), have + 1,
               x.k == T_STRING ? "'" : "", now, x.k == T_STRING ? "'" : ""), 1;
}

// The rank of node i among the query nodes; -1 when it is not one.
static int query_rank(const Ainb *g, uint32_t i) {
    if (!(elem(g, i)[6] & 1)) return -1;
    int r = 0;
    for (uint32_t j = 0; j < i; j++) r += elem(g, j)[6] & 1;
    return r;
}

// Node i's query list made the query nodes its inputs read, the ones it keeps first in their order.
static int requery(Ainb *g, uint32_t i) {
    uint8_t *e = elem(g, i), *bl = block(g, i);
    int want[256], nw = 0;
    for (int k = 0; k < NTYPES; k++) {
        uint32_t ib = rd32(bl + 0x30 + 16 * k), ic = rd32(bl + 0x34 + 16 * k);
        for (uint32_t j = 0; j < ic; j++) {
            const uint8_t *x = in_entry(g, k, ib + j) + (k == T_PTR ? 8 : 4);
            int src = rds16(x), cnt = rds16(x + 2), srcs[64], ns = 0;
            if (src >= 0) srcs[ns++] = src;
            else if (src <= -100)
                for (int q = 0; q < cnt && ns < 64; q++) srcs[ns++] = rds16(multi_entry(g, (uint32_t)(-100 - src + q)));
            for (int q = 0; q < ns; q++) {
                int r = srcs[q] >= 0 && (uint32_t)srcs[q] < nodes(g) ? query_rank(g, (uint32_t)srcs[q]) : -1;
                int dup = 0;
                for (int w = 0; w < nw; w++) dup |= want[w] == r;
                if (r >= 0 && !dup && nw < 256) want[nw++] = r;
            }
        }
    }
    uint32_t qb = rd16(e + 0x24), qc = rd16(e + 0x26);
    int list[256], nl = 0;
    for (uint32_t j = 0; j < qc && nl < 256; j++) {   // the ones kept, in their order
        int r = rd16(query_entry(g, qb + j));
        for (int w = 0; w < nw; w++)
            if (want[w] == r) list[nl++] = r;
    }
    for (int w = 0; w < nw; w++) {
        int have = 0;
        for (int l = 0; l < nl; l++) have |= list[l] == want[w];
        if (!have) list[nl++] = want[w];
    }
    int same = (uint32_t)nl == qc;
    for (int l = 0; same && l < nl; l++) same = rd16(query_entry(g, qb + (uint32_t)l)) == list[l];
    if (same) return 1;
    if ((uint32_t)nl != qc) {   // a new list at the end of the array; the old entries stay, read by no one
        qb = g->query_n + (uint32_t)(g->query.n / 4);
        if (!nl) qb = 0;
        else if (!buf_put(&g->query, NULL, 4 * (size_t)nl)) return 0;
        wr16(e + 0x24, (uint16_t)qb);
        wr16(e + 0x26, (uint16_t)nl);
    }
    for (int l = 0; l < nl; l++) {
        uint8_t *q = query_entry(g, qb + (uint32_t)l);
        wr16(q, (uint16_t)list[l]);
        wr16(q + 2, 0);
    }
    return 1;
}

// LINK with was "+...": output idx (`out`) of node `from` added to the outputs input x of node `to` reads (those in
// was, as show_param writes them): a single connection becomes a multi-input, a multi-input gets one more, written
// as a new list at the end of the multi-input array.
static int add_source(Ainb *g, Param *x, int from, int idx, const char *out, int to, const char *in, const char *was,
                      const char *have, char *why, size_t why_n) {
    uint8_t *src = source_of(x), *e = elem(g, (uint32_t)to);
    int old = rds16(src), cnt = rds16(src + 2), list[64][2], nl = 0;
    if (old >= 0) list[nl][0] = old, list[nl][1] = cnt, nl++;
    else if (old <= -100)
        for (int q = 0; q < cnt && nl < 63; q++, nl++) {
            const uint8_t *m = multi_entry(g, (uint32_t)(-100 - old + q));
            list[nl][0] = rds16(m), list[nl][1] = rds16(m + 2);
        }
    for (int q = 0; q < nl; q++)
        if (list[q][0] == from && list[q][1] == idx)
            return say(why, why_n, "%s of node #%d (%s) reads #%d.%s already", in, to, ainb_type(g, to), from, out), 2;
    if (!nl || (old >= 0 ? !holds(g, x, was) : strcmp(have, was)))
        return say(why, why_n, "%s of node #%d (%s) reads %s, not %s", in, to, ainb_type(g, to), have, was), 0;
    uint32_t base = g->multi_n + (uint32_t)(g->multi.n / 8);
    if (base > 0x7f00) return say(why, why_n, "the graph has too many multi-inputs"), 0;
    list[nl][0] = from, list[nl][1] = idx, nl++;
    for (int q = 0; q < nl; q++) {
        uint8_t m[8] = {0};
        wr16(m, (uint16_t)list[q][0]);
        wr16(m + 2, (uint16_t)list[q][1]);
        if (!buf_put(&g->multi, m, 8)) return say(why, why_n, "out of memory"), 0;
    }
    // the node's count of multi-input sources: the old list's go, the new one's come
    wr16(e + 0x1c, (uint16_t)(rd16(e + 0x1c) - (old <= -100 ? cnt : 0) + nl));
    wr16(src, (uint16_t)(-100 - (int)base));
    wr16(src + 2, (uint16_t)nl);
    wr32(flags_of(x), LINKED_FLAGS);
    int oi;
    uint8_t *o = output_of(g, (uint32_t)from, x->k, out, &oi);
    wr32(o, rd32(o) | USED);
    if (!requery(g, (uint32_t)to)) return say(why, why_n, "out of memory"), 0;
    return say(why, why_n, "%s of node #%d (%s): %s -> %s,#%d.%s", in, to, ainb_type(g, to), have, have, from, out), 1;
}

int ainb_link(Ainb *g, int from, const char *out, int to, const char *in, const char *was, char *why, size_t why_n) {
    Param x;
    char have[128];
    if (from < 0 || (uint32_t)from >= nodes(g) || to < 0 || (uint32_t)to >= nodes(g))
        return say(why, why_n, "no such node"), 0;
    if (!param_of(g, (uint32_t)to, in, &x) || !x.input)
        return say(why, why_n, "node #%d (%s) has no input %s", to, ainb_type(g, to), in), 0;
    int idx;
    uint8_t *o = output_of(g, (uint32_t)from, x.k, out, &idx);
    if (!o)
        return say(why, why_n, "node #%d (%s) has no %s output %s", from, ainb_type(g, from), TYPE_NAME[x.k], out), 0;
    uint8_t *src = source_of(&x);
    int old = rds16(src);
    show_param(g, &x, have, sizeof have);
    if (was[0] == '+') return add_source(g, &x, from, idx, out, to, in, was + 1, have, why, why_n);
    if (old == from && rds16(src + 2) == idx)
        return say(why, why_n, "%s of node #%d (%s) is connected to #%d.%s already", in, to, ainb_type(g, to), from, out), 2;
    if (!holds(g, &x, was))
        return say(why, why_n, "%s of node #%d (%s) holds %s, not %s", in, to, ainb_type(g, to), have, was), 0;
    if (old >= 0 && (uint32_t)to < g->nel) {   // the plug that records the old connection follows it (added nodes
        uint8_t *bl = block(g, (uint32_t)to);  // have no plugs)
        int cat = x.k == T_INT ? 5 : x.k == T_STRING ? 4 : 0;
        unsigned cnt = bl[0x90 + 2 * cat], base = bl[0x90 + 2 * cat + 1];
        for (unsigned j = 0; j < cnt; j++) {
            uint8_t *plug = g->b + rd32(bl + BLOCK + 4 * (base + j));
            const char *s = str_at(g, rd32(plug + 4));
            if (rd32(plug) == (uint32_t)old && s && !strcmp(s, in)) wr32(plug, (uint32_t)from);
        }
    }
    wr16(src, (uint16_t)from);
    wr16(src + 2, (uint16_t)idx);
    wr32(flags_of(&x), LINKED_FLAGS);
    wr32(o, rd32(o) | USED);
    if (!requery(g, (uint32_t)to)) return say(why, why_n, "out of memory"), 0;
    return say(why, why_n, "%s of node #%d (%s): %s -> #%d.%s", in, to, ainb_type(g, to), have, from, out), 1;
}

// A string of the graph src as a string of g.
static int restring(Ainb *g, const Ainb *src, uint32_t o, uint32_t *out) {
    if (g == src) return *out = o, 1;
    const char *s = str_at(src, o);
    return s && intern(g, s, out);
}

int ainb_add(Ainb *g, const Ainb *src, int t, char *why, size_t why_n) {
    if (t < 0 || (uint32_t)t >= nodes(src)) return say(why, why_n, "no such node to copy"), -1;
    const uint8_t *te = elem(src, (uint32_t)t), *tb = block(src, (uint32_t)t);
    const char *type = ainb_type(src, t);
    if (te[6] & 2) return say(why, why_n, "node #%d (%s) calls a module; it is not copied", t, type), -1;
    if (rd16(te + 4) || rd16(te + 0x18) || rd16(te) == 20)
        return say(why, why_n, "node #%d (%s) holds an expression or attachments; it is not copied", t, type), -1;
    for (int k = 0; k < 10; k++)
        if ((k == 2 || k == 3) && tb[0x90 + 2 * k])
            return say(why, why_n, "node #%d (%s) has children; it is not copied", t, type), -1;
    uint32_t ni = nodes(g);
    if (ni >= 0x7fff) return say(why, why_n, "the graph has too many nodes"), -1;
    size_t *po = realloc(g->param_off, sizeof *po * (g->add_nel + 1));
    if (!po) return say(why, why_n, "out of memory"), -1;
    g->param_off = po;
    uint8_t e[ELEM], bl[BLOCK];
    memcpy(e, te, ELEM);
    memset(bl, 0, BLOCK);
    wr16(e + 2, (uint16_t)ni);
    wr16(e + 0x1c, 0);   // its multi-inputs go with its connections
    wr32(e + 0x20, 0);
    wr16(e + 0x24, 0);
    wr16(e + 0x26, 0);
    uint32_t name;
    if (!restring(g, src, rd32(te + 8), &name)) return say(why, why_n, "out of memory"), -1;
    wr32(e + 8, name);
    uint64_t h = 0xcbf29ce484222325ull;   // a GUID of its own (the game shows GUIDs in debug messages only)
    for (const char *c = type; *c; c++) h = (h ^ (uint8_t)*c) * 0x100000001b3ull;
    for (int c = 0; c < 16; c++) {
        h = (h ^ (ni >> (c % 4 * 8) & 0xff)) * 0x100000001b3ull;
        e[0x2c + c] = (uint8_t)(h >> 56);
    }
    for (int k = 0; k < NTYPES; k++) {
        uint32_t pb = rd32(tb + 8 * k), pc = rd32(tb + 8 * k + 4);
        uint32_t ib = rd32(tb + 0x30 + 16 * k), ic = rd32(tb + 0x34 + 16 * k);
        uint32_t ob = rd32(tb + 0x38 + 16 * k), oc = rd32(tb + 0x3c + 16 * k);
        wr32(bl + 8 * k, count_of(&g->prop[k], PROP_SIZE[k], g->prop_n[k]));
        wr32(bl + 8 * k + 4, pc);
        wr32(bl + 0x30 + 16 * k, count_of(&g->in[k], IN_SIZE[k], g->in_n[k]));
        wr32(bl + 0x34 + 16 * k, ic);
        wr32(bl + 0x38 + 16 * k, count_of(&g->out[k], OUT_SIZE[k], g->out_n[k]));
        wr32(bl + 0x3c + 16 * k, oc);
        for (int part = 0; part < 3; part++) {
            uint32_t base = part == 0 ? pb : part == 1 ? ib : ob, cnt = part == 0 ? pc : part == 1 ? ic : oc;
            unsigned size = part == 0 ? PROP_SIZE[k] : part == 1 ? IN_SIZE[k] : OUT_SIZE[k];
            Buf *to = part == 0 ? &g->prop[k] : part == 1 ? &g->in[k] : &g->out[k];
            for (uint32_t j = 0; j < cnt; j++) {
                uint8_t x[24];
                memcpy(x, part == 0 ? prop_entry(src, k, base + j) : part == 1 ? in_entry(src, k, base + j)
                                                                              : out_entry(src, k, base + j), size);
                Param p = {part == 1, k, x};
                uint32_t o;
                const char *pname = str_at(src, rd32(x) & ~USED);
                if (!restring(g, src, rd32(x) & ~USED, &o)) return say(why, why_n, "out of memory"), -1;
                wr32(x, o);   // an output starts used by nothing
                if (k == T_PTR) {
                    if (!restring(g, src, rd32(x + 4), &o)) return say(why, why_n, "out of memory"), -1;
                    wr32(x + 4, o);
                }
                if (part < 2) {
                    uint32_t fl = rd32(flags_of(&p));
                    if (part == 1 && rds16(source_of(&p)) != -1) {   // connected: now holding its value
                        wr16(source_of(&p), 0xffff);
                        wr16(source_of(&p) + 2, 0);
                        wr32(flags_of(&p), fl = OWN_VALUE);
                    }
                    if ((fl & BOUND) && g != src)
                        return say(why, why_n, "node #%d (%s) binds %s to its graph's blackboard; it is not copied",
                                   t, type, pname ? pname : "?"), -1;
                    if (k == T_STRING && !(fl & BOUND)) {
                        if (!restring(g, src, rd32(value_of(&p)), &o)) return say(why, why_n, "out of memory"), -1;
                        wr32(value_of(&p), o);
                    }
                }
                if (!buf_put(to, x, size)) return say(why, why_n, "out of memory"), -1;
            }
        }
    }
    g->param_off[g->add_nel] = g->params.n;
    if (!buf_put(&g->elems, e, ELEM) || !buf_put(&g->params, bl, BLOCK)) return say(why, why_n, "out of memory"), -1;
    g->add_nel++;
    g->add_nq += e[6] & 1;
    say(why, why_n, "new node #%u (%s), a copy of #%d", ni, type, t);
    return (int)ni;
}
