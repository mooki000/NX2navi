// Sharing a save (save_share.h). The zip, as the launcher's launcher/saves.py export_save writes it:
//
//   manifest.json   {"format": "nx2navi-save", "format_version": 1, "game", "title_id", "game_version", "nx2navi",
//                    "exported" (UTC), "exported_by", "files", "bug_report"}
//   save/...        the save's files as the game keeps them (account.sav, option.sav, slot_00/progress.sav, ...)
//   bug-report/...  with a bug report: this run's log and nx2navi.ini (secret settings left out); the menu's Send a bug
//                   report (bug_report.c) adds more there and "report" and "commit" to the manifest; an import leaves
//                   them out
//
// An import accepts a zip whose manifest has this format, a version up to FORMAT_VERSION and the game's title id, whose
// save/ paths stay inside the save, with no links, at most MAX_FILES files and MAX_BYTES, among them a file of this
// game's save (save_file). Every member's CRC-32 is checked before anything is written.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "folder_pick.h"
#include "fs.h"
#include "host.h"
#include "platform/platform.h"
#include "save_share.h"
#include "save_zip.h"

#define FORMAT "nx2navi-save"
#define FORMAT_VERSION 1
#define TITLE_ID "0400486024dec000"   // the game's title id (fs_host.c NX2NAVI_APPLICATION_ID)
#define GAME "The Legend of Zelda: Ocarina of Time"
#define MAX_FILES 4000
#define MAX_BYTES (256ull << 20)
#define MAX_ZIP (512ll << 20)
#define MANIFEST_MAX 65536

// The executable's folder, where nx2navi.ini, logs and saves are (cfg.c find_ini, main.c log_open). 0 when unknown.
static int exe_dir(char *out, size_t cap) {
    if (!plat_exe_path(out, cap)) return 0;
    char *cut = plat_last_sep(out), *slash = strrchr(out, '/');
    if (slash > cut) cut = slash;
    if (!cut) return 0;
    *cut = 0;
    return 1;
}

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int same_text(const char *a, const char *b) {   // ASCII, any case
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) a++, b++;
    return !*a && !*b;
}

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), k = strlen(tail);
    return n >= k && same_text(s + n - k, tail);
}

// Key files never go into a zip or out of one, wherever a stray copy sits in a save folder.
static int key_file(const char *rel) { return ends_with(rel, ".keys") || ends_with(rel, ".keys.txt"); }

// A file only this game's save has: what makes the zip's save this game's.
static int save_file(const char *rel) {
    if (!strcmp(rel, "account.sav") || !strcmp(rel, "option.sav")) return 1;
    return !strncmp(rel, "slot_", 5) && rel[5] >= '0' && rel[5] <= '9' && rel[6] >= '0' && rel[6] <= '9' &&
           !strcmp(rel + 7, "/progress.sav");
}

// A relative path that stays inside the save: '/' separators, no empty, "." or ".." part, no drive or backslash.
static int safe_rel(const char *rel) {
    if (!*rel || *rel == '/' || strchr(rel, '\\') || strchr(rel, ':')) return 0;
    for (const char *p = rel;;) {
        const char *slash = strchr(p, '/');
        size_t k = slash ? (size_t)(slash - p) : strlen(p);
        if (!k || (k == 1 && p[0] == '.') || (k == 2 && p[0] == '.' && p[1] == '.')) return 0;
        if (!slash) return 1;
        p = slash + 1;
    }
}

int save_share_default_dir(char *out, size_t cap) { return folder_downloads(out, cap) || exe_dir(out, cap); }

// ---- secrets: settings whose values never leave the machine in a zip, a bug report or its system text
static int has_text(const char *s, const char *part) {   // ASCII, any case
    for (size_t k = strlen(part); *s; s++) {
        size_t i = 0;
        while (i < k && s[i] && lower((unsigned char)s[i]) == lower((unsigned char)part[i])) i++;
        if (i == k) return 1;
    }
    return 0;
}

int save_share_secret(const char *key) {
    if (!strncmp(key, "NX2NAVI_", 8) || !strncmp(key, "nx2navi_", 8)) key += 8;
    return same_text(key, "REPORT_KEY") || has_text(key, "SECRET") || has_text(key, "TOKEN") ||
           has_text(key, "PASSWORD") || has_text(key, "PASSPHRASE") ||
           ((!strncmp(key, "COOP", 4) || !strncmp(key, "coop", 4)) && has_text(key, "KEY"));
}

