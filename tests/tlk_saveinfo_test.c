/* The savestate sidecar (tlk_saveinfo.c), in a scratch directory given as argv[1]. */
#include "tlk_saveinfo.h"

#include <stdio.h>
#include <string.h>

static int s_failed;
#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (!(cond)) {                                                        \
            s_failed++;                                                       \
            fprintf(stderr, "%s:%d: %s: %s\n", __FILE__, __LINE__, #cond, what); \
        }                                                                     \
    } while (0)

static char s_dir[1024];

static void At(const char *name, char *out, size_t n) { snprintf(out, n, "%s/%s", s_dir, name); }

static int Exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

static void WriteText(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

static TlkSaveInfo Sample(void) {
    TlkSaveInfo s;
    memset(&s, 0, sizeof s);
    snprintf(s.build, sizeof s.build, "0123456789ab-Linux-x86_64");
    snprintf(s.version, sizeof s.version, "v0.3.1-9-g1725500-dirty");
    s.frame = 6000;
    s.saved_unix = 1792800000LL;
    s.level = 9;
    snprintf(s.level_name, sizeof s.level_name, "Pride Rock");
    s.lives = 3;
    return s;
}

static void saveinfo_path_replaces_state_extension_or_appends(void) {
    char out[256];
    tlk_saveinfo_path("/a/tlk_slot2.state", ".json", out, sizeof out);
    CHECK(!strcmp(out, "/a/tlk_slot2.json"), out);
    tlk_saveinfo_path("/a/b.state.bak", ".png", out, sizeof out);
    CHECK(!strcmp(out, "/a/b.state.bak.png"), out);
    tlk_saveinfo_path("x", ".json", out, sizeof out);
    CHECK(!strcmp(out, "x.json"), out);
}

static void saveinfo_write_then_read_round_trips_every_field(void) {
    char state[1100];
    At("rt.state", state, sizeof state);
    TlkSaveInfo w = Sample(), r;
    CHECK(tlk_saveinfo_write(state, &w), "written");
    memset(&r, 0x55, sizeof r);
    CHECK(tlk_saveinfo_read(state, &r), "read back");
    CHECK(!strcmp(r.build, w.build), r.build);
    CHECK(!strcmp(r.version, w.version), r.version);
    CHECK(r.frame == 6000 && r.saved_unix == 1792800000LL && r.level == 9 && r.lives == 3, "numbers");
    CHECK(!strcmp(r.level_name, "Pride Rock"), r.level_name);
    char tmp[1100];
    tlk_saveinfo_path(state, ".json.tmp", tmp, sizeof tmp);
    CHECK(!Exists(tmp), "no .tmp left behind");
}

static void saveinfo_quotes_in_names_survive(void) {
    char state[1100];
    At("q.state", state, sizeof state);
    TlkSaveInfo w = Sample(), r;
    snprintf(w.level_name, sizeof w.level_name, "a \"b\" \\c");
    CHECK(tlk_saveinfo_write(state, &w), "written");
    CHECK(tlk_saveinfo_read(state, &r) && !strcmp(r.level_name, w.level_name), r.level_name);
}

static void saveinfo_missing_json_reads_as_none_and_a_cut_short_one_by_its_build(void) {
    char state[1100], json[1100];
    TlkSaveInfo r;
    At("none.state", state, sizeof state);
    CHECK(!tlk_saveinfo_read(state, &r), "no file");
    At("broken.state", state, sizeof state);
    At("broken.json", json, sizeof json);
    WriteText(json, "{\"frame\": 12");
    CHECK(!tlk_saveinfo_read(state, &r), "no build key");
    WriteText(json, "{\"build\": \"another\", \"frame\": 12");
    CHECK(tlk_saveinfo_check(state, "this", &r) == TLK_SAVE_OTHER_BUILD, "cut short, but it names its build");
}

static void saveinfo_check_tells_this_build_other_build_and_none(void) {
    char state[1100];
    TlkSaveInfo w = Sample(), r;
    At("chk.state", state, sizeof state);
    tlk_saveinfo_remove(state);
    CHECK(tlk_saveinfo_check(state, w.build, &r) == TLK_SAVE_NO_INFO, "no sidecar: an earlier build's state");
    tlk_saveinfo_write(state, &w);
    CHECK(tlk_saveinfo_check(state, w.build, &r) == TLK_SAVE_THIS_BUILD, "same build");
    CHECK(tlk_saveinfo_check(state, "0123456789ac-Linux-x86_64", &r) == TLK_SAVE_OTHER_BUILD, "one digit off");
    CHECK(!strcmp(r.build, w.build), "and says whose it is");
    CHECK(tlk_saveinfo_check(state, "0123456789ab-Linux-x86", &r) == TLK_SAVE_OTHER_BUILD, "a prefix of it");
    tlk_saveinfo_remove(state);
    CHECK(tlk_saveinfo_check(state, w.build, &r) == TLK_SAVE_NO_INFO, "removed");
}

static void saveinfo_reads_keys_in_any_order_with_spacing(void) {
    char state[1100], json[1100];
    TlkSaveInfo r;
    At("order.state", state, sizeof state);
    At("order.json", json, sizeof json);
    WriteText(json, "{\n  \"lives\" : 5,\n  \"level_name\":\"Exile\", \"build\": \"b1\",\"level\":4,"
                    " \"frame\": 77, \"saved_unix\": 9 }\n");
    CHECK(tlk_saveinfo_read(state, &r), "parsed");
    CHECK(!strcmp(r.build, "b1") && r.lives == 5 && r.level == 4 && r.frame == 77 && r.saved_unix == 9, "values");
    CHECK(!strcmp(r.level_name, "Exile"), r.level_name);
}

int main(int argc, char **argv) {
    snprintf(s_dir, sizeof s_dir, "%s", argc > 1 ? argv[1] : ".");
    saveinfo_path_replaces_state_extension_or_appends();
    saveinfo_write_then_read_round_trips_every_field();
    saveinfo_quotes_in_names_survive();
    saveinfo_missing_json_reads_as_none_and_a_cut_short_one_by_its_build();
    saveinfo_check_tells_this_build_other_build_and_none();
    saveinfo_reads_keys_in_any_order_with_spacing();
    printf("%s (%d failed)\n", s_failed ? "FAIL" : "PASS", s_failed);
    return s_failed != 0;
}
