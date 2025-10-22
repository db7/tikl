// REQUIRES: check
// RUN: printf 'first\nsecond\n' | %check --check-prefix=FOO --check-prefix=BAR
// FOO: first
// BAR: second
int main(void)
{
	return 0;
}