char *save_share_clean_ini(const char *text, size_t n, size_t *out_n) {
    static const char gone[] = " (left out)";
    size_t lines = 1;
    for (size_t i = 0; i < n; i++) lines += text[i] == '\n';
    char *out = malloc(n + lines * (sizeof gone - 1) + 1);   // each line may get the note in place of its value
    if (!out) return NULL;
    size_t o = 0;
    for (size_t at = 0; at < n;) {
        size_t end = at;
        while (end < n && text[end] != '\n') end++;
        // "key = value": the key as cfg.c reads it, trimmed, before the first '='
        size_t k = at;
        while (k < end && (text[k] == ' ' || text[k] == '\t')) k++;
        size_t eq = k;
        while (eq < end && text[eq] != '=') eq++;
        char key[96];
        size_t kn = eq - k;
        while (kn && (text[k + kn - 1] == ' ' || text[k + kn - 1] == '\t')) kn--;
        int secret = 0;
        if (eq < end && kn && kn < sizeof key && text[k] != '#' && text[k] != ';') {
            memcpy(key, text + k, kn);
            key[kn] = 0;
            secret = save_share_secret(key);
        }
        if (secret) {
            memcpy(out + o, text + at, eq + 1 - at);   // the key and its '=' stay, so the setting reads as set
            o += eq + 1 - at;
            memcpy(out + o, gone, sizeof gone - 1);
            o += sizeof gone - 1;
        } else {
            memcpy(out + o, text + at, end - at);
            o += end - at;
        }
        if (end < n) out[o++] = '\n';
        at = end + 1;
    }
    out[o] = 0;
    *out_n = o;
    return out;
}

// ---- export
typedef struct {
    SzWriter w;
    int files;
} Export;

static int add_save_file(void *ctx, const char *rel, const uint8_t *data, size_t n) {
    Export *e = ctx;
    if (key_file(rel)) return 1;
    char name[PLAT_PATH_MAX];
    if (snprintf(name, sizeof name, "save/%s", rel) >= (int)sizeof name) return 0;
    sz_writer_add(&e->w, name, data, n);
    e->files++;
    return !e->w.failed;
}

static void add_file(SzWriter *w, const char *name, const char *path) {
    uint64_t n = 0;
    uint8_t *data = path ? plat_file_read(path, &n) : NULL;
    if (data) sz_writer_add(w, name, data, (size_t)n);
    free(data);
}

int save_share_add_save(SzWriter *w) {
    Export e;
    memset(&e, 0, sizeof e);
    e.w = *w;
    int n = fs_save_export(add_save_file, &e);
    *w = e.w;
    return n < 0 ? -1 : n == 0 || w->failed ? 0 : e.files;
}

void save_share_add_settings(SzWriter *w, const char *name) {
    char base[PLAT_PATH_MAX], file[PLAT_PATH_MAX + 32];
    if (!exe_dir(base, sizeof base)) return;
    snprintf(file, sizeof file, "%s/nx2navi.ini", base);
    uint64_t n = 0;
    uint8_t *data = plat_file_read(file, &n);
    size_t cn = 0;
    char *clean = data ? save_share_clean_ini((const char *)data, (size_t)n, &cn) : NULL;
    if (clean) sz_writer_add(w, name, clean, cn);
    free(clean);
    free(data);
}

int save_share_manifest(char *out, size_t cap, int files, int bug_report, const char *extra) {
    struct tm u;
    plat_gmtime((int64_t)(plat_unix_time_ns() / 1000000000u), &u);
    char ver[96];
    sz_json_escape(rt_port_version, ver, sizeof ver);
    int n = snprintf(out, cap,
                     "{\n \"format\": \"" FORMAT "\",\n \"format_version\": %d,\n \"game\": \"" GAME "\",\n"
                     " \"title_id\": \"" TITLE_ID "\",\n \"game_version\": \"%d.%d.%d\",\n \"nx2navi\": \"%s\",\n"
                     " \"exported\": \"%04d-%02d-%02dT%02d:%02d:%02dZ\",\n \"exported_by\": \"game\",\n%s"
                     " \"files\": %d,\n \"bug_report\": %s\n}\n",
                     FORMAT_VERSION, rt_game_version / 100, rt_game_version / 10 % 10, rt_game_version % 10, ver,
                     u.tm_year + 1900, u.tm_mon + 1, u.tm_mday, u.tm_hour, u.tm_min, u.tm_sec, extra ? extra : "",
                     files, bug_report ? "true" : "false");
    return n < 0 || (size_t)n >= cap ? -1 : n;
}

