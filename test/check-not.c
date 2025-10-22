// REQUIRES: check
// RUN: printf 'alpha\nbravo\n' | %check
// CHECK: alpha
// CHECK: bravo
// CHECK-NOT: charlie
int main(void)
{
	return 0;
}
