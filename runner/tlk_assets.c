/* tlk_assets.c -- where tlk_sdl finds its shader presets and menu font.
 *
 * They are looked up in assets/ next to the binary (SDL_GetBasePath: the
 * binary's folder, an AppImage's usr/bin, a .app's Contents/Resources). The
 * single Windows .exe has no folder, so the files are also compiled in
 * (cmake/TlkEmbedAssets.cmake); when assets/ is missing they are written to
 * the per-user folder (SDL_GetPrefPath: %APPDATA%\TLKRecomp on Windows) and
 * used from there. Files that already match are left alone. */
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

typedef struct { const char *path; const unsigned char *data; size_t size; } TlkAsset;
extern const TlkAsset g_tlk_assets[];
extern const size_t g_tlk_asset_count;

static int FileExists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

static int SameContent(const char *path, const TlkAsset *a) {
    size_t n = 0;
    void *data = SDL_LoadFile(path, &n);
    int same = data && n == a->size && !memcmp(data, a->data, n);
    SDL_free(data);
    return same;
}

/* Write the compiled-in files under dir. Returns 0 if any write failed. */
static int Unpack(const char *dir) {
    for (size_t i = 0; i < g_tlk_asset_count; i++) {
        const TlkAsset *a = &g_tlk_assets[i];
        char path[1200], parent[1200];
        snprintf(path, sizeof path, "%s%s", dir, a->path);
        if (SameContent(path, a)) continue;
        snprintf(parent, sizeof parent, "%s", path);
        char *sl = strrchr(parent, '/');
        if (sl) { *sl = 0; SDL_CreateDirectory(parent); }
        if (!SDL_SaveFile(path, a->data, a->size)) {
            fprintf(stderr, "assets: cannot write %s: %s\n", path, SDL_GetError());
            return 0;
        }
    }
    return 1;
}

/* The folder that holds assets/, with a trailing separator ("" if unknown). */
const char *TlkAssetDir(void) {
    static char dir[1100];
    static int done;
    if (done) return dir;
    done = 1;
    const char *base = SDL_GetBasePath();
    snprintf(dir, sizeof dir, "%s", base ? base : "");
    char probe[1200];
    snprintf(probe, sizeof probe, "%s%s", dir, g_tlk_asset_count ? g_tlk_assets[0].path : "");
    if (!g_tlk_asset_count || FileExists(probe)) return dir;
    char *pref = SDL_GetPrefPath("", "TLKRecomp");
    if (pref && Unpack(pref)) {
        snprintf(dir, sizeof dir, "%s", pref);
        fprintf(stderr, "assets: using %sassets\n", dir);
    }
    SDL_free(pref);
    return dir;
}

/* Path of an asset such as "assets/shaders/crt-soft.glslp". */
void TlkAssetPath(const char *rel, char *out, size_t n) {
    snprintf(out, n, "%s%s", TlkAssetDir(), rel);
}
