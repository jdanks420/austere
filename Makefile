CC      = cc
CFLAGS  = -std=c11 -g -Wall -Wextra -Werror -pedantic -Isrc -I3rdparty \
          $(shell pkg-config --cflags fontconfig freetype2)
LDFLAGS = -lxcb -lxcb-keysyms -lxcb-icccm -lxcb-xtest -lxcb-randr \
          -lxcb-shape -lm $(shell pkg-config --libs fontconfig freetype2)

SRC     = $(wildcard src/*.c) $(wildcard src/layouts/*.c) \
          $(wildcard src/bar_modules/*.c) 3rdparty/tomlc17.c
OBJS    = $(SRC:.c=.o)
DEPS    = $(OBJS:.o=.d)

ifeq ($(AUSTERE_NO_IMLIB2),1)
WPFLAGS = -DAUSTERE_NO_IMLIB2
else
WPLIBS  = -lImlib2
endif
BIN     = austere

.PHONY: clean install uninstall

$(BIN): $(OBJS) Makefile
	$(CC) $(CFLAGS) $(WPFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(WPLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(WPFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

contrib/austere-cmd: contrib/austere-cmd.c
	$(CC) $(CFLAGS) -o $@ contrib/austere-cmd.c

clean:
	rm -f $(BIN) $(OBJS) $(DEPS) contrib/austere-cmd

PREFIX    ?= /usr/local
SESSIONDIR ?= /usr/share/xsessions

.PHONY: install
install: $(BIN) contrib/austere-cmd
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm755 contrib/austere-cmd $(DESTDIR)$(PREFIX)/bin/austere-cmd
	install -Dm644 contrib/austere.desktop \
	    $(DESTDIR)$(SESSIONDIR)/austere.desktop

.PHONY: uninstall
uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/bin/austere-cmd
	rm -f $(DESTDIR)$(SESSIONDIR)/austere.desktop