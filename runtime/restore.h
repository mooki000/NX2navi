// Restorations (restore.c, docs/restorations.md): behaviours of the original game that this release changed, each a
// switch of its own, NX2NAVI_RESTORE_<KEY> = 1 (nx2navi.ini [restorations], the Restorations tab of the menu). One is a
// list of edits to the player's own game files: remove a placed object, add a copy of one (placed in the same file or
// in another), set a number, bool or text in a placement or in a BYML member of an actor pack (also under a key that is
// not there yet), add a member to a pack, replace a message of a text archive, change a logic graph (find a node, set
// its values, add a copy of a node, connect an output to an input). Once at start, romfs.c applies the edits of the
// switched-on restorations to each file as the game would read it (a mod's version, else the original) and serves the
// result from <profile>/restore/; an edit that does not find what it names is skipped and logged, never applied
// elsewhere.
//
// A restoration is declared in a file of its own, runtime/restore_<name>.c:
//
//   #include "restore.h"
//   static const RestoreEdit edits[] = {
//       RESTORE_SET("Banc/X/X_Static.bcett.byml.zs", "Area", 1234567890123456789u, 1.0, 100.0, -2.5, "Scale/2", 7, 3),
//       RESTORE_REMOVE("Banc/X/X_Dynamic.bcett.byml.zs", "Obj_Y", 1234567890123456790u, 4.0, 100.0, 2.5),
//   };
//   RESTORATION(example, "EXAMPLE", "Menu label", "One-line description.", edits);
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    RESTORE_KIND_SET = 1, RESTORE_KIND_REMOVE, RESTORE_KIND_ADD, RESTORE_KIND_SET_TEXT, RESTORE_KIND_ADD_MEMBER,
    RESTORE_KIND_SET_MESSAGE, RESTORE_KIND_GRAPH,
} RestoreKind;

// What a GRAPH edit does (RestoreEdit.graph_op).
enum { RESTORE_GRAPH_OP_FIND = 1, RESTORE_GRAPH_OP_SET, RESTORE_GRAPH_OP_ADD, RESTORE_GRAPH_OP_LINK };

typedef struct {
    RestoreKind kind;
    const char *file;     // RomFS path of a placement file or other BYML (".zs" ones are unpacked), or of a pack
    // the BYML member of that pack (ADD_MEMBER: the member added; SET_MESSAGE: the MSBT); NULL: the file is a BYML
    const char *member;
    // The placed object an edit is about, as the game ships it: its Gyaml, its Hash and its Translate (each within
    // 1 cm). hash 0: a SET on the document itself (a pack member's parameters).
    const char *gyaml;
    uint64_t hash;
    float at[3];
    // SET: the value below the object (or the document) at path, keys and array indices separated by '/' ("Scale/2"),
    // a number or a bool. was: what the game ships (within 1e-5 of it, relative); now: ours. new_type (a BYML_* type
    // of byml.h, not a container or a text): the key is not there yet, and SET adds it to the dict that path's last
    // '/' names, which must be there, as a value of that type.
    const char *path;
    double was, now;
    uint8_t new_type;
    // SET_TEXT: the same for a text: text_was the game's (NULL: the key is not there yet and is added), text_now ours.
    const char *text_was, *text_now;
    // ADD: a copy of the object with this Hash (in its Phive Placement ID too), Translate and Rotate (radians); the
    // copy leaves out the object's Links and SRTHash (a hash of its own transform). from: the placement file that
    // places the object copied, NULL for this file.
    uint64_t new_hash;
    float pos[3], rot[3];
    const char *from;
    // ADD_MEMBER: the member's bytes, declared (bytes, nbytes) or copied from member from_member of the pack from
    // (NULL: this pack). The pack must not have the member yet.
    const void *bytes;
    size_t nbytes;
    const char *from_member;
    // SET_MESSAGE: the message labelled path in the MSBT member, which the game ships as bytes whose msbt_hash (msbt.h:
    // FNV-1a, 64 bits) is text_hash, becomes text_now: its leading control tags (window, voice) stay, the rest is
    // replaced (UTF-8; a control tag written {group.type:hex parameters}, a closing one {/group.type}).
    uint64_t text_hash;
    // GRAPH: a change to a logic graph (an AINB file or pack member, ainb.h). The nodes it changes have names that the
    // graph's edits before it gave them: FIND names `node` the one node of type node_type whose values match `match`
    // (ainb_find's grammar); ADD names `node` a new node, a copy of the one node of type node_type matching `match` in
    // the graph `from` / from_member (from NULL: this graph); SET sets `param` of `node` from text_was to text_now;
    // LINK connects output src_out of node `src` to input `param` of `node`, which held text_was ("@name.Output",
    // "#3.Output", "bb:Name" or "=value").
    uint8_t graph_op;
    const char *node, *node_type, *match, *param, *src, *src_out;
} RestoreEdit;