int save_share_write(const char *dir, const char *stem, const uint8_t *zip, size_t zn, char *path, size_t cap) {
    plat_file_mkdir_p(dir);
    for (int k = 1; k < 100; k++) {   // two exports in one second: "(2)" as the launcher names them
        int len = k == 1 ? snprintf(path, cap, "%s/%s.zip", dir, stem) : snprintf(path, cap, "%s/%s (%d).zip", dir, stem, k);
        if (len < 0 || (size_t)len >= cap) break;
        if (plat_fs_kind(path) != PLAT_FS_MISSING) continue;
        if (plat_file_write_atomic(path, zip, zn)) return 1;
        break;
    }
    *path = 0;
    return 0;
}

SaveShareResult save_share_export(const char *dir, int bug_report, char *path, size_t cap) {
    *path = 0;
    struct tm t;
    plat_local_time(&t);
    SzWriter w;
    sz_writer_init(&w, t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    int files = save_share_add_save(&w);
    if (files <= 0) {
        size_t unused;
        free(sz_writer_finish(&w, &unused));
        rt_log("save share: export: %s", files < 0 ? "a save file could not be read" : "the game has no save yet");
        return files < 0 ? SAVE_SHARE_READ_FAILED : SAVE_SHARE_NO_SAVE;
    }
    if (bug_report) {
        add_file(&w, "bug-report/nx2navi.log", rt_log_path());
        save_share_add_settings(&w, "bug-report/nx2navi.ini");
    }
    char manifest[1024];
    int mn = save_share_manifest(manifest, sizeof manifest, files, bug_report, NULL);
    if (mn > 0) sz_writer_add(&w, "manifest.json", manifest, (size_t)mn);
    size_t zn = 0;
    uint8_t *zip = sz_writer_finish(&w, &zn);
    if (!zip || mn <= 0) {
        free(zip);
        rt_log("save share: export: out of memory for the zip");
        return SAVE_SHARE_WRITE_FAILED;
    }
    char stem[64];
    snprintf(stem, sizeof stem, "NX2navi-save-%04d-%02d-%02d_%02d-%02d-%02d%s", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, bug_report ? "-bug-report" : "");
    int ok = save_share_write(dir, stem, zip, zn, path, cap);
    free(zip);
    if (!ok) {
        rt_log("save share: export: cannot write the zip into %s", dir);
        return SAVE_SHARE_WRITE_FAILED;
    }
    rt_log("save share: exported %d save files%s to %s (%zu bytes)", files, bug_report ? " with the log and settings" : "",
           path, zn);
    return SAVE_SHARE_OK;
}

// ---- import
static const SzEntry *find_member(const SzEntry *e, int count, const char *name) {
    for (int i = 0; i < count; i++)
        if (!strcmp(e[i].name, name)) return &e[i];
    return NULL;
}

// The manifest's verdict on the zip; detail gets its NX2navi version and export date.
static SaveShareResult check_manifest(const uint8_t *zip, size_t n, const SzEntry *e, int count, char *detail,
                                      size_t cap) {
    const SzEntry *m = find_member(e, count, "manifest.json");
    if (!m || m->usize > MANIFEST_MAX) return SAVE_SHARE_NOT_A_SAVE;
    int err;
    char *json = (char *)sz_extract(zip, n, m, &err);
    if (!json) return err == SZ_E_MEMORY ? SAVE_SHARE_READ_FAILED : SAVE_SHARE_DAMAGED;
    char format[32] = "", title[32] = "", ver[64] = "?", when[32] = "?";
    long long version = 0;
    SaveShareResult r = SAVE_SHARE_OK;
    if (!sz_json_str(json, "format", format, sizeof format) || strcmp(format, FORMAT) ||
        !sz_json_int(json, "format_version", &version) || version < 1)
        r = SAVE_SHARE_NOT_A_SAVE;
    else if (version > FORMAT_VERSION)
        r = SAVE_SHARE_NEWER;
    else if (!sz_json_str(json, "title_id", title, sizeof title) || !same_text(title, TITLE_ID))
        r = SAVE_SHARE_OTHER_GAME;
    sz_json_str(json, "nx2navi", ver, sizeof ver);
    sz_json_str(json, "exported", when, sizeof when);
    snprintf(detail, cap, "%s, %.10s", *ver ? ver : "?", when);
    free(json);
    return r;
}

SaveShareResult save_share_import(const char *zip_path, char *detail, size_t cap) {
    *detail = 0;
    int64_t size = plat_file_size(zip_path);
    if (size < 0) return SAVE_SHARE_READ_FAILED;
    if (size > MAX_ZIP) return SAVE_SHARE_UNSAFE;
    uint64_t n = 0;
    uint8_t *zip = plat_file_read(zip_path, &n);
    if (!zip) return size ? SAVE_SHARE_READ_FAILED : SAVE_SHARE_NOT_A_SAVE;
    SzEntry *e = NULL;
    int count = 0, zr = sz_read_dir(zip, (size_t)n, &e, &count);
    SaveShareResult r = zr == SZ_OK ? check_manifest(zip, (size_t)n, e, count, detail, cap)
                        : zr == SZ_E_NOT_ZIP ? SAVE_SHARE_NOT_A_SAVE
                        : zr == SZ_E_MEMORY  ? SAVE_SHARE_READ_FAILED
                                             : SAVE_SHARE_DAMAGED;
    // the save's members: checked all before any is extracted
    int files = 0, has_save = 0;
    uint64_t bytes = 0;
    for (int i = 0; r == SAVE_SHARE_OK && i < count; i++) {
        if (strncmp(e[i].name, "save/", 5) || e[i].is_dir) continue;
        const char *rel = e[i].name + 5;
        if (!safe_rel(rel) || e[i].is_link) r = SAVE_SHARE_UNSAFE;
        else if (key_file(rel)) continue;
        bytes += e[i].usize;
        if (++files > MAX_FILES || bytes > MAX_BYTES) r = SAVE_SHARE_UNSAFE;
        has_save |= save_file(rel);
    }
    if (r == SAVE_SHARE_OK && !has_save) r = SAVE_SHARE_NOT_A_SAVE;
    const char **rels = NULL;
    uint8_t **data = NULL;
    size_t *sizes = NULL;
    int k = 0;
    if (r == SAVE_SHARE_OK) {
        rels = calloc((size_t)files, sizeof *rels);
        data = calloc((size_t)files, sizeof *data);
        sizes = calloc((size_t)files, sizeof *sizes);
        if (!rels || !data || !sizes) r = SAVE_SHARE_READ_FAILED;
    }
    for (int i = 0; r == SAVE_SHARE_OK && i < count; i++) {
        if (strncmp(e[i].name, "save/", 5) || e[i].is_dir || key_file(e[i].name + 5)) continue;
        int err;
        data[k] = sz_extract(zip, (size_t)n, &e[i], &err);
        if (!data[k]) {
            r = err == SZ_E_MEMORY ? SAVE_SHARE_READ_FAILED : SAVE_SHARE_DAMAGED;
            break;
        }
        rels[k] = e[i].name + 5;
        sizes[k++] = (size_t)e[i].usize;
    }
    if (r == SAVE_SHARE_OK && fs_save_import_stage(k, rels, (const uint8_t *const *)data, sizes))
        r = SAVE_SHARE_WRITE_FAILED;
    for (int i = 0; i < k; i++) free(data[i]);
    free(rels), free(data), free(sizes), free(e), free(zip);
    static const char *const why[] = {"waits for the next start", "", "the zip cannot be read", "its save cannot be put in place",
                                      "not a zip of an NX2navi save", "a save of another game",
                                      "made by a newer NX2navi", "damaged", "paths outside the save, links or too large"};
    rt_log("save share: import of %s (NX2navi %s): %s", zip_path, *detail ? detail : "?", why[r]);
    return r;
}

int save_share_pending(void) { return fs_save_import_waiting(); }

int save_share_cancel(void) { return fs_save_import_cancel(); }
