// Sharing a save (save_share.c): the menu's Export writes the save the game would load next into one zip, for a bug
// report or another player, and its Import checks such a zip and leaves its save for the game's next start
// (fs_host.c import_pending). The launcher writes and reads the same zip (launcher/saves.py export_save, import_zip).
#pragma once
#include <stddef.h>

#include "save_zip.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SAVE_SHARE_OK,
    SAVE_SHARE_NO_SAVE,        // export: the game has not saved yet
    SAVE_SHARE_READ_FAILED,    // export: a save file could not be read; import: the zip could not be read
    SAVE_SHARE_WRITE_FAILED,   // export: the zip could not be written; import: its save could not be put in place
    SAVE_SHARE_NOT_A_SAVE,     // import: not a zip of an NX2navi save (no manifest, another format, no save files)
    SAVE_SHARE_OTHER_GAME,     // import: the manifest names another game
    SAVE_SHARE_NEWER,          // import: a newer NX2navi made it, in a format this one does not know
    SAVE_SHARE_DAMAGED,        // import: a member's data or CRC is wrong
    SAVE_SHARE_UNSAFE,         // import: a path outside the save, a link, or far more than a save
} SaveShareResult;

// Writes NX2navi-save-<local date>_<time>[-bug-report].zip into dir (made when missing); bug_report adds this run's
// log and nx2navi.ini. path gets the zip's path. Never a key file.
SaveShareResult save_share_export(const char *dir, int bug_report, char *path, size_t cap);
// Checks the zip at zip_path (manifest, game, paths, sizes, CRCs) and stages its save for the next start, which makes it
// a save file of its own (fs_host.c import_pending). detail gets
// "<NX2navi version>, <export date>" from its manifest ("?" for what it lacks).
SaveShareResult save_share_import(const char *zip_path, char *detail, size_t cap);
int save_share_pending(void);   // 1 while an import waits for the next start
int save_share_cancel(void);    // drops the waiting import; 0 on success
// Where Export writes and Import's file dialog opens: the player's Downloads folder, else the game folder. 0 when
// neither is known.
int save_share_default_dir(char *out, size_t cap);

// The pieces of Export, which a bug report's zip (bug_report.c) shares: the same zip with more in bug-report/.
// The save the game would load next into w as save/... (never a key file): the count of its files, 0 when the game has
// not saved yet, -1 when a save file could not be read.
int save_share_add_save(SzWriter *w);
// nx2navi.ini, next to the executable, into w as name with each secret setting's value left out (nothing when missing).
void save_share_add_settings(SzWriter *w, const char *name);
// manifest.json for a zip of files save files into out; extra: more top-level members, each "\"key\": value,\n"
// (NULL: none). Its length, -1 when it does not fit.
int save_share_manifest(char *out, size_t cap, int files, int bug_report, const char *extra);
// zip[0..n) written as dir/<stem>.zip (made when missing), "<stem> (2).zip" and so on when taken; path gets the file.
// 1 on success, else 0 with path "".
int save_share_write(const char *dir, const char *stem, const uint8_t *zip, size_t n, char *path, size_t cap);
// 1 for a setting whose value never leaves the machine (report_key, co-op keys, anything named a secret, token or
// password); key with or without NX2NAVI_, any case.
int save_share_secret(const char *key);
// text[0..n), an nx2navi.ini, with the value of each secret setting replaced by "(left out)": malloc'd and NUL
// terminated, *out_n bytes; NULL when out of memory.
char *save_share_clean_ini(const char *text, size_t n, size_t *out_n);

#ifdef __cplusplus
}
#endif
