// The menu's bug report window (runtime/ui/report_page.cpp) without the game: the page's own code, the menu's rows and
// shapes and Dear ImGui, driven frame by frame as menu.cpp's overlay_frame drives it (report_frame first, then on an
// opening report_cancel, then the menu's window with the report window over it). Checks that the offer after a fatal
// error, opened while the menu is closed, is still shown once the menu has opened (its gather ends before the menu's
// first frame); that a report the player saw and left waiting closes with the menu; that the test hook's Send sends
// once and its Not now sends nothing; and that the row opens the window with a picture.
// tests/report_page_check.py builds and runs it; the exit status is the number of failed checks.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../runtime/ui/report_page.cpp"   // its state is checked below
#include "ui/menu_art.h"
extern "C" {
#include "cfg.h"
}

// ---- what the page, the rows and the catalog ask of the runtime

static int last_crash, n_make, n_preview, n_send, n_deleted;
static char last_note[512], last_picture[1200];

extern "C" {
const CfgMeta *cfg_meta(const char *) { return NULL; }
const CfgMeta *cfg_meta_at(int) { return NULL; }
const char *cfg_get(const char *) { return NULL; }
const char *cfg_value(const char *) { return ""; }
const char *cfg_default(const char *) { return NULL; }
int cfg_int(const char *) { return 0; }
double cfg_double(const char *) { return 0; }
int cfg_bool(const char *) { return 0; }
int cfg_from_env(const char *) { return 0; }
int cfg_pending(const char *) { return 0; }
void cfg_set(const char *, const char *) {}
int cfg_save(void) { return 1; }
int cfg_reset(const char *const *, int) { return 0; }
unsigned cfg_version(void) { return 1; }
void menu_sound_play(MenuSound) {}
const char *ov_language(void) { return "en-US"; }
const char *plat_ui_language(void) { return "en"; }
uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
void rt_log(const char *, ...) {}
void rt_fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(99);
}
int menu_art(MenuArt, ImTextureID *, float *, float *) { return 0; }
void plat_mutex_lock(PlatMutex *) {}
void plat_mutex_unlock(PlatMutex *) {}
// the gather and the send run on the caller: done before the next frame, as a fast gather is in the game
int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *, size_t) { return fn(arg), 1; }
void plat_file_delete(const char *) { n_deleted++; }
void plat_file_mkdir_p(const char *) {}
int menu_is_open(void) { return 1; }
void gfx_window_text_input(int) {}
int resume_booting(void) { return 0; }

int bug_report_last_crash(char *fatal, size_t cap) {
    if (cap) snprintf(fatal, cap, "%s", last_crash ? "FATAL: on purpose" : "");
    return last_crash;
}
int bug_report_dir(char *out, size_t cap) { return snprintf(out, cap, "/profile/reports"), 1; }
int bug_report_make(const char *, const BugReportWhat *what, int preview, BugReportMade *made) {
    memset(made, 0, sizeof *made);
    n_make++;
    n_preview += preview;
    snprintf(last_note, sizeof last_note, "%s", what->note ? what->note : "");
    snprintf(last_picture, sizeof last_picture, "%s", what->picture ? what->picture : "");
    snprintf(made->summary, sizeof made->summary, "NX2navi test\nFiles:\n  bug-report/nx2navi.log  1 KB\n");
    if (!preview) snprintf(made->path, sizeof made->path, "/profile/reports/NX2navi-report.zip");
    return 1;
}
const char *bug_report_url(void) { return "http://127.0.0.1:8787/report"; }
int bug_report_send(const char *, char *answer, size_t cap) {
    n_send++;
    snprintf(answer, cap, "https://github.com/owner/repo/issues/1");
    return BUG_REPORT_SENT;
}
}

// ---- frames

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
        }                                                                                                              \
    } while (0)

static uint64_t g_frame = 1;
static bool menu_open, menu_reset;

static void set_menu(bool open) {
    if (open != menu_open) menu_reset = true;   // menu.cpp change_open: g_reset
    menu_open = open;
}

