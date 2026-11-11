// Bug reports (bug_report.h). A report is the shared save's zip (save_share.c) with these in bug-report/:
//
//   report.json       what the receiver (tools/report_server.py) builds the issue from: kind, versions, commit, system,
//                     where the player was, the fatal line, the settings changed from their defaults, the files
//   nx2navi.log       this run's log (its last 8 MiB)
//   nx2navi.previous.log, crash.txt   after a crash: that run's log and its report (from its last FATAL line on)
//   nx2navi.ini       the settings file with each secret setting's value left out (save_share_secret)
//   settings.txt      the settings in effect, from nx2navi.ini and the environment, secrets left out
//   system.txt        sysinfo_text: OS, CPU, memory, GPU and driver
//   state.txt         the player's state as far as the runtime knows it
//   frame.png         the frame under the menu when the report was made
//   perf.txt          the last perf lines of the log, when NX2NAVI_TRACE has perf
//   note.txt          what the player wrote
//
// and "report" (crash or report) and "commit" in the manifest. The save is the one the game would load next, as
// Export has it; a report from a game that has not saved yet goes without.
//
// Sending: curl, started with its options in a file next to the zip (the key never on a command line, where other
// programs of the machine could read it), deleted as soon as curl ends. curl rather than a socket of our own: the
// receiver is meant to sit behind HTTPS on a server, and curl brings TLS and the system's certificates on Linux and on
// Windows 10 and later (System32\curl.exe), while the game links no TLS library. It runs on the caller's thread, which
// the menu starts for it, so the game does not wait for the network.
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bug_report.h"
#include "cfg.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "save_share.h"
#include "save_summary.h"
#include "save_zip.h"
#include "sysinfo.h"

#define LOG_TAIL (8ull << 20)     // the part of a log a report carries: its end, where a problem shows
#define CRASH_MAX (256u << 10)    // crash.txt at most
#define PERF_LINES 30
#define MAX_MEMBERS 48

// ---- the last run's crash
static char g_note_path[PLAT_PATH_MAX + 32];
static char g_note[PLAT_PATH_MAX + 64];
static size_t g_note_n;
static int g_armed;
static volatile int g_noted;
static int g_last_crash;
static char g_last_log[PLAT_PATH_MAX + 32], g_last_fatal[512];

// This run's log: logs/nx2navi.log, else the file stderr goes to (run_game.py, `2> file`). 0 when none.
static int this_log(char *out, size_t cap) {
    const char *p = rt_log_path();
    if (p) return snprintf(out, cap, "%s", p) < (int)cap;
    return plat_stderr_path(out, cap);
}

// The last max bytes of the file at path, from a line start, NUL-terminated (*n bytes); *cut gets the bytes left out
// before. NULL when unreadable.
static char *read_tail(const char *path, uint64_t max, size_t *n, uint64_t *cut) {
    *n = 0, *cut = 0;
    int64_t size = plat_file_size(path);
    PlatFile *f = size >= 0 ? plat_file_open(path) : NULL;
    if (!f) return NULL;
    uint64_t from = (uint64_t)size > max ? (uint64_t)size - max : 0, len = (uint64_t)size - from;
    char *buf = malloc(len + 1);
    uint64_t got = 0;
    while (buf && got < len) {
        uint32_t k = len - got > (16u << 20) ? 16u << 20 : (uint32_t)(len - got);
        int64_t r = plat_file_pread(f, buf + got, k, from + got);
        if (r <= 0) break;
        got += (uint64_t)r;
    }
    plat_file_close(f);
    if (!buf) return NULL;
    buf[got] = 0;
    size_t skip = 0;
    if (from) {   // from the first whole line
        char *nl = memchr(buf, '\n', got);
        skip = nl ? (size_t)(nl + 1 - buf) : 0;
        memmove(buf, buf + skip, got - skip + 1);
    }
    *n = (size_t)(got - skip);
    *cut = from + skip;
    return buf;
}

// The start of the last line that begins with "FATAL: " in text, NULL when none.
static const char *last_fatal(const char *text, size_t n) {
    const char *found = NULL;
    for (const char *p = text; p && p < text + n; p = memchr(p, '\n', (size_t)(text + n - p)), p = p ? p + 1 : NULL)
        if (!strncmp(p, "FATAL: ", 7)) found = p;
    return found;
}

static void line_of(const char *p, char *out, size_t cap) {
    size_t k = strcspn(p, "\r\n");
    snprintf(out, cap, "%.*s", (int)(k < cap ? k : cap - 1), p);
}

void bug_report_start(void) {
    char dir[PLAT_PATH_MAX], log[PLAT_PATH_MAX + 32];
    if (!sysinfo_log_folder(dir, sizeof dir)) {
        rt_log("bug report: no log folder; a fatal error of this run cannot be offered at the next start");
        return;
    }
    snprintf(g_note_path, sizeof g_note_path, "%s/crash-pending.txt", dir);
    const int have_log = this_log(log, sizeof log);
    uint64_t n = 0;
    uint8_t *note = plat_file_read(g_note_path, &n);
    if (note) {
        char prev[PLAT_PATH_MAX + 32] = "";
        if (n > 4 && !memcmp(note, "log ", 4)) {
            size_t k = 4;
            while (k < n && note[k] != '\n' && k - 4 < sizeof prev - 1) k++;
            memcpy(prev, note + 4, k - 4);
            prev[k - 4] = 0;
        }
        free(note);
        plat_file_delete(g_note_path);
        // logs/nx2navi.log of the last run is nx2navi.1.log now (main.c log_open moves each start's logs on)
        if (have_log && !strcmp(prev, log) && rt_log_path()) snprintf(prev, sizeof prev, "%s/nx2navi.1.log", dir);
        snprintf(g_last_log, sizeof g_last_log, "%s", prev);
        size_t tn;
        uint64_t cut;
        char *text = *prev ? read_tail(prev, LOG_TAIL, &tn, &cut) : NULL;
        const char *f = text ? last_fatal(text, tn) : NULL;
        if (f) line_of(f, g_last_fatal, sizeof g_last_fatal);
        free(text);
        g_last_crash = 1;
        rt_log("bug report: the last run ended with a fatal error: %s (its log: %s); the menu offers its report",
               *g_last_fatal ? g_last_fatal : "no FATAL line in its log", *prev ? prev : "unknown");
    }
    if (have_log) {
        int k = snprintf(g_note, sizeof g_note, "log %s\n", log);
        if (k > 0 && (size_t)k < sizeof g_note) g_note_n = (size_t)k, g_armed = 1;
    }
}

// Called on the failing thread, maybe in a signal handler: one open and one write of text made at start.
void bug_report_crashed(void) {
    if (!g_armed || g_noted) return;
    g_noted = 1;
    PlatAppend *f = plat_append_open(g_note_path);
    if (f) plat_append_write(f, g_note, g_note_n);
}

int bug_report_last_crash(char *fatal, size_t cap) {
    if (cap) snprintf(fatal, cap, "%s", g_last_fatal);
    return g_last_crash;
}

// ---- the player's state from the game thread
static atomic_int st_want, st_have, st_sec;
static _Atomic int64_t st_area, st_child;

int bug_report_state_wanted(void) { return atomic_load_explicit(&st_want, memory_order_relaxed); }

void bug_report_state_put(int64_t area, int64_t child, int day_second) {
    atomic_store(&st_area, area);
    atomic_store(&st_child, child);
    atomic_store(&st_sec, day_second);
    atomic_store(&st_have, 1);
    atomic_store(&st_want, 0);
}

// ---- text buffers
typedef struct { char *p; size_t n, cap; int failed; } Text;

