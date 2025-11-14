.POSIX:

CC=		cc
CFLAGS=		-O2
CFLAGS_=	${CFLAGS}
CFLAGS_+=	-std=c11
CFLAGS_+=	-D_XOPEN_SOURCE=500
CFLAGS_+=	-Wall -Wextra -Wpedantic -Wshadow -Werror

COV_FLAGS=	-g -O0 --coverage

PREFIX=		/usr/local
BINDIR=		${PREFIX}/bin
MANDIR=		${PREFIX}/man/man1

TARGETS=	tikl tikl-check tikl.1
all: ${TARGETS}

coverage: CFLAGS=${COV_FLAGS}
coverage: clean all test_unit

clean:
	rm -f test_unit ${TARGETS} version.h
	@find . -name '*.gcov' -exec rm -f {} +
	@find . -name '*.gcno' -exec rm -f {} +
	@find . -name '*.gcda' -exec rm -f {} +

tikl: tikl.c version.h
	${CC} ${CFLAGS_} -o $@ tikl.c

tikl-check: tikl-check.c version.h
	${CC} ${CFLAGS_} -o $@ tikl-check.c

tikl.1: tikl.1.in
	./versionize.sh tikl.1.in > $@

version.h: version.h.in
	./versionize.sh version.h.in > $@

install: ${TARGETS}
	mkdir -p ${DESTDIR}${BINDIR}
	install -m 755 tikl ${DESTDIR}${BINDIR}/
	install -m 755 tikl-check ${DESTDIR}${BINDIR}/
	mkdir -p ${DESTDIR}${MANDIR}
	install -m 644 tikl.1 ${DESTDIR}${MANDIR}/

test: version.h test_unit test_integration selfcheck

test_unit: tikl.c test/unit/test_tikl.c
	${CC} ${CFLAGS} -o test_unit test/unit/test_tikl.c

test_integration: all
	sh test/run-tests.sh

selfcheck: all
	./tikl -q -c tikl.conf test/self

format:
	@find . -name '*.h' -exec astyle --options=.astylerc {} +
	@find . -name '*.c' -exec astyle --options=.astylerc {} +

.PHONY: all clean coverage test test_integration selfcheck install format
