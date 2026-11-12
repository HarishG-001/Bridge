CC      ?= cc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -s

pty_tcp_bridge: bridge.c
	$(CC) $(CFLAGS) $(LDFLAGS) bridge.c -o $@

clean:
	rm -f pty_tcp_bridge

.PHONY: clean
