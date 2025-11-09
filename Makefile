.POSIX:

CFLAGS=		-O2
CFLAGS_=	${CFLAGS}
CFLAGS_+=	-std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS_+=	-Wall -Wextra -Wpedantic -Wshadow -Werror
CFLAGS_+=	-DTIKL_VERSION=\"${VERSION}\"

PREFIX=		/usr/local
BINDIR=		${PREFIX}/bin
MANDIR=		${PREFIX}/man/man1

DATE=		$$(date '+%Y-%m-%d' | xargs printf '%s')
VERSION=	$$(./version.sh)

TARGETS=	tikl tikl-check tikl.1
all: ${TARGETS}

clean:
	rm -f test_unit ${TARGETS}
.o:
	${CC} -o $@ $<
.c.o:
	${CC} ${CFLAGS_} -c -o $@ $<

tikl.1: tikl.1.in
	sed -e 's/__TIKL_MAN_DATE__/'${DATE}'/' \
	    -e 's/__TIKL_MAN_VERSION__/'${VERSION}'/' $< > $@

install: ${TARGETS}
	${MKDIR_P} ${DESTDIR}${BINDIR}
	${INSTALL} -m 755 ${TARGET} ${DESTDIR}${BINDIR}/tikl
	${INSTALL} -m 755 ${CHECK_TARGET} ${DESTDIR}${BINDIR}/tikl-check
	${MKDIR_P} ${DESTDIR}${MANDIR}
	${INSTALL} -m 644 ${MAN_PAGE} ${DESTDIR}${MANDIR}/tikl.1

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
