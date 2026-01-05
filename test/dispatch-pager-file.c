// RUN: cc -o %b %s
// RUN: env SRC=%s BIN=%b BINDIR=%B SRCDIR=%S %b %S/data/sample0.md | tikl-check -x %s
// CHECK: ARGS: {{.*}}/%b %S/data/sample0.md
// CHECK: SRC=%s

#include <stdio.h>
#include <stdlib.h>

int
main(int argc, char **argv)
{
    const char *value;
    printf("ARGS:");
    for (int i = 0; i < argc; ++i)
        printf(" %s", argv[i]);
    printf("\n");
    value = getenv("SRC");
    printf("SRC=%s\n", value ? value : "(null)");
    value = getenv("BIN");
    printf("BIN=%s\n", value ? value : "(null)");
    value = getenv("SRCDIR");
    printf("SRCDIR=%s\n", value ? value : "(null)");
    value = getenv("BINDIR");
    printf("BINDIR=%s\n", value ? value : "(null)");
    return 0;
}
