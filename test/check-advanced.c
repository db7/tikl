// REQUIRES: check
// RUN: printf 'alpha alpha\nbeta\n\ncount me\ncount me\nnoise\n' | %check
// CHECK: alpha
// CHECK-SAME: alpha
// CHECK-NEXT: beta
// CHECK-EMPTY:
// CHECK-NOT: gamma
// CHECK-COUNT: 2 count me
int main(void)
{
	return 0;
}