typedef struct {
    const char *key;     // the setting is NX2NAVI_RESTORE_<key>: upper case, digits and '_'
    const char *name;    // the menu's label
    const char *desc;    // the menu's one-line description
    const RestoreEdit *edits;
    int nedits;
} Restoration;

#define RESTORE_OBJ_(f, g, h, x, y, z) .file = f, .gyaml = g, .hash = h, .at = {x, y, z}
// Set the value at path below the placed object gyaml / hash at (x, y, z) from was to now.
#define RESTORE_SET(file, gyaml, hash, x, y, z, path_, was_, now_) \
    {.kind = RESTORE_KIND_SET, RESTORE_OBJ_(file, gyaml, hash, x, y, z), .path = path_, .was = was_, .now = now_}
// Set the value at path of the pack's BYML member (NULL: of the file itself) from was to now.
#define RESTORE_SET_MEMBER(file_, member_, path_, was_, now_) \
    {.kind = RESTORE_KIND_SET, .file = file_, .member = member_, .path = path_, .was = was_, .now = now_}
// Remove the placed object gyaml / hash at (x, y, z).
#define RESTORE_REMOVE(file, gyaml, hash, x, y, z) \
    {.kind = RESTORE_KIND_REMOVE, RESTORE_OBJ_(file, gyaml, hash, x, y, z)}
// Add a copy of the placed object gyaml / hash at (x, y, z) as new_hash, at (px, py, pz) rotated by (rx, ry, rz).
#define RESTORE_ADD(file, gyaml, hash, x, y, z, new_hash_, px, py, pz, rx, ry, rz)                             \
    {.kind = RESTORE_KIND_ADD, RESTORE_OBJ_(file, gyaml, hash, x, y, z), .new_hash = new_hash_, .pos = {px, py, pz}, \
     .rot = {rx, ry, rz}}
// The same with the object copied from the placement file src (the game's Gyaml, Hash and Translate there).
#define RESTORE_ADD_FROM(file, src, gyaml, hash, x, y, z, new_hash_, px, py, pz, rx, ry, rz)                   \
    {.kind = RESTORE_KIND_ADD, RESTORE_OBJ_(file, gyaml, hash, x, y, z), .new_hash = new_hash_, .pos = {px, py, pz}, \
     .rot = {rx, ry, rz}, .from = src}
// Add the key at path, missing so far, below the placed object (_MEMBER: in the pack member) with the value now of
// type (BYML_FLOAT, BYML_INT, BYML_UINT, BYML_BOOL, ...).
#define RESTORE_SET_NEW(file, gyaml, hash, x, y, z, path_, type, now_)                                          \
    {.kind = RESTORE_KIND_SET, RESTORE_OBJ_(file, gyaml, hash, x, y, z), .path = path_, .now = now_,              \
     .new_type = type}
#define RESTORE_SET_MEMBER_NEW(file_, member_, path_, type, now_) \
    {.kind = RESTORE_KIND_SET, .file = file_, .member = member_, .path = path_, .now = now_, .new_type = type}
