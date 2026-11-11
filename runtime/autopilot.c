// Test autopilot (NX2NAVI_AUTOPILOT=<file>, test runs). Once the save has loaded it plays a script of steps on the
// game's input: walk or run Link to a point, turn the camera, hold buttons, wait, take a named picture, write a log
// line. Movement closes the loop over Link's position and the camera's direction, read from the game at every poll, so
// a route reaches the same places at any frame rate and load length, where an open-loop input script drifts with the
// frame rate and with every bump. Every step logs its result with Link's position. From the first step to the end of
// the last one the presented frames are timed (autopilot_present), and the route ends with one line of frame figures.
//
// The script, one step per line, '#' starts a comment. Times are seconds of game time (frame_step_ticks), distances
// metres, x and z the ground coordinates of the world (y up) as the placement files and the saves give them.
//   wait <s>
//   walk <x> <z> [radius <m>] [timeout <s>]   the left stick at half tilt towards the point: "reached" once Link is
//                                              within the radius (default 1 m), "timeout" after the timeout (default
//                                              30 s), "stuck" when he got no 0.25 m nearer in 3 s
//   run <x> <z> [radius <m>] [timeout <s>]    the same at full tilt
//   look <x> <z> [timeout <s>]                the right stick turns the camera until it faces the point within 10
//                                              degrees (default timeout 8 s)
//   camera <x> <y> <s>                        the right stick held at x, y (-1..1, up +y) for s seconds
//   stick <x> <y> <s>                         the left stick held at x, y for s seconds (open loop: into water, off a
//                                              ledge, through a door)
//   press <button>[+<button>...] [<s>]        the buttons held for s seconds (default 0.1), then released for 0.1 s so
//                                              two presses stay two; the names of input scripts (A B X Y L R ZL ZR PLUS
//                                              MINUS LEFT UP RIGHT DOWN LCLICK RCLICK)
//   hold <button>[+<button>...]               the buttons held from here on, through the steps that follow (aim the
//                                              bow and take a picture, run with Z held)
//   release [<button>[+<button>...]]          those held buttons let go, all of them without a name
//   shot <name>                               the next presented frame as <NX2NAVI_SHOT_DIR>/ap-<step>-<name>.png
//   log <text>                                the text in the log
// A script with an error is refused whole, with its line, so a broken route never half runs.
//
// The left stick moves Link relative to the camera: up is the camera's forward direction on the ground and right its
// right, so the stick for a ground direction d is (d . right, d . forward) with right = (-forward.z, forward.x) (y up,
// right-handed). Turning the camera, the right stick's sense is measured on the first look step and kept: a camera
// that turned away from the point after 0.4 s turns the other way from then on.
//
// Game 1.1.1's reads: the Player actor at [[main+0x4e371d0] + 0x480] (name pointer +0x20 reads "Player", position
// +0x31c), empty until the first save has loaded; the camera's eye and target in the renderer's first view
// ([[[main+0x4e3e258] + 0x1e8] + 0xb28] + 0x18, eye +0x733c, target +0x7348). The unit vector the graphics module keeps
// at [main+0x4e3e258] + 0x2b4 is not the camera's direction: in play it pointed 90 to 185 degrees away from the view's,
// and was not of unit length in some areas. Another game version leaves the autopilot off.
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "autopilot.h"
#include "cfg.h"
#include "host.h"
#include "input.h"
#include "layout.h"
#include "platform/platform.h"

enum { AP_WAIT, AP_WALK, AP_RUN, AP_LOOK, AP_CAMERA, AP_STICK, AP_PRESS, AP_HOLD, AP_RELEASE, AP_SHOT, AP_LOG, AP_KINDS };
static const char *const ap_verbs[AP_KINDS] = {"wait", "walk", "run", "look", "camera", "stick", "press", "hold",
                                                "release", "shot", "log"};

