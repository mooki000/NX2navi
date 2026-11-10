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
static char logs[200][320];
static int nlogs;

const char *cfg_get(const char *key) {
    if (!strcmp(key, "NX2NAVI_AUTOPILOT")) return script_path;
    if (!strcmp(key, "NX2NAVI_SHOT_DIR")) return shot_dir;
    return NULL;
}
void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (nlogs < 200) vsnprintf(logs[nlogs++], sizeof logs[0], fmt, ap);
    va_end(ap);
}
uint64_t frame_step_ticks(void) { return tick; }
uint64_t plat_now_ns(void) { return now_ns; }
uint64_t kern_now_ns(void) { return now_ns; }
int input_script_button(const char *name) { return !strcmp(name, "A") ? 0 : !strcmp(name, "B") ? 1 : !strcmp(name, "ZL") ? 8 : -1; }

// The game's memory: the two slots of main the autopilot reads, every other address a host pointer.
static struct { uint64_t pad[0x90]; uint64_t player; } mgr;
static struct { uint8_t b[0x400]; } actor, module;
static struct { uint64_t pad[3]; uint64_t views; } views_mgr;
static uint8_t view[0x7400];
static uint64_t scene_slot[0x200];
static uint64_t mgr_ptr, module_ptr;
int plat_mem_read(uint64_t addr, void *out, size_t n) {
    if (addr == MOD_MAIN_BASE + 0x4e371d0) { memcpy(out, &mgr_ptr, n); return 1; }
    if (addr == MOD_MAIN_BASE + 0x4e3e258) { memcpy(out, &module_ptr, n); return 1; }
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
// the wall, the camera pans with the right stick at 120 degrees a second.
static uint64_t held, last_b;
static void frame(void) {
    uint64_t b = 0;
    int32_t sl[2] = {0, 0}, sr[2] = {0, 0};
    autopilot_input(&b, sl, sr);
    held |= b, last_b = b;
    float fx = sinf(cam_yaw * 3.14159265f / 180), fz = cosf(cam_yaw * 3.14159265f / 180);
    float sx = sl[0] / 32767.0f, sy = sl[1] / 32767.0f;
    link_pos[0] += (sy * fx + sx * -fz) * 6 / 60, link_pos[2] += (sy * fz + sx * fx) * 6 / 60;
    if (link_pos[0] > wall_x) link_pos[0] = wall_x;
    cam_yaw -= cam_sense * sr[0] / 32767.0f * 120 / 60;
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
    const char *bad[] = {"walk 1", "walk 1 2 radius", "walk 1 2 speed 3", "look 1 2 radius 3", "press Q", "press A+",
                         "press A -1", "camera 2 0 1", "stick 0 0 0", "shot bad/name", "shot", "log", "jump 1 2",
                         "wait -1", "wait x", "run 1 2x", "hold", "hold A 2", "release A B"};
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

int main(int argc, char **argv) {
    assert(argc == 3);
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


if __name__ == '__main__':
    main()
