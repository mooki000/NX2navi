"""Check the test autopilot (runtime/autopilot.c) without the game: its script parser, the stick it steers with for each
camera direction, and whole routes against a simulated Link and camera (reached, stuck, timeout, the camera's sense
measured and turned round, button presses and their release, a named picture handed to the present, the route's
frame figures).

Usage: python tests/autopilot_check.py --cc <clang>
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include "autopilot.c"

const int rt_game_version = 111;
uint64_t rt_guest_start_ns;
static uint64_t tick, now_ns = 1000000000ull;
static const char *script_path, *shot_dir;
static char logs[600][320];
static int nlogs;

const char *cfg_get(const char *key) {
    if (!strcmp(key, "NX2NAVI_AUTOPILOT")) return script_path;
    if (!strcmp(key, "NX2NAVI_SHOT_DIR")) return shot_dir;
    return NULL;
}
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (nlogs < 600) vsnprintf(logs[nlogs++], sizeof logs[0], fmt, ap);
    va_end(ap);
}
uint64_t frame_step_ticks(void) { return tick; }
int64_t frame_step_world_us(void) { return (int64_t)tick * 1000000 / 60; }
uint64_t plat_now_ns(void) { return now_ns; }
uint64_t kern_now_ns(void) { return now_ns; }
int input_script_button(const char *name) { return !strcmp(name, "A") ? 0 : !strcmp(name, "B") ? 1 : !strcmp(name, "ZL") ? 8 : -1; }

// The game's memory: the slots of main the autopilot reads, every other address a host pointer.
static struct { uint64_t pad[0x90]; uint64_t player; } mgr;
static struct { uint8_t b[0x400]; } actor, module;
static struct { uint64_t pad[3]; uint64_t views; } views_mgr;
static uint8_t view[0x7400];
static uint64_t scene_slot[0x200];
static uint64_t mgr_ptr, module_ptr, flags_ptr, system_slot, system_got;

// The flag manager (Bool, Int, Enum, String64 and UInt containers, 16 slots each) and its flags by name
static uint8_t fmgr[0xd80], fent[5][16 * 0x108];
static uint32_t ftab[5][32];
static const uint32_t fbase[5] = {0x008, 0x0c8, 0x248, 0x608, 0x788}, fstride[5] = {0x18, 0x28, 0x38, 0x108, 0x28};
static int fcount[5];
static uint8_t *add_flag(int k, const char *name) {
    uint32_t h = ap_murmur(name), i = h % 16;
    while (ftab[k][2 * i]) i = (i + 1) % 16;
    ftab[k][2 * i] = h, ftab[k][2 * i + 1] = (uint32_t)fcount[k];
    uint8_t *c = fmgr + fbase[k];
    memcpy(c + 0x10, &(uint64_t){(uintptr_t)fent[k]}, 8), memcpy(c + 0x18, &(uint64_t){(uintptr_t)ftab[k]}, 8);
    memcpy(c + 0x24, &(uint32_t){16}, 4);
    return fent[k] + fstride[k] * (uint32_t)fcount[k]++;
}
static uint8_t *talked;
static int32_t *life;
static char area[64] = "KokiriForest_Child";

// The AutoSaveMgr: component 12; its check passes while no cooldown runs, a request sets bit 0 of byte 5 of the
// object at +0x130, and the manager's update saves half a second later and restarts its cooldown (+0x15c, 1800)
static uint8_t autosave[0x200], save_req[8];
static int saves, save_wait;
// The world manager's time manager: its function at vtable +0x30 (a made-up address) answers the second of the day,
// 6:00 at the start and a game second each 4 ticks, as a negative number (the autopilot takes it modulo a day)
static uint64_t world_ptr, vtable[8] = {[6] = 0x7000abc000ull}, time_mgr[4] = {0};
static uint8_t world[0x600];
uint64_t rt_call(Cpu *c, uint64_t fn, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint64_t a0 = va_arg(ap, uint64_t), a1 = n > 1 ? va_arg(ap, uint64_t) : 0;
    va_end(ap);
    float cool;
    memcpy(&cool, autosave + 0x15c, 4);
    (void)c;
    if (fn == MOD_MAIN_BASE + 0xba7880) return a0 && a1 == 12 ? (uintptr_t)autosave : 0;
    if (fn == MOD_MAIN_BASE + 0xba72d0) return a0 == (uintptr_t)autosave && a1 == 6 && cool <= 0;
    if (fn == MOD_MAIN_BASE + 0xba7730) save_req[5] |= 1;
    if (fn == 0x7000abc000ull) return a0 == (uintptr_t)time_mgr ? (uint64_t)(-86400 + 6 * 3600 + (int64_t)tick / 4) : 0;
    return 0;
}
static void autosave_update(void) {
    float cool;
    memcpy(&cool, autosave + 0x15c, 4);
    if (cool > 0) cool -= 1;
    if ((save_req[5] & 1) && ++save_wait == 30) save_req[5] &= (uint8_t)~1, save_wait = 0, saves++, cool = 1800;
    memcpy(autosave + 0x15c, &cool, 4);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    if (addr == MOD_MAIN_BASE + 0x4e371d0) { memcpy(out, &mgr_ptr, n); return 1; }
    if (addr == MOD_MAIN_BASE + 0x4e3e258) { memcpy(out, &module_ptr, n); return 1; }
    if (addr == MOD_MAIN_BASE + 0x4e842a8) { memcpy(out, &flags_ptr, n); return 1; }
    if (addr == MOD_MAIN_BASE + 0x4c7e178) { memcpy(out, &system_got, n); return 1; }
    if (addr == MOD_MAIN_BASE + 0x4e7eab0) { memcpy(out, &world_ptr, n); return 1; }
    if (addr >= MOD_MAIN_BASE && addr < MOD_MAIN_BASE + 0x5000000) return 0;
    memcpy(out, (const void *)(uintptr_t)addr, n);
    return 1;
}

static float link_pos[3] = {3, 0, -2}, cam_yaw = 30, wall_x = 1e9f;
static int cam_sense = 1;   // the simulated game's sense of the right stick
static void put_world(void) {
    memcpy(actor.b + 0x31c, link_pos, 12);
    float fx = sinf(cam_yaw * 3.14159265f / 180), fz = cosf(cam_yaw * 3.14159265f / 180);
    float eye[3] = {link_pos[0] - 4 * fx, link_pos[1] + 2, link_pos[2] - 4 * fz};
    float at[3] = {link_pos[0], link_pos[1] + 1.36f, link_pos[2]};
    memcpy(view + 0x733c, eye, 12), memcpy(view + 0x7348, at, 12);
}
// One 60 Hz frame: the game reads the input, Link moves along the camera-relative stick at up to 6 m/s and stops at
// the wall, the camera pans with the right stick at 120 degrees a second. In the state route (state_mode) also: a
// ladder at x 30, z 0 that a straight up or down stick climbs at 2 m/s between heights 0 and 5, the area that changes
// to LostWood_Child a second after Link passed x 20, a talk that sets its flag a third of a second after A, and the
// AutoSaveMgr's update.
static uint64_t held, last_b;
static int state_mode, area_timer, talk_timer;
static Cpu cpu;
static void frame(void) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    autopilot_input(&b, sl, sr);
    autopilot_guest(&cpu);
    held |= b, last_b = b;
    float fx = sinf(cam_yaw * 3.14159265f / 180), fz = cosf(cam_yaw * 3.14159265f / 180);
    float sx = sl[0] / 32767.0f, sy = sl[1] / 32767.0f;
    if (state_mode && fabsf(link_pos[0] - 30) < 1 && fabsf(link_pos[2]) < 1 && sl[0] == 0 && sl[1]) {
        link_pos[1] += sy * 2 / 60;
        link_pos[1] = link_pos[1] < 0 ? 0 : link_pos[1] > 5 ? 5 : link_pos[1];
    } else {
        link_pos[0] += (sy * fx + sx * -fz) * 6 / 60, link_pos[2] += (sy * fz + sx * fx) * 6 / 60;
    }
    if (link_pos[0] > wall_x) link_pos[0] = wall_x;
    cam_yaw -= cam_sense * sr[0] / 32767.0f * 120 / 60;
    if (state_mode) {
        if (link_pos[0] > 20 && !area_timer && !strcmp(area, "KokiriForest_Child")) area_timer = 60;
        if (area_timer && !--area_timer) strcpy(area, "LostWood_Child");
        if ((b & 1) && !talk_timer) talk_timer = 20;
        if (talk_timer && !--talk_timer) *talked = 1;
        autosave_update();
    }
    put_world();
    const char *shot = autopilot_present(1);
    if (shot) assert(strstr(shot, "/shots/ap-") && strstr(shot, "-first-stop.png"));
    tick++, now_ns += 16666667;
}
static const char *find(const char *text) {
    for (int i = 0; i < nlogs; i++)
        if (strstr(logs[i], text)) return logs[i];
    return NULL;
}
// the first log line with text also holds part
static int has(const char *text, const char *part) {
    const char *line = find(text);
    return line && strstr(line, part);
}
static void dump(void) { for (int i = 0; i < nlogs; i++) fprintf(stderr, "%s\n", logs[i]); }
#define CHECK(c) do { if (!(c)) { dump(); fprintf(stderr, "failed: %s (line %d)\n", #c, __LINE__); abort(); } } while (0)

static void parse_checks(void) {
    ApStep s;
    const char *why = NULL;
    CHECK(ap_parse("  # only a comment\n", &s, &why) == 0);
    CHECK(ap_parse("walk 1.5 -2 radius 0.5 timeout 3  # to the house\n", &s, &why) == 1);
    CHECK(s.kind == AP_WALK && s.x == 1.5f && s.z == -2 && s.radius == 0.5f && s.timeout == 3);
    CHECK(!strcmp(s.src, "walk 1.5 -2 radius 0.5 timeout 3"));
    CHECK(ap_parse("run 4 5", &s, &why) == 1 && s.kind == AP_RUN && s.radius == 1 && s.timeout == 30);
    CHECK(ap_parse("look 4 5 timeout 2", &s, &why) == 1 && s.kind == AP_LOOK && s.timeout == 2);
    CHECK(ap_parse("press A+ZL 0.5", &s, &why) == 1 && s.buttons == ((1ull << 0) | (1ull << 8)) && s.secs == 0.5f);
    CHECK(ap_parse("press B", &s, &why) == 1 && s.buttons == 2 && s.secs == 0.1f);
    CHECK(ap_parse("camera -1 0.5 2", &s, &why) == 1 && s.kind == AP_CAMERA && s.sx == -1 && s.sy == 0.5f && s.secs == 2);
    CHECK(ap_parse("log the water,  seen from the ledge  \n", &s, &why) == 1 && !strcmp(s.arg, "the water,  seen from the ledge"));
    CHECK(ap_parse("shot ledge-2_a", &s, &why) == 1 && !strcmp(s.arg, "ledge-2_a"));
    CHECK(ap_parse("wait 0", &s, &why) == 1 && s.secs == 0);
    CHECK(ap_parse("hold ZL+A", &s, &why) == 1 && s.kind == AP_HOLD && s.buttons == ((1ull << 8) | 1));
    CHECK(ap_parse("release", &s, &why) == 1 && s.kind == AP_RELEASE && !s.buttons);
    CHECK(ap_parse("release ZL", &s, &why) == 1 && s.buttons == (1ull << 8));
    CHECK(ap_parse("until flag HavePlayedEvent.Obj_Kokiri_MessageBoard_A_02=1 timeout 20", &s, &why) == 1 &&
          s.kind == AP_UNTIL && !strcmp(s.arg, "HavePlayedEvent.Obj_Kokiri_MessageBoard_A_02") && s.op == AP_EQ &&
          s.is_num && s.num == 1 && s.timeout == 20 && s.hash == ap_murmur("HavePlayedEvent.Obj_Kokiri_MessageBoard_A_02"));
    CHECK(ap_parse("until area Market_Child", &s, &why) == 1 && s.hash == 0x1d6189dau && !strcmp(s.val, "Market_Child") &&
          !s.is_num && s.timeout == 30);
    CHECK(ap_parse("expect flag PlayerStatus.Life>=8", &s, &why) == 1 && s.kind == AP_EXPECT && s.op == AP_GE &&
          s.num == 8 && s.hash == 0x31ab5580u);
    CHECK(ap_parse("expect flag IsHyruleFieldBridgeClose!=true", &s, &why) == 1 && s.op == AP_NE && s.num == 1 &&
          s.hash == 0x673fc08au);
    CHECK(ap_parse("expect flag Step_DoorOfTime=Ready", &s, &why) == 1 && !s.is_num && !strcmp(s.val, "Ready"));
    CHECK(ap_parse("expect flag PlayerStatus.Life<4", &s, &why) == 1 && s.op == AP_LT);
    CHECK(ap_parse("climb down timeout 9", &s, &why) == 1 && s.kind == AP_CLIMB && s.sy == -1 && s.timeout == 9);
    CHECK(ap_parse("climb up", &s, &why) == 1 && s.sy == 1 && s.timeout == 20);
    CHECK(ap_parse("trace 0", &s, &why) == 1 && s.kind == AP_TRACE && s.secs == 0);
    CHECK(ap_parse("save", &s, &why) == 1 && s.kind == AP_SAVE && s.timeout == 60);
    CHECK(ap_parse("save timeout 90", &s, &why) == 1 && s.timeout == 90);
    CHECK(ap_parse("expect flag 0x9824206c=Open", &s, &why) == 1 && s.hash == 0x9824206cu && !strcmp(s.val, "Open"));
    const char *bad[] = {"walk 1", "walk 1 2 radius", "walk 1 2 speed 3", "look 1 2 radius 3", "press Q", "press A+",
                         "press A -1", "camera 2 0 1", "stick 0 0 0", "shot bad/name", "shot", "log", "jump 1 2",
                         "wait -1", "wait x", "run 1 2x", "hold", "hold A 2", "release A B", "until", "until area",
                         "until flag Life", "until flag =1", "until flag Life=", "until flag Li-fe=1", "until flag Life!1",
                         "until time 12", "until area X timeout", "until area X timeout -1", "until area X radius 2",
                         "expect flag A=1 timeout 3", "climb", "climb sideways", "climb up timeout", "climb up 3",
                         "trace", "trace -1", "trace 1 2", "save 3", "save timeout x", "save timeout 0"};
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        why = NULL;
        if (ap_parse(bad[i], &s, &why) != -1 || !why) { fprintf(stderr, "accepted: %s\n", bad[i]); abort(); }
    }
}

static void steer_checks(void) {
    int32_t st[2];
    // the camera along +z: forward is stick up, +x is to the left (y up, right-handed), -x to the right
    ap_steer(0, 1, 0, 5, 1, st); CHECK(st[0] == 0 && st[1] == 32767);
    ap_steer(0, 1, 3, 0, 1, st); CHECK(st[0] == -32767 && st[1] == 0);
    ap_steer(0, 1, -3, 0, 0.5f, st); CHECK(st[0] == 16384 && st[1] == 0);
    ap_steer(0, 1, 0, -2, 1, st); CHECK(st[0] == 0 && st[1] == -32767);
    // the camera along +x: +z is to the right
    ap_steer(1, 0, 0, 4, 1, st); CHECK(st[0] == 32767 && st[1] == 0);
    // any camera: the stick's world direction is the wanted one
    for (int c = 0; c < 360; c += 15)
        for (int d = 0; d < 360; d += 20) {
            float fx = sinf(c * 3.14159265f / 180), fz = cosf(c * 3.14159265f / 180);
            float dx = sinf(d * 3.14159265f / 180), dz = cosf(d * 3.14159265f / 180);
            ap_steer(fx, fz, 7 * dx, 7 * dz, 1, st);
            float sx = st[0] / 32767.0f, sy = st[1] / 32767.0f;
            float wx = sy * fx + sx * -fz, wz = sy * fz + sx * fx;
            CHECK(fabsf(wx - dx) < 1e-3f && fabsf(wz - dz) < 1e-3f);
        }
    ap_steer(0, 1, 0, 0, 1, st); CHECK(st[0] == 0 && st[1] == 0);
}

// The route of the game's state: conditions on flags and the area, a talk's flag, a ladder, the game's autosave and the
// trace, against the simulated flag manager and AutoSaveMgr.
static int state_route(void) {
    state_mode = 1;
    flags_ptr = (uintptr_t)fmgr;
    system_got = (uintptr_t)&system_slot, system_slot = 0x1234;
    world_ptr = (uintptr_t)world, time_mgr[0] = (uintptr_t)vtable;
    memcpy(world + 0x578, &(uint64_t){(uintptr_t)time_mgr}, 8);
    memcpy(autosave + 0x130, &(uint64_t){(uintptr_t)save_req}, 8);
    life = (int32_t *)(add_flag(1, "PlayerStatus.Life") + 0x1c), *life = 12;
    memcpy(add_flag(1, "PlayerStatus.MaxLife") + 0x1c, &(int32_t){12}, 4);
    talked = add_flag(0, "HaveTalked.Npc_Test") + 0x16;
    memcpy(add_flag(2, "Step_Meet_DekuTree") + 0x1c, &(uint32_t){ap_murmur("Step01")}, 4);
    memcpy(add_flag(3, "Sequence_CurrentBanc") + 0x68, &(const char *){area}, 8);
    memcpy(add_flag(4, "System_Seconds") + 0x1c, &(uint32_t){4000000000u}, 4);
    mgr_ptr = (uintptr_t)&mgr;
    put_world();
    for (int i = 0, cool_set = 0; i < 60 * 200 && ap_state != 3; i++) {
        // the game's cooldown runs from the stuck climb on (4 s): the first save is refused, the second waits for it
        if (ap_cur == 17 && !cool_set) memcpy(autosave + 0x15c, &(float){240}, 4), cool_set = 1;
        frame();
    }
    CHECK(ap_state == 3);
    CHECK(has("(line 3, expect flag PlayerStatus.Life=12): met after 0.0 s;", "; PlayerStatus.Life is 12"));
    CHECK(has("(line 4, expect flag PlayerStatus.Life>12): unmet", "; PlayerStatus.Life is 12"));
    CHECK(has("(line 5, expect area KokiriForest_Child): met", "; Sequence_CurrentBanc is \"KokiriForest_Child\""));
    CHECK(has("(line 6, expect flag Step_Meet_DekuTree=Step01): met", "; Step_Meet_DekuTree is Step01"));
    CHECK(has("(line 7, expect flag Step_Meet_DekuTree=Complete): unmet", ", not Complete"));
    CHECK(has("(line 8, expect flag System_Seconds>=4000000000): met", "System_Seconds is 4000000000"));
    CHECK(has("(line 9, expect flag NoSuchFlag=1): unmet", "; NoSuchFlag cannot be read"));
    CHECK(has("(line 11, until area LostWood_Child timeout 5): reached after", "is \"LostWood_Child\""));
    CHECK(has("(line 12, until flag PlayerStatus.Life<=4 timeout 1): timeout after 1.0 s", "Life is 12"));
    CHECK(has("(line 14, until flag HaveTalked.Npc_Test=1 timeout 3): reached after", "Npc_Test is 1"));
    CHECK(has("(line 16, climb up): reached after 3.5 s", ", +5.0 m of height"));
    CHECK(has("(line 17, climb down): reached after 3.5 s", ", -5.0 m of height"));
    CHECK(has("(line 19, climb up timeout 4): stuck after 1.5 s", ", +0.0 m of height"));
    CHECK(has("(line 21, save timeout 1): refused after 1.0 s", "; the game's check refused it"));
    CHECK(has("(line 22, save timeout 10): done after 2.0 s", "; the game took the request") && saves == 1);
    CHECK(has("(line 23, save timeout 2): refused after 2.0 s", "refused it") && saves == 1);
    CHECK(has("autopilot: done: 24 steps, 7 reached, 1 timed out, 1 stuck, 0 pictures missed, 3 expectations unmet", "Link at"));
    const char *t = find("autopilot: trace 0.0 s (step ");
    CHECK(t && strstr(t, "; area KokiriForest_Child; life 12 of 12; time "));
    CHECK(find("; area LostWood_Child; life 12 of 12; time 06:0"));
    CHECK(find("; area KokiriForest_Child; life 12 of 12; time 06:00"));
    int traces = 0;
    for (int i = 0; i < nlogs; i++) traces += !strncmp(logs[i], "autopilot: trace ", 17);
    CHECK(traces >= 15 && traces <= 40);   // one a second from the start to the trace 0 step
    puts("autopilot: conditions on flags and the area (until, expect), climb, the game's autosave and the trace passed");
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    script_path = argv[1], shot_dir = argv[2];
    parse_checks();
    steer_checks();
    module_ptr = (uintptr_t)&module;
    memcpy(module.b + 0x1e8, &(uint64_t){(uintptr_t)scene_slot}, 8);
    scene_slot[0xb28 / 8] = (uintptr_t)&views_mgr;
    views_mgr.views = (uintptr_t)&view;
    memcpy(actor.b + 0x20, &(const char *){"Player"}, 8);
    mgr.player = (uintptr_t)&actor;
    cam_sense = -1;   // a game with the camera's horizontal axis the other way round: the first look measures it
    if (argc == 4) return state_route();
    // the title screen: no Player actor yet, nothing happens
    for (int i = 0; i < 30; i++) frame();
    CHECK(find("autopilot: script ") && !find("gameplay at"));
    mgr_ptr = (uintptr_t)&mgr;
    put_world();
    for (int i = 0; i < 60 * 120 && ap_state != 3; i++) {
        // the wall of the stuck step stands from the moment its run begins
        if (ap_cur == 11) wall_x = 5;
        frame();
        if (ap_cur == 9) CHECK(last_b & (1ull << 8));
        if (ap_cur == 11) CHECK(!(last_b & (1ull << 8)));
    }
    frame();   // the present after the end logs the frame figures
    CHECK(ap_state == 3);
    CHECK(find("gameplay at tick 30") && strstr(find("gameplay at"), "; camera yaw 30"));
    CHECK(strstr(find("step 1/13 (line 2, wait 0.5)"), ": done after 0.5 s; Link at 3.0 0.0 -2.0"));
    CHECK(strstr(find("step 2/13 (line 3, run 10 -4 radius 1)"), ": reached after"));
    CHECK(strstr(find("step 3/13 (line 4, walk 2 2)"), ": reached after"));
    CHECK(find("the right stick turns it the other way from now on"));
    CHECK(strstr(find("step 4/13 (line 5, look 0 50)"), ": reached after"));
    CHECK(strstr(find("step 5/13 (line 6, camera 1 0 0.5)"), "the camera turned 60 degrees"));
    CHECK(strstr(find("step 6/13 (line 7, press A+B 0.2)"), "done after 0.3 s"));
    CHECK(held == ((1ull << 8) | 3));
    CHECK(strstr(find("step 7/13 (line 8, shot first-stop)"), ": taken after") && strstr(find("step 7/13"), "/ap-07-first-stop.png"));
    CHECK(find("autopilot: note: hello there"));
    CHECK(strstr(find("step 9/13 (line 10, hold ZL)"), ": done after 0.0 s"));
    CHECK(strstr(find("step 10/13 (line 11, stick 0 1 0.5)"), "3.0 m from where the step began"));
    CHECK(strstr(find("step 11/13 (line 12, release)"), ": done after 0.0 s"));
    CHECK(strstr(find("step 12/13 (line 13, run 50 0 timeout 20)"), ": stuck after"));
    CHECK(strstr(find("step 13/13 (line 14, run 0 -500 timeout 2)"), ": timeout after 2.0 s"));
    CHECK(strstr(find("autopilot: done: 13 steps, 3 reached, 1 timed out, 1 stuck, 0 pictures missed"), "Link at"));
    CHECK(find("autopilot:   step 2 at 1.0 s: Link at"));
    const char *fr = find("autopilot: route frames: ");
    CHECK(fr && strstr(fr, "mean 60.0 fps, 1% low 59.7 fps, over 25 ms 0, 50 ms 0, 100 ms 0") &&
          strstr(fr, "speed 1.000; 1 picture frames left out"));
    // a broken script is refused whole
    nlogs = 0;
    ap_state = 0, ap_n = 0;
    FILE *f = fopen(script_path, "w");
    fputs("wait 1\nwalk 1\n", f);
    fclose(f);
    uint64_t b = 0;
    int32_t sl[2] = {0}, sr[2] = {0};
    autopilot_input(&b, sl, sr);
    CHECK(ap_state == -1 && find("line 2: walk|run <x> <z> [radius <m>] [timeout <s>] (\"walk 1\"); the script is refused"));
    puts("autopilot: parser, steering for every camera direction, reached/stuck/timeout, camera sense, presses, "
         "named picture and route frame figures passed");
    return 0;
}
'''

SCRIPT = '''# a route through every kind of step
wait 0.5
run 10 -4 radius 1
walk 2 2
look 0 50
camera 1 0 0.5
press A+B 0.2
shot first-stop
log hello there
hold ZL
stick 0 1 0.5
release
run 50 0 timeout 20
run 0 -500 timeout 2
'''

STATE = '''# the game's state: the flags, the area, a talk, a ladder, the game's autosave, the trace
trace 1
expect flag PlayerStatus.Life=12
expect flag PlayerStatus.Life>12
expect area KokiriForest_Child
expect flag Step_Meet_DekuTree=Step01
expect flag Step_Meet_DekuTree=Complete
expect flag System_Seconds>=4000000000
expect flag NoSuchFlag=1
run 25 0 timeout 10
until area LostWood_Child timeout 5
until flag PlayerStatus.Life<=4 timeout 1
press A
until flag HaveTalked.Npc_Test=1 timeout 3
walk 30 0 radius 0.5 timeout 5
climb up
climb down
run 30 8 timeout 5
climb up timeout 4
log saves
save timeout 1
save timeout 10
save timeout 2
trace 0
wait 2
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', required=True)
    args = ap.parse_args()
    with tempfile.TemporaryDirectory(prefix='nx2navi-autopilot-') as tmp:
        folder = Path(tmp)
        (folder / 'layout.h').write_text('#define MOD_MAIN_BASE 0x7000000000ull\n')
        source = folder / 'check.c'
        source.write_text(HARNESS)
        script = folder / 'route.txt'
        script.write_text(SCRIPT)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        link = [] if os.name == 'nt' else ['-lm']
        subprocess.run([args.cc, '-O2', '-Wall', '-Wextra', '-I', str(folder), '-I', str(ROOT / 'runtime'),
                        str(source), *link, '-o', str(exe)], check=True)
        subprocess.run([str(exe), str(script), str(folder / 'shots')], check=True)
        state = folder / 'state.txt'
        state.write_text(STATE)
        subprocess.run([str(exe), str(state), str(folder / 'shots'), 'state'], check=True)


if __name__ == '__main__':
    main()