#define AP_MAX_STEPS 256
#define AP_FULL 32767.0f
#define AP_TICKS(s) ((uint64_t)((s) * 60.0 + 0.5))   // frame_step_ticks counts 60 Hz ticks of game time
#define AP_STUCK_TICKS 180                            // 3 s without getting AP_STUCK_GAIN nearer
#define AP_STUCK_GAIN 0.25f
#define AP_RELEASE_TICKS 6                            // 0.1 s with the buttons up after a press
#define AP_LOOK_DEG 10.0f
#define AP_LOOK_CHECK_TICKS 24                        // the camera's sense is judged 0.4 s into the first look
#define AP_SHOT_TICKS 120                             // a picture not taken within 2 s is missed
#define AP_NOTE_TICKS 60                              // a progress line per second of a moving step
#define AP_BINS 800                                   // frame times in 0.25 ms bins up to 200 ms

typedef struct {
    int kind, line;
    float x, z, radius, timeout, secs, sx, sy;   // the point; radius m; timeout and duration s; stick -1..1
    uint64_t buttons;
    char arg[96];    // shot name or log text
    char src[80];    // the line as written, for the result
} ApStep;

static ApStep *ap_steps;
static int ap_n;
// Polling side (input.c's lock): 0 not started, 1 waiting for the save to load, 2 running, 3 done, -1 off
static int ap_state;
static int ap_cur, ap_begun;
static uint64_t ap_t0, ap_best_at, ap_next_note;
static float ap_best, ap_start_pos[3], ap_note_pos[3], ap_yaw_last, ap_turned, ap_look_e0;
static int ap_look_sign = 1, ap_look_known, ap_no_camera_logged;
static int ap_count[4];   // reached, timed out, stuck, pictures missed
static uint64_t ap_held;  // buttons of hold steps not yet released
// Present side: 0 idle, 1 timing the route, 2 the route ended (log the figures), 3 logged
static _Atomic int ap_route;
static _Atomic int ap_shot;   // 0 none, 1 a picture pending at ap_shot_path, 2 taken
static char ap_shot_path[1024];

// ---- the script

static int ap_num(const char *t, float *out) {
    char *e;
    double v = strtod(t, &e);
    if (e == t || *e || !isfinite(v)) return 0;
    *out = (float)v;
    return 1;
}

