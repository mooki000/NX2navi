// Restoration: the first-person look. In the original, a button held while Link stood on the ground put the view in his
// eyes, the stick turned it, and letting go returned to the camera behind him; Link stood still and could not act. The
// remake has no such view: no camera mode, state or data for it is left in the game (the player camera's modes are
// chase, lock-on, item aiming over the shoulder, swim, climb, horse, fixed and event cameras).
//
// Here the look button is the Switch L3 (the left stick click, key V; the Controls tab moves it), which the game does
// not use. The patch set first_person (recomp/patches-v111.txt) adds two host calls to 1.1.1's main:
//  - camera_raw, in the camera component's apply (main+0x4c4120) where it copies the pose its camera mode wrote (the
//    "raw" pose) into the pose the renderer, the engine camera, the camera actor's transform and the listener take:
//    while the look is on it writes the eye, the target and the near plane there first;
//  - switcher_mode, in the player camera's mode switcher (main+0x27bf960) after it picked this update's mode: it
//    keeps the mode id and the switcher node, which also say that the game branch of the camera ran (an event, a talk
//    or a cutscene runs the event branch instead).
// first_person_input, called by input.c input_poll, reads the button and the sticks for the view and, while the view
// is on, gives the game only Plus and Minus, so Link stands still and acts on nothing, as in the original.
//
// When the look may start (all of it, read when the button goes down):
//  - the game branch of the player camera ran in one of the last two camera updates, and its mode is 0 (Normal) or 5
//    (Indoor): not targeting (13..19), aiming (20, 21, 28..34, 36), swimming (8..12), climbing (4, 23), crawling (3),
//    pushing (1, 2), on a horse (25..30), in a fixed camera (40, 41) or an event;
//  - ZL is not held: targeting with nothing in reach leaves the camera's mode at Normal (seen in a run), so the
//    button stands for it (the game's Targeting Type "toggle" keeps targeting after a release: not covered);
//  - Link stands on the ground: bit 6 of the 16-bit flags at +0x1c38 of the player's component at module slot +0x358,
//    what the game's QueryIsPlayerOnGroundForEvent (main+0x2a38b30) reads;
//  - the player camera is the camera shown: a view of the renderer (GameGraphics +0x1e8, +0xb28: count +0x10, views
//    +0x18 of 0x7408 bytes) has this component's last eye at +0x733c.
// The view leaves when the button is released or one of these stops holding (the view's camera for 15 updates in a
// row); a new press is needed to look again.
//
// The view: Link's eyes in the skeleton's bind pose (child 0.98 m above his feet and 0.044 m ahead, adult 1.49 m and
// 0.053 m), his facing from his rotation (actor +0x328, row-major, model forward +z), the right stick (the mouse
// camera too) turning it up to 150 degrees a second across and 100 up and down, 70 degrees up to 60 down. It blends in
// over 0.15 s and back out to the game's camera over 0.3 s; the game's own camera keeps running underneath, so it is
// where it would have been. Link's
// model is hidden through its GameModelController (module slot +0x260): a bit of the hide byte +0xe4 of our own (the
// game's OneShotGameModelControllerSetVisible uses bit 2; the model is drawn while the byte is 0) and the update bit
// +0x120 bit 0, as the game sets them; the near plane goes to 0.2 m for what is not part of that model.
//
// Off (the setting, NX2NAVI_RESTORE_FIRST_PERSON, read while the game runs), the patch set is off: the game runs its
// own instructions at both sites. The setting going off while the view is on first lets the view blend out.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "input.h"
#include "input_map.h"
#include "platform/platform.h"
#include "restore.h"

#if __has_include("patches.h")
#include "layout.h"
#include "patches.h"
#endif

RESTORATION_CODE(first_person, "FIRST_PERSON", "First-person look",
                 "Hold L3 (the left stick click) on the ground to look through Link's eyes; the stick looks around.");

enum { OFF, IN, ON, OUT };   // the view's state, camera side

// Input side and camera side meet here: input_poll runs on the game's pad read, the camera on its update.
static volatile int32_t on_now;        // the setting
static volatile int32_t look_held;     // the look button is down (input)
static volatile int32_t target_held;   // ZL, targeting, is down (input)
static volatile int64_t look_stick;    // the stick that turns the view (x in the low half, y in the high), input
static volatile int32_t engaged;       // the view takes the input
static volatile int32_t state_now;     // OFF while no view is shown or blending
static volatile uint64_t camera_ns;    // the player camera's last update with the view on (plat_now_ns)

