# tikl-check C Port Plan

## Goals

- Reimplement `tikl-check` in C without regressing behaviour.
- Keep the tool dependency-free (use POSIX regex via `<regex.h>`).
- Preserve current CLI surface: `--check-prefix`, lit compatibility via
  `TIKL_LIT_COMPAT`, and reading expectations from the test file while consuming
  the stream over stdin.
- Make it easy to add new directives or matching modes later.

## Behavioural Contract

The new implementation must match the shell version with respect to:

1. Directive parsing order and prefix handling (`CHECK`, `CHECK-NOT`, `NEXT`,
   `SAME`, `EMPTY`, `COUNT`).
2. `%placeholder` expansion (including `%%` escapes) driven by
   `TIKL_CHECK_SUBSTS` when lit compatibility is off.
3. Literal vs regex text: everything outside `{{...}}` is literal by default,
   unless `TIKL_LIT_COMPAT=1`, where FileCheck semantics apply.
4. Streaming discipline: matches occur in-order, and failure diagnostics include
   the directive text just like today.
5. Exit codes: zero on success, non-zero when an expectation is violated, and
   2 for usage errors.

Existing regression tests in `test/run-tests.sh` (especially the substitution,
parentheses, and lit-mode cases) define the acceptance criteria.

## Architecture Sketch

```
source file -> directive parser -> matcher engine -> exit status
                             ^
                             |
                    prefix/filter state
```

### Modules

1. **CLI / Config**
   - Parse `--check-prefix` (repeatable) and the single test-file argument.
   - Default prefix list to `{"CHECK"}` when not specified.
   - Open the test file, read stdin into a temporary buffer (similar to current
     script) so multiple passes are cheap.

2. **Directive Parser**
   - Read the test file line by line, extracting directives matching the active
     prefixes.
   - Normalise into an internal representation, e.g.:
     ```c
     typedef enum {
         MATCH_FORWARD,
         MATCH_NEXT,
         MATCH_SAME,
         MATCH_EMPTY,
         MATCH_NOT,
         MATCH_COUNT
     } check_kind;

     typedef struct {
         check_kind kind;
         unsigned count;   // for MATCH_COUNT
         char *pattern;    // raw string before regex compilation
         size_t lineno;    // for diagnostics
     } check_directive;
     ```
   - Apply `%` substitution (unless lit-compat) before storing the pattern.

3. **Pattern Compiler**
   - Convert each directive’s pattern into a POSIX ERE by walking literal text,
     honouring `{{...}}` blocks, and escaping appropriately.
   - Pre-compile via `regcomp` so repeated matches don’t reparse the regex.
   - Keep both the compiled regex and the original text for error messages.

4. **Matcher Engine**
   - Maintain per-prefix cursor state (`last_line`, `have_last`), mirroring the
     shell script’s behaviour.
   - Walk the captured stdout/stderr buffer once, applying directives in order.
   - Implement the semantic differences for NEXT/SAME/EMPTY/COUNT/NOT exactly as
     today.

5. **Diagnostics**
   - Share helper functions to emit the same `tikl-check: ...` messages so
     existing tests that grep for wording keep passing.

### Data Structures

- Dynamic arrays for directives and prefixes (`vec` helpers similar to tikl’s
  `vecstr`).
- A `buffer` struct to hold stdin contents with length + pointer.
- `regex_t` wrappers stored alongside directives for efficient reuse.

## Incremental Delivery

1. **Scaffolding** ✅
   - `tikl-check.c` now hosts the CLI, placeholder handling, and matcher.

2. **Core Port** ✅
   - Directive parsing and literal/regex matching are implemented, with `%check`
     pointing to the C binary by default.

3. **Directive/Feature Parity** ✅
   - All CHECK variants, `%` substitutions, lit compatibility, and diagnostics
     match the old script; the regression suite runs entirely through the new
     helper.

4. **Considerations Going Forward**
   - Keep fuzzing the parser/matcher (reuse `fuzz/` harness) to catch edge cases.
   - If new directives or matching modes are added, update both this document
     and the regression tests to keep the contract explicit.
   - Monitor performance on large logs; if needed, explore streaming instead of
     buffering all lines.

Keeping the plan in-tree lets us track progress towards tikl 0.4, and we can
update this document as design decisions evolve.