// Splits line in place into at most max blank-separated words; returns how many.
static int ap_words(char *line, char **w, int max) {
    int n = 0;
    for (char *p = line; *p && n < max;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;
        w[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

// One script line into *s: 1 a step, 0 a blank or comment line, -1 an error (*why says which).
static int ap_parse(const char *text, ApStep *s, const char **why) {
    char line[256], *w[12] = {0};
    snprintf(line, sizeof line, "%s", text);
    char *h = strchr(line, '#');
    if (h) *h = 0;
    memset(s, 0, sizeof *s);
    while (*text == ' ' || *text == '\t') text++;
    snprintf(s->src, sizeof s->src, "%.*s", (int)strcspn(text, "#\r\n"), text);
    for (size_t k = strlen(s->src); k && (s->src[k - 1] == ' ' || s->src[k - 1] == '\t'); k--) s->src[k - 1] = 0;
    char *rest = line + strspn(line, " \t\r\n");
    size_t vlen = strcspn(rest, " \t\r\n");
    if (!vlen) return 0;
    s->kind = -1;
    for (int k = 0; k < AP_KINDS; k++)
        if (strlen(ap_verbs[k]) == vlen && !strncmp(rest, ap_verbs[k], vlen)) s->kind = k;
    if (s->kind < 0) return *why = "unknown step", -1;
    if (s->kind == AP_LOG || s->kind == AP_SHOT) {   // the rest of the line, trimmed
        char *a = rest + vlen + strspn(rest + vlen, " \t");
        size_t n = strcspn(a, "\r\n");
        while (n && (a[n - 1] == ' ' || a[n - 1] == '\t')) n--;
        if (!n) return *why = s->kind == AP_LOG ? "log needs a text" : "shot needs a name", -1;
        if (s->kind == AP_SHOT) {   // a file name: letters, digits, '-' and '_'
            if (n > 48) return *why = "a shot name has at most 48 characters", -1;
            for (size_t i = 0; i < n; i++)
                if (!((a[i] >= 'a' && a[i] <= 'z') || (a[i] >= 'A' && a[i] <= 'Z') || (a[i] >= '0' && a[i] <= '9') ||
                      a[i] == '-' || a[i] == '_'))
                    return *why = "a shot name is letters, digits, '-' and '_'", -1;
        }
        snprintf(s->arg, sizeof s->arg, "%.*s", (int)n, a);
        return 1;
    }
    int n = ap_words(rest, w, 12) - 1;
    char **a = w + 1;
    switch (s->kind) {
    case AP_WAIT:
        if (n != 1 || !ap_num(a[0], &s->secs) || s->secs < 0) return *why = "wait <seconds>", -1;
        return 1;
    case AP_WALK:
    case AP_RUN:
    case AP_LOOK:
        s->radius = 1, s->timeout = s->kind == AP_LOOK ? 8 : 30;
        if (n < 2 || !ap_num(a[0], &s->x) || !ap_num(a[1], &s->z))
            return *why = s->kind == AP_LOOK ? "look <x> <z> [timeout <s>]" : "walk|run <x> <z> [radius <m>] [timeout <s>]", -1;
        for (int i = 2; i < n; i += 2) {
            float v;
            if (i + 1 >= n || !ap_num(a[i + 1], &v) || v <= 0) return *why = "an option needs a positive number", -1;
            if (!strcmp(a[i], "timeout")) s->timeout = v;
            else if (!strcmp(a[i], "radius") && s->kind != AP_LOOK) s->radius = v;
            else return *why = s->kind == AP_LOOK ? "look takes timeout" : "walk and run take radius and timeout", -1;
        }
        return 1;
    case AP_CAMERA:
    case AP_STICK:
        if (n != 3 || !ap_num(a[0], &s->sx) || !ap_num(a[1], &s->sy) || !ap_num(a[2], &s->secs) || s->secs <= 0 ||
            fabsf(s->sx) > 1 || fabsf(s->sy) > 1)
            return *why = "camera|stick <x> <y> <seconds>, x and y from -1 to 1", -1;
        return 1;
    case AP_PRESS:
    case AP_HOLD:
    case AP_RELEASE:
        s->secs = 0.1f;
        if (s->kind == AP_PRESS ? n < 1 || n > 2 || (n == 2 && (!ap_num(a[1], &s->secs) || s->secs <= 0))
                                : s->kind == AP_HOLD ? n != 1 : n > 1)
            return *why = s->kind == AP_PRESS ? "press <button>[+<button>...] [<seconds>]"
                          : s->kind == AP_HOLD ? "hold <button>[+<button>...]" : "release [<button>[+<button>...]]", -1;
        for (char *b = a[0]; n; b++) {   // names joined by '+', none empty
            char name[16];
            size_t k = strcspn(b, "+");
            snprintf(name, sizeof name, "%.*s", (int)(k < 15 ? k : 15), b);
            int bit = k && k < 15 ? input_script_button(name) : -1;
            if (bit < 0) return *why = "unknown button", -1;
            s->buttons |= 1ull << bit;
            if (!b[k]) break;
            b += k;
        }
        return 1;
    }
    return *why = "unknown step", -1;
}

static int ap_load(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        rt_log("autopilot: cannot open NX2NAVI_AUTOPILOT %s; off", path);
        return 0;
    }
    char line[256];
    int ln = 0, cap = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        ApStep s;
        const char *why = NULL;
        int r = ap_parse(line, &s, &why);
        if (r < 0 || (r && ap_n == AP_MAX_STEPS)) {
            rt_log("autopilot: %s line %d: %s (\"%s\"); the script is refused, the autopilot is off", path, ln,
                   r < 0 ? why : "more than 256 steps", s.src);
            fclose(f);
            return 0;
        }
        if (!r) continue;
        s.line = ln;
        if (ap_n == cap) {
            cap = cap ? cap * 2 : 32;
            ApStep *grown = realloc(ap_steps, (size_t)cap * sizeof *ap_steps);
            if (!grown) { fclose(f); return 0; }
            ap_steps = grown;
        }
        ap_steps[ap_n++] = s;
    }
    fclose(f);
    if (!ap_n) rt_log("autopilot: %s has no steps; off", path);
    return ap_n > 0;
}

// ---- the game's state

static uint64_t ap_ptr(uint64_t a) {
    uint64_t p = 0;
    return a && plat_mem_read(a, &p, 8) ? p : 0;
}

static int ap_link(float p[3]) {
    uint64_t mgr = ap_ptr(MOD_MAIN_BASE + 0x4e371d0), actor = mgr ? ap_ptr(mgr + 0x480) : 0,
             name = actor ? ap_ptr(actor + 0x20) : 0;
    char text[7];
    return name && plat_mem_read(name, text, 7) && !memcmp(text, "Player", 7) && plat_mem_read(actor + 0x31c, p, 12) &&
           isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]);
}

