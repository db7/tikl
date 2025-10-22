// RUN: sh -c 'if [ -f %t.retry ]; then rm -f %t.retry; exit 0; else touch %t.retry; exit 1; fi'
// ALLOW_RETRIES: 1
int main(void)
{
	return 0;
}