static void put(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(Text *t, const char *fmt, ...) {
    for (int pass = 0; pass < 2 && !t->failed; pass++) {
        va_list ap;
        va_start(ap, fmt);
        int k = vsnprintf(t->p ? t->p + t->n : NULL, t->p ? t->cap - t->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (t->p && t->n + (size_t)k < t->cap) {
            t->n += (size_t)k;
            return;
        }
        size_t cap = (t->cap ? t->cap * 2 : 4096) + (size_t)k + 1;
        char *p = realloc(t->p, cap);
        if (!p) {
            t->failed = 1;
            return;
        }
        t->p = p, t->cap = cap;
        t->p[t->n] = 0;
    }
}

static void put_json(Text *t, const char *s) {   // a JSON string with its quotes
    put(t, "\"");
    char chunk[512];
    for (size_t k = 0, n = strlen(s); k < n; k += 120) {   // escaped in pieces: 120 bytes grow to at most 720
        char part[121];
        snprintf(part, sizeof part, "%.*s", (int)(n - k < 120 ? n - k : 120), s + k);
        put(t, "%s", sz_json_escape(part, chunk, sizeof chunk));
    }
    put(t, "\"");
}

static void size_text(size_t n, char *out, size_t cap) {
    if (n < 10240) snprintf(out, cap, "%zu bytes", n);
    else if (n < (10u << 20)) snprintf(out, cap, "%.0f KB", n / 1024.0);
    else snprintf(out, cap, "%.1f MB", n / 1048576.0);
}

// ---- gathering
typedef struct {
    SzWriter w;
    struct { char name[64]; size_t bytes; } m[MAX_MEMBERS];
    int nm;
} Build;

static void add(Build *b, const char *name, const void *data, size_t n) {
    sz_writer_add(&b->w, name, data, n);
    if (b->nm < MAX_MEMBERS) {
        snprintf(b->m[b->nm].name, sizeof b->m[b->nm].name, "%s", name);
        b->m[b->nm++].bytes = n;
    }
}

static void add_text(Build *b, const char *name, const Text *t) {
    if (t->p && !t->failed) add(b, name, t->p, t->n);
}

// A log's last LOG_TAIL bytes as name, with a first line saying what was left out.
static int add_log(Build *b, const char *name, const char *path, char **keep, size_t *keep_n) {
    size_t n;
    uint64_t cut;
    char *text = path && *path ? read_tail(path, LOG_TAIL, &n, &cut) : NULL;
    if (!text) return 0;
    char head[96];
    int hn = cut ? snprintf(head, sizeof head, "[report: the first %llu bytes of this log are left out]\n",
                            (unsigned long long)cut)
                 : 0;
    char *both = hn > 0 ? malloc((size_t)hn + n + 1) : NULL;
    if (both) {
        memcpy(both, head, (size_t)hn);
        memcpy(both + hn, text, n + 1);
        add(b, name, both, (size_t)hn + n);
        free(both);
    } else {
        add(b, name, text, n);
    }
    if (keep) *keep = text, *keep_n = n;
    else free(text);
    return 1;
}

typedef struct {
    Text *t;
    int count;
} SettingsWalk;

static void setting_line(const char *key, const char *value, int from_env, void *ctx) {
    SettingsWalk *s = ctx;
    const int secret = save_share_secret(key);
    const char *def = cfg_default(key);
    if (secret) value = "(left out)";
    char lower[80];
    size_t k = 0;
    for (; key[k] && k < sizeof lower - 1; k++) lower[k] = key[k] >= 'A' && key[k] <= 'Z' ? key[k] + 32 : key[k];
    lower[k] = 0;
    if (from_env) put(s->t, "  NX2NAVI_%s=%s (environment)", key, value);
    else put(s->t, "  %s = %s", lower, value);
    if (!secret && (!def || strcmp(def, value))) {
        put(s->t, def ? "   (default %s)\n" : "   (no fixed default)\n", def);
        s->count++;
    } else {
        put(s->t, "\n");
    }
}

typedef struct {
    Text *t;
    int first;
} ChangedWalk;

static void changed_json(const char *key, const char *value, int from_env, void *ctx) {
    ChangedWalk *c = ctx;
    const char *def = cfg_default(key);
    if (save_share_secret(key) || (def && !strcmp(def, value))) return;
    char line[512];
    char lower[80];   // a file's key as players write it; the environment's as it is named
    size_t k = 0;
    for (; key[k] && k < sizeof lower - 1; k++) lower[k] = !from_env && key[k] >= 'A' && key[k] <= 'Z' ? key[k] + 32 : key[k];
    lower[k] = 0;
    snprintf(line, sizeof line, "%s%s = %s", from_env ? "NX2NAVI_" : "", lower, value);
    put(c->t, "%s\n  ", c->first ? "" : ",");
    put_json(c->t, line);
    c->first = 0;
}

// The value after "<prefix>" on a line of text, into out; "" when no line starts so.
static void line_after(const char *text, const char *prefix, char *out, size_t cap) {
    *out = 0;
    size_t k = strlen(prefix);
    for (const char *p = text; p && *p; p = strchr(p, '\n'), p = p ? p + 1 : NULL)
        if (!strncmp(p, prefix, k)) {
            line_of(p + k, out, cap);
            return;
        }
}

// The player's state: state.txt's text into t, a one-line form into place.
static void player_state(Text *t, char *place, size_t cap) {
    float pos[3];
    const int in_play = frame_player_pos(pos);
    int64_t area = -1, child = -1;
    int sec = -1;
    if (rt_game_version == 111 && in_play) {   // restore_drawbridge.c answers on the game thread within a frame
        atomic_store(&st_have, 0);
        atomic_store(&st_want, 1);
        for (int i = 0; i < 40 && !atomic_load(&st_have); i++) plat_sleep_ms(10);
        atomic_store(&st_want, 0);
        if (atomic_load(&st_have)) {
            area = atomic_load(&st_area), child = atomic_load(&st_child);
            sec = atomic_load(&st_sec);
        }
    }
    const int64_t world_us = frame_step_world_us();
    put(t, "Game: The Legend of Zelda: Ocarina of Time %d.%d.%d\n", rt_game_version / 100, rt_game_version / 10 % 10,
        rt_game_version % 10);
    if (in_play) put(t, "In play: yes, the player at %.1f %.1f %.1f (metres)\n", pos[0], pos[1], pos[2]);
    else put(t, "In play: no (the title screen, a load or a cutscene without the player)\n");
    const char *area_name = area == 15 ? " (Hyrule Field)" : area == 23 ? " (the market, as a child)" : "";
    if (area >= 0) put(t, "Area: index %lld%s (the game's System_CurrentBancIndex)\n", (long long)area, area_name);
    else put(t, "Area: not known%s\n", rt_game_version == 111 ? " (the game's flag update did not answer)" : "");
    if (child >= 0) put(t, "Age: %s\n", child ? "child" : "adult");
    if (sec >= 0) put(t, "Time of day: %02d:%02d (the game's clock)\n", sec / 3600, sec / 60 % 60);
    else put(t, "Time of day: not known\n");
    if (world_us >= 0) put(t, "Game time since the save loaded: %.0f s\n", world_us / 1e6);
    FsSaveFile files[64];
    int nf = fs_save_files(files, 64);
    const FsSaveFile *run = NULL;
    for (int i = 0; i < nf; i++)
        if (files[i].running) run = &files[i];
    if (run) {
        const SaveSummary *s = &run->sum;
        put(t, "Save file in use: %d%s%s%s\n", run->id, *run->name ? " (" : "", run->name, *run->name ? ")" : "");
        if (s->slots && s->year)
            put(t, "Its last save: %04d-%02d-%02d %02d:%02d", s->year, s->month, s->day, s->hour, s->minute);
        else put(t, "Its last save: none yet");
        if (s->slots && s->adult >= 0) put(t, ", %s", s->adult ? "adult" : "child");
        if (s->slots && s->life >= 0 && s->max_life > 0)
            put(t, ", %.2g of %d hearts", s->life / 4.0, s->max_life / 4);
        if (s->slots && *s->area) put(t, ", %s", s->area);
        put(t, "\n");
    }
    put(t, "Not known to the runtime: the scene's and the room's names and the hearts now (the last save's are above)\n");
    int k = snprintf(place, cap, "%s", in_play ? "in play" : "not in play");
    if (area >= 0 && k > 0 && (size_t)k < cap) k += snprintf(place + k, cap - (size_t)k, ", area %lld%s", (long long)area, area_name);
    if (child >= 0 && k > 0 && (size_t)k < cap) k += snprintf(place + k, cap - (size_t)k, ", %s", child ? "child" : "adult");
    if (sec >= 0 && k > 0 && (size_t)k < cap) k += snprintf(place + k, cap - (size_t)k, ", %02d:%02d", sec / 3600, sec / 60 % 60);
    if (in_play && k > 0 && (size_t)k < cap) snprintf(place + k, cap - (size_t)k, ", at %.1f %.1f %.1f", pos[0], pos[1], pos[2]);
    if (run && run->sum.slots && *run->sum.area && k > 0 && (size_t)k < cap)
        snprintf(place + strlen(place), cap - strlen(place), "; last saved in %s", run->sum.area);
}

// The last PERF_LINES lines of log holding "] perf: ".
static void perf_lines(Text *t, const char *log, size_t n) {
    const char *at[PERF_LINES];
    int k = 0;
    for (const char *p = log; p && p < log + n; p = memchr(p, '\n', (size_t)(log + n - p)), p = p ? p + 1 : NULL) {
        const char *e = memchr(p, '\n', (size_t)(log + n - p));
        const char *m = strstr(p, "] perf: ");
        if (m && (!e || m < e)) at[k++ % PERF_LINES] = p;
    }
    for (int i = k > PERF_LINES ? k - PERF_LINES : 0; i < k; i++) {
        const char *p = at[i % PERF_LINES];
        put(t, "%.*s\n", (int)strcspn(p, "\n"), p);
    }
}

int bug_report_dir(char *out, size_t cap) {
    const RtConfig *c = rt_config();
    return c && c->profile && snprintf(out, cap, "%s/reports", c->profile) < (int)cap;
}

int bug_report_make(const char *dir, const BugReportWhat *what, int preview, BugReportMade *made) {
    memset(made, 0, sizeof *made);
    struct tm t;
    plat_local_time(&t);
    Build *b = calloc(1, sizeof *b);
    if (!b) return 0;
    sz_writer_init(&b->w, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    const int crash = what->crash && g_last_crash;
    const char *note = what->note ? what->note : "";

    int saved = save_share_add_save(&b->w);
    int save_members = b->w.count;
    size_t save_bytes = b->w.n;

    rt_log_sync();   // the lines queued for the log thread, so the log ends with now
    char log[PLAT_PATH_MAX + 32] = "", *log_text = NULL;
    size_t log_n = 0;
    if (this_log(log, sizeof log)) add_log(b, "bug-report/nx2navi.log", log, &log_text, &log_n);
    char fatal[512] = "";
    if (crash) {
        char *prev = NULL;
        size_t pn = 0;
        add_log(b, "bug-report/nx2navi.previous.log", g_last_log, &prev, &pn);
        const char *f = prev ? last_fatal(prev, pn) : NULL;
        if (f) {
            size_t cn = (size_t)(prev + pn - f);
            add(b, "bug-report/crash.txt", f, cn < CRASH_MAX ? cn : CRASH_MAX);
        }
        snprintf(fatal, sizeof fatal, "%s", g_last_fatal);
        free(prev);
    }
    const int before_ini = b->w.count;
    save_share_add_settings(&b->w, "bug-report/nx2navi.ini");
    if (b->w.count > before_ini && b->nm < MAX_MEMBERS) {   // save_share_add_settings keeps no list of its own
        snprintf(b->m[b->nm].name, sizeof b->m[b->nm].name, "bug-report/nx2navi.ini");
        b->m[b->nm++].bytes = b->w.m[b->w.count - 1].usize;
    }

    Text settings = {0};
    put(&settings, "Settings in effect: nx2navi.ini, then the environment, which wins. Secret settings are left out.\n");
    SettingsWalk sw = {&settings, 0};
    cfg_each_active(setting_line, &sw);
    put(&settings, "%d of them differ from their defaults.\n", sw.count);
    add_text(b, "bug-report/settings.txt", &settings);

    char *sys = malloc(16384);
    if (sys) {
        sysinfo_text(sys, 16384);
        add(b, "bug-report/system.txt", sys, strlen(sys));
    }

    Text state = {0};
    char place[512];
    player_state(&state, place, sizeof place);
    add_text(b, "bug-report/state.txt", &state);

    uint64_t pn = 0;
    uint8_t *png = what->picture ? plat_file_read(what->picture, &pn) : NULL;
    if (png) add(b, "bug-report/frame.png", png, (size_t)pn);
    free(png);

    const int tracing = (rt_trace & RT_TRACE_PERF) != 0;
    Text perf = {0};
    if (tracing && log_text) perf_lines(&perf, log_text, log_n);
    if (perf.n) add_text(b, "bug-report/perf.txt", &perf);
    free(log_text);
    if (*note) add(b, "bug-report/note.txt", note, strlen(note));

    // report.json: what the receiver needs for the issue, so it reads no other file to write it
    char os[256], cpu[256], gpu[256], driver[256], ram[160];
    line_after(sys ? sys : "", "OS: ", os, sizeof os);
    line_after(sys ? sys : "", "CPU: ", cpu, sizeof cpu);
    line_after(sys ? sys : "", "RAM: ", ram, sizeof ram);
    line_after(sys ? sys : "", "GPU: ", gpu, sizeof gpu);
    line_after(sys ? sys : "", "Driver: ", driver, sizeof driver);
    free(sys);
    char game[16];
    snprintf(game, sizeof game, "%d.%d.%d", rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10);
    struct tm u;
    plat_gmtime((int64_t)(plat_unix_time_ns() / 1000000000u), &u);
    char made_at[32];
    snprintf(made_at, sizeof made_at, "%04d-%02d-%02dT%02d:%02d:%02dZ", u.tm_year + 1900, u.tm_mon + 1, u.tm_mday,
             u.tm_hour, u.tm_min, u.tm_sec);
    Text j = {0};
    put(&j, "{\n \"format\": \"nx2navi-report\",\n \"format_version\": 1,\n \"kind\": \"%s\",\n", crash ? "crash" : "report");
    put(&j, " \"nx2navi\": "), put_json(&j, rt_port_version), put(&j, ",\n \"commit\": "), put_json(&j, rt_port_commit);
    put(&j, ",\n \"game_version\": \"%s\",\n \"made\": \"%s\",\n \"os\": ", game, made_at), put_json(&j, os);
    put(&j, ",\n \"cpu\": "), put_json(&j, cpu), put(&j, ",\n \"ram\": "), put_json(&j, ram);
    put(&j, ",\n \"gpu\": "), put_json(&j, gpu), put(&j, ",\n \"driver\": "), put_json(&j, driver);
    put(&j, ",\n \"place\": "), put_json(&j, place), put(&j, ",\n \"fatal\": "), put_json(&j, fatal);
    put(&j, ",\n \"previous_log\": %s", crash && *g_last_log ? "true" : "false");
    put(&j, ",\n \"note\": "), put_json(&j, note);
    put(&j, ",\n \"save_files\": %d,\n \"tracing_perf\": %s,\n \"settings_changed\": [", saved > 0 ? saved : 0,
        tracing ? "true" : "false");
    ChangedWalk cw = {&j, 1};
    cfg_each_active(changed_json, &cw);
    put(&j, "],\n \"left_out\": \"the values of report_key, co-op keys and any setting named a secret, token or password\",\n");
    put(&j, " \"files\": [");
    for (int i = 0; i < b->nm; i++) {
        put(&j, "%s\n  {\"name\": ", i ? "," : "");
        put_json(&j, b->m[i].name);
        put(&j, ", \"bytes\": %zu}", b->m[i].bytes);
    }
    put(&j, "]\n}\n");
    add_text(b, "bug-report/report.json", &j);

    char extra[256], commit[96];
    sz_json_escape(rt_port_commit, commit, sizeof commit);
    snprintf(extra, sizeof extra, " \"report\": \"%s\",\n \"commit\": \"%s\",\n", crash ? "crash" : "report", commit);
    char manifest[1536];
    int mn = save_share_manifest(manifest, sizeof manifest, saved > 0 ? saved : 0, 1, extra);
    if (mn > 0) sz_writer_add(&b->w, "manifest.json", manifest, (size_t)mn);

    // what the menu shows before anything goes
    Text s = {0};
    put(&s, "NX2navi %s, commit %s, game %s\n", rt_port_version, rt_port_commit, game);
    if (crash) put(&s, "The last run's fatal error: %s\n", *fatal ? fatal : "(no FATAL line in its log)");
    put(&s, "System: %s\nGPU: %s\nDriver: %s\nWhere: %s\n", os, gpu, driver, place);
    put(&s, "Settings: %d differ from their defaults; report_key and co-op keys are left out\n", sw.count);
    if (!tracing) put(&s, "Performance lines: none (NX2NAVI_TRACE does not have perf)\n");
    if (!png && what->picture) put(&s, "Picture: none (the frame could not be read)\n");
    put(&s, "Files:\n");
    char sz[32];
    if (saved > 0) {
        size_text(save_bytes, sz, sizeof sz);
        put(&s, "  save/  %d files of the save in use, %s packed\n", save_members, sz);
    } else {
        put(&s, "  save/  none (%s)\n", saved < 0 ? "a save file could not be read" : "the game has not saved yet");
    }
    for (int i = 0; i < b->nm; i++) {
        size_text(b->m[i].bytes, sz, sizeof sz);
        put(&s, "  %s  %s\n", b->m[i].name, sz);
    }
    put(&s, "  manifest.json\n");
    free(settings.p), free(state.p), free(perf.p), free(j.p);

    size_t zn = 0;
    uint8_t *zip = sz_writer_finish(&b->w, &zn);
    free(b);
    int ok = zip != NULL;
    if (ok && !preview) {
        char stem[96];
        snprintf(stem, sizeof stem, "NX2navi-report-%04d-%02d-%02d_%02d-%02d-%02d%s", t.tm_year + 1900, t.tm_mon + 1,
                 t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, crash ? "-crash" : "");
        ok = save_share_write(dir, stem, zip, zn, made->path, sizeof made->path);
        if (ok) rt_log("bug report: %s written (%zu bytes)", made->path, zn);
        else rt_log("bug report: cannot write the report into %s", dir);
    }
    if (!zip) rt_log("bug report: out of memory for the report");
    made->bytes = zn;
    if (s.p) {
        size_text(zn, sz, sizeof sz);
        snprintf(made->summary, sizeof made->summary, "%sPacked: %s\n", s.p, sz);
    }
    free(s.p);
    free(zip);
    return ok;
}

// ---- sending
const char *bug_report_url(void) {
    const char *u = cfg_get("NX2NAVI_REPORT_URL");
    return u ? u : "";
}

static int clean_value(const char *s) {   // a value for curl's option file: no control characters
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) return 0;
    return 1;
}

static void put_quoted(Text *t, const char *opt, const char *value) {   // curl's option file: a quoted value
    put(t, "%s = \"", opt);
    for (const char *p = value; *p; p++) put(t, *p == '"' || *p == '\\' ? "\\%c" : "%c", *p);
    put(t, "\"\n");
}

int bug_report_send(const char *zip, char *answer, size_t cap) {
    const char *url = bug_report_url(), *key = cfg_get("NX2NAVI_REPORT_KEY");
    if (!*url || (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) || !clean_value(url) ||
        (key && !clean_value(key))) {
        snprintf(answer, cap, *url ? "report_url is no http:// or https:// address" : "report_url is not set");
        return BUG_REPORT_NO_URL;
    }
    char conf[PLAT_PATH_MAX + 64], out[PLAT_PATH_MAX + 64], err[PLAT_PATH_MAX + 64];
    snprintf(conf, sizeof conf, "%s.curl", zip);
    snprintf(out, sizeof out, "%s.answer", zip);
    snprintf(err, sizeof err, "%s.error", zip);
    Text c = {0};
    put_quoted(&c, "url", url);
    if (key && *key) {
        char header[512];
        snprintf(header, sizeof header, "X-Report-Key: %s", key);
        put_quoted(&c, "header", header);
    }
    put_quoted(&c, "header", "Content-Type: application/zip");
    char data[PLAT_PATH_MAX + 8];
    snprintf(data, sizeof data, "@%s", zip);
    put_quoted(&c, "data-binary", data);
    put_quoted(&c, "output", out);
    put_quoted(&c, "stderr", err);
    put(&c, "silent\nshow-error\nconnect-timeout = 15\nmax-time = 280\n");
    plat_file_delete(out);
    plat_file_delete(err);
    int wrote = c.p && !c.failed && plat_file_write_atomic(conf, c.p, c.n);
    free(c.p);
    if (!wrote) {
        snprintf(answer, cap, "the options for curl could not be written next to the report");
        return BUG_REPORT_NO_CURL;
    }
    const char *argv[] = {"curl", "--config", conf, NULL};
    rt_log("bug report: sending %s to %s", zip, url);
    int code = plat_run(argv, 300000);
    plat_file_delete(conf);   // it holds the key
    uint64_t n = 0;
    uint64_t en = 0;
    char *body = (char *)plat_file_read(out, &n), *etext = (char *)plat_file_read(err, &en);
    if (etext) {
        char *e = realloc(etext, (size_t)en + 1);
        if (e) etext = e, etext[en] = 0;
        else free(etext), etext = NULL;
    }
    plat_file_delete(out);
    plat_file_delete(err);
    int r;
    char why[256] = "";
    if (etext) line_of(etext, why, sizeof why);
    if (code == -1) {
        snprintf(answer, cap, "curl could not be started (it is part of Linux and of Windows 10 and later)");
        r = BUG_REPORT_NO_CURL;
    } else if (code != 0) {
        snprintf(answer, cap, "%s did not answer (curl %s %d%s%s)", url, code == -2 ? "stopped after" : "exit code",
                 code == -2 ? 300 : code, *why ? ": " : "", why);
        r = BUG_REPORT_UNREACHED;
    } else {
        char *text = body ? realloc(body, (size_t)n + 1) : NULL;
        if (text) body = text, body[n] = 0;
        char issue[512] = "", error[256] = "";
        if (body && sz_json_str(body, "issue_url", issue, sizeof issue) && *issue) {
            snprintf(answer, cap, "%s", issue);
            r = BUG_REPORT_SENT;
        } else if (body && sz_json_str(body, "error", error, sizeof error)) {
            snprintf(answer, cap, "the receiver refused it: %s", error);
            r = BUG_REPORT_REFUSED;
        } else {
            snprintf(answer, cap, "%s answered, but not as a report receiver", url);
            r = BUG_REPORT_ANSWER;
        }
    }
    free(body);
    free(etext);
    rt_log("bug report: %s: %s", r == BUG_REPORT_SENT ? "sent, the issue is" : "not sent", answer);
    return r;
}
