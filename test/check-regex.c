// REQUIRES: check
// RUN: printf 'value=123\nliteral braces {}\n' | %check
// CHECK: value={{[0-9]+}}
// CHECK-NEXT: literal braces {{\{\}}}
int main(void)
{
	return 0;
}