// Set the text at path from was to now (was NULL: add the key, missing so far, with the text now).
#define RESTORE_SET_TEXT(file, gyaml, hash, x, y, z, path_, was_, now_)                                         \
    {.kind = RESTORE_KIND_SET_TEXT, RESTORE_OBJ_(file, gyaml, hash, x, y, z), .path = path_, .text_was = was_,    \
     .text_now = now_}
#define RESTORE_SET_MEMBER_TEXT(file_, member_, path_, was_, now_)                                               \
    {.kind = RESTORE_KIND_SET_TEXT, .file = file_, .member = member_, .path = path_, .text_was = was_,             \
     .text_now = now_}
// Add the member to the pack: its bytes declared (an array), or a copy of member src_member of the pack src (NULL:
// this pack).
#define RESTORE_ADD_MEMBER(file_, member_, data) \
    {.kind = RESTORE_KIND_ADD_MEMBER, .file = file_, .member = member_, .bytes = data, .nbytes = sizeof data}
#define RESTORE_COPY_MEMBER(file_, member_, src, src_member) \
    {.kind = RESTORE_KIND_ADD_MEMBER, .file = file_, .member = member_, .from = src, .from_member = src_member}
// Replace the message labelled label in the MSBT member of a text archive (Mals/<language>.Product.<n>.sarc.zs): the
// game's message hashes to hash (msbt_hash of its bytes as stored), ours is text (its leading control tags stay).
#define RESTORE_SET_MESSAGE(file_, member_, label, hash, text)                                                     \
    {.kind = RESTORE_KIND_SET_MESSAGE, .file = file_, .member = member_, .path = label, .text_hash = hash,       \
     .text_now = text}
// Logic graph edits (RESTORE_KIND_GRAPH) on the graph `member` of the pack `file` (member NULL: the file is the graph).
// They apply in their order and all or none: when one is skipped, the graph keeps its bytes. Names are written
// without the '@' that refers to them.
// Name `name` the one node of type `type` whose values match `match` ("InstanceName=Enemy_X;Input<-#13.Bool").
#define RESTORE_GRAPH_FIND(file_, member_, name, type, match_)                                                     \
    {.kind = RESTORE_KIND_GRAPH, .graph_op = RESTORE_GRAPH_OP_FIND, .file = file_, .member = member_, .node = name, \
     .node_type = type, .match = match_}
// Set property or input `param` of the node named `name` from was to now (text: "0.3", "true", "Name", "x,y,z").
#define RESTORE_GRAPH_SET(file_, member_, name, param_, was, now)                                                  \
    {.kind = RESTORE_KIND_GRAPH, .graph_op = RESTORE_GRAPH_OP_SET, .file = file_, .member = member_, .node = name,  \
     .param = param_, .text_was = was, .text_now = now}
// Add a node named `name`: a copy of the one node of type `type` matching `match` in the graph src_member of the
// file src (src NULL: this graph; src_member NULL: src is a graph), unconnected.
#define RESTORE_GRAPH_ADD(file_, member_, name, type, match_, src, src_member)                                     \
    {.kind = RESTORE_KIND_GRAPH, .graph_op = RESTORE_GRAPH_OP_ADD, .file = file_, .member = member_, .node = name,  \
     .node_type = type, .match = match_, .from = src, .from_member = src_member}
// Connect output `out` of the node named `from_` to input `in` of the node named `to`, which held was.
#define RESTORE_GRAPH_LINK(file_, member_, from_, out, to, in, was)                                                \
    {.kind = RESTORE_KIND_GRAPH, .graph_op = RESTORE_GRAPH_OP_LINK, .file = file_, .member = member_, .node = to,   \
     .param = in, .src = from_, .src_out = out, .text_was = was}

