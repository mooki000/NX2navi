// MSBT message files (msbt.c): the game's texts, the members of Mals/<language>.Product.<n>.sarc.zs
// (EventFlowMsg/NaviHint_Enemy.msbt, ...), little-endian, UTF-8 or UTF-16. restore_engine.c replaces a message with it.
#pragma once
#include <stddef.h>
#include <stdint.h>

// FNV-1a, 64 bits, of p[0..n): how a restoration names the message it expects without holding its text.
uint64_t msbt_hash(const void *p, size_t n);

typedef enum { MSBT_SET = 1, MSBT_ALREADY, MSBT_OTHER, MSBT_FAILED } MsbtResult;

// The message labelled `label` in the MSBT p[0..n) set to `text`: the message's leading control tags (its window and
// Navi's voice) stay, the rest becomes text, UTF-8 with a control tag written {group.type:hex bytes of its
// parameters} and a closing one {/group.type}, then the terminating NUL. Only when the message as stored hashes
// (msbt_hash) to want: *found gets its hash.
// MSBT_SET: *out (malloc'd, *out_n bytes) is the file with the new message, every other section and message as they
// were. MSBT_ALREADY: the message is that text already. MSBT_OTHER: it is another message. MSBT_FAILED: why says what
// is wrong (no such label, a file that is not one, malformed markup).
MsbtResult msbt_set_message(const uint8_t *p, size_t n, const char *label, uint64_t want, const char *text,
                            uint8_t **out, size_t *out_n, uint64_t *found, const char **why);
