// The menu's bug report (report_page.h). Send a bug report takes a picture of the frame under the menu (one frame drawn
// without it), gathers the report on a thread of its own and shows what goes; Send packs it again with the player's
// note, writes it into <profile>/reports and sends it (bug_report.c) on another thread, so the game never waits for
// the disk or the network. The start after a fatal error opens the menu on this window by itself, with that run's log
// and crash report in the report and no picture (the frame of a new start shows nothing of the crash).
#include <atomic>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "imgui_internal.h"
#include "menu_sound.h"
#include "ui/menu_skin.h"
#include "ui/option_rows.h"
#include "ui/report_page.h"

extern "C" {
#include "bug_report.h"
#include "platform/platform.h"
#include "resume.h"
#include "ui/menu.h"
#include "ui_text.h"
void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rt_fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
}

enum State { S_CLOSED, S_SHOT, S_SHOT_WAIT, S_GATHER, S_READY, S_SENDING, S_DONE };
static std::atomic<int> g_state{S_CLOSED};
static PlatMutex g_lock = PLAT_MUTEX_INIT;   // g_made, g_result*, g_kept
static BugReportMade g_made;                 // the preview: what goes
static int g_result = -1;                    // the outcome's UiTextId, a format with g_result_arg
static char g_result_arg[1024], g_kept[1200];
static bool g_result_warn;
static bool g_crash;                         // this report carries the last run's fatal error
static std::atomic<bool> g_crash_gone;       // ... and it went (sent or saved): the row says no more of it
static std::atomic<int> g_gen;               // each opening: a gather of an earlier one drops its preview
static std::atomic<bool> g_cursor_due;       // the outcome is shown: its Close button takes the cursor
static bool g_window_shown, g_fresh, g_test_send;
static char g_note[512], g_shot[1200];
static uint64_t g_shot_frame, g_first_frame;
static bool g_offered;

static bool crash_waiting(void) {
    char fatal[8];
    return bug_report_last_crash(fatal, sizeof fatal) && !g_crash_gone;
}

static void open_report(bool crash) {
    int s = g_state.load();
    if (s != S_CLOSED && s != S_READY && s != S_DONE) return;   // one gathers or sends already
    g_crash = crash;
    g_note[0] = g_shot[0] = 0;
    g_fresh = true;
    plat_mutex_lock(&g_lock);
    g_made.summary[0] = 0;
    g_result = -1;
    plat_mutex_unlock(&g_lock);
    g_gen++;
    g_shot_frame = 0;   // without a picture the gather starts at the present thread's next frame (report_frame)
    g_state.store(crash ? S_SHOT_WAIT : S_SHOT);
    rt_log("bug report: the window opens%s", crash ? " with the last run's fatal error" : "");
}

static void gather_main(void *arg) {
    const int gen = (int)(intptr_t)arg;
    char dir[1024];
    BugReportWhat what = {g_crash, "", g_shot[0] ? g_shot : nullptr};
    BugReportMade *made = new BugReportMade;
    bool ok = bug_report_dir(dir, sizeof dir) && bug_report_make(dir, &what, 1, made);
    plat_mutex_lock(&g_lock);
    int expect = S_GATHER;
    if (gen == g_gen.load() && g_state.compare_exchange_strong(expect, S_READY)) {
        if (ok) memcpy(&g_made, made, sizeof *made);
        else snprintf(g_made.summary, sizeof g_made.summary, "%s", "(the report could not be gathered; the log says why)");
    }
    plat_mutex_unlock(&g_lock);
    delete made;
}

static void say(UiTextId text, const char *arg, bool warn, const char *kept) {
    plat_mutex_lock(&g_lock);
    g_result = text;
    snprintf(g_result_arg, sizeof g_result_arg, "%s", arg ? arg : "");
    snprintf(g_kept, sizeof g_kept, "%s", kept ? kept : "");
    g_result_warn = warn;
    plat_mutex_unlock(&g_lock);
}

static void send_main(void *arg) {
    (void)arg;
    char dir[1024] = "", answer[1024];
    BugReportWhat what = {g_crash, g_note, g_shot[0] ? g_shot : nullptr};
    BugReportMade *made = new BugReportMade;
    if (!bug_report_dir(dir, sizeof dir) || !bug_report_make(dir, &what, 0, made)) {
        say(UI_MENU_REPORT_E_SAVE, dir, true, nullptr);
    } else {
        if (g_shot[0]) plat_file_delete(g_shot);   // the picture is in the zip now
        if (g_crash) g_crash_gone = true;
        if (!*bug_report_url()) say(UI_MENU_REPORT_SAVED, made->path, false, nullptr);
        else if (bug_report_send(made->path, answer, sizeof answer) == BUG_REPORT_SENT)
            say(UI_MENU_REPORT_SENT, answer, false, nullptr);
        else say(UI_MENU_REPORT_E_SEND, answer, true, made->path);
    }
    delete made;
    g_cursor_due = true;
    g_state.store(S_DONE);
}

