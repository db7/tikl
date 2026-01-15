// RUN: %cc %s -o %b
// RUN: (! %b 2>&1) | %check
// CHECK: [{{[[:digit:]]+}}](estimated) SIGABRT
#include <stdio.h>
#include <stdlib.h>

int
main(void)
{
    fputs("[0](estimated) SIGABRT\n", stderr);
    return EXIT_FAILURE;
}
