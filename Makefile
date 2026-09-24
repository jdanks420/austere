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

# Desktop notification service (org.freedesktop.Notifications). Optional:
# AUSTERE_NO_DBUS=1 builds a WM that never owns the bus name.
ifeq ($(AUSTERE_NO_DBUS),1)
NOTIFYFLAGS = -DAUSTERE_NO_DBUS
else
NOTIFYFLAGS = $(shell pkg-config --cflags dbus-1)
NOTIFYLIBS  = $(shell pkg-config --libs dbus-1)
endif
BIN     = austere

.PHONY: clean install uninstall install-states

$(BIN): $(OBJS) Makefile
	$(CC) $(CFLAGS) $(WPFLAGS) $(NOTIFYFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(WPLIBS) $(NOTIFYLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(WPFLAGS) $(NOTIFYFLAGS) -MMD -MP -c $< -o $@

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

STATESDIR ?= $(PREFIX)/share/austere/states

.PHONY: install-states
install-states:
	install -dm755 $(DESTDIR)$(STATESDIR)
	install -m644 states/*.toml $(DESTDIR)$(STATESDIR)