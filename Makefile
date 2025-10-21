.POSIX:

CC=		cc
CFLAGS=		-O2
CFLAGS_=	${CFLAGS}
CFLAGS_+=	-std=c11 -D_POSIX_C_SOURCE=200809L
CFLAGS_+=	-Wall -Wextra -Wpedantic -Wshadow -Werror

PREFIX?=	/usr/local
BINDIR?=	${PREFIX}/bin
MANDIR?=	${PREFIX}/share/man/man1
INSTALL?=	install
MKDIR_P?=	mkdir -p
TARGET=		tinl

all: ${TARGET}

${TARGET}: tinl.c
	${CC} ${CFLAGS_} -o ${TARGET} tinl.c

clean:
	rm -f ${TARGET} test_unit
	rm -rf bin
	${MKDIR_P} bin

test_unit: tinl.c test/unit/test_tinl.c
	${CC} ${CFLAGS} -o test_unit test/unit/test_tinl.c

test_integration: all
	sh test/run-tests.sh

selfcheck: all
	./tinl -q -c tinl.conf test/self

test: test_unit test_integration selfcheck

install: ${TARGET} tinl.1
	${MKDIR_P} ${DESTDIR}${BINDIR}
	${INSTALL} -m 755 ${TARGET} ${DESTDIR}${BINDIR}/${TARGET}
	${INSTALL} -m 755 tinl-check ${DESTDIR}${BINDIR}/tinl-check
	${MKDIR_P} ${DESTDIR}${MANDIR}
	${INSTALL} -m 644 tinl.1 ${DESTDIR}${MANDIR}/tinl.1

.PHONY: all clean test test_unit test_integration selfcheck install