static int patch_set(void) {
    static int set = -2;
    if (set == -2) set = rt_patch_set("first_person");
    return set;
}

void first_person_input(uint64_t *buttons, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) {
    int set = patch_set();
    if (set < 0) return;   // a game version without the sites
    static unsigned seen = ~0u;
    static int on = -1, switched = -1;
    unsigned v = cfg_version();
    if (v != seen) {
        seen = v;
        int now = restore_key_on("FIRST_PERSON");
        if (now != on && on >= 0) rt_log("restore: FIRST_PERSON: %s", now ? "on" : "off");
        on = now;
        __atomic_store_n(&on_now, on, __ATOMIC_RELAXED);
    }
    // the sites go off only once the camera has left the view (blended out, Link shown again)
    int want = on || __atomic_load_n(&state_now, __ATOMIC_RELAXED) != OFF;
    if (want != switched) {
        switched = want;
        rt_patch_switch(set, want);
    }
    if (!on) {
        __atomic_store_n(&look_held, 0, __ATOMIC_RELAXED);
        return;
    }
    __atomic_store_n(&look_held, (int32_t)(*buttons >> B_STICK_L & 1), __ATOMIC_RELAXED);
    // ZL held is targeting (the camera's mode stays Normal when nothing is in reach to target)
    __atomic_store_n(&target_held, (int32_t)(*buttons >> B_ZL & 1), __ATOMIC_RELAXED);
    // The right stick turns the view; the mouse camera arrives as right-stick deflection (input.c mouse_camera). The
    // left stick is the one L3 presses, so it is left out: a thumb that clicks it would turn the view.
    __atomic_store_n(&look_stick, (int64_t)(uint32_t)*rx | (int64_t)*ry << 32, __ATOMIC_RELAXED);
    // Masked only while the camera updates the view: a game that stopped updating its camera (a pause) gets its input.
    static uint64_t kept;   // the buttons kept from the game during this view, for the log
    if (__atomic_load_n(&engaged, __ATOMIC_RELAXED) &&
        plat_now_ns() - __atomic_load_n(&camera_ns, __ATOMIC_RELAXED) < 100000000ull) {
        const uint64_t pass = 1ull << B_PLUS | 1ull << B_MINUS, keep = *buttons & ~pass & 0xffffull & ~(1ull << B_STICK_L);
        if (keep & ~kept) {
            kept |= keep;
            rt_log("restore: FIRST_PERSON: buttons %#llx kept from the game while looking", (unsigned long long)keep);
        }
        *buttons &= pass;
        *lx = *ly = *rx = *ry = 0;
    } else {
        kept = 0;
    }
}

#ifdef RT_PATCH_PAGE
#define HOST_CAMERA_RAW (RT_PATCH_PAGE + 0xf60)   // not +0xf80: ov_zstd.c's stream feed takes it on 1.1.1
#define HOST_SWITCHER_MODE (RT_PATCH_PAGE + 0xf70)

// 1.1.1 main
#define GAME_GRAPHICS (MOD_MAIN_BASE + 0x4e3e258)   // the GameGraphics module (main+0x4c4a28 reads it the same way)
#define ACTOR_TABLE (MOD_MAIN_BASE + 0x4e371d0)     // an actor table whose +0x480 has held the player
// The camera component (main+0x4c4120 apply, main+0x4c48c0 update)
enum { CAM_OWNER = 0x10, RAW_EYE_F = 0x60, RAW_EYE = 0x70, RAW_TARGET_F = 0x88, RAW_TARGET = 0x98, RAW_NEAR = 0xbc,
       FINAL_EYE_F = 0xd8, RAW_FROM = 0x60, RAW_SIZE = 0x60 };
// The camera switcher node (main+0x27bf960): mode id, its AI context; actors: the component set, names, pose
enum { NODE_MODE = 0x34, ACTOR_COMPONENTS = 0x270, ACTOR_NAME = 0x260, ACTOR_POS = 0x31c, ACTOR_ROT = 0x328,
       SLOT_MODEL = 0x260, SLOT_PLAYER = 0x358, SLOT_CAMERA_STATE = 0x360, PLAYER_FLAGS = 0x1c38, ON_GROUND = 0x40,
       MODEL_HIDE = 0xe4, MODEL_UPDATE = 0x120, HIDE_OURS = 0x80, VIEW_STRIDE = 0x7408, VIEW_EYE = 0x733c };