// (x, z) as a unit ground direction in f; 0 for a (near) vertical or broken one.
static int ap_ground(float x, float z, float f[2]) {
    float l = hypotf(x, z);
    if (!(l > 1e-3f && l < 1e4f)) return 0;
    f[0] = x / l, f[1] = z / l;
    return 1;
}

// The camera's forward direction on the ground: the renderer's first view, target - eye.
static int ap_camera(float f[2]) {
    uint64_t module = ap_ptr(MOD_MAIN_BASE + 0x4e3e258), scene = module ? ap_ptr(module + 0x1e8) : 0,
             mgr = scene ? ap_ptr(scene + 0xb28) : 0, views = mgr ? ap_ptr(mgr + 0x18) : 0;
    float eye[3], at[3];
    if (!views || !plat_mem_read(views + 0x733c, eye, 12) || !plat_mem_read(views + 0x7348, at, 12)) return 0;
    return ap_ground(at[0] - eye[0], at[2] - eye[2], f);
}

// Degrees of a ground direction: 0 along +z, 90 along +x (the saves' yaw).
static float ap_yaw(float x, float z) { return atan2f(x, z) * (180.0f / 3.14159265f); }
static float ap_wrap(float d) {
    while (d > 180) d -= 360;
    while (d <= -180) d += 360;
    return d;
}

// The left stick (out, -32767..32767, up +y) that moves Link along the ground direction (dx, dz) at tilt 0..1 with the
// camera looking along the unit ground direction (fx, fz).
static void ap_steer(float fx, float fz, float dx, float dz, float tilt, int32_t out[2]) {
    float l = hypotf(dx, dz);
    out[0] = out[1] = 0;
    if (!(l > 1e-4f)) return;
    dx /= l, dz /= l;
    float up = dx * fx + dz * fz, right = dx * -fz + dz * fx;
    out[0] = (int32_t)lroundf(right * tilt * AP_FULL);
    out[1] = (int32_t)lroundf(up * tilt * AP_FULL);
}

// ---- the route

static void ap_result(const ApStep *s, const char *what, uint64_t now, int have, const float p[3], const char *extra) {
    char at[64] = "no player read";
    if (have) snprintf(at, sizeof at, "Link at %.1f %.1f %.1f", p[0], p[1], p[2]);
    rt_log("autopilot: step %d/%d (line %d, %s): %s after %.1f s; %s%s", (int)(s - ap_steps) + 1, ap_n, s->line, s->src,
           what, (now - ap_t0) / 60.0, at, extra ? extra : "");
}

static void ap_note(const ApStep *s, uint64_t now, const float p[3], float want, int cam, float camyaw, const int32_t st[2]) {
    if (now < ap_next_note) return;
    char moving[48] = "standing";
    float mx = p[0] - ap_note_pos[0], mz = p[2] - ap_note_pos[2];
    if (ap_next_note && hypotf(mx, mz) > 0.1f) snprintf(moving, sizeof moving, "moving %.0f", ap_yaw(mx, mz));
    char cams[32] = "no camera";
    if (cam) snprintf(cams, sizeof cams, "camera %.0f", camyaw);
    rt_log("autopilot:   step %d at %.1f s: Link at %.1f %.1f %.1f, %.1f m to go, %s (want %.0f), %s, stick %d %d",
           (int)(s - ap_steps) + 1, (now - ap_t0) / 60.0, p[0], p[1], p[2], hypotf(s->x - p[0], s->z - p[2]), moving,
           want, cams, st[0], st[1]);
    memcpy(ap_note_pos, p, sizeof ap_note_pos);
    ap_next_note = now + AP_NOTE_TICKS;
}

