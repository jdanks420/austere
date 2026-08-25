CC      = cc
CFLAGS  = -std=c11 -g -Wall -Wextra -Werror -pedantic -Isrc -I3rdparty
LDFLAGS = -lxcb -lxcb-keysyms -lxcb-icccm -lxcb-xtest -lxcb-randr \
          -lxcb-shape -lm

SRC     = $(wildcard src/*.c) $(wildcard src/layouts/*.c) \
          $(wildcard src/bar_modules/*.c) 3rdparty/tomlc17.c

ifeq ($(AUSTERE_NO_IMLIB2),1)
WPFLAGS = -DAUSTERE_NO_IMLIB2
else
WPLIBS  = -lImlib2
endif
BIN     = austere

.PHONY: run check tools clean xephyr

$(BIN): $(SRC) Makefile
	$(CC) $(CFLAGS) $(WPFLAGS) -o $@ $(SRC) $(LDFLAGS) $(WPLIBS)

tools: contrib/keyinject contrib/btninject contrib/testclient \
    contrib/monpoke contrib/austere-cmd contrib/hostile-client

contrib/austere-cmd: contrib/austere-cmd.c
	$(CC) $(CFLAGS) -o $@ contrib/austere-cmd.c

contrib/keyinject: contrib/keyinject.c
	$(CC) $(CFLAGS) -o $@ contrib/keyinject.c -lxcb -lxcb-xtest

contrib/btninject: contrib/btninject.c
	$(CC) $(CFLAGS) -o $@ contrib/btninject.c -lxcb -lxcb-keysyms \
	    -lxcb-xtest

contrib/testclient: contrib/testclient.c
	$(CC) $(CFLAGS) -o $@ contrib/testclient.c -lxcb -lxcb-icccm

contrib/monpoke: contrib/monpoke.c
	$(CC) $(CFLAGS) -o $@ contrib/monpoke.c -lxcb
	$(CC) $(CFLAGS) -o contrib/monpoke contrib/monpoke.c -lxcb

# Start the long-lived test display (no-op if already up).
xephyr:
	scripts/xephyr.sh start

# Launch/restart the WM inside Xephyr. --replace takes over WM_Sn from a
# running instance without tearing down clients.
run: $(BIN) tools xephyr
	@D=$$(scripts/xephyr.sh disp); \
	echo "launching austere on $$D"; \
	DISPLAY=$$D exec ./$(BIN) --replace

# Build, run under valgrind when available, drive smoke.sh against it;
# smoke's final quit-bind ends the session so valgrind can produce its
# report. Leak gate is skipped (with a note) if valgrind is absent.
check: $(BIN) tools xephyr
	@scripts/xephyr.sh start >/dev/null; \
	D=`scripts/xephyr.sh disp`; \
	pkill -9 -x testclient 2>/dev/null; \
	pkill -9 -x austere 2>/dev/null; \
	rm -rf ./.smoke-conf; \
	export XDG_CONFIG_HOME=$$PWD/.smoke-conf; \
	export XDG_DATA_HOME=$$PWD/.smoke-data; \
	rm -rf ./.smoke-data; \
	rm -f /tmp/austere-valgrind.log; \
	if command -v valgrind >/dev/null 2>&1 && [ -z "$$NOVG" ]; then \
	  RUN="valgrind --leak-check=full --log-file=/tmp/austere-valgrind.log"; \
	else \
	  echo "note: valgrind not installed; running without leak gate"; \
	  RUN=""; \
	fi; \
	DISPLAY=$$D XDG_CONFIG_HOME=$$PWD/.smoke-conf \
	  AUSTERE_BAR_SCRIPTS="smoke:$$PWD/contrib/ticker-test.sh" \
	  $$RUN ./$(BIN) --replace </dev/null >>/tmp/austere-gate.log 2>&1 & \
	PID=$$!; \
	i=0; \
	while [ $$i -lt 180 ] && ! DISPLAY=$$D xprop -root \
	    _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id'; do \
	  sleep 0.25; i=`expr $$i + 1`; \
	done; \
	if ! DISPLAY=$$D xprop -root _NET_SUPPORTING_WM_CHECK 2>/dev/null | \
	    grep -q 'window id'; then \
	  echo "note: first launch failed to announce; retrying"; \
	  kill -9 $$PID 2>/dev/null; \
	  DISPLAY=$$D XDG_CONFIG_HOME=$$PWD/.smoke-conf \
	    ./$(BIN) --replace </dev/null >/dev/null 2>&1 & \
	  PID=$$!; \
	  i=0; \
	  while [ $$i -lt 120 ] && ! DISPLAY=$$D xprop -root \
	      _NET_SUPPORTING_WM_CHECK 2>/dev/null | grep -q 'window id'; do \
	    sleep 0.25; i=`expr $$i + 1`; \
	  done; \
	fi; \
	ST=0; \
	env DISPLAY=$$D WM_PID=$$PID scripts/smoke.sh || ST=$$?; \
	i=0; \
	while [ $$i -lt 160 ] && kill -0 $$PID 2>/dev/null; do \
	  sleep 0.25; i=`expr $$i + 1`; \
	done; \
	if kill -0 $$PID 2>/dev/null; then \
	  echo "FAIL: austere ignored the quit path"; \
	  kill -9 $$PID 2>/dev/null; ST=1; \
	fi; \
	wait $$PID 2>/dev/null; \
	if [ -s /tmp/austere-valgrind.log ]; then \
	  if grep -qE "definitely lost: 0 bytes|no leaks are possible" \
	      /tmp/austere-valgrind.log; then \
	    echo "valgrind: clean"; \
	  else \
	    echo "valgrind: leaks detected"; ST=1; \
	  fi; \
	fi; \
	exit $$ST

clean:
	rm -f $(BIN) contrib/keyinject contrib/testclient contrib/monpoke

PREFIX ?= /usr/local

.PHONY: install
install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm755 contrib/austere-cmd $(DESTDIR)$(PREFIX)/bin/austere-cmd
	install -Dm644 contrib/austere.desktop \
	    $(DESTDIR)$(PREFIX)/share/xsessions/austere.desktop
