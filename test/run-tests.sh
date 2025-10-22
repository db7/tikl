#!/bin/sh
set -eu
./tinl -q -c tinl.conf test/basic.c
./tinl -q -c tinl.conf test/check-not.c
./tinl -q -c tinl.conf test/check-advanced.c
./tinl -q -c tinl.conf test/check-prefix.c
./tinl -q -c tinl.conf test/check-prefix-multi.c
./tinl -q -c tinl.conf test/multi-run.c
./tinl -q -c tinl.conf test/a/b/c.c
./tinl -q -c tinl.conf test/allow-retries.c
./tinl -q -c tinl.conf test/xfail.c
./tinl -q -c tinl.conf test/requires.c | grep -E "SKIP|skip|Skip" >/dev/null 2>&1 || true
./tinl -q -c tinl.conf test/robust/unsupported.c | grep -E "SKIP|skip|Skip" >/dev/null 2>&1 || true
./tinl -q -c tinl.conf test/robust/failing.c
./tinl -q -c tinl.conf test/robust/assert-fail.c
if ./tinl -q -c tinl.conf test/robust/check-mismatch.c ; then exit 1; fi
if ./tinl -q -c tinl.conf test/robust/check-not-hit.c ; then exit 1; fi
if ./tinl -q -c tinl.conf test/robust/check-next-fail.c ; then exit 1; fi
if ./tinl -q -c tinl.conf test/robust/check-count-miss.c ; then exit 1; fi
if ./tinl -q -c tinl.conf test/robust/allow-retries-exhaust.c ; then exit 1; fi
if ./tinl -q -c tinl.conf test/robust/xfail-xpass.c ; then exit 1; fi
if ./tinl -q -c tinl.conf test/robust/check-prefix-miss.c ; then exit 1; fi
./tinl -q test/robust/empty.txt
./tinl -q test/robust/long-continue.txt
echo "integration: OK"
