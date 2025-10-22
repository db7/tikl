// RUN: %cc %s -o %b
// RUN: %b | %check
// CHECK: goodbye
#include <stdio.h>
int main(void) {
    puts("hello");
    return 0;
}
