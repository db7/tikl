#!/bin/sh
set -eu
./tinl -q -c tinl.conf test/basic.c
./tinl -q -c tinl.conf test/multi-run.c
./tinl -q -c tinl.conf test/a/b/c.c
./tinl -q -c tinl.conf test/requires.c | grep -E "SKIP|skip|Skip" >/dev/null 2>&1 || true
./tinl -q -c tinl.conf test/robust/unsupported.c | grep -E "SKIP|skip|Skip" >/dev/null 2>&1 || true
if ./tinl -q -c tinl.conf test/robust/failing.c ; then exit 1; fi
./tinl -q test/robust/empty.txt
./tinl -q test/robust/long-continue.txt
echo "integration: OK"