#define YAW_RATE 150.0     // degrees a second at full deflection
#define PITCH_RATE 100.0
#define PITCH_UP 70.0
#define PITCH_DOWN 60.0
#define IN_S 0.15
#define OUT_S 0.3
#define NEAR_M 0.2f
#define DEG (3.14159265358979323846 / 180)

typedef struct { double x, y, z; } V3;
static V3 v3(double x, double y, double z) { return (V3){x, y, z}; }
static V3 add(V3 a, V3 b) { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 sub(V3 a, V3 b) { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 scale(V3 a, double k) { return v3(a.x * k, a.y * k, a.z * k); }
static double len(V3 a) { return sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

static uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static uint8_t rd8(uint64_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }
static float rdf(uint64_t a) { float v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static double rdd(uint64_t a) { double v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static void wrf(uint64_t a, float v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static void wrd(uint64_t a, double v) { memcpy((void *)(uintptr_t)a, &v, 8); }
static V3 rd3f(uint64_t a) { return v3(rdf(a), rdf(a + 4), rdf(a + 8)); }
static V3 rd3d(uint64_t a) { return v3(rdd(a), rdd(a + 8), rdd(a + 16)); }

// Reads of pointers the game may have let go (a chain into another object): 0 instead of a fault
static uint64_t peek64(uint64_t a) {
    uint64_t v = 0;
    if (a < 0x10000 || a >= RT_ADDRESS_LIMIT || !plat_mem_read(a, &v, 8)) return 0;
    return v;
}
// The definition name of an actor (+0x260, "Player_Child", "PlayerCamera") into out; 0 when it cannot be read. The
// first read stops at the string's page end, so a short name at the end of a page still reads.
static int actor_name(uint64_t actor, char *out, size_t n) {
    uint64_t p = actor ? peek64(actor + ACTOR_NAME) : 0;
    size_t first = p ? (size_t)(((p | 4095) + 1) - p) : 0, got = 0;
    out[0] = 0;
    if (!p || p >= RT_ADDRESS_LIMIT) return 0;
    while (got < n - 1) {
        size_t k = got ? n - 1 - got : first < n - 1 ? first : n - 1;
        if (!plat_mem_read(p + got, out + got, k)) return out[0] = 0, 0;
        got += k;
        for (size_t i = got - k; i < got; i++)
            if (!out[i]) return 1;
            else if (out[i] < 0x20 || out[i] > 0x7e) return out[0] = 0, 0;
    }
    out[n - 1] = 0;
    return 1;
}

// Switcher side (switcher_mode)
static volatile uint64_t switch_node;
static volatile uint32_t switch_mode;
static volatile uint64_t switch_seq;   // camera_seq + 1 when the switcher last ran (0: never)
static volatile uint64_t camera_seq;   // player camera updates

typedef enum { WHY_NONE, WHY_RELEASED, WHY_OFF, WHY_EVENT, WHY_MODE, WHY_TARGET, WHY_AIR, WHY_NO_LINK, WHY_NOT_SHOWN } Why;
static const char *const why_text[] = {"", "the button was released", "the setting was switched off",
                                       "no camera mode this update (an event, a talk or a cutscene)", "camera mode",
                                       "ZL (targeting) is held", "Link is not on the ground", "Link was not found",
                                       "another camera is shown"};

static struct {
    int state, armed, missed, adult;
    uint64_t comp, owner, comp_ns, last_ns, since_ns, player;
    uint64_t model;   // the model controller that has our hide bit
    double t, yaw, pitch;
    uint8_t game[RAW_SIZE];   // the game's raw pose (from RAW_FROM: eye, target, up, near) when it last wrote one
    int have_game;
    V3 wrote_eye, wrote_target;
    uint64_t node, context_actor, camera_state;   // what the switcher node leads to
} fp = {.armed = 1};

// The camera component of the player camera: its owner is the actor whose AI runs the switcher, or one named
// PlayerCamera. Other components (the event, UI and hint cameras) are remembered so their updates cost one compare.
static int player_camera(uint64_t comp, uint64_t owner) {
    static uint64_t no[8];
    static int next;
    if (owner && owner == fp.context_actor) return 1;
    for (int i = 0; i < 8; i++)
        if (no[i] == (comp ^ owner)) return 0;
    char name[24];
    if (actor_name(owner, name, sizeof name) && !strcmp(name, "PlayerCamera")) return 1;
    if (fp.context_actor) no[next++ & 7] = comp ^ owner;   // judged against a known switcher
    return 0;
}

// The switcher node's AI context leads to its actor ([[node+8]+8]+0x1f0, as main+0xc70e70 reads it) and the camera
// state the switcher reads ([[actor+0x270]+0x360])
static void node_follow(uint64_t node) {
    if (node == fp.node) return;
    fp.node = node;
    uint64_t ctx = peek64(peek64(node + 8) + 8);
    fp.context_actor = ctx ? peek64(ctx + 0x1f0) : 0;
    uint64_t set = fp.context_actor ? peek64(fp.context_actor + ACTOR_COMPONENTS) : 0;
    fp.camera_state = set ? peek64(set + SLOT_CAMERA_STATE) : 0;
}

// Link's actor: the actor the camera state follows ([[state+0x10]+0x150], an actor reference), the switcher's own
// actor, or the actor table's +0x480; the one whose definition name starts with Player_ (and says Child or Adult).
static uint64_t player_find(void) {
    uint64_t ref = fp.camera_state ? peek64(fp.camera_state + 0x10) : 0;
    uint64_t table = peek64(ACTOR_TABLE);
    const uint64_t cand[3] = {ref ? peek64(ref + 0x150) : 0, fp.context_actor, table ? peek64(table + 0x480) : 0};
    static const char *const how[3] = {"the camera's target", "the camera AI's actor", "the actor table"};
    for (int i = 0; i < 3; i++) {
        if (!cand[i]) continue;
        if (cand[i] == fp.player) return fp.player;
        char name[24];
        if (!actor_name(cand[i], name, sizeof name) || strncmp(name, "Player_", 7)) continue;
        fp.player = cand[i];
        fp.adult = strstr(name, "Adult") != NULL;
        uint64_t set = peek64(cand[i] + ACTOR_COMPONENTS);
        rt_log("restore: FIRST_PERSON: Link is %s at %#llx (found as %s); his model controller %#llx", name,
               (unsigned long long)cand[i], how[i], (unsigned long long)(set ? peek64(set + SLOT_MODEL) : 0));
        return fp.player;
    }
    static int told;
    if (!told) {   // what the candidates were, once: a layout that moved shows here
        told = 1;
        char names[3][24];
        for (int i = 0; i < 3; i++) actor_name(cand[i], names[i], sizeof names[i]);
        rt_log("restore: FIRST_PERSON: Link not found: %#llx (%s), %#llx (%s), %#llx (%s)",
               (unsigned long long)cand[0], names[0], (unsigned long long)cand[1], names[1],
               (unsigned long long)cand[2], names[2]);
    }
    return 0;
}

static int on_ground(uint64_t player) {
    uint64_t set = peek64(player + ACTOR_COMPONENTS);
    uint64_t st = set ? peek64(set + SLOT_PLAYER) : 0;
    uint16_t flags = 0;
    return st && plat_mem_read(st + PLAYER_FLAGS, &flags, 2) && (flags & ON_GROUND);
}

// A view of the renderer shows this component's last eye (main+0x4c48c0 copies it there when the component is that
// view's camera)
static int shown(uint64_t comp) {
    uint64_t gg = peek64(GAME_GRAPHICS), r = gg ? peek64(gg + 0x1e8) : 0, mgr = r ? peek64(r + 0xb28) : 0;
    uint32_t n = 0;
    uint64_t views = mgr ? peek64(mgr + 0x18) : 0;
    if (!views || !plat_mem_read(mgr + 0x10, &n, 4)) return 0;
    for (uint32_t i = 0; i < n && i < 8; i++) {
        uint8_t eye[12];
        if (plat_mem_read(views + (uint64_t)i * VIEW_STRIDE + VIEW_EYE, eye, 12) &&
            !memcmp(eye, (const void *)(uintptr_t)(comp + FINAL_EYE_F), 12))
            return 1;
    }
    return 0;
}

// Link's model hidden (or shown again) through his model controller, as the game's own hide does. A controller that
// is no longer his (another actor, another area) is let go of, never written.
static void model_hide(int hide) {
    uint64_t set = fp.player ? peek64(fp.player + ACTOR_COMPONENTS) : 0, mc = set ? peek64(set + SLOT_MODEL) : 0;
    if (mc != fp.model) fp.model = 0;
    if (!mc || (!hide && !fp.model)) return;
    uint8_t *b = (uint8_t *)(uintptr_t)(mc + MODEL_HIDE);
    uint8_t was = *b, now = hide ? was | HIDE_OURS : was & ~HIDE_OURS;
    fp.model = hide ? mc : 0;
    if (now == was) return;
    *b = now;
    if (!was != !now) __atomic_fetch_or((uint32_t *)(uintptr_t)(mc + MODEL_UPDATE), 1u, __ATOMIC_RELAXED);
}

static void pose_write(uint64_t comp, V3 eye, V3 target, float near) {
    wrd(comp + RAW_EYE, eye.x), wrd(comp + RAW_EYE + 8, eye.y), wrd(comp + RAW_EYE + 16, eye.z);
    wrf(comp + RAW_EYE_F, (float)eye.x), wrf(comp + RAW_EYE_F + 4, (float)eye.y), wrf(comp + RAW_EYE_F + 8, (float)eye.z);
    wrd(comp + RAW_TARGET, target.x), wrd(comp + RAW_TARGET + 8, target.y), wrd(comp + RAW_TARGET + 16, target.z);
    wrf(comp + RAW_TARGET_F, (float)target.x), wrf(comp + RAW_TARGET_F + 4, (float)target.y);
    wrf(comp + RAW_TARGET_F + 8, (float)target.z);
    wrf(comp + RAW_NEAR, near);
    fp.wrote_eye = eye, fp.wrote_target = target;
}

static int same(V3 a, V3 b) { return !memcmp(&a, &b, sizeof a); }

static void leave_now(uint64_t comp) {
    // The game's pose back where it would be, when no camera mode wrote one over ours since
    if (fp.have_game && same(rd3d(comp + RAW_EYE), fp.wrote_eye) && same(rd3d(comp + RAW_TARGET), fp.wrote_target))
        memcpy((void *)(uintptr_t)(comp + RAW_FROM), fp.game, RAW_SIZE);
    model_hide(0);
    fp.state = OFF, fp.t = 0, fp.have_game = 0;
    __atomic_store_n(&engaged, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&state_now, OFF, __ATOMIC_RELAXED);
}

// The view starts back to the game's camera; eye: Link's eyes now (NULL: unknown), logged with the reason
static void leave(Why why, uint32_t mode, const V3 *eye) {
    if (fp.state == OUT) return;
    fp.state = OUT;
    __atomic_store_n(&engaged, 0, __ATOMIC_RELAXED);
    char why_s[96], at[64] = "";
    if (why == WHY_MODE) snprintf(why_s, sizeof why_s, "camera mode %u", mode);
    else snprintf(why_s, sizeof why_s, "%s", why_text[why]);
    if (eye) snprintf(at, sizeof at, ", eye (%.2f, %.2f, %.2f)", eye->x, eye->y, eye->z);
    rt_log("restore: FIRST_PERSON: view off after %.1f s (%s%s)", (double)(plat_now_ns() - fp.since_ns) / 1e9, why_s, at);
}

static void step(uint64_t comp) {
    uint64_t owner = rd64(comp + CAM_OWNER), now = plat_now_ns();
    uint64_t seq = __atomic_load_n(&camera_seq, __ATOMIC_RELAXED);
    uint64_t ran = __atomic_load_n(&switch_seq, __ATOMIC_RELAXED);
    int fresh = ran && ran >= seq;   // the switcher ran since the update before this one
    uint64_t node = __atomic_load_n(&switch_node, __ATOMIC_RELAXED);
    if (fresh && node) node_follow(node);
    if (comp != fp.comp || owner != fp.owner) {
        // Another player camera only once the one in use has stopped updating (a new area made a new one)
        if (!player_camera(comp, owner) || (fp.comp && now - fp.comp_ns < 500000000ull)) return;
        if (fp.state != OFF) model_hide(0);
        fp.state = OFF, fp.t = 0, fp.have_game = 0, fp.last_ns = 0;
        __atomic_store_n(&engaged, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&state_now, OFF, __ATOMIC_RELAXED);
        char name[24];
        actor_name(owner, name, sizeof name);
        rt_log("restore: FIRST_PERSON: the player camera is component %#llx of %#llx (%s, %s)",
               (unsigned long long)comp, (unsigned long long)owner, name[0] ? name : "no name",
               owner == fp.context_actor ? "the actor of the mode switcher" : "by name");
        fp.comp = comp, fp.owner = owner;
    }
    fp.comp_ns = now;
    __atomic_store_n(&camera_seq, seq + 1, __ATOMIC_RELAXED);
    double dt = fp.last_ns ? (double)(now - fp.last_ns) / 1e9 : 0;
    fp.last_ns = now;
    if (dt > 0.1) dt = 0.1;

    int on = __atomic_load_n(&on_now, __ATOMIC_RELAXED), held = __atomic_load_n(&look_held, __ATOMIC_RELAXED);
    if (!held) fp.armed = 1;
    if (fp.state == OFF && !(held && fp.armed)) return;   // nothing to do: the game's pose goes through

    // The game's pose of this update, unless the raw pose still holds the one written last
    V3 g_eye = rd3d(comp + RAW_EYE), g_target = rd3d(comp + RAW_TARGET);
    if (fp.state == OFF || !same(g_eye, fp.wrote_eye) || !same(g_target, fp.wrote_target)) {
        memcpy(fp.game, (const void *)(uintptr_t)(comp + RAW_FROM), RAW_SIZE);
        fp.have_game = 1;
    }
    const uint64_t game = (uint64_t)(uintptr_t)fp.game - RAW_FROM;   // the saved pose at the component's offsets
    g_eye = rd3d(game + RAW_EYE), g_target = rd3d(game + RAW_TARGET);
    float g_near = rdf(game + RAW_NEAR);

    // Where the view may be
    uint32_t mode = __atomic_load_n(&switch_mode, __ATOMIC_RELAXED);
    uint64_t player = player_find();
    Why why = !on ? WHY_OFF : !held ? WHY_RELEASED : !fresh ? WHY_EVENT : mode != 0 && mode != 5 ? WHY_MODE
              : __atomic_load_n(&target_held, __ATOMIC_RELAXED) ? WHY_TARGET : !player ? WHY_NO_LINK
              : !on_ground(player) ? WHY_AIR : WHY_NONE;
    int is_shown = shown(comp);
    if (why == WHY_NONE && !is_shown && (fp.state == OFF || ++fp.missed >= 15)) why = WHY_NOT_SHOWN;
    if (is_shown) fp.missed = 0;
    if (!player) {   // nothing to place the view at: the game's camera at once
        if (fp.state != OFF) {
            leave(WHY_NO_LINK, mode, NULL);
            leave_now(comp);
        }
        return;
    }

    // Link's eyes and facing
    V3 pos = rd3f(player + ACTOR_POS);
    V3 fwd = v3(rdf(player + ACTOR_ROT + 8), 0, rdf(player + ACTOR_ROT + 32));
    double fl = len(fwd);
    fwd = fl > 1e-4 ? scale(fwd, 1 / fl) : v3(0, 0, 1);
    V3 eye = add(add(pos, v3(0, fp.adult ? 1.488 : 0.979, 0)), scale(fwd, fp.adult ? 0.053 : 0.044));

    if (fp.state == OFF || fp.state == OUT) {
        if (held && fp.armed) {
            fp.armed = 0;
            if (why != WHY_NONE) {
                if (why == WHY_MODE) rt_log("restore: FIRST_PERSON: not here (camera mode %u)", mode);
                else rt_log("restore: FIRST_PERSON: not here (%s)", why_text[why]);
                if (fp.state == OFF) return;
            } else {
                if (fp.state == OFF) {
                    fp.yaw = atan2(fwd.x, fwd.z), fp.pitch = 0;
                    V3 gd = sub(g_target, g_eye);
                    rt_log("restore: FIRST_PERSON: view on (%s, camera mode %u): eye (%.2f, %.2f, %.2f), Link "
                           "faces %.0f deg, the game's camera %.0f deg",
                           fp.adult ? "adult" : "child", mode, eye.x, eye.y, eye.z, fp.yaw / DEG,
                           atan2(gd.x, gd.z) / DEG);
                }
                fp.state = IN, fp.missed = 0, fp.since_ns = now;
                __atomic_store_n(&engaged, 1, __ATOMIC_RELAXED);
                __atomic_store_n(&state_now, IN, __ATOMIC_RELAXED);
            }
        }
    } else if (why != WHY_NONE) {
        leave(why, mode, &eye);
    }
    if (fp.state == OFF) return;
    __atomic_store_n(&camera_ns, now, __ATOMIC_RELAXED);

    if (fp.state != OUT) {   // the stick turns the view: right turns right, up looks up
        int64_t st = __atomic_load_n(&look_stick, __ATOMIC_RELAXED);
        double x = (int32_t)(uint32_t)st / 32767.0, y = (int32_t)(st >> 32) / 32767.0;
        fp.yaw = remainder(fp.yaw - x * YAW_RATE * DEG * dt, 2 * 3.14159265358979323846);
        fp.pitch += y * PITCH_RATE * DEG * dt;
        if (fp.pitch > PITCH_UP * DEG) fp.pitch = PITCH_UP * DEG;
        if (fp.pitch < -PITCH_DOWN * DEG) fp.pitch = -PITCH_DOWN * DEG;
        fp.t += dt / IN_S;
        if (fp.t >= 1) fp.t = 1, fp.state = ON;
    } else {
        fp.t -= dt / OUT_S;
        if (fp.t <= 0) {
            leave_now(comp);
            return;
        }
    }

    // Blend between the game's pose and the eyes: the eye along the line, the direction turned between the two
    double s = fp.t * fp.t * (3 - 2 * fp.t);
    V3 fd = v3(cos(fp.pitch) * sin(fp.yaw), sin(fp.pitch), cos(fp.pitch) * cos(fp.yaw));
    V3 gd = sub(g_target, g_eye);
    double dist = len(gd);
    gd = dist > 1e-4 ? scale(gd, 1 / dist) : fd;
    if (!(dist >= 1)) dist = 1;
    if (dist > 20) dist = 20;
    V3 d = add(gd, scale(sub(fd, gd), s));
    double dl = len(d);
    d = dl > 1e-3 ? scale(d, 1 / dl) : s >= 0.5 ? fd : gd;
    V3 e = add(g_eye, scale(sub(eye, g_eye), s));
    pose_write(comp, e, add(e, scale(d, dist)), g_near > NEAR_M ? g_near : NEAR_M);
    // Link's model goes once the view is nearer his head than the game's camera, and comes back as it leaves
    model_hide(s >= 0.5);
}

// Host call at main+0x4c41d4 in place of `ldp x8, x9, [x0, #0x70]`, the apply's first read of the raw pose: every
// guest register is kept but x8 and x9, which get the displaced load (of the pose written here).
static void camera_raw(Cpu *c) {
    uint64_t comp = c->x[0];
    if (__atomic_load_n(&on_now, __ATOMIC_RELAXED) || fp.state != OFF) step(comp);
    c->x[8] = rd64(comp + RAW_EYE);
    c->x[9] = rd64(comp + RAW_EYE + 8);
}

// Host call at main+0x27bfcb0 in place of `ldrb w9, [x19, #0x1e]`, where every path of the switcher's update meets
// after it chose this update's mode (+0x34): x9 gets the displaced load, nothing else changes.
static void switcher_mode(Cpu *c) {
    uint64_t node = c->x[19];
    if (__atomic_load_n(&on_now, __ATOMIC_RELAXED) || __atomic_load_n(&state_now, __ATOMIC_RELAXED) != OFF) {
        __atomic_store_n(&switch_mode, rd32(node + NODE_MODE), __ATOMIC_RELAXED);
        __atomic_store_n(&switch_node, node, __ATOMIC_RELAXED);
        __atomic_store_n(&switch_seq, __atomic_load_n(&camera_seq, __ATOMIC_RELAXED) + 1, __ATOMIC_RELAXED);
    }
    c->x[9] = rd8(node + 0x1e);
}

__attribute__((constructor)) static void first_person_register(void) {
    rt_register_host(HOST_CAMERA_RAW, camera_raw);
    rt_register_host(HOST_SWITCHER_MODE, switcher_mode);
}
#endif