static void start_thread(void (*fn)(void *), State busy) {
    g_state.store(busy);
    if (!plat_thread_spawn(fn, (void *)(intptr_t)g_gen.load(), "bug report", 0)) {
        say(UI_MENU_REPORT_E_SAVE, "", true, nullptr);
        g_cursor_due = true;
        g_state.store(S_DONE);
    }
}

const char *report_frame(uint64_t frame, bool *open_menu) {
    *open_menu = false;
    if (!g_first_frame) g_first_frame = frame;
    // the start after a fatal error: once the game shows its first frames (two seconds in), and not under the box of a
    // restart in place
    if (!g_offered && frame >= g_first_frame + 120 && !resume_booting()) {
        g_offered = true;
        if (crash_waiting() && g_state.load() == S_CLOSED) {
            open_report(true);
            *open_menu = true;
        }
    }
    int s = g_state.load();
    if (s == S_SHOT) {
        char dir[1024];
        if (bug_report_dir(dir, sizeof dir)) {
            plat_file_mkdir_p(dir);
            snprintf(g_shot, sizeof g_shot, "%s/frame-%llu.png", dir, (unsigned long long)frame);
        }
        g_shot_frame = frame;
        g_state.store(S_SHOT_WAIT);
        return g_shot[0] ? g_shot : nullptr;
    }
    // the shot frame's picture is written at the end of its present, before this next frame
    if (s == S_SHOT_WAIT && frame > g_shot_frame) start_thread(gather_main, S_GATHER);
    if (s == S_READY && g_test_send) {
        g_test_send = false;
        start_thread(send_main, S_SENDING);
    }
    return nullptr;
}

void report_rows(void) {
    opt_group(UI_MENU_REPORT_GROUP);
    const int s = g_state.load();
    const bool busy = s == S_SHOT || s == S_SHOT_WAIT || s == S_GATHER || s == S_SENDING;
    if (busy) opt_disable_next(UI_MENU_SAVE_BUSY);
    const bool crash = crash_waiting();
    if (opt_button("##report_send", UI_MENU_REPORT_SEND, UI_MENU_REPORT_SEND_DESC,
                   crash ? menu_text(UI_MENU_REPORT_LAST_CRASH) : "") && !busy)
        open_report(crash);
}

bool report_modal(void) { return g_window_shown; }

void report_cancel(void) {
    int s = g_state.load();
    if (s == S_READY || s == S_DONE) {
        if (s == S_READY && g_shot[0]) plat_file_delete(g_shot);   // not sent: no picture stays behind
        g_state.store(S_CLOSED);
    }
}

static void take_cursor(void) {
    ImGui::SetItemDefaultFocus();
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
}

