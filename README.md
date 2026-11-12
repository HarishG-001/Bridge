# pty_tcp_bridge

A small C program that exposes a Linux serial console (`/dev/ttyS5`, getty login) over a raw TCP socket. It creates a PTY, links the slave side to `/dev/ttyS5`, starts `getty` on it, and forwards bytes between the PTY master and one TCP client on port 5000.

## Why this exists (instead of minicom)

`minicom` (or `screen`, `picocom`, `socat`) already covers serial access, but this target is an embedded device where flash/RAM footprint is the main constraint:

- Adding `minicom` means pulling in its binary plus its library dependencies (ncurses, etc.), which does not fit in the image.
- The requirement is narrow: one serial console, one TCP client, getty login. No terminal UI, dial scripts or modem handling are needed.
- A single C file that depends only on libc/POSIX produces a very small static-friendly binary, which is shipped directly (see `bin/`).

## Architecture

```
NMC / serial console                     Remote user
        |                                      |
   /dev/ttyS5  (symlink -> /dev/pts/X)     TCP client (PuTTY / nc / telnet raw)
        |                                      |
   PTY slave  <----->  PTY master  <--select--> TCP socket (port 5000)
        ^
     /sbin/getty -L ttyS5 115200 vt100
```

| Component | Implementation |
|---|---|
| PTY setup | `posix_openpt()` -> `grantpt()` -> `unlockpt()` -> `ptsname_r()` -> `open(slave)` |
| Device emulation | `symlink(slave_name, "/dev/ttyS5")` |
| Serial config | `termios`: 115200, 8N1, no echo, no SW flow control, raw output |
| Getty lifecycle | `fork()` -> `dup2(slave_fd, 0/1/2)` -> `execl("/sbin/getty", "getty", "-L", "ttyS5", "115200", "vt100")` |
| TCP server | `socket()` -> `bind(INADDR_ANY:5000)` -> `listen(1)` -> `accept()` |
| I/O loop | `select()` on PTY master and client socket; `read()`/`write()` both directions |
| Cleanup | `SIGINT` handler, `cleanup()` / `stop_getty()`, `unlink("/dev/ttyS5")` |

### Flow

1. Create PTY master/slave and symlink the slave to `/dev/ttyS5`.
2. Configure the slave as a 115200 8N1 serial line.
3. Fork and exec `getty` with the slave as stdin/stdout/stderr.
4. Listen on TCP 5000 and accept one client.
5. Forward PTY <-> TCP. When the client disconnects the bridge keeps running and waits for the next client.
6. If getty (or the login session) ends, it is respawned automatically.
7. On `SIGINT`/`SIGTERM`/`quit`: kill getty, close fds, remove the symlink.

### Commands typed from the TCP client

Matched on a whole line (terminated by Enter), not on keystrokes.

- `exit` - kills the current session and starts getty again (new login prompt).
- `quit` - stops getty, cleans up and terminates the bridge.

## Build

```sh
make                      # native
make CC=arm-linux-gnueabihf-gcc LDFLAGS="-s -static"   # cross / static example
```

Or directly: `cc -Os bridge.c -o pty_tcp_bridge`

## Run

```sh
sudo ./pty_tcp_bridge        # root needed for getty and the /dev symlink
nc <host> 5000               # from another machine
```

## systemd unit (optional)

```ini
[Unit]
Description=PTY TCP Bridge
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/pty_tcp_bridge
Restart=on-failure
User=root

[Install]
WantedBy=multi-user.target
```

## Limitations and security

- Single client at a time; requires root and `/sbin/getty` (util-linux/busybox).
- Binds to `INADDR_ANY` with plaintext transport. Restrict with a firewall or tunnel through SSH/stunnel; authentication is only the getty login.

