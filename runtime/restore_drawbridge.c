// Restoration DRAWBRIDGE_NIGHT (game 1.1.1): the castle town drawbridge is raised at night. In the original game the
// drawbridge over the moat in front of the castle town goes up at nightfall while
// Link is a child and comes down at daybreak: night ran from 18:00 to 6:30 of the game's clock. This release keeps it
// down, except in one stretch of the story.
//
// This release already has the raised bridge. Hyrule Field's child placements hold the lowered bridge
// (Land_HrlField_DrawBridge_A_01) and the raised one (_A_02), and the market holds its view of the bridge
// (Obj_HrlField_DrawBridge_A_01); each is there or not by the saved flag IsHyruleFieldBridgeClose, and so is the
// lowered bridge's collision (Phive/VolumeStatsPresenceSettingParam/HyruleField_Bridge_Open). Only the story sets the
// flag (Event/EventFlow/DoorOfTime): on when Link holds the three spiritual stones, off after the scene of Zelda's
// escape (Dm_HrlField_0060), which also sets the clock to 12:00.
//
// The patch set "drawbridge" (recomp/patches-v111.txt) sends the game's once-a-frame update of its flags (main+0x3b500a0
// calls main+0xb99980 with the flag manager, [main+0x4e842a8]) to bridge_site below, which runs that update and then
// decides the flag from the clock: the world manager's ([main+0x4e7eab0]) time manager (+0x578), whose function at
// vtable +0x30 the AI query QueryWorldGetCurrentTime asks for the second of the day. It sets the flag through the game's
// own setter (main+0x682b20, manager, value, hash); the change takes effect at the next frame's update.
//
// Keeping the saved flag honest:
// - From when Link has the Goron Ruby and the Zora Sapphire until the escape scene has played, the flag is the story's
//   and is never touched.
// - Otherwise the flag is wanted on only with the switch on, Link a child, and night. Whatever else holds it on (this
//   restoration at an earlier night, the switch turned off since, sunrise) is put off, whatever the area: a lowered
//   bridge strands no one. So a save made at night with the switch on loads with the bridge down after the switch is
//   turned off, and the set runs with every restoration off.
// - It is put on only while Link is in neither Hyrule Field nor the market, so the raised bridge is there when he next
//   comes to the field at night. In the market at night it stays down: leaving the market puts Link on the bridge.
#include <stdint.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "restore.h"

#if __has_include("patches.h")
#include "patches.h"
#endif

RESTORATION_CODE(drawbridge_night, "DRAWBRIDGE_NIGHT", "Drawbridge up at night",
                 "The drawbridge to the castle town is raised from 18:00 to 6:30 while Link is a child.");

#ifdef RT_PATCH_PAGE

#define MAIN 0x7000000000ull
#define HOST_BRIDGE (RT_PATCH_PAGE + 0xec0)
#define GAMEDATA_UPDATE 0xb99980   // (manager): applies the flag changes the frame asked for
#define SET_BOOL 0x682b20          // (manager, value, hash)
#define WORLD_MANAGER 0x4e7eab0    // pointer variable; the world manager's time manager is at +0x578
#define TIME_MANAGER 0x578
#define TIME_SECONDS 0x30          // the time manager's vtable entry: the game's clock in seconds
// Flags (MurmurHash3 x86 32 of their names) and the manager's containers: Bool at +0x08 (entries 0x18 bytes, the
// current value at +0x16), Int at +0xc8 (0x28 bytes, current at +0x1c); each has its entries at +0x10, an
// open-addressing table of {hash, index} at +0x18 and the table's size at +0x24.
#define BRIDGE_CLOSE 0x673fc08au   // IsHyruleFieldBridgeClose
#define CHILDHOOD 0xcea220d3u      // Sequence_IsChildhood
#define GOT_RUBY 0x8908d6adu       // IsGet.Item_Stone_Fire
#define GOT_SAPPHIRE 0xeec383f5u   // IsGet.Item_Stone_Water
#define ESCAPE_SEEN 0x60992704u    // HavePlayedEvent.Dm_HrlField_0060
#define AREA 0x73736cdeu           // System_CurrentBancIndex: GameBancParam AreaIndex of the area Link is in
#define AREA_FIELD 15              // HyruleField_Child (and _Adult)
#define AREA_MARKET 23             // Market_Child
#define NIGHT_FROM (18 * 3600)
#define NIGHT_UNTIL (6 * 3600 + 30 * 60)