// Runs the current step at game tick now; 1 when it finished (its result logged), 0 while it goes on, with its input
// in b, sl, sr.
static int ap_step(ApStep *s, uint64_t now, int have, const float p[3], uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    float f[2];
    int cam = ap_camera(f);
    if (!ap_begun) {
        ap_begun = 1, ap_t0 = now, ap_next_note = 0, ap_best = INFINITY, ap_best_at = now, ap_turned = 0;
        if (have) memcpy(ap_start_pos, p, sizeof ap_start_pos), memcpy(ap_note_pos, p, sizeof ap_note_pos);
        if (cam) ap_yaw_last = ap_yaw(f[0], f[1]);
        if (s->kind == AP_LOOK) ap_look_e0 = -1;
        if (s->kind == AP_SHOT) {
            const char *dir = cfg_get("NX2NAVI_SHOT_DIR");
            snprintf(ap_shot_path, sizeof ap_shot_path, "%s/ap-%02d-%s.png", dir && *dir ? dir : ".",
                     (int)(s - ap_steps) + 1, s->arg);
            atomic_store_explicit(&ap_shot, 1, memory_order_release);
        }
    }
    uint64_t span = now - ap_t0;
    char extra[160];
    switch (s->kind) {
    case AP_WAIT:
        if (span < AP_TICKS(s->secs)) return 0;
        ap_result(s, "done", now, have, p, NULL);
        return 1;
    case AP_LOG:
        rt_log("autopilot: note: %s", s->arg);
        return 1;
    case AP_PRESS:
        if (span < AP_TICKS(s->secs)) *b |= s->buttons;
        if (span < AP_TICKS(s->secs) + AP_RELEASE_TICKS) return 0;
        ap_result(s, "done", now, have, p, NULL);
        return 1;
    case AP_HOLD:
    case AP_RELEASE:
        if (s->kind == AP_HOLD) ap_held |= s->buttons;
        else ap_held &= s->buttons ? ~s->buttons : 0;
        ap_result(s, "done", now, have, p, NULL);
        return 1;
    case AP_SHOT: {
        int taken = atomic_load_explicit(&ap_shot, memory_order_acquire) == 2, pending = 1;
        if (!taken && span < AP_SHOT_TICKS) return 0;
        // withdraw a picture still pending; one the present took meanwhile counts as taken
        if (!taken) taken = !atomic_compare_exchange_strong(&ap_shot, &pending, 0);
        atomic_store_explicit(&ap_shot, 0, memory_order_relaxed);
        if (!taken) ap_count[3]++;
        snprintf(extra, sizeof extra, "; %s", ap_shot_path);
        ap_result(s, taken ? "taken" : "missed", now, have, p, extra);
        return 1;
    }
    case AP_CAMERA:
    case AP_STICK: {
        if (cam && s->kind == AP_CAMERA) {
            float y = ap_yaw(f[0], f[1]);
            ap_turned += ap_wrap(y - ap_yaw_last);
            ap_yaw_last = y;
        }
        if (span < AP_TICKS(s->secs)) {
            int32_t *st = s->kind == AP_CAMERA ? sr : sl;
            st[0] = (int32_t)lroundf(s->sx * AP_FULL), st[1] = (int32_t)lroundf(s->sy * AP_FULL);
            return 0;
        }
        if (s->kind == AP_STICK) snprintf(extra, sizeof extra, ", %.1f m from where the step began",
                                          have ? hypotf(p[0] - ap_start_pos[0], p[2] - ap_start_pos[2]) : 0.0f);
        else if (cam) snprintf(extra, sizeof extra, ", the camera turned %.0f degrees", ap_turned);
        else snprintf(extra, sizeof extra, ", no camera read");
        ap_result(s, "done", now, have, p, extra);
        return 1;
    }
    case AP_LOOK: {
        float e = 0;
        if (have && cam) e = ap_wrap(ap_yaw(s->x - p[0], s->z - p[2]) - ap_yaw(f[0], f[1]));
        if (have && cam && fabsf(e) <= AP_LOOK_DEG) {
            snprintf(extra, sizeof extra, ", the camera %.0f degrees off the point", e);
            ap_result(s, "reached", now, have, p, extra);
            ap_count[0]++;
            return 1;
        }
        if (span >= AP_TICKS(s->timeout)) {
            snprintf(extra, sizeof extra, have && cam ? ", the camera %.0f degrees off the point" : ", no camera read", e);
            ap_result(s, "timeout", now, have, p, extra);
            ap_count[1]++;
            return 1;
        }
        if (!(have && cam)) return 0;
        if (ap_look_e0 < 0) ap_look_e0 = fabsf(e);
        if (!ap_look_known && span >= AP_LOOK_CHECK_TICKS) {
            ap_look_known = 1;
            if (fabsf(e) > ap_look_e0 + 3) {
                ap_look_sign = -ap_look_sign;
                rt_log("autopilot: the camera turned away from the point (%.0f to %.0f degrees off): the right stick "
                       "turns it the other way from now on", ap_look_e0, fabsf(e));
            }
        }
        // the view pans right (yaw falls) with the stick to the right; at least a third of the tilt, so it moves
        float k = e / 45.0f;
        k = k > 1 ? 1 : k < -1 ? -1 : fabsf(k) < 0.33f ? (k < 0 ? -0.33f : 0.33f) : k;
        sr[0] = (int32_t)lroundf(-k * ap_look_sign * AP_FULL), sr[1] = 0;
        ap_note(s, now, p, ap_yaw(s->x - p[0], s->z - p[2]), cam, ap_yaw(f[0], f[1]), sr);
        return 0;
    }
    case AP_WALK:
    case AP_RUN: {
        float d = have ? hypotf(s->x - p[0], s->z - p[2]) : INFINITY;
        const char *end = NULL;
        if (have && d <= s->radius) end = "reached", ap_count[0]++;
        else if (span >= AP_TICKS(s->timeout)) end = "timeout", ap_count[1]++;
        else if (have) {
            if (d < ap_best - AP_STUCK_GAIN) ap_best = d, ap_best_at = now;
            if (now - ap_best_at >= AP_STUCK_TICKS) end = "stuck", ap_count[2]++;
        }
        if (end) {
            if (have) snprintf(extra, sizeof extra, ", %.1f m from the point", d);
            ap_result(s, end, now, have, p, have ? extra : NULL);
            return 1;
        }
        if (!have) return 0;
        if (!cam) {
            if (!ap_no_camera_logged) rt_log("autopilot: no camera direction read; Link waits");
            ap_no_camera_logged = 1;
            return 0;
        }
        float tilt = s->kind == AP_RUN ? 1.0f : 0.5f;
        if (d < 2.5f) tilt = tilt * d / 2.5f > 0.3f ? tilt * d / 2.5f : 0.3f;   // slow down near the point, not to a stop
        ap_steer(f[0], f[1], s->x - p[0], s->z - p[2], tilt, sl);
        ap_note(s, now, p, ap_yaw(s->x - p[0], s->z - p[2]), cam, ap_yaw(f[0], f[1]), sl);
        return 0;
    }
    }
    return 1;
}