void report_window(float scale) {
    g_window_shown = false;
    const int s = g_state.load();
    if (s == S_CLOSED || s == S_SHOT) return;
    if (!ImGui::IsPopupOpen("##report")) ImGui::OpenPopup("##report");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0);
    const bool shown = ImGui::BeginPopupModal("##report", nullptr,
                                              ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar();
    if (!shown) return;
    g_window_shown = true;
    const bool fresh = g_fresh;
    g_fresh = false;
    const ImVec2 box = ImGui::GetWindowPos(), box_size = ImGui::GetWindowSize();
    skin_panel(ImGui::GetWindowDrawList(), box, ImVec2(box.x + box_size.x, box.y + box_size.y), scale);
    const float wrap = ImGui::GetFontSize() * 36;
    const ImGuiStyle &st = ImGui::GetStyle();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(SKIN_GOLD));
    ImGui::TextUnformatted(menu_text(g_crash ? UI_MENU_REPORT_CRASH_TITLE : UI_MENU_REPORT_TITLE));
    ImGui::PopStyleColor();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrap);
    if (g_crash) ImGui::TextUnformatted(menu_text(UI_MENU_REPORT_CRASH_TEXT));
    bool close = false;
    char line[1400];
    if (s == S_SHOT_WAIT || s == S_GATHER) {
        ImGui::TextDisabled("%s", menu_text(UI_MENU_REPORT_GATHERING));
        if (skin_widget_button(menu_text(UI_MENU_REPORT_CANCEL), ImVec2(0, 0), scale)) close = true;
    } else if (s == S_READY || s == S_SENDING) {
        ImGui::TextUnformatted(menu_text(UI_MENU_REPORT_WHAT));
        ImGui::BeginChild("##report_list", ImVec2(wrap, ImGui::GetTextLineHeightWithSpacing() * 13),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_NavFlattened);
        plat_mutex_lock(&g_lock);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(g_made.summary);
        ImGui::PopTextWrapPos();
        plat_mutex_unlock(&g_lock);
        ImGui::EndChild();
        ImGui::TextUnformatted(menu_text(UI_MENU_REPORT_NOTE));
        ImGui::SetNextItemWidth(wrap);
        ImGui::BeginDisabled(s == S_SENDING);
        ImGui::InputText("##report_note", g_note, sizeof g_note);
        ImGui::EndDisabled();
        const char *url = bug_report_url();
        if (*url) {
            snprintf(line, sizeof line, menu_text(UI_MENU_REPORT_TO), url);
            ImGui::TextDisabled("%s", line);
        } else {
            ImGui::TextDisabled("%s", menu_text(UI_MENU_REPORT_NO_URL));
        }
        ImGui::Spacing();
        if (s == S_SENDING) {
            ImGui::TextDisabled("%s", menu_text(UI_MENU_REPORT_SENDING));
        } else {
            const char *no = menu_text(UI_MENU_REPORT_CANCEL), *yes = menu_text(*url ? UI_MENU_REPORT_DO_SEND : UI_MENU_REPORT_DO_SAVE);
            float bw = fmaxf(ImGui::CalcTextSize(no).x, ImGui::CalcTextSize(yes).x) + st.FramePadding.x * 4;
            if (skin_widget_button(no, ImVec2(bw, 0), scale)) close = true;
            if (fresh) take_cursor();   // on the safe button: a press of A right after opening sends nothing
            ImGui::SameLine(0, st.ItemSpacing.x + skin_cursor_reach(scale));
            snprintf(line, sizeof line, "%s##report_yes", yes);
            if (skin_widget_button(line, ImVec2(bw, 0), scale)) {
                menu_sound_play(MENU_SOUND_DECIDE);
                start_thread(send_main, S_SENDING);
            }
        }
    } else {   // S_DONE
        plat_mutex_lock(&g_lock);
        bool warn = g_result_warn;
        if (g_result >= 0) snprintf(line, sizeof line, menu_text((UiTextId)g_result), g_result_arg);
        else line[0] = 0;
        char kept[1400] = "";
        if (g_kept[0]) snprintf(kept, sizeof kept, menu_text(UI_MENU_REPORT_KEPT), g_kept);
        plat_mutex_unlock(&g_lock);
        if (warn) ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.35f, 1), "%s", line);
        else ImGui::TextUnformatted(line);
        if (kept[0]) ImGui::TextUnformatted(kept);
        ImGui::Spacing();
        if (skin_widget_button(menu_text(UI_MENU_REPORT_CLOSE), ImVec2(0, 0), scale)) close = true;
        if (g_cursor_due.exchange(false)) take_cursor();
    }
    ImGui::PopTextWrapPos();
    if (!fresh && !ImGui::IsAnyItemActive() && s != S_SENDING &&
        (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        close = true;
        menu_sound_play(MENU_SOUND_CANCEL);
    }
    if (close) {
        // a gather still running finishes into a closed window: its preview is dropped with the next opening
        if (g_shot[0] && s != S_DONE) plat_file_delete(g_shot);
        g_state.store(S_CLOSED);
        ImGui::CloseCurrentPopup();
        opt_focus_key("##report_send");
    }
    ImGui::EndPopup();
}

int report_test_set(const char *key, const char *value) {
    if (!strcmp(key, "NX2NAVI_REPORT_OPEN")) {
        if (strcmp(value, "row")) open_report(crash_waiting());
        return 2;
    }
    if (!strcmp(key, "NX2NAVI_REPORT_NOTE")) {
        snprintf(g_note, sizeof g_note, "%s", value);
        return 1;
    }
    if (!strcmp(key, "NX2NAVI_REPORT_SEND")) {
        g_test_send = true;   // report_frame presses Send once what goes is shown
        return 1;
    }
    if (!strcmp(key, "NX2NAVI_CRASH")) {
        if (!strcmp(value, "segv")) {
            rt_log("menu: test hook: a crash on purpose (CRASH=segv)");
            *(volatile int *)(uintptr_t)8 = 1;
        }
        rt_fatal("menu: test hook: a fatal error on purpose (CRASH=%s)", value);
    }
    return 0;
}
