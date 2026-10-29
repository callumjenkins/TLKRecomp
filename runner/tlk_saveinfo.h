#pragma once
/* The sidecar beside a savestate: NAME.json (and NAME.png) next to NAME.state, saying which build wrote it,
 * when, and where in the game. The state file itself is unchanged, so states from builds without sidecars
 * still load. */
#include <stddef.h>

typedef struct {
    char build[96];         /* what a load compares: the code hash and the data layout tag */
    char version[64];       /* git describe, a label only */
    long frame;
    long long saved_unix;   /* seconds since 1970, UTC */
    int level;              /* $7FFF9E */
    char level_name[32];
    int lives;              /* $7FFFAA */
    char device[64];        /* the machine that wrote it; may be empty */
    char state_sha256[65];  /* hex, of NAME.state as written */
} TlkSaveInfo;

/* NAME.ext for NAME.state; a path without ".state" gets ext appended. */
void tlk_saveinfo_path(const char *state_path, const char *ext, char *out, size_t n);
/* Writes the json through .tmp and a rename. Returns 1 on success. */
int tlk_saveinfo_write(const char *state_path, const TlkSaveInfo *info);
/* Returns 1 when the json exists and has a build; `version` may be missing. */
int tlk_saveinfo_read(const char *state_path, TlkSaveInfo *info);
void tlk_saveinfo_remove(const char *state_path);

enum { TLK_SAVE_NO_INFO, TLK_SAVE_THIS_BUILD, TLK_SAVE_OTHER_BUILD };
/* Whose state it is, by its sidecar; `info` gets the sidecar when there is one. */
int tlk_saveinfo_check(const char *state_path, const char *build, TlkSaveInfo *info);
