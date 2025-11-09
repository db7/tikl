.POSIX:

CC=		cc
CFLAGS=		-O2
CFLAGS_=	${CFLAGS}
CFLAGS_+=	-std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS_+=	-Wall -Wextra -Wpedantic -Wshadow -Werror

PREFIX=		/usr/local
BINDIR=		${PREFIX}/bin
MANDIR=		${PREFIX}/man/man1

DATE=		$$(date '+%Y-%m-%d' | xargs printf '%s')
VERSION=	$$(./version.sh)
CFLAGS_+=	-DTIKL_VERSION=\"${VERSION}\"

TARGETS=	tikl tikl-check tikl.1
all: ${TARGETS}

clean:
	rm -f test_unit ${TARGETS}

tikl: tikl.c
	${CC} ${CFLAGS_} -o $@ tikl.c

tikl-check: tikl-check.c
	${CC} ${CFLAGS_} -o $@ tikl-check.c

tikl.1: tikl.1.in
	sed -e 's/__TIKL_MAN_DATE__/'${DATE}'/' \
	    -e 's/__TIKL_MAN_VERSION__/'${VERSION}'/' tikl.1.in > $@

install: ${TARGETS}
	mkdir -p ${DESTDIR}${BINDIR}
	install -m 755 tikl ${DESTDIR}${BINDIR}/
	install -m 755 tikl-check ${DESTDIR}${BINDIR}/
	mkdir -p ${DESTDIR}${MANDIR}
	install -m 644 tikl.1 ${DESTDIR}${MANDIR}/

test: test_unit test_integration selfcheck

test_unit: tikl.c test/unit/test_tikl.c
	${CC} ${CFLAGS} -o test_unit test/unit/test_tikl.c

test_integration: all
	sh test/run-tests.sh

selfcheck: all
	./tikl -q -c tikl.conf test/self

format:
	@find . -name '*.h' -exec astyle --options=.astylerc {} +
	@find . -name '*.c' -exec astyle --options=.astylerc {} +

.PHONY: all clean test test_integration selfcheck install format
