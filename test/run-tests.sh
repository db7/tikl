#!/bin/sh
set -eu

./tikl -q -c tikl.conf test/basic.c
./tikl -q -c tikl.conf test/check-not.c
./tikl -q -c tikl.conf test/check-advanced.c
./tikl -q -c tikl.conf test/check-regex.c
./tikl -q -c tikl.conf test/check-prefix.c
./tikl -q -c tikl.conf test/check-prefix-multi.c
./tikl -q -c tikl.conf test/multi-run.c
./tikl -q -c tikl.conf test/subst-abs.txt
./tikl -q -c tikl.conf test/check-subst.c
./tikl -q -c tikl.conf test/check-functions.txt
./tikl -q -c tikl.conf test/check-paren.txt
./tikl -q -c tikl.conf test/check-paren-escape.txt
./tikl -q -c tikl.conf test/run-placeholder-helpers.txt
./tikl -q -c tikl.conf test/a/b/c.c
./tikl -q -c tikl.conf test/allow-retries.c
./tikl -q -c tikl.conf test/xfail.c
./tikl -q -c tikl.conf test/requires.c | grep -E "SKIP|skip|Skip" >/dev/null 2>&1 || true
./tikl -q -c tikl.conf test/robust/unsupported.c | grep -E "SKIP|skip|Skip" >/dev/null 2>&1 || true
./tikl -q -c tikl.conf test/robust/failing.c
./tikl -q -c tikl.conf test/robust/assert-fail.c
if ./tikl -q -c tikl.conf test/robust/check-mismatch.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/check-not-hit.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/check-next-fail.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/check-count-miss.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/allow-retries-exhaust.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/xfail-xpass.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/check-prefix-miss.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/check-regex-miss.c ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/missing-run.c ; then exit 1; fi
if sh -c 'set -o pipefail' 2>/dev/null; then
    if ./tikl -q -c tikl.conf test/robust/pipefail-middle.txt ; then exit 1; fi
    if ./tikl -v -c tikl.conf test/robust/pipefail-middle.txt 2>&1 | grep -F "SHOULD_NOT_RUN_PIPEFAIL" >/dev/null; then exit 1; fi
fi
if ./tikl -q -L -c tikl.conf test/check-paren.txt ; then exit 1; fi
./tikl -q -L -c tikl.conf test/check-paren-escape.txt
if ./tikl -q -L -c tikl.conf test/check-subst.c ; then exit 1; fi
if ./tikl -q -L -c tikl.conf test/check-functions.txt ; then exit 1; fi
if ./tikl -q -c tikl.conf test/robust/empty.txt ; then exit 1; fi
./tikl -q -c tikl.conf test/robust/long-continue.txt
echo "integration: OK"
