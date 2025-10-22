// RUN: echo "first"
// RUN: echo "second" | tr a-z A-Z
// RUN: mkdir -p %T && echo ok > %t && test -f %t
int main(void)
{
	return 0;
}
