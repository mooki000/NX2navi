// Restorations AUTO_HOP and LEDGE_DIVE (game 1.1.1): what Link does when he leaves a ledge. In the original game he
// jumps by himself when he runs off a ledge, and a jump off a high ledge with deep water below is a dive. This release
// has a jump button instead: run off a ledge and Link falls, and its dive is used only by the Zora's Domain diving game.
//
// Link's moves are graphs the game's C++ code runs; on foot the walk scheduler picks his next action (main+0x7f70d0,
// its per-frame update, and main+0x7fa150, the transitions it checks on entering and when its child ends). The child
// it picks is an index the scheduler keeps per action name (main+0xd48e90 fills them): Jump at +0x32, Fall at +0x33,
// WaterDive at +0x42 of the scheduler. The patch set "ledge" (recomp/patches-v111.txt) puts a call to ledge_site below
// at five instructions:
//
// - main+0x7f755c, in the update where the ground check (main+0x7f76b0: nothing within reach below Link's feet) found
//   no ground: the instruction loads the stick's tilt, and above 0.65 the update keeps Link walking a few frames in the
//   air (a grace in which the jump button still jumps) before it picks Fall at main+0x7f75a0. AUTO_HOP: running (see
//   HOP_SPEED) and allowed to jump, Link skips the grace (the tilt reads 0) and the pick at main+0x7f75a0 becomes Jump.
// - main+0x7f75a0, that pick of Fall: Jump after AUTO_HOP's decision, else Fall as before.
// - main+0x7f72a4 and main+0x7fa524, where the update and the transitions pick Jump for the jump button.
// - main+0x301e9b4, in the diving game's water check (main+0x301e700), where it compares Link's height with the
//   surface of the water it found ahead: LEDGE_DIVE asks that check with Link at least DIVE_HEIGHT above the water.
//
// The Jump the hook picks is the game's own: the action's entry (main+0x93a9b0) pushes Link up with the jump's height
// and picks the running jump above 3 m/s, as for the button. Every handler does the instruction it replaces, so with
// both switches off the game runs as it ships; the switches are read again when the settings change (cfg_version).
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "restore.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

RESTORATION_CODE(auto_hop, "AUTO_HOP", "Jump off ledges",
                 "Link jumps by himself when he runs off a ledge; walking off still drops him.");
RESTORATION_CODE(ledge_dive, "LEDGE_DIVE", "Dive from ledges",
                 "A jump off a ledge at least 5 m above deep water is a dive into it.");

#ifdef RT_PATCH_PAGE

#define MAIN 0x7000000000ull
#define HOST_LEDGE (RT_PATCH_PAGE + 0xf50)
// The five sites (main offsets): ledge_site tells them apart by the return address the bl leaves in x30.
#define SITE_GRACE 0x7f755c        // ldr x8, [x22, #0xa0]: the stick tilt the grace compares with 0.65
#define SITE_FALL 0x7f75a0         // ldrb w1, [x19, #0x33]: the Fall child
#define SITE_JUMP_UPDATE 0x7f72a4  // ldrb w1, [x19, #0x32]: the Jump child, picked by the update
#define SITE_JUMP_ENTER 0x7fa524   // mov w8, #0x32: the Jump child's offset, picked by the transitions
#define SITE_WATER 0x301e9b4       // ldr s1, [sp, #0x44]: the height of the water surface found ahead
// Game functions the hook asks (x0 = Link's actor).
#define CAN_JUMP 0x7fb690          // (actor, force): may Link jump now; force 1 leaves out the button
#define DIVE_STATE_OK 0x301b410    // (actor): the dive's test of Link's state in the diving game
#define DEEP_WATER 0x301e700       // (actor, 0): deep water ahead, the diving game's test (see ledge_dive_ok)
// Offsets of the walk scheduler's children (main+0xd48e90) and of Link's actor.
#define CHILD_JUMP 0x32
#define CHILD_FALL 0x33
#define CHILD_DIVE 0x42
#define ACTOR_VEL_X 0x388          // Link's velocity in m/s, x and z (the jump's entry reads these two for his speed)
#define ACTOR_VEL_Z 0x390
#define ACTOR_POS 0x31c
// AUTO_HOP: Link's speed over the ground from which a run off a ledge is a jump. 3 m/s is the speed above which the
// jump's entry (main+0x93a9b0) plays its running jump; a full stick runs faster, a walk at a third of it is slower.
#define HOP_SPEED 3.0f
// LEDGE_DIVE: the height above the water from which the jump is a dive: PlayerGameParam's LargeLandHeight, the drop
// after which a landing on the ground is the heavy one.
#define DIVE_HEIGHT 5.0f

static int hop_on, dive_on;
static unsigned seen_version = ~0u;
static _Thread_local uint64_t hop_scheduler;   // the walk scheduler whose fall SITE_GRACE turned into a jump
static _Thread_local float water_margin;       // added to the water's height while ledge_dive_ok asks DEEP_WATER
static const float no_tilt = 0;                // the tilt SITE_GRACE hands the grace's test when Link hops

