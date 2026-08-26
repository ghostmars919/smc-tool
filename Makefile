CC	?= gcc
CFLAGS	?= -O2 -Wall -Wextra -Werror
PREFIX	?= /usr/local
BINDIR	:= $(PREFIX)/bin
DESTDIR	?= 

VERSION	:= 0.0.1
CPPFLAGS += -DVERSION=\"$(VERSION)\"


all:	smc

smc:	src/smc-tool.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $<


clean:
	rm -f smc

test:	smc
	bash tests/test_cli.sh

install: all
	install -Dm0755 smc $(DESTDIR)$(BINDIR)/smc
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/smc

.PHONY: all clean install uninstall

