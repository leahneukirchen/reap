ALL=reap

CFLAGS=-g -O2 -Wall -Wno-switch -Wextra -Wwrite-strings

DESTDIR=
PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

all: $(ALL)

clean: FRC
	rm -f $(ALL)

FRC:
