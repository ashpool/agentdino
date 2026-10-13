# agentdino

An Arduino UNO R4 WiFi that shows Claude Code's status on its 12x8 LED matrix, one lane per Claude session. Claude Code hooks send commands to the board over USB serial or UDP; the firmware turns them into animations. No program runs on the laptop: each hook starts `signal.pl`, which sends one packet and exits.

## Layout

- `signal/signal.ino`: firmware. Reads USB serial lines and, once WiFi is up, UDP port 4210; renders up to 6 session lanes.
- `signal/arduino_secrets.h`: `SECRET_SSID` / `SECRET_PASS`. Gitignored. Never print, log or commit it.
- `signal.pl`: host side. Perl because it ships with macOS and starts fast (measure with `time (printf '{"session_id":"abcd-1"}' | AGENTDINO_HOST=127.0.0.1 ./signal.pl W)`, about 30 ms including the shell; `python3` through the pyenv shim costs ~250 ms, check with `time python3 -c pass`). Reads the first chunk of the hook JSON on stdin for `session_id` and sends `<first 4 chars>:<commands>`. Exits 0 even when sending fails, so a missing board never fails a hook (only a missing argument is an error).
- `test/signal.t`: tests for `signal.pl` against local UDP listeners. Run with `prove test/`. No board or external network needed.

## Protocol

Datagram (UDP) or newline-terminated line (USB serial): `<4-char session key>:<command bytes>`, e.g. `ab12:OW` or `ab12:S5eb7`. Commands are handled in order; `S` and `s` are followed by a 4-char agent id (the last 4 chars of the hook's `agent_id`, `0000` if absent). Anything not matching this shape is ignored.

| Byte | Meaning | Lane display (2 columns wide, one lane per session) |
|---|---|---|
| `W` | working | 2x2 block bouncing vertically |
| `S<id>` / `s<id>` | subagent started / stopped | one dot per running subagent in the bottom two rows (max 4 shown, 8 tracked); ignored for unknown sessions |
| `D` | done | solid lane for 3 s, then the lane is freed |
| `I` | input needed | blinking `!` until the next command for that session |
| `O` | session gone | lane freed |

Rules worth knowing:
- Lanes are allocated on first `W` / `D` / `I`. A session gets its previous lane back after it was freed (so `OW` does not make it jump), unless another session has taken that lane meanwhile. With more than 6 active sessions the least recently active lane is evicted.
- Repeating the current mode is ignored, so frequent `W` does not restart the animation.
- The firmware keeps the set of running agent ids per lane: a duplicate start, a duplicate stop, and a stop for an agent it never saw start are all ignored. This matters because `SubagentStop` fires for more agents than `SubagentStart` (observed: 8 stops for 2 starts, one agent stopping twice).
- `D` and `O` clear the lane's agent set.
- Watchdogs (Esc interrupts fire no hook): a `WORKING` lane with no command for 10 minutes is freed, unless it has running subagents; an `INPUT_NEEDED` lane is freed after 10 minutes. Subagent `PostToolUse` hooks also fire with the parent's `session_id` and act as heartbeats (observed in a hook log).
- `OW` is the "new prompt" reset: clear the lane, then start working.
- Session key: with no stdin data (manual use, terminal or `/dev/null`) it is `0000`. If stdin is open but silent for 1 s, `signal.pl` sends nothing rather than creating a stray `0000` lane.
- WiFi: the firmware does not wait for it at boot (USB serial works without it), reconnects without blocking the display and reopens its UDP socket afterwards (the socket reopen is untested on real hardware).

## Hook wiring

Hooks live in the user's global `~/.claude/settings.json`, not in this repo. All use `"timeout": 2`. They sit alongside pre-existing terminal-notifier hooks, which must not be changed.

| Hook | Command |
|---|---|
| `UserPromptSubmit` | `signal.pl OW` |
| `PostToolUse` | `signal.pl W` (also the heartbeat, and what clears `!` after a permission prompt is answered) |
| `SubagentStart` / `SubagentStop` | `signal.pl S` / `signal.pl s` |
| `Stop` | `signal.pl D` |
| `Notification`, matcher `permission_prompt` | `signal.pl I` |
| `SessionEnd` | `signal.pl O` |

## Discovery

USB first: without `AGENTDINO_HOST`, `signal.pl` writes to the first `/dev/cu.usbmodem*` (override with `AGENTDINO_SERIAL`, required if a second USB serial device is attached), so a board plugged into the laptop needs no WiFi or address. `AGENTDINO_HOST` without `AGENTDINO_SERIAL` means UDP. If the serial write fails (port held by the IDE, board mid-upload) it falls back to UDP; with no host that is a broadcast. `signal.pl` gives up after 1.5 s so a stuck port cannot time out the hook.

By default over UDP `signal.pl` broadcasts to `255.255.255.255:4210`, so no board address is needed. Set `AGENTDINO_HOST` (an IP, not a hostname: a DNS lookup would run on every hook) to unicast instead (needed if a VPN routes the broadcast to the wrong interface, or the network drops broadcasts); `AGENTDINO_PORT` overrides the port. Every board on the LAN would react to a broadcast. Hooks inherit Claude Code's environment, so set `AGENTDINO_HOST` either in the `env` block of `~/.claude/settings.json` or by exporting it in your shell profile before launching `claude`.

## Known limitations of the hook mapping

- There is no `PreToolUse` hook wired, so after you approve a permission prompt the `!` stays until the tool finishes and `PostToolUse` fires. With parallel tool calls, one tool finishing can clear a `!` while another still waits.
- A long single tool call or thinking step over 10 minutes frees the working lane until the next tool call.
- A straggling `PostToolUse` after `Stop` (for example from a background subagent) can recreate a working lane for up to 10 minutes.
- Esc at a permission prompt leaves the `!` until the 10-minute watchdog.

## Common tasks

Send a signal by hand (shows as a lane with key `0000`):
```sh
./signal.pl W
```

Simulate a session:
```sh
printf '{"session_id":"abcd-1"}' | ./signal.pl OW
```

Build and flash (needs USB; `arduino-cli` and the `arduino:renesas_uno` core are installed via brew):
```sh
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi signal
arduino-cli upload -p /dev/cu.usbmodem* --fqbn arduino:renesas_uno:unor4wifi signal
```

## No signal? Troubleshooting

`signal.pl` fails silently by design, so check these in order:

1. Is the board on? It needs USB power and joins WiFi at boot; it retries forever and shows nothing until connected.
2. Does broadcast reach it? On this macOS setup `nc -b` is not broadcast, so use Perl: `perl -MIO::Socket::INET -e 'IO::Socket::INET->new(Proto=>"udp",PeerAddr=>"255.255.255.255",PeerPort=>4210,Broadcast=>1)->send("0000:W")'`. If that fails, set `AGENTDINO_HOST` (step 3) and try unicast.
3. Find the board's IP by reading serial over USB (it prints the IP once WiFi is connected; press the board's reset button if it already booted):
   ```sh
   python3 - <<'PY'
   import os, select, glob, re, time
   fd = os.open(glob.glob('/dev/cu.usbmodem*')[0], os.O_RDONLY | os.O_NONBLOCK)
   buf, end = b'', time.time() + 30
   while time.time() < end and not re.search(rb'\d+\.\d+\.\d+\.\d+', buf):
       if select.select([fd], [], [], 1)[0]:
           buf += os.read(fd, 256)
   print(buf.decode(errors='replace').strip() or 'no output')
   PY
   ```
   Then `AGENTDINO_HOST=<ip> ./signal.pl W`, or ping it. A DHCP reservation keeps the IP stable.
4. Hooks do nothing but manual sends work: run `/hooks`, and restart the session after settings changes.
5. Board never connects: check `signal/arduino_secrets.h` (without printing it) and that the network is 2.4 GHz.

## Gotchas

- Flashing is the only thing that needs the USB cable. If an upload hangs, something else is holding the port (Arduino IDE serial monitor, a stuck `arduino-cli`).
- macOS: there is no `timeout` command, `sed -i` needs `sed -i ''`, and a BSD `sed` replacement containing `&` needs escaping. Prefer a Python or Edit-based change for file edits.
- macOS does not loop broadcasts back to local sockets, so broadcast delivery cannot be tested on the laptop itself; `test/signal.t` checks the target address and `SO_BROADCAST` instead.
- The pixel buffer passed to `loadPixels` must be non-const `uint8_t`.
- The board only joins 2.4 GHz networks. Corporate WiFi with client isolation or WPA2-Enterprise will not work. The corporate VPN may route the broadcast to the wrong interface.
- UDP has no acknowledgement and no authentication. A dropped packet leaves a stale lane until the next command or the watchdog; anyone on the network can change the display.
- When verifying visually, run the test sequence first and ask the user afterwards. Asking in the same step makes them answer before it has played.
