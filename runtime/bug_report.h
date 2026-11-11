// Bug reports (bug_report.c): the menu's Send a bug report makes one zip of what a developer needs and sends it to a
// report receiver (tools/report_server.py), which files it as an issue; after a fatal error the next start offers the
// crash's report. The zip is the shared save's (save_share.h) with more in bug-report/.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- the last run's crash. A fatal error (dispatch.c rt_fatal) or a crash (main.c crash_handler) leaves a note in the
// log folder naming its log; the next start reads it and offers that run's report.
void bug_report_start(void);     // main.c, a game run, after log_open: takes the last run's note, arms this run's
void bug_report_crashed(void);   // after the fatal error's lines are in the log: writes the note (no allocation)
// 1 when this start found the last run's note: fatal gets its first FATAL line ("" when the log has none), cut to cap.
int bug_report_last_crash(char *fatal, size_t cap);

// ---- the player's state, read on the game thread where the game updates its flags once a frame
// (restore_drawbridge.c, game 1.1.1): wanted while a report gathers; put gives the area index (System_CurrentBancIndex),
// 1 for a child, and the second of the game's day (-1 each when unknown).
int bug_report_state_wanted(void);
void bug_report_state_put(int64_t area, int64_t child, int day_second);

// ---- a report
typedef struct {
    int crash;              // the last run's crash goes with it: its log and its crash report
    const char *note;       // what the player wrote; "" or NULL none
    const char *picture;    // a PNG of the frame to include; NULL none
} BugReportWhat;

enum { BUG_REPORT_SUMMARY = 6144 };
typedef struct {
    char path[1200];                 // the zip written; "" for a preview
    size_t bytes;                    // the zip's size
    char summary[BUG_REPORT_SUMMARY];   // what goes, one line each: the key facts, then every file with its size
} BugReportMade;

// The folder reports are written into: <profile>/reports. 0 when unknown.
int bug_report_dir(char *out, size_t cap);
// Gathers the report and packs it. preview: only in memory, for the menu to show what goes (made->path ""); else
// written into dir. 1 on success, 0 when out of memory or when the zip could not be written. Any thread but the
// present thread (it waits for the game thread's answer on the player's state).
int bug_report_make(const char *dir, const BugReportWhat *what, int preview, BugReportMade *made);

// Sending the zip at zip to report_url with report_key, through curl. Waits for curl (a thread of its own; at most five
// minutes). answer gets the issue's address when sent, else why not (English, for the menu's line and the log).
enum {
    BUG_REPORT_SENT,
    BUG_REPORT_NO_URL,     // report_url is not set (or is no http:// or https:// address): the zip stays where it is
    BUG_REPORT_NO_CURL,    // curl could not be started
    BUG_REPORT_UNREACHED,  // curl ran but reached no receiver, or timed out
    BUG_REPORT_REFUSED,    // the receiver answered with an error (a wrong key, too large, too many reports)
    BUG_REPORT_ANSWER,     // something answered, but not a receiver
};
int bug_report_send(const char *zip, char *answer, size_t cap);
// The address the report goes to ("" when report_url is unset), for the menu.
const char *bug_report_url(void);

#ifdef __cplusplus
}
#endif