// One frame as menu.cpp overlay_frame runs it: report_frame first (its picture frame lays out nothing), the offer
// opening the menu; a closed menu lays out nothing and keeps its reset for the next opening, which first calls
// report_cancel; then the menu's window with the Saves tab's rows and the report window over it.
static void frame(void) {
    bool offer = false;
    const char *picture = report_frame(g_frame++, &offer);
    if (picture) return;
    if (offer) set_menu(true);
    if (!menu_open) return;
    if (menu_reset) {
        menu_reset = false;
        report_cancel();
        if (GImGui->OpenPopupStack.Size) ImGui::ClosePopupToLevel(0, false);
    }
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1 / 60.0f;
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(1200, 640), ImGuiCond_Always);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    opt_begin("##saves", 1);
    report_rows();
    opt_end();
    report_window(1);
    ImGui::End();
    ImGui::Render();
}

static void frames(int n) {
    for (int i = 0; i < n; i++) frame();
}

int main(void) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;   // as menu.cpp
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.Fonts->AddFontDefault();
    unsigned char *px;
    int w, h;
    io.Fonts->GetTexDataAsRGBA32(&px, &w, &h);

    // the start after a fatal error: two seconds in the offer opens the menu on the report, gathered at once
    last_crash = 1;
    frames(120);   // the first frame and the 119 after it
    CHECK(!menu_open && g_state.load() == S_CLOSED, "the offer came before two seconds of frames");
    frames(1);
    CHECK(menu_open, "the offer did not open the menu");
    CHECK(n_preview == 1 && !last_picture[0], "the offer's gather: %d previews, picture \"%s\"", n_preview, last_picture);
    frames(3);
    CHECK(g_state.load() == S_READY, "the offer's report is in state %d once the menu opened, not ready (%d)",
          g_state.load(), (int)S_READY);
    CHECK(report_modal(), "the offer's window is not shown once the menu opened");

    // the test hook's note and Send: one send, with the note, and the window says so
    report_test_set("NX2NAVI_REPORT_NOTE", "a note");
    report_test_set("NX2NAVI_REPORT_SEND", "1");
    frames(2);
    CHECK(n_send == 1 && !strcmp(last_note, "a note"), "Send: %d sends, note \"%s\"", n_send, last_note);
    CHECK(g_state.load() == S_DONE && g_result == UI_MENU_REPORT_SENT, "after Send: state %d, result %d", g_state.load(), g_result);
    CHECK(!crash_waiting(), "the sent offer still waits on the row");
    frames(3);
    CHECK(report_modal(), "the outcome is not shown");

    // the outcome seen, the menu closes and opens again: the window is gone
    set_menu(false);
    frames(3);
    set_menu(true);
    frames(3);
    CHECK(g_state.load() == S_CLOSED && !report_modal(), "a report seen and left stays over a new opening (state %d)",
          g_state.load());

    // the row with the menu open: a picture frame without the menu, then the gather with that picture
    last_crash = 0;
    const int sends = n_send, made = n_make;
    report_test_set("NX2NAVI_REPORT_OPEN", "1");
    frames(4);
    CHECK(n_make == made + 1 && strstr(last_picture, "/profile/reports/frame-"), "the row's gather: %d made, picture \"%s\"",
          n_make - made, last_picture);
    CHECK(report_modal() && g_state.load() == S_READY, "the row's window is not shown (state %d)", g_state.load());

    // Not now through the test hook: nothing is sent, the picture is deleted, the window closes
    const int deleted = n_deleted;
    report_test_set("NX2NAVI_REPORT_CANCEL", "1");
    frames(3);
    CHECK(n_send == sends && g_state.load() == S_CLOSED && !report_modal(), "Not now: %d sends, state %d",
          n_send - sends, g_state.load());
    CHECK(n_deleted == deleted + 1, "Not now deleted %d pictures", n_deleted - deleted);

    // the hook opening the window while the menu is closed (as the offer does): it shows at the opening
    set_menu(false);
    frames(3);
    report_test_set("NX2NAVI_REPORT_OPEN", "1");
    frames(3);   // the picture and the gather, the menu closed
    set_menu(true);
    frames(3);
    CHECK(report_modal() && g_state.load() == S_READY, "a report opened while the menu was closed is gone at its opening "
          "(state %d)", g_state.load());

    printf("report page: %s\n", failures ? "FAILED" : "ok");
    ImGui::DestroyContext();
    return failures;
}
