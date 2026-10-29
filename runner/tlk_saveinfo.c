#include "tlk_saveinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tlk_saveinfo_path(const char *state_path, const char *ext, char *out, size_t n) {
    size_t len = strlen(state_path), sl = strlen(".state");
    if (len >= sl && !strcmp(state_path + len - sl, ".state")) len -= sl;
    snprintf(out, n, "%.*s%s", (int)len, state_path, ext);
}

static void PutString(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fputc('\\', f);
        fputc(*s, f);
    }
    fputc('"', f);
}

int tlk_saveinfo_write(const char *state_path, const TlkSaveInfo *info) {
    char path[4096], tmp[4096];
    tlk_saveinfo_path(state_path, ".json", path, sizeof path);
    tlk_saveinfo_path(state_path, ".json.tmp", tmp, sizeof tmp);
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    fputs("{\"build\": ", f);
    PutString(f, info->build);
    fputs(", \"version\": ", f);
    PutString(f, info->version);
    fprintf(f, ", \"frame\": %ld, \"saved_unix\": %lld, \"level\": %d, \"level_name\": ", info->frame,
            info->saved_unix, info->level);
    PutString(f, info->level_name);
    fprintf(f, ", \"lives\": %d}\n", info->lives);
    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok) {
        remove(path);   /* Windows' rename does not replace */
        ok = rename(tmp, path) == 0;
    }
    if (!ok) remove(tmp);
    return ok;
}

/* The text after "key": with spaces skipped, or NULL. */
static const char *Find(const char *json, const char *key) {
    char want[48];
    snprintf(want, sizeof want, "\"%s\"", key);
    const char *p = strstr(json, want);
    if (!p) return NULL;
    p += strlen(want);
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (*p++ != ':') return NULL;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static int GetString(const char *json, const char *key, char *out, size_t n) {
    const char *p = Find(json, key);
    if (!p || *p++ != '"' || !n) return 0;
    size_t i = 0;
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) p++;
        if (i + 1 < n) out[i++] = *p;
    }
    out[i] = 0;
    return *p == '"';
}

static long long GetNumber(const char *json, const char *key) {
    const char *p = Find(json, key);
    return p ? strtoll(p, NULL, 10) : 0;
}

int tlk_saveinfo_read(const char *state_path, TlkSaveInfo *info) {
    char path[4096], json[2048];
    tlk_saveinfo_path(state_path, ".json", path, sizeof path);
    memset(info, 0, sizeof *info);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    size_t n = fread(json, 1, sizeof json - 1, f);
    fclose(f);
    json[n] = 0;
    if (!GetString(json, "build", info->build, sizeof info->build)) return 0;
    GetString(json, "version", info->version, sizeof info->version);
    GetString(json, "level_name", info->level_name, sizeof info->level_name);
    info->frame = (long)GetNumber(json, "frame");
    info->saved_unix = GetNumber(json, "saved_unix");
    info->level = (int)GetNumber(json, "level");
    info->lives = (int)GetNumber(json, "lives");
    return 1;
}

void tlk_saveinfo_remove(const char *state_path) {
    char path[4096];
    tlk_saveinfo_path(state_path, ".json", path, sizeof path);
    remove(path);
}

int tlk_saveinfo_check(const char *state_path, const char *build, TlkSaveInfo *info) {
    if (!tlk_saveinfo_read(state_path, info)) return TLK_SAVE_NO_INFO;
    return strcmp(info->build, build) ? TLK_SAVE_OTHER_BUILD : TLK_SAVE_THIS_BUILD;
}
