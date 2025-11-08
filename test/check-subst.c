// RUN: %cc %s -o %b
// RUN: %b | %check
// CHECK: value = %foo
// CHECK: literal %%foo

#include <stdio.h>

int main(void) {
    puts("value = substituted");
    puts("literal %foo");
    return 0;
}