// The registry: every RESTORATION of the linked objects meets in one section (as host.h's HOST_OVERRIDE entries do),
// which restore.c walks: on PE/COFF ".rst$m", sorted between restore.c's ".rst$a" and ".rst$z" markers; on ELF
// "nx2navi_rst", between the linker's __start_nx2navi_rst and __stop_nx2navi_rst.
#ifdef _WIN32
#define RESTORE_SECTION section(".rst$m")
#else
#define RESTORE_SECTION section("nx2navi_rst")
#endif
#define RESTORATION(id, key, name, desc, edits)                                                                   \
    static const Restoration restore_##id = {key, name, desc, edits, (int)(sizeof edits / sizeof *edits)};         \
    __attribute__((used, RESTORE_SECTION)) static const Restoration *const restore_entry_##id = &restore_##id

// The declared restorations, sorted by key; NULL past the end.
int restore_count(void);
const Restoration *restore_at(int i);
// 1 when its setting is on (NX2NAVI_RESTORE_<key> set and not "0"); restore_set writes it (the menu).
int restore_on(const Restoration *r);
void restore_set(const Restoration *r, int on);
// The edits of r this start applied and skipped; both 0 when it was off at start, or before the start applied them.
// Any thread.
void restore_started(const Restoration *r, int *applied, int *skipped);

// romfs.c overlay_init: applies the switched-on restorations. read returns a RomFS file as the game would read it
// (malloc'd, NULL when missing): the files edited, and the files edits copy from; serve takes over out[0..n) and serves
// it in place of rel, and returns 0 when it cannot.
void restore_apply(void *(*read)(const char *rel, size_t *n), int (*serve)(const char *rel, void *out, size_t n));
// The pack members the last start added (ADD_MEMBER) or rewrote, for romfs.c restbl_overlay, which sizes the game's
// buffer for each from the member's own entry: member i's RomFS-style path, its size before the edits (0: added) and
// after, and whether its bytes were declared (else copied from a member of the game's or edited from the game's); 0
// past the last.
int restore_member(int i, const char **path, size_t *was, size_t *now, int *declared);
// --probe-restore (main.c): every declared restoration against the files as the game would read them, nothing served;
// prints a line per edit. 0 when every edit applied.
int restore_probe(void);
// romfs.c: RomFS file rel as the game reads it (the overlay that replaces it, else the original); malloc'd, NULL when
// missing or 16 MiB or more.
void *romfs_read_served(const char *rel, size_t *n);
// romfs.c: the i-th RomFS path that starts with prefix, in the index's order; NULL past the last.
const char *romfs_with(const char *prefix, int i);

// What the edits did to a pack member (restore_file): was its size before (0: the member was added), now after; both
// 0 when the edit changed no member's size bookkeeping (another edit of the same member carries it, or none applied).
typedef struct { size_t was, now; } RestoreMemberSize;

// The edit engine on one file (restore.c; tests/restore_check.c runs it on files of its own): applies edits[0..n),
// all naming this file, to in[0..size), the file's bytes as stored. A zstd frame is unpacked and packed again with the
// dictionary of its id among the members of dic[0..dic_n), Pack/ZsDic.pack.zs unpacked (NULL: none). fetch returns
// another file as stored (malloc'd, NULL when missing): what ADD (from) and ADD_MEMBER (from) copy from, and the
// area's other placement file a copy's Hash is checked against. list returns the i-th RomFS path that starts with
// prefix (NULL past the last; NULL list: none): the area's static compound collision (Phive/StaticCompoundBody/...),
// which a removal is checked against. ok[k] gets 1 for each edit applied; sizes[k] (NULL: not wanted) the member an
// ADD_MEMBER added (was 0), and for the first applied edit of each pack member rewritten, that member's size before
// and after its edits; note gets what each edit did or why it was skipped (k = -1: the file itself). Returns the
// edited file (malloc'd, packed as the original was) when an edit applied, else NULL.
void *restore_file(const RestoreEdit *const *edits, int n, const void *in, size_t size, const void *dic, size_t dic_n,
                   void *(*fetch)(const char *rel, size_t *n), const char *(*list)(const char *prefix, int i), int *ok,
                   RestoreMemberSize *sizes, void (*note)(void *ctx, int k, const char *text), void *ctx,
                   size_t *out_n);