static int ap_start(void) {
    const char *path = cfg_get("NX2NAVI_AUTOPILOT");
    if (!path || !*path) return 0;
    if (!RT_V111) {
        rt_log("autopilot: off: its reads are game 1.1.1's, this is %d", rt_game_version);
        return 0;
    }
    if (!ap_load(path)) return 0;
    rt_log("autopilot: script %s, %d steps; it starts once the save has loaded", path, ap_n);
    return 1;
}

void autopilot_input(uint64_t *b, int32_t sl[2], int32_t sr[2]) {
    if (ap_state < 0 || ap_state == 3) return;
    if (!ap_state) {
        ap_state = ap_start() ? 1 : -1;
        if (ap_state < 0) return;
    }
    uint64_t now = frame_step_ticks();
    float p[3];
    int have = ap_link(p);
    if (ap_state == 1) {   // the save has loaded once the Player actor stands away from the origin
        if (!have || fabsf(p[0]) + fabsf(p[2]) < 1.0f) return;
        float f[2];
        char cams[32] = "no camera read";
        if (ap_camera(f)) snprintf(cams, sizeof cams, "camera yaw %.0f", ap_yaw(f[0], f[1]));
        rt_log("autopilot: gameplay at tick %llu, %.1f s after the guest started; Link at %.1f %.1f %.1f; %s",
               (unsigned long long)now, rt_guest_start_ns ? (kern_now_ns() - rt_guest_start_ns) / 1e9 : 0.0, p[0], p[1],
               p[2], cams);
        ap_state = 2, ap_cur = 0, ap_begun = 0;
        atomic_store_explicit(&ap_route, 1, memory_order_release);
    }
    while (ap_cur < ap_n && ap_step(&ap_steps[ap_cur], now, have, p, b, sl, sr)) ap_cur++, ap_begun = 0;
    if (ap_cur < ap_n) {
        *b |= ap_held;
        return;
    }
    ap_state = 3;   // the end lets every held button go
    char at[64] = "no player read";
    if (have) snprintf(at, sizeof at, "Link at %.1f %.1f %.1f", p[0], p[1], p[2]);
    rt_log("autopilot: done: %d steps, %d reached, %d timed out, %d stuck, %d pictures missed; %s", ap_n, ap_count[0],
           ap_count[1], ap_count[2], ap_count[3], at);
    atomic_store_explicit(&ap_route, 2, memory_order_release);
}

