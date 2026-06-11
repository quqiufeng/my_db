#include <stdio.h>
#include <string.h>
#include "../include/json.h"

static int tests_pass = 0, tests_fail = 0;
#define TEST(name, expr) do { \
    if (!(expr)) { fprintf(stderr, "FAIL: %s (%s)\n", name, #expr); tests_fail++; } \
    else { tests_pass++; } \
} while(0)

int main(void) {
    char out[256];

    /* normal string */
    json_escape("hello", out, sizeof(out));
    TEST("normal", strcmp(out, "hello") == 0);

    /* escape quotes */
    json_escape("say \"hi\"", out, sizeof(out));
    TEST("quotes", strcmp(out, "say \\\"hi\\\"") == 0);

    /* escape backslash */
    json_escape("a\\b", out, sizeof(out));
    TEST("backslash", strcmp(out, "a\\\\b") == 0);

    /* escape newline */
    json_escape("a\nb", out, sizeof(out));
    TEST("newline", strcmp(out, "a\\nb") == 0);

    /* escape tab */
    json_escape("a\tb", out, sizeof(out));
    TEST("tab", strcmp(out, "a\\tb") == 0);

    /* empty string */
    json_escape("", out, sizeof(out));
    TEST("empty", strcmp(out, "") == 0);

    /* null input */
    json_escape(NULL, out, sizeof(out));
    TEST("null", strcmp(out, "") == 0 || out[0] == '\0');

    /* buffer too small */
    json_escape("hello world!", out, 6);
    TEST("truncated", strlen(out) < 6);

    printf("Results: %d passed, %d failed\n", tests_pass, tests_fail);
    return tests_fail > 0 ? 1 : 0;
}
