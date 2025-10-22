// RUN: %cc %s -o %b
// RUN: (! %b) 2>&1 | %check
// CHECK: boom assertion triggered
#include <stdio.h>
#include <stdlib.h>
int main(void){
    fprintf(stderr, "boom assertion triggered\n");
    abort();
}
