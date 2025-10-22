# tinl — tiny lit-inspired tester

tinl ("tinl is not lit") is a deliberately small test driver inspired by [LLVM's
lit](https://llvm.org/docs/CommandGuide/lit.html). It keeps the familiar `//
RUN:` style annotations but trims the feature set down so it stays portable and
easy to hack on.

## Highlights

- Reads test files looking for `RUN:`, `REQUIRES:`, and `UNSUPPORTED:`
  directives, then executes the resulting shell pipelines.
- Simple `%placeholder` substitution system that can be extended via an optional
  config file (see `tinl.conf`), e.g. `%s` expands to the current test
  path and `%b` maps it into `bin/…`.
- Scratch directories default to `/tmp`, but `-T DIR` lets you keep temp files
  on a different volume or sandbox.
- Optional per-step timeout via `-t SECONDS` keeps hung tests from blocking the
  whole run.
- `%b`/`%B` map into `bin/` by default, yet `-b DIR` lets you route build
  artefacts to a custom tree.
- Feature flags (`-D feature`) gate tests via `REQUIRES`/`UNSUPPORTED`, which
  keeps suites portable across different hosts.
- Zero dependencies beyond a POSIX-ish `sh`, so it travels well across systems.

## Building

```sh
make
```

This produces the `tinl` binary in the project root. A unit test and integration
smoke tests are available via `make test`.

To install into a prefix, run:

```sh
make install PREFIX=/usr/pkg DESTDIR=/path/to/staging
```

After installation you will have `tinl`, the helper `tinl-check`, and the
accompanying manpage; `man tinl` offers a condensed reference for day-to-day
use.

## Quick start

```sh
./tinl -c tinl.conf test/basic.c
```

The example above runs a single test under `test/`, relying on the built-in
`%check` placeholder and the provided config to map it to the local
`tinl-check`. After `tinl` and `tinl-check` are installed on your `PATH`, you no
longer need `-c tinl.conf`. For suites, point tinl at multiple files (or
glob via your shell) and it will execute each test in sequence, reporting
`[ RUN ]`, `[ SKIP ]`, `[FAIL]`, and `[  OK ]` statuses. Use `-q` for concise
output or `-v` to echo the shell commands as they run.

### `%check` in action

A typical test pairs a `RUN:` directive with `%check` so the helper script can
verify a file’s contents:

```c
// RUN: %cc %s -o %b
// RUN: %b | %check
// CHECK: expected content

#include <stdio.h>

int main(void) {
    printf("expected content\n");
    return 0;
}
```

With the default `tinl.conf` in this repository, `%check` expands to
`tinl-check %s`, which reads `CHECK:` lines in the test file and ensures every
pattern appears in the piped output. You don't have to pass `%s` explicitly—tinl
fills it in during substitution. Add more `CHECK:` lines if you need to verify
multiple fragments. Patterns are matched in-order, so `CHECK:` expectations
cannot leap backwards in the output. Use the helper’s siblings to cover more
cases:

- `CHECK-NOT:` fails when a substring appears anywhere in the stream.
- `CHECK-NEXT:` insists the next output line contains the fragment.
- `CHECK-SAME:` keeps matching on the current line.
- `CHECK-EMPTY:` expects the next line to be blank.
- `CHECK-COUNT: N foo` requires `foo` to be seen exactly `N` times.
- Embed regular expressions inline with `{{...}}`; literal text around the block
  is matched verbatim, so `CHECK: value={{[0-9]+}}` accepts `value=123`.

Need a different tag? Append options after `%check`, e.g. `| %check --check-prefix=ALT`,
so only `ALT:` directives are honoured. Use multiple `--check-prefix` flags to match
several prefixes in one pass.

Override `%check` via your own config when your project
needs different tooling.

## Configuration cheat sheet

| Placeholder | Expands to                               |
|-------------|------------------------------------------|
| `%s`        | Current test file path                   |
| `%S`        | Directory containing the current test    |
| `%t`        | Scratch file path unique to this command |
| `%T`        | Scratch directory                        |
| `%b`        | Path in `bin/` mirroring the source name |
| `%B`        | Directory portion of `%b`                |

`%check` maps to `tinl-check %s` out of the box. Additional placeholders come
from `key = value` pairs in the config file. For example, `cc = cc -O2 -g` in
`tinl.conf` makes `%cc` available inside `RUN:` lines. Use `-b DIR` if you
need `%b` to land somewhere other than `bin/`.

### Handling flakes and expected failures

- `ALLOW_RETRIES: N` gives each `RUN:` step up to `N + 1` attempts. tinl reruns a
  failing command until it succeeds or the allowance is exhausted, logging each
  retry when it happens.
- `XFAIL:` marks a test as an expected failure. tinl reports `[XFAIL]` when a
  step fails (or times out) and considers the test successful. If every step
  passes instead, the run is flagged as `[XPASS]` and fails overall so the stale
  expectation gets noticed. Add an optional reason after the colon for context.

## Options refresher

- `-T DIR` — change the scratch directory root used for `%t`/`%T`.
- `-b DIR` — change the root directory used for `%b`/`%B` (defaults to `bin`).
- `-t SECONDS` — terminate any `RUN` command that exceeds the given wall-clock
  budget (returns exit code 124).
- `-V` — print the tinl version and exit.

## Disclaimers

tinl borrows the broad idea—and a few naming conventions—from LLVM's lit, but
intentionally keeps to a smaller scope so it can stay approachable. If you need
the full-featured original, check out the [LLVM lit
documentation](https://llvm.org/docs/CommandGuide/lit.html).

Note that the original version of this program was mostly vibe-coded.