static void switches(void) {
    unsigned v = cfg_version();
    if (v == seen_version) return;
    seen_version = v;
    hop_on = restore_key_on("AUTO_HOP");
    dive_on = restore_key_on("LEDGE_DIVE");
}

static float f32_at(uint64_t a) {
    float f;
    memcpy(&f, (const void *)(uintptr_t)a, sizeof f);
    return f;
}

// A game predicate called from a site in the middle of a function: the guest's registers stay as they were (the call
// may change any caller-saved one), the answer is its bit 0.
static int ask(Cpu *c, uint64_t fn, uint64_t a0, uint64_t a1) {
    uint64_t x[31], nzcv = c->nzcv;
    __uint128_t v[32];
    memcpy(x, c->x, sizeof x);
    memcpy(v, c->v, sizeof v);
    uint64_t r = rt_call(c, MAIN + fn, 2, a0, a1);
    memcpy(c->x, x, sizeof x);
    memcpy(c->v, v, sizeof v);
    c->nzcv = nzcv;
    return (int)(r & 1);
}

static void note(const char *what, uint64_t actor) {
    static int logged;
    if (logged >= 16) return;
    logged++;
    rt_log("ledge: %s at %.2f %.2f %.2f", what, f32_at(actor + ACTOR_POS), f32_at(actor + ACTOR_POS + 4),
           f32_at(actor + ACTOR_POS + 8));
}

// LEDGE_DIVE: the jump Link is about to make is a dive. The diving game's own tests decide where: its test of Link's
// state, and its water test (main+0x301e700) 6 m ahead of him: water within 100 m below that point and under his feet,
// at least 2 m deep (nothing solid within 2 m below the surface) and nothing in the way of the dive. Asked with
// water_margin, the surface must also lie DIVE_HEIGHT below his feet.
static int ledge_dive_ok(Cpu *c, uint64_t scheduler, uint64_t actor) {
    if (!dive_on || LD8(scheduler + CHILD_DIVE) == 0xff || !ask(c, DIVE_STATE_OK, actor, 0)) return 0;
    water_margin = DIVE_HEIGHT;
    int ok = ask(c, DEEP_WATER, actor, 0);
    water_margin = 0;
    if (ok) note("LEDGE_DIVE: the jump is a dive", actor);
    return ok;
}

// SITE_GRACE (in main+0x7f70d0: x19 the walk scheduler, x20 Link's actor, x22 his state): no ground under Link.
static void at_grace(Cpu *c) {
    uint64_t state = c->x[22], actor = c->x[20];
    c->x[8] = LD64(state + 0xa0);
    hop_scheduler = 0;
    if (!hop_on) return;
    float vx = f32_at(actor + ACTOR_VEL_X), vz = f32_at(actor + ACTOR_VEL_Z);
    if (vx * vx + vz * vz < HOP_SPEED * HOP_SPEED || !ask(c, CAN_JUMP, actor, 1)) return;
    hop_scheduler = c->x[19];
    c->x[8] = (uint64_t)(uintptr_t)&no_tilt;
}

// SITE_FALL: the update picks Fall; after at_grace's decision it picks Jump (or the dive).
static void at_fall(Cpu *c) {
    uint64_t scheduler = c->x[19];
    int child = CHILD_FALL;
    if (hop_scheduler == scheduler) {
        child = ledge_dive_ok(c, scheduler, c->x[20]) ? CHILD_DIVE : CHILD_JUMP;
        if (child == CHILD_JUMP) note("AUTO_HOP: Link jumps off the ledge", c->x[20]);
    }
    hop_scheduler = 0;
    c->x[1] = LD8(scheduler + child);
}

// SITE_JUMP_UPDATE (main+0x7f70d0, x20 the actor) and SITE_JUMP_ENTER (main+0x7fa150, x21 the actor): the jump button.
static void at_jump_update(Cpu *c) {
    c->x[1] = LD8(c->x[19] + (ledge_dive_ok(c, c->x[19], c->x[20]) ? CHILD_DIVE : CHILD_JUMP));
}

static void at_jump_enter(Cpu *c) { c->x[8] = ledge_dive_ok(c, c->x[19], c->x[21]) ? CHILD_DIVE : CHILD_JUMP; }

// SITE_WATER (main+0x301e700): the surface's height, raised by water_margin while ledge_dive_ok asks.
static void at_water(Cpu *c) {
    float y = f32_at(c->sp + 0x44) + water_margin;
    uint32_t bits;
    memcpy(&bits, &y, sizeof bits);
    c->v[1] = bits;
}

static void ledge_site(Cpu *c) {
    switches();
    switch (c->x[30] - 4 - MAIN) {
    case SITE_GRACE: at_grace(c); break;
    case SITE_FALL: at_fall(c); break;
    case SITE_JUMP_UPDATE: at_jump_update(c); break;
    case SITE_JUMP_ENTER: at_jump_enter(c); break;
    case SITE_WATER: at_water(c); break;
    default: rt_fatal("ledge: called from %#llx, not one of its sites", (unsigned long long)c->x[30]);
    }
}

__attribute__((constructor)) static void ledge_register(void) {
    if (rt_patch_set("ledge") >= 0) rt_register_host(HOST_LEDGE, ledge_site);
}

#endif
