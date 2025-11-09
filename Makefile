.POSIX:

CC=		cc
CFLAGS=		-O2
CFLAGS_=	${CFLAGS}
CFLAGS_+=	-std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS_+=	-Wall -Wextra -Wpedantic -Wshadow -Werror

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin
MANDIR?=	${PREFIX}/man/man1
INSTALL?=	install
MKDIR_P?=	mkdir -p
TARGET=		tikl
CHECK_TARGET=	tikl-check-c

all: ${TARGET} ${CHECK_TARGET}

${TARGET}: tikl.c
	${CC} ${CFLAGS_} -o ${TARGET} tikl.c

${CHECK_TARGET}: tikl-check.c
	${CC} ${CFLAGS_} -o ${CHECK_TARGET} tikl-check.c

clean:
	rm -f ${TARGET} ${CHECK_TARGET} test_unit
	rm -rf bin
	${MKDIR_P} bin

test_unit: tikl.c test/unit/test_tikl.c
	${CC} ${CFLAGS} -o test_unit test/unit/test_tikl.c

test_integration: all
	sh test/run-tests.sh

selfcheck: all
	./tikl -q -c tikl.conf test/self

test: test_unit test_integration selfcheck

install: ${TARGET} tikl.1
	${MKDIR_P} ${DESTDIR}${BINDIR}
	${INSTALL} -m 755 ${TARGET} ${DESTDIR}${BINDIR}/${TARGET}
	${INSTALL} -m 755 tikl-check ${DESTDIR}${BINDIR}/tikl-check
	${MKDIR_P} ${DESTDIR}${MANDIR}
	${INSTALL} -m 644 tikl.1 ${DESTDIR}${MANDIR}/tikl.1

.PHONY: all clean test test_unit test_integration selfcheck install
