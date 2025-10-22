// REQUIRES: check
// RUN: printf 'alpha\nbeta\n' | %check --check-prefix=FOO
// FOO: alpha
// FOO-NEXT: beta
int main(void)
{
	return 0;
}
