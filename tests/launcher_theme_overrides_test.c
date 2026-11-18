#include "launcher_theme_overrides.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "FAIL: %s\n", message);
    return 0;
}

static int near(float a, float b) { return fabsf(a - b) < 1e-4f; }

static LauncherTheme themed(const char* ini) {
    LauncherThemeOverrides o;
    LauncherTheme t = launcher_theme_psx();
    launcher_theme_overrides_parse(ini, &o);
    launcher_theme_overrides_apply(&o, &t);
    return t;
}

static int parsed_count(const char* ini) {
    LauncherThemeOverrides o;
    return launcher_theme_overrides_parse(ini, &o);
}

static int test_accent_hex_applies(void) {
    const LauncherTheme t = themed("[colors]\naccent = #ECC57D\n");
    return expect(near(t.accent.r, 0xEC / 255.0f) && near(t.accent.g, 0xC5 / 255.0f) &&
                  near(t.accent.b, 0x7D / 255.0f) && near(t.accent.a, 1.0f),
                  "accent #ECC57D applies");
}

static int test_alpha_suffix_applies(void) {
    const LauncherTheme t = themed("[colors]\npanel = #0F2547F0\n");
    return expect(near(t.panel.a, 0xF0 / 255.0f), "alpha suffix applies");
}

static int test_unset_token_keeps_scheme(void) {
    const LauncherTheme t = themed("[colors]\naccent = #ECC57D\n");
    return expect(near(t.text.r, launcher_theme_psx().text.r), "unset token keeps scheme");
}

static int test_crlf_lines_apply(void) {
    return expect(parsed_count("[colors]\r\naccent = #ecc57d\r\n") == 1, "CRLF lines apply");
}

static int test_keys_outside_colors_section_ignored(void) {
    return expect(parsed_count("[other]\naccent = #ECC57D\n") == 0,
                  "keys outside [colors] ignored");
}

static int test_invalid_value_skipped_others_apply(void) {
    return expect(parsed_count("[colors]\naccent = #FFF\ntext = #GGGGGG\n"
                               "border = 25497D\npanel = #0F2547\n") == 1,
                  "invalid values skipped, valid one applies");
}

static int test_comment_line_ignored(void) {
    return expect(parsed_count("; note\n[colors]\naccent = #ECC57D\n") == 1,
                  "comment line ignored");
}

static int test_inline_comments_stripped(void) {
    const LauncherTheme t = themed("[colors]\naccent = #ECC57D ; gold\n"
                                   "text = #112233 # hash comment\n"
                                   "panel = #0F2547F0; tight\n"
                                   "border = #445566\t; tab\n");
    return expect(near(t.accent.r, 0xEC / 255.0f) && near(t.text.g, 0x22 / 255.0f) &&
                  near(t.panel.a, 0xF0 / 255.0f) && near(t.border.b, 0x66 / 255.0f) &&
                  parsed_count("[colors]\naccent = #ECC57D ; gold\ntext = #112233 # c\n"
                               "panel = #0F2547F0; x\n") == 3,
                  "inline ; and # comments are stripped");
}

static int test_inline_comment_does_not_hide_bad_value(void) {
    return expect(parsed_count("[colors]\naccent = #ECC5 ; short\ntext = ; nothing\n"
                               "border = #ECC57DAB12 ; long\n") == 0,
                  "comment stripping does not make bad values valid");
}

static int test_missing_file_leaves_theme_unchanged(void) {
    LauncherTheme t = launcher_theme_psx();
    launcher_theme_load_game_overrides("/nonexistent/theme_game.ini");
    launcher_theme_overrides_apply(launcher_theme_game_overrides(), &t);
    return expect(near(t.accent.r, launcher_theme_psx().accent.r),
                  "missing file leaves theme unchanged");
}

static int test_load_file_applies(void) {
    char path[] = "/tmp/rui-theme-XXXXXX";
    const int fd = mkstemp(path);
    FILE* f = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!f) return expect(0, "temp file");
    fputs("[colors]\naccent = #ECC57D\n", f);
    fclose(f);
    LauncherTheme t = launcher_theme_psx();
    launcher_theme_load_game_overrides(path);
    launcher_theme_overrides_apply(launcher_theme_game_overrides(), &t);
    remove(path);
    return expect(near(t.accent.r, 0xEC / 255.0f), "loaded file applies");
}

int main(void) {
    int ok = 1;
    ok &= test_accent_hex_applies();
    ok &= test_alpha_suffix_applies();
    ok &= test_unset_token_keeps_scheme();
    ok &= test_crlf_lines_apply();
    ok &= test_keys_outside_colors_section_ignored();
    ok &= test_invalid_value_skipped_others_apply();
    ok &= test_comment_line_ignored();
    ok &= test_inline_comments_stripped();
    ok &= test_inline_comment_does_not_hide_bad_value();
    ok &= test_missing_file_leaves_theme_unchanged();
    ok &= test_load_file_applies();
    return ok ? 0 : 1;
}
