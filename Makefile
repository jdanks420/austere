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

.PHONY: clean install uninstall install-states test test-sni test-ui test-menu

$(BIN): $(OBJS) Makefile
	$(CC) $(CFLAGS) $(WPFLAGS) $(NOTIFYFLAGS) -o $@ $(OBJS) $(LDFLAGS) $(WPLIBS) $(NOTIFYLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(WPFLAGS) $(NOTIFYFLAGS) -MMD -MP -c $< -o $@

-include $(DEPS)

contrib/austere-cmd: contrib/austere-cmd.c
	$(CC) $(CFLAGS) -o $@ contrib/austere-cmd.c

clean:
	rm -f $(BIN) $(OBJS) $(DEPS) contrib/austere-cmd

test: test-sni test-ui test-menu

# StatusNotifierWatcher conformance: private Xvfb display, private session
# bus, throwaway HOME. Needs Xvfb, dbus-run-session, dbus-send; the script
# exits 77 when one is missing, which is a skip - not a pass, and not a
# failure of the tree either.
test-sni: $(BIN)
	@rc=0; ./scripts/test-sni-watcher.sh $(BIN) || rc=$$?; \
	if [ $$rc -eq 77 ]; then \
		echo "tray conformance: SKIPPED (test dependencies missing)"; \
	elif [ $$rc -ne 0 ]; then \
		echo "tray conformance: FAILED ($$rc)"; exit $$rc; \
	else \
		echo "tray conformance: PASSED"; \
	fi

# Tray bar module interaction: the same harness plus pointer injection, so
# it needs xdotool/xwininfo/xwd/convert on top. It exits 77 when those are
# missing - a UI-only dependency must never fail the tree. The real-app
# acceptance section inside the script is evidence only and never fails it.
test-ui: $(BIN)
	@rc=0; ./scripts/test-tray-ui.sh $(BIN) || rc=$$?; \
	if [ $$rc -eq 77 ]; then \
		echo "tray UI interaction: SKIPPED (test dependencies missing)"; \
	elif [ $$rc -ne 0 ]; then \
		echo "tray UI interaction: FAILED ($$rc)"; exit $$rc; \
	else \
		echo "tray UI interaction: PASSED"; \
	fi

# Tray item menu: the real popup over a private display and bus, with fake
# SNI + com.canonical.dbusmenu peers. Same dependency rule as test-ui - exit
# 77 is a skip, never a pass and never a failure of the tree.
test-menu: $(BIN)
	@rc=0; ./scripts/test-tray-menu.sh $(BIN) || rc=$$?; \
	if [ $$rc -eq 77 ]; then \
		echo "tray menu: SKIPPED (test dependencies missing)"; \
	elif [ $$rc -ne 0 ]; then \
		echo "tray menu: FAILED ($$rc)"; exit $$rc; \
	else \
		echo "tray menu: PASSED"; \
	fi

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