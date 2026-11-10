// The menu's bug report (ui/report_page.cpp, bug_report.h): the Saves tab's Send a bug report row and the window it
// opens, which shows what goes before anything goes. Present path only (ImGui), but for the threads it starts.
#pragma once
#include <stdint.h>

// Inside the Saves page's opt_begin / opt_end: the Bug report group.
void report_rows(void);
// In the menu window after its pages, on every tab: the report's window while one is open.
void report_window(float scale);
// The window is open: B, Esc, L / R and the tab keys stay with it.
bool report_modal(void);
// The menu opened or closed (menu.cpp): a window that waits for the player closes; gathering and sending go on, and
// the window shows again when the menu opens.
void report_cancel(void);
// Each frame the overlay is made, before the menu is laid out. A path: the report's picture is this frame without the
// menu, to be written there (the caller passes it as the frame's shot and lays out nothing). *open_menu: the start
// after a fatal error offers that report, so the menu opens.
const char *report_frame(uint64_t frame, bool *open_menu);
// NX2NAVI_MENU_TEST_SET pseudo-keys (menu.cpp test_apply_sets): REPORT_OPEN=1 opens the window as the row does,
// REPORT_OPEN=row only shows the row, REPORT_NOTE=<text> writes the note, REPORT_SEND=1 presses Send (Save the report
// without report_url) once what goes is shown, CRASH=fatal or CRASH=segv ends the game with a fatal error or a crash
// (the start after it offers the report). 0: not one of them; 1: done; 2: done, and the menu opens on the Saves tab
// with the cursor on the row.
int report_test_set(const char *key, const char *value);
