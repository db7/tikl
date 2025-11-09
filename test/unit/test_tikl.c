#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define main tikl_main
#include "../../tikl.c"
#undef main
int
main(void)
{
    char out[4096];
    map_source_to_bin("a/b/c.c", out, sizeof(out));
    assert(strcmp(out, "bin/a/b/c") == 0);
    char r[256];
    assert(parse_comment_run("// RUN: echo hi", r, sizeof(r))
           && strcmp(r, "echo hi") == 0);
    puts("unit: OK");
    return 0;
}
