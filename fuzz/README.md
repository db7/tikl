# tikl AFL Fuzzing

This directory contains the bits needed to fuzz `tikl` with
[AFL++](https://github.com/AFLplusplus/AFLplusplus). The idea is to build a
lightweight harness that only exercises tikl's parsing and directive handling,
then let AFL mutate test files.

## Contents

- `run-afl.sh` – builds an instrumented binary and launches `afl-fuzz`.
- `seeds/` – a small, curated corpus of legal directives to guide mutations.
- `tikl.dict` – dictionary entries for common directives and placeholders.

The harness compiles `tikl` with the `TIKL_FUZZ` flag, which stubs out command
execution (`RUN:` steps always succeed). This keeps AFL from spawning arbitrary
shell commands while still exercising the parser and substitution machinery.

## Running

1. Install AFL or AFL++ and make sure `afl-fuzz` plus an AFL compiler wrapper
   (e.g. `afl-clang-lto`, `afl-clang-fast`, `afl-clang`, or `afl-cc`) are on
   your `PATH`.
2. From the repository root run:

   ```sh
   fuzz/run-afl.sh
   ```

   The script automatically:
   - selects an AFL compiler (preferring LTO/FAST variants),
   - sets helpful defaults such as ASan (`AFL_USE_ASAN=1`) and auto-resume,
   - builds `fuzz/build/tikl-afl`, and
   - launches `afl-fuzz` with `fuzz/seeds` as the input corpus and
     `fuzz/out` as the output working directory.

Environment variables:

- `TIKL_AFL_CC` – explicitly choose the AFL wrapper (avoids recursion issues on
  classic AFL where `AFL_CC` is reserved for the *real* compiler). If unset the
  script falls back to `AFL_CC` and then auto-detection.
- `TIKL_REAL_CC` – force the underlying compiler passed to the wrapper. Defaults
  to `clang` for `afl-clang*` and `cc` for `afl-cc` / `afl-gcc`.
- `AFL_CFLAGS` – append extra compiler flags.
- `AFL_FUZZ_ARGS` – extra arguments passed to `afl-fuzz`
  (e.g. `-M tikl0` for distributed mode).
- Disable sanitizers by exporting `AFL_USE_ASAN=0 AFL_DONT_OPTIMIZE=0`.

## After fuzzing

- Minimise the corpus: `afl-cmin -i fuzz/out/default/queue -o fuzz/out/min -- fuzz/build/tikl-afl -q @@`
  (optional but useful for future runs).
- Any crashes or hangs will be under `fuzz/out/default/crashes` and
  `fuzz/out/default/hangs`. Keep the interesting inputs and report back with:
  - the triggering test file,
  - `afl-fuzz` stats (cycle count, exec/sec),
  - `tikl` version/commit,
  - reproduction steps (`./tikl -q CRASHFILE`).

Clean up temporary state by removing `fuzz/out` and `fuzz/build`.
