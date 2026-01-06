# tikl — Tikl ist kein Lit

tikl is a deliberately small test driver inspired by [LLVM's
lit](https://llvm.org/docs/CommandGuide/lit.html). It keeps the familiar `//
RUN:` style annotations but trims the feature set down so it stays portable and
easy to hack on.

## Highlights

- Reads test files looking for `RUN:`, `REQUIRES:`, and `UNSUPPORTED:`
  directives, then executes the resulting shell pipelines.
- Simple `%placeholder` substitution system that can be extended via an optional
  config file (see `tikl.conf`), e.g. `%s` expands to the current test
  path and `%b` maps it into `bin/…`.
- Scratch directories default to `/tmp`, but `-T DIR` lets you keep temp files
  on a different volume or sandbox.
- Optional per-step timeout via `-t SECONDS` keeps hung tests from blocking the
  whole run.
- `%b`/`%B` map into `bin/` by default, yet `-b DIR` lets you route build
  artefacts to a custom tree.
- Feature flags (`-D feature`) gate tests via `REQUIRES`/`UNSUPPORTED`, which
  keeps suites portable across different hosts.
- In default mode, tikl enables `pipefail` when supported (falling back to
  `/bin/bash` if needed), so `RUN:` pipelines fail if any stage fails.
- Zero dependencies beyond a POSIX-ish `sh`, so it travels well across systems.

## Building

```sh
make
```

This produces the `tikl` binary in the project root. A unit test and integration
smoke tests are available via `make test`.

To install into a prefix, run:

```sh
make install PREFIX=/usr/pkg DESTDIR=/path/to/staging
```

After installation you will have `tikl`, the helper `tikl-check`, and the
accompanying manpage; `man tikl` offers a condensed reference for day-to-day
use.

## Quick start

```sh
./tikl -c tikl.conf test/basic.c
```

The example above runs a single test under `test/`, relying on the built-in
`%check` placeholder and the provided config to map it to the local
`tikl-check`. After `tikl` and `tikl-check` are installed on your `PATH`, you no
longer need `-c tikl.conf`. For suites, point tikl at multiple files (or
glob via your shell) and it will execute each test in sequence, reporting
`[ RUN ]`, `[ SKIP ]`, `[FAIL]`, and `[  OK ]` statuses. Use `-q` for concise
output or `-v` to echo the shell commands as they run.

### `%check` in action

A typical test pairs a `RUN:` directive with `%check` so the helper script can
verify a file's contents:

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

## `%check` and `CHECK:` helpers

With the default `tikl.conf` in this repository, `%check` expands to
`tikl-check %s`, which reads `CHECK:` family directives in the test file and
ensures the output obeys each expectation. You don't have to pass `%s`
explicitly—tikl fills it in during substitution. Patterns are matched in order,
so `CHECK:` expectations cannot leap backwards in the stream.

Supported directives:

- `CHECK:` looks for the literal fragment (or regex block) anywhere after the
  previous match.
- `CHECK-NOT:` fails when the substring appears anywhere in the remaining
  output.
- `CHECK-NEXT:` insists the next output line contains the fragment.
- `CHECK-SAME:` keeps matching on the current line.
- `CHECK-EMPTY:` expects the next line to be blank.
- `CHECK-COUNT: N foo` requires `foo` to be seen exactly `N` times.
- Embed regular expressions inline with `{{...}}`; surrounding text is matched
  literally, so `CHECK: value={{[0-9]+}}` accepts `value=123`.
- Use inline helper calls anywhere tikl performs substitutions—`RUN:` commands,
  config values, and `CHECK` directives: `%(basename ARG [SUFFIX])` strips a
  path down to its filename (optionally removing a trailing `SUFFIX` just like
  the shell command), `%(dirname ARG)` returns the containing directory, and
  `%(realpath ARG)` resolves symlinks. Arguments can contain other placeholders
  or helper calls. These helpers are only active when tikl is running in its
  default (non-`-L`) mode.

tikl deliberately diverges from LLVM's FileCheck/lit in two ways:

1. `%placeholder` tokens are expanded inside every `CHECK` variant. Values come
   from your substitution config (e.g. `%foo` from `foo = ...`) plus the built-in
   `%s`, `%S`, `%b`, and `%B`. Use `%%name` to keep the literal text `%name`.
2. Literal text outside `{{...}}` is treated as literal text, so parentheses and
   other regex metacharacters do not need escaping.
3. Inline helper expressions `%(basename ARG [SUFFIX])` / `%(dirname ARG)` /
   `%(realpath ARG)` run inside `CHECK` patterns, allowing quick path
   manipulation without touching the surrounding shell script.

Pass `-L` to tikl when you need lit-compatible behaviour: `%` tokens are left
verbatim and regex metacharacters regain their default meaning (so `foo(bar)`
must be written as `foo\(bar\)`).

Need a different tag? Append options after `%check`, e.g. `| %check --check-prefix=ALT`
or `| %check -p ALT`, so only `ALT:` directives are honoured. Use multiple
`--check-prefix`/`-p` flags to match several prefixes in one pass, or override
`%check` entirely via your own config when a different helper better suits your
project. Add `--print-output-on-fail` (`-x`) to `%check` when you want
`tikl-check` to dump the checked program's output after any failing directive.

## Configuration

| Placeholder | Expands to                               |
|-------------|------------------------------------------|
| `%s`        | Absolute path to the current test file   |
| `%S`        | Absolute directory containing the test   |
| `%t`        | Scratch file path unique to this command |
| `%T`        | Scratch directory                        |
| `%b`        | Path in `bin/` mirroring the source name |
| `%B`        | Directory portion of `%b`                |

`%check` maps to `tikl-check %s` out of the box. Additional placeholders come
from `key = value` pairs in the config file. For example, `cc = cc -O2 -g` in
`tikl.conf` makes `%cc` available inside `RUN:` lines. Use `-b DIR` if you
need `%b` to land somewhere other than `bin/`.

### Handling flakes and expected failures

- `ALLOW_RETRIES: N` gives each `RUN:` step up to `N + 1` attempts. tikl reruns a
  failing command until it succeeds or the allowance is exhausted, logging each
  retry when it happens.
- `XFAIL:` marks a test as an expected failure. tikl reports `[XFAIL]` when a
  step fails (or times out) and considers the test successful. If every step
  passes instead, the run is flagged as `[XPASS]` and fails overall so the stale
  expectation gets noticed. Add an optional reason after the colon for context.

## Options summary

- `-T DIR` — change the scratch directory root used for `%t`/`%T`.
- `-b DIR` — change the root directory used for `%b`/`%B` (defaults to `bin`).
- `-L` — force lit-compatible behaviour (turn off non-standard tikl extensions).
- `-t SECONDS` — terminate any `RUN` command that exceeds the given wall-clock
  budget (returns exit code 124).
- `-V` — print the tikl version and exit.

## Fuzzing

An AFL++ harness lives in `fuzz/`. It builds a tikl variant with command
execution disabled and feeds it curated directive seeds. See `fuzz/README.md`
for setup instructions and post-run triage tips.

## Disclaimers

tikl borrows the broad idea—and a few naming conventions—from LLVM's lit, but
intentionally keeps to a smaller scope so it can stay approachable. If you need
the full-featured original, check out the [LLVM lit
documentation](https://llvm.org/docs/CommandGuide/lit.html).

Note that the original version of this program was mostly vibe-coded.
