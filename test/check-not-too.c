// REQUIRES: check
// RUN: printf 'alpha 100\nbravo\ngamma\n' | %check
// CHECK: alpha 100
// CHECK: bravo
// CHECK-NOT: charlie 100
int
main(void)
{
    return 0;
}
