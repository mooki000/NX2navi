// Test autopilot (autopilot.c): NX2NAVI_AUTOPILOT=<file> drives Link through a script of steps once a save has loaded.
#pragma once
#include <stdint.h>

// input.c poll_merged, under its lock, after the input script: ORs the autopilot's buttons into *b and sets the stick it
// drives (sl left, sr right; -32767..32767, up +y). Returns at once without NX2NAVI_AUTOPILOT.
void autopilot_input(uint64_t *b, int32_t sl[2], int32_t sr[2]);
// gfx_present.c, once per presented game image on the present thread: counts the route's frame times and returns the
// path of a named picture to take of this frame, NULL for none. can_shoot 0 (the frame is already being taken for
// another reason) keeps a pending picture for the next frame.
const char *autopilot_present(int can_shoot);
// ov_input.c npad_states, on the guest thread that polls the pad, right after its poll: the save step's request for the
// game's autosave, which needs a guest thread to call the game. Returns at once unless a save step asks.
struct Cpu;
void autopilot_guest(struct Cpu *c);