const char *autopilot_present(int can_shoot) {
    int r = atomic_load_explicit(&ap_route, memory_order_acquire);
    if (r != 1 && r != 2) return NULL;
    static uint64_t t0, last, ticks0, frames, sum, over[3], skipped;
    static uint32_t hist[AP_BINS];
    static int skip;
    uint64_t now = plat_now_ns();
    if (r == 2) {   // the route ended: its figures, once
        uint64_t cut = (frames + 99) / 100, acc = 0, low = 0;   // the slowest 1% of the frames
        for (int i = AP_BINS - 1; i >= 0 && frames; i--)
            if ((acc += hist[i]) >= cut) { low = (uint64_t)(i + 1) * 250000ull; break; }
        double wall = t0 ? (now - t0) / 1e9 : 0, game = (frame_step_ticks() - ticks0) / 60.0;
        rt_log("autopilot: route frames: %llu frames in %.1f s, mean %.1f fps, 1%% low %.1f fps, over 25 ms %llu, "
               "50 ms %llu, 100 ms %llu; game %.1f s, speed %.3f; %llu picture frames left out",
               (unsigned long long)frames, wall, sum ? frames * 1e9 / sum : 0.0, low ? 1e9 / low : 0.0,
               (unsigned long long)over[0], (unsigned long long)over[1], (unsigned long long)over[2], game,
               wall > 0 ? game / wall : 0.0, (unsigned long long)skipped);
        atomic_store_explicit(&ap_route, 3, memory_order_release);
        return NULL;
    }
    if (!t0) {
        t0 = last = now, ticks0 = frame_step_ticks();
    } else {
        uint64_t d = now - last;
        last = now;
        if (skip) skipped++;   // the frame after a picture carries its capture and PNG write
        else {
            frames++, sum += d;
            hist[d / 250000ull < AP_BINS ? d / 250000ull : AP_BINS - 1]++;
            over[0] += d > 25000000ull, over[1] += d > 50000000ull, over[2] += d > 100000000ull;
        }
        skip = 0;
    }
    int pending = 1;
    if (can_shoot && atomic_compare_exchange_strong(&ap_shot, &pending, 2)) {
        skip = 1;
        return ap_shot_path;
    }
    return NULL;
}