static int on = -1;
static unsigned seen_version = ~0u;

// The current value of the flag hash in the container at mgr + base, or -1 when the manager has no such flag.
static int64_t flag(uint64_t mgr, uint64_t base, uint32_t hash, uint64_t stride, uint64_t value, int bytes) {
    uint64_t c = mgr + base, entries = LD64(c + 0x10), table = LD64(c + 0x18);
    uint32_t size = LD32(c + 0x24);
    if (!entries || !table || !size) return -1;
    for (uint32_t i = hash % size, n = 0; n < size; n++, i = i + 1 == size ? 0 : i + 1) {
        uint32_t h = LD32(table + 8ull * i);
        if (!h) return -1;
        if (h != hash) continue;
        uint64_t e = entries + LD32(table + 8ull * i + 4) * stride + value;
        return bytes == 1 ? (int64_t)LD8(e) : (int64_t)(int32_t)LD32(e);
    }
    return -1;
}
static int64_t get_bool(uint64_t mgr, uint32_t hash) { return flag(mgr, 0x08, hash, 0x18, 0x16, 1); }
static int64_t get_int(uint64_t mgr, uint32_t hash) { return flag(mgr, 0xc8, hash, 0x28, 0x1c, 4); }

// The second of the day, or -1 before the world manager is up.
static int day_second(Cpu *c) {
    uint64_t wm = LD64(MAIN + WORLD_MANAGER), tm = wm ? LD64(wm + TIME_MANAGER) : 0;
    if (!tm) return -1;
    int64_t s = (int32_t)rt_call(c, LD64(LD64(tm) + TIME_SECONDS), 1, tm) % 86400;
    return (int)(s < 0 ? s + 86400 : s);
}

static void bridge_site(Cpu *c) {
    uint64_t mgr = c->x[0];
    rt_call(c, MAIN + GAMEDATA_UPDATE, 1, mgr);
    unsigned v = cfg_version();
    if (v != seen_version) seen_version = v, on = restore_key_on("DRAWBRIDGE_NIGHT");
    if (!mgr) return;
    int64_t closed = get_bool(mgr, BRIDGE_CLOSE), area = get_int(mgr, AREA);
    if (closed < 0 || area < 0) return;   // no save loaded yet
    if (get_bool(mgr, GOT_RUBY) == 1 && get_bool(mgr, GOT_SAPPHIRE) == 1 && get_bool(mgr, ESCAPE_SEEN) == 0) return;
    int want = 0, sec = -1;
    if (on && get_bool(mgr, CHILDHOOD) == 1) {
        if ((sec = day_second(c)) < 0) return;
        int night = sec >= NIGHT_FROM || sec < NIGHT_UNTIL;
        if (night && (area == AREA_FIELD || area == AREA_MARKET)) want = area == AREA_FIELD ? (int)closed : 0;
        else want = night;
    }
    if (want == closed) return;
    rt_call(c, MAIN + SET_BOOL, 3, mgr, (uint64_t)want, (uint64_t)BRIDGE_CLOSE);
    if (sec < 0) sec = day_second(c);
    rt_log("drawbridge: %s the castle town drawbridge at %02d:%02d (area %lld, %s)", want ? "raised" : "lowered",
           sec < 0 ? 0 : sec / 3600, sec < 0 ? 0 : sec / 60 % 60, (long long)area,
           !on ? "the switch is off" : want ? "night" : "day, an adult or the market");
}

__attribute__((constructor)) static void drawbridge_register(void) {
    if (rt_patch_set("drawbridge") >= 0) rt_register_host(HOST_BRIDGE, bridge_site);
}

#endif
