// runtime/bug_report.c with save_share.c, save_zip.c, fs_host.c, save_summary.c and cfg.c, without the game, driven by
// tests/bug_report_check.py. The rest of the runtime is stubbed below. Each run is one start of the game:
//   bug_report_check crash                      bug_report_start, then bug_report_crashed: this run "fails"
//   bug_report_check make PROFILE DIR [PNG] [NOTE]   bug_report_start, a preview and a report written into DIR; prints
//                                                "preview N", "result N path P" and the summary after "summary:"
//   bug_report_check send ZIP                   bug_report_send; prints "send N answer A"
// Environment: BUG_REPORT_CHECK_INI (nx2navi.ini, loaded as the game loads it), BUG_REPORT_CHECK_LOG (this run's log),
// BUG_REPORT_CHECK_LOGDIR (the log folder, where the crash note goes).
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bug_report.h"
#include "cfg.h"
#include "platform/platform.h"

void fs_host_set_profile(const char *dir);

// ---- stubs for the runtime around these files
const int rt_game_version = 111;
const char rt_port_version[] = "9.9.9-check";
const char rt_port_commit[] = "0123456789ab-modified";
unsigned rt_trace = 32;   // RT_TRACE_PERF: the report carries the perf lines

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FATAL: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(99);
}

void rt_log_sync(void) {}
const char *rt_log_path(void) { return getenv("BUG_REPORT_CHECK_LOG"); }
static struct { const char *exefs, *romfs, *profile; } config;
const void *rt_config(void) { return &config; }
void account_uid(uint64_t uid[2]) { uid[0] = 0x1111222233334444ull, uid[1] = 0x5555666677778888ull; }
int folder_downloads(char *out, size_t cap) { (void)out, (void)cap; return 0; }
int frame_player_pos(float xyz[3]) {
    xyz[0] = 12.5f, xyz[1] = 3.25f, xyz[2] = -7.75f;
    return 1;
}
int64_t frame_step_world_us(void) { return 754000000; }
int sysinfo_text(char *buf, size_t n) {
    return snprintf(buf, n, "NX2navi system info\nOS: CheckOS 1.0\nCPU: Check CPU, 8 logical processors\n"
                            "RAM: 32.0 GB, 8.0 GB in use, the game 2.0 GB\nGPU: Check GPU (vendor 0x10de, device 0x2684)\n"
                            "Driver: CheckDriver 580.1\n");
}
int sysinfo_log_folder(char *out, size_t n) {
    const char *d = getenv("BUG_REPORT_CHECK_LOGDIR");
    return d && snprintf(out, n, "%s", d) < (int)n;
}

#define NEVER(name) \
    void name(void) { rt_fatal(#name " called"); }
NEVER(fs_init) NEVER(hostfn_override) NEVER(hostfn_register) NEVER(mem_kernel_alloc) NEVER(mem_kernel_free)
NEVER(mem_thaw) NEVER(romfs_close) NEVER(romfs_entry) NEVER(romfs_entry_type) NEVER(romfs_file_count)
NEVER(romfs_handle) NEVER(romfs_image_path) NEVER(romfs_list) NEVER(romfs_open) NEVER(romfs_overlay)
NEVER(romfs_overlay_count) NEVER(romfs_read) NEVER(romfs_size) NEVER(rt_call) NEVER(mem_probing)
int romfs_owns(const char *path) { (void)path; return 0; }
void fs_trace(const char *what, const char *path) { (void)what, (void)path; }
void resume_save_file(const char *path, int write) { (void)path, (void)write; }

// The game thread's answer: restore_drawbridge.c puts the state once a frame while a report wants it.
static void game_thread(void *arg) {
    (void)arg;
    for (;;) {
        if (bug_report_state_wanted()) bug_report_state_put(15, 1, 18 * 3600 + 20 * 60);
        plat_sleep_ms(5);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *ini = getenv("BUG_REPORT_CHECK_INI");
    if (ini) cfg_load(ini);
    const char *mode = argv[1];
    if (!strcmp(mode, "crash")) {
        bug_report_start();
        bug_report_crashed();
        printf("noted\n");
    } else if (!strcmp(mode, "make") && argc >= 4) {
        config.profile = argv[2];
        fs_host_set_profile(argv[2]);
        bug_report_start();
        char fatal[512];
        int crash = bug_report_last_crash(fatal, sizeof fatal);
        printf("crash %d fatal %s\n", crash, fatal);
        plat_thread_spawn(game_thread, NULL, "game", 0);
        BugReportWhat what = {crash, argc > 5 ? argv[5] : "", argc > 4 && *argv[4] ? argv[4] : NULL};
        static BugReportMade made;
        printf("preview %d bytes %zu path [%s]\n", bug_report_make(argv[3], &what, 1, &made), made.bytes, made.path);
        int ok = bug_report_make(argv[3], &what, 0, &made);
        printf("result %d path %s\nsummary:\n%s", ok, made.path, made.summary);
    } else if (!strcmp(mode, "send") && argc == 3) {
        char answer[1024];
        int r = bug_report_send(argv[2], answer, sizeof answer);
        printf("send %d answer %s\n", r, answer);
    } else {
        return 2;
    }
    fflush(stdout);
    _Exit(0);   // the game thread above never ends
}
