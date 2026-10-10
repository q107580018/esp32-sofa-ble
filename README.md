# esp32-sofa-ble

An ESP32 that talks to a BLE recliner sofa directly, using the same protocol the
vendor's Android app uses — from a web page on your phone, or from a serial
console.

```
   phone / Mac browser                     ESP32-D0WD-V3              Sofa
   ───────────────────                     ──────────────             ────
   http://esp32-sofa.local/  ──WiFi──▶  esp_http_server  ─┐
   sofa> up 1                ──UART──▶  console           ├─ NimBLE central ─BLE─▶ 0000ae30
                                                        ┘   write no-rsp  ──▶   ..ae01
                                                            notify          ◀──  ..ae02
```

## Where this comes from

The BLE protocol was reverse engineered from the vendor's Android app
`8228A212F28AB37ACC274D284E70115B.apk` ("小派智控", an APICloud hybrid app).
Its UI logic is plaintext JavaScript under `assets/widget/html/` inside the APK,
so the frame bytes below are read rather than guessed; the transport details
(hex payloads, Write Without Response, CCCD handling) come from decompiling the
native `com.apicloud.uzble` module in `classes.dex`.

The APK itself is **not** in this repository — it is the vendor's, not mine.
What is here is the result: a table of frame bytes observed from it. Everything
else (the firmware, the dashboard, the tooling) is written for this project.

See [Protocol](#protocol) for the table and
[Verification status](#verification-status) for what is and is not proven.

## Hardware

Classic **ESP32-D0WD-V3** on a CH340 bridge. The serial device node changes when
the board is re-plugged — check it rather than assuming:

```bash
ls /dev/cu.*        # e.g. /dev/cu.usbserial-2120 (was -2110)
```

The onboard LED reports one thing: **is this usable yet**. Steady on means the
sofa link is ready *and*, when WiFi is configured, the station holds a lease.
1 Hz blink means something is missing — which half shows up in the log and in
`/api/status`. (Measured on this board: GPIO2, **active HIGH**, the opposite of
the usual classic-ESP32 wiring.)

## Quick start

ESP-IDF v5.5.5 via EIM. The login shell on this machine is `fish`, so it needs
the `.fish` activation script — the `.sh` one is bash/POSIX syntax and fails
there:

```bash
source ~/.espressif/tools/activate_idf_v5.5.5.fish   # fish
# or prefix a single command instead of activating:
#   eim run "idf.py build"
```

### 1. Give it your WiFi credentials (optional)

Credentials are **never** committed — not in `sdkconfig.defaults`, not in C.
They live in your local, git-ignored `sdkconfig`:

```bash
idf.py menuconfig     # esp32-sofa-ble configuration -> WiFi SSID / WiFi password
```

It must be a **2.4 GHz** network; the classic ESP32 radio cannot see 5 GHz.

> **Skip this and the board still works** — BLE and the serial console do not
> need a network at all. But with an empty SSID there is no web page, *and* the
> compiler removes the whole WiFi + HTTP path from the image (538 KB instead of
> 1.15 MB). A build with no SSID therefore proves nothing about the web page;
> see [Verification status](#verification-status) for how to build the real one.

### 2. Build and flash

```bash
idf.py build
idf.py -p /dev/cu.usbserial-2120 flash      # check the port first
./tools/monitor.sh 20 --reset               # bounded capture, then exits
```

Look for:

```
I (1234) sofa-wifi: got IP 192.168.1.42 (netmask 255.255.255.0, gateway 192.168.1.1)
I (1234) sofa-wifi: sofa control is at http://192.168.1.42/
I (1250) app:       control page: http://esp32-sofa.local/ (answers once the station has an IP)
I (3100) sofa-ble:  state -> scanning
I (7400) sofa-ble:  found Sofa_wwt_03 (matched on name, rssi -61)
I (8100) sofa-ble:  state -> ready
I (8101) sofa-led:  everything up -> LED steady on
```

Then open **http://esp32-sofa.local/** (or the IP).

## The web page

A single page with a sofa drawn on it. Each cushion turns brass while that seat
is moving, which is the one piece of live state the sofa actually gives us.

The buttons behave differently depending on the sofa's movement mode, and the
page says which is active:

| Mode | What a button does |
|---|---|
| **Runs on its own** (one-shot, `一键启停`) | tap once, the seat runs to its end stop; tap the same button again to end it early |
| **Moves while held** (jog, `点动`) | hold the button, the seat moves; let go and it stops |

The page is self-contained — no CDN, no web fonts, no external requests. It has
to work on a LAN that may have no route to the internet.

### Safety: the hold watchdog

In jog mode the *firmware* is what keeps re-sending the press frame, so a phone
that loses WiFi, a closed tab, or a backgrounded page mid-press would otherwise
leave the seat running. The dashboard sends a keep-alive every 300 ms while a
button is down; if that stops, the board releases the seat itself after
`CONFIG_SOFA_HOLD_TIMEOUT_MS` (800 ms) and logs a warning. The serial console
never arms the watchdog — a person at a terminal is their own watchdog.

### HTTP API

Everything that moves furniture is a `POST`. A `GET` is something a browser will
issue on its own — prefetch, a link preview, a reload — and this is a sofa, not
an idempotent resource.

| Method | Path | Notes |
|---|---|---|
| `GET` | `/` | the dashboard |
| `GET` | `/api/status` | everything below, as JSON |
| `POST` | `/api/move?seat=1\|2\|3\|4\|all&dir=up\|down` | press a seat |
| `POST` | `/api/stop[?seat=…]` | without a seat, stop whatever is held |
| `POST` | `/api/hold` | jog keep-alive |
| `POST` | `/api/mode?value=oneshot\|jog` | |
| `POST` | `/api/dir?value=forward\|reverse` | |
| `POST` | `/api/raw` | body is hex text, e.g. `fa 55` |
| `POST` | `/api/scan` | rescan / reconnect |

Actions answer `{"ok":true,"action":"move"}`, or fail with a status and a
sentence:

```
$ curl -sX POST 'http://esp32-sofa.local/api/move?seat=1&dir=up'
{"ok":true,"action":"move"}

$ curl -s -X POST 'http://esp32-sofa.local/api/stop'
{"ok":false,"action":"stop","error":"no seat has been commanded yet, so there is nothing to stop"}
```

`/api/status` carries the WiFi link, the BLE link and its discovered handles,
the movement state, and the peripherals seen while scanning:

```json
{ "wifi": { "connected": true, "ssid": "home-2g", "ip": "192.168.1.42", "rssi_dbm": -54 },
  "link": { "state": "ready", "ready": true, "peer": "aa:bb:cc:dd:ee:ff",
            "write_handle": 11, "notify_handle": 13, "cccd_handle": 14, "mtu": 23,
            "tx_sent": 12, "notify_count": 3, "motor_stop_count": 1, "last_motor": 3 },
  "movement": { "mode": "oneshot", "direction": "forward", "held": false,
                "reported_mode": null, "reported_direction": null },
  "devices": [ { "addr": "aa:bb:cc:dd:ee:ff", "name": "Sofa_wwt_03", "rssi": -61,
                 "matched": true, "seen": 41 } ] }
```

## Console reference

The firmware also runs an `esp_console` REPL on UART0. Any serial terminal works
at 115200 8N1; type `sofa help`.

```
sofa status                 link state, discovered handles, network, counters
sofa list                   peripherals seen so far, with names and RSSI
sofa scan                   rescan (also reconnects if connected)
sofa connect <aa:bb:..>     connect to one address directly

sofa up   <1|2|3|4|all>     raise a seat
sofa down <1|2|3|4|all>     lower a seat
sofa stop [1|2|3|4|all]     stop; see Movement modes

sofa mode oneshot|jog       movement mode        (f1 00 / f1 01)
sofa dir  forward|reverse   movement direction   (f2 00 / f2 01)
sofa handshake              send fe 5a again
sofa raw <hexbytes>         send arbitrary bytes, e.g. 'sofa raw fa 55'
```

An agent or a script cannot hold an interactive TTY, so the bundled capture
wrapper can send lines itself:

```bash
./tools/monitor.sh 8 --reset --send "sofa status"
./tools/monitor.sh 10 --send "sofa list" --send "sofa up 1"
```

Find the sofa's address once, then pin it in `menuconfig` so boots skip the scan
(`esp32-sofa-ble configuration` → *Target BLE address*).

## Movement modes

The sofa has two, and which one is active changes what "stop" means. The
firmware follows the sofa — the mode arrives in the `f5` status frame — and
`sofa mode` / the web page can change it.

| Mode | Frame sent while a button is pressed | Stops when |
|---|---|---|
| **One-shot** (`一键启停`, default) | `fa <btn>` once | the run reaches its end stop, or the **same button is pressed again** |
| **Jog** (`点动`) | `f8 <btn>` every 50 ms, then `fb <btn>` on release | the frames stop arriving |

That asymmetry is why `sofa stop <seat>` refuses to act when nothing is held in
**one-shot** mode: stopping there means pressing the *same* button again, and
guessing the other direction would start the sofa moving instead of stopping it.
Use plain `sofa stop` (repeats the last button) or the explicit direction.

## Protocol

Recovered from the APK. All frames are written to `ae01` as **Write Without
Response** — the app never sets a write type, and the native module's default
(`"auto"`) resolves to `WRITE_TYPE_NO_RESPONSE`.

| | |
|---|---|
| Service (advertised) | `0000ae30-0000-1000-8000-00805f9b34fb` |
| Write (us → sofa) | `0000ae01-0000-1000-8000-00805f9b34fb` |
| Notify (sofa → us) | `0000ae02-0000-1000-8000-00805f9b34fb` |
| CCCD | `0x2902`, written `0x0001` (notify) |
| Device name | `Sofa_wwt_01` … `Sofa_wwt_04` (1- to 4-seater) |
| Pairing / bonding / encryption | none |

All three UUIDs are Bluetooth SIG base UUIDs, so they appear on air as the
16-bit forms `0xAE30`, `0xAE01`, `0xAE02`.

**Sent to `ae01`** (hex):

| Frame | Meaning |
|---|---|
| `fe 5a` | handshake, written once after subscribing |
| `fa <btn>` | press, one-shot mode |
| `f8 <btn>` | press, jog mode (repeat while held) |
| `fb <btn>` | release, jog mode |
| `f1 00` / `f1 01` | movement mode: one-shot / jog |
| `f2 00` / `f2 01` | movement direction: forward / reverse |

**Button codes** — the low nibble raises a seat, the high nibble lowers it:

| | seat 1 | seat 2 | seat 3 | seat 4 | all seats |
|---|---|---|---|---|---|
| up | `01` | `02` | `04` | `08` | `55` |
| down | `10` | `20` | `40` | `80` | `66` |

**Received on `ae02`** (notifications), parsed positionally the same way the app
does:

| Frame | Meaning |
|---|---|
| `f5 mm f6 dd` | `mm` = movement mode (1 = one-shot), `dd` = direction (1 = forward) |
| `f7 nn` | motor `nn` stopped: 1–4 = that seat, 5 = all seats together |

The sofa never announces that a motor *started*, only that one stopped — which
is why the dashboard lights a cushion from what it commanded and clears it from
the notification.

Two frames the app contains but never sends are worth knowing about:
`ff 09 10 00 00 00 0d 0a`, commented out in `main.html:263` of the APK.

## Configuration

`idf.py menuconfig` → *esp32-sofa-ble configuration*, or as defaults in
`sdkconfig.defaults`:

| Option | Default | Meaning |
|---|---|---|
| `SOFA_DEVICE_NAME_PREFIX` | `Sofa_wwt` | name filter; empty = match on the service UUID only |
| `SOFA_TARGET_ADDR` | `ADDR_ANY` | pin to one sofa's address, skipping the scan |
| `SOFA_CONNECT_TIMEOUT_MS` | 30000 | per-attempt connect timeout |
| `SOFA_SCAN_INTERVAL_MS` | 100 | how often the scanner listens (see below) |
| `SOFA_SCAN_WINDOW_MS` | 30 | how long each interval listens — a 30% duty cycle |
| `SOFA_RETRY_DELAY_MS` | 3000 | pause before rescanning after a disconnect |
| `SOFA_HANDSHAKE` | y | send `fe 5a` after subscribing |
| `SOFA_JOG_REPEAT_MS` | 50 | jog repeat interval, matching the app |
| `SOFA_ANNOUNCE_NEW_DEVICES` | y | log every new device seen while scanning |
| `SOFA_HOLD_TIMEOUT_MS` | 800 | release a held seat if the page stops checking in |
| `SOFA_WIFI_SSID` | *(empty)* | network to join; empty = BLE and console only |
| `SOFA_WIFI_PASSWORD` | *(empty)* | passphrase; empty = open network |
| `SOFA_WIFI_AUTH_THRESHOLD` | WPA2 PSK | weakest auth mode accepted while scanning |
| `SOFA_WIFI_RETRY_DELAY_MS` | 2000 | pause between WiFi reconnects, forever |
| `SOFA_HTTP_PORT` | 80 | control page and API |
| `SOFA_ENABLE_MDNS` | y | advertise `http://<hostname>.local/` |
| `SOFA_MDNS_HOSTNAME` | `esp32-sofa` | must be unique per board |
| `SOFA_STATUS_LED_GPIO` | 2 | onboard LED |
| `SOFA_STATUS_LED_ACTIVE_LOW` | n | measured HIGH on this board |

## How it fits together

`sofa_ble.c` is a NimBLE **central**. On boot it scans actively (the name lives
in the scan response, so duplicate filtering is off), matches on the name prefix
or the `0xAE30` service, connects, discovers `ae01`/`ae02`, writes the CCCD, and
only then is the link "ready". Disconnects rescan on a timer, forever.

The NimBLE GATT client API is only safe to call from the host task, so neither
the console nor an HTTP handler ever touches it: `sofa_ble_send()` drops the
frame in a FreeRTOS queue and posts a `ble_npl_event` onto the host task's own
event queue. Rescan and connect-by-address requests go the same way.

Boot order matters and is not the order the names suggest — `wifi_sta_init()`
(TCP/IP stack) must run before `http_server_start()` (which creates a socket),
and both before `wifi_sta_start()` (asynchronous association). Binding before an
IP exists is fine; creating a socket before `esp_netif_init()` aborts at runtime
with an `Invalid mbox` assert while compiling perfectly happily.

### IRAM is the scarce resource, not flash

WiFi and NimBLE together do **not** fit in IRAM with the stock settings — the
link fails with `IRAM0 segment data does not fit`. `sdkconfig.defaults`
therefore turns off `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT` and
`ESP_WIFI_SLP_IRAM_OPT`, which moves WiFi code back to flash at some latency
cost. The workload is a 2-byte write every 50 ms on a mains-powered board, so
this is cheap. The result is 82% of IRAM used with ~23 KB free: **anything new
that wants `IRAM_ATTR` may not link.** Check `idf.py size` before adding any.

## Verification status

Verified **on the board** (ESP32-D0WD-V3, ESP-IDF v5.5.5, target `esp32`) —
everything except the sofa itself:

- Boots clean; HTTP binds port 80 in ~650 ms; LED blinks because the sofa link
  never becomes ready, which is correct with no sofa.
- WiFi joins the configured AP, negotiated as **WPA3-SAE H2E with PMF**, and
  takes a DHCP lease. An unrelated device in range advertises service `0xAE30`
  without a sofa name; it is now logged and ignored instead of being connected
  to (see below).
- `GET /` returns `main/index.html` **byte-identical** (14228 bytes).
- `/api/status` returns valid JSON with live WiFi, link and device data.
- The scanner found 13–16 devices in an ordinary flat, several of them with
  names — the radio and the scanner work.
- Every API error path, with `tx_sent` still `0` afterwards, i.e. nothing
  reached the radio: `400` for a bad seat, a bad direction, a bad mode and a
  non-hex body; `404` for an unknown path; `409` with
  `"the sofa link is not ready"` for a move, and with
  `"no seat has been commanded yet"` for a stop.
- `http://esp32-sofa.local/` resolves over mDNS and serves the page.

Two bugs were found and fixed *by* that run, both invisible without hardware:

1. **Matching on the service UUID instead of the name.** The board connected to
   that unrelated `0xAE30` advertiser and retried forever. The app requires the
   `Sofa_wwt` name; the firmware now does too, and logs the mismatch.
2. **The device table leaked across evictions.** It always overwrote slot 0 and
   kept the previous occupant's `matched` flag, so `sofa list` showed a
   different device as a sofa while the address actually being chased was
   silently dropped. It now evicts the least-seen entry and resets the slot.

Verified **on the host**, without a board:

- Both configurations build clean — no warnings under the IDF default
  `-Wall -Wextra -Werror=all`:
  - no SSID: 538 KB, 65% of the app partition free (WiFi + HTTP *removed by the
    compiler*, see the warning in Quick start)
  - with an SSID: 1.15 MB, 25% free, and `http_server_start`, `wifi_sta_start`,
    `mdns_init`, `get_status`, `post_move` all present in the ELF with the
    embedded `index.html` byte-exact.
- Every frame builder and the status parser, via a throwaway harness over
  `sofa_proto.c` (`cc -Wall -Wextra -Werror`) asserting the exact bytes in the
  protocol table.
- The dashboard, against a mock of its own API in a real browser: it renders
  live status; in jog mode a press produces one `/api/move`, keep-alives every
  300 ms, and exactly one `/api/stop` on release; in one-shot mode a press
  produces the move and **no** stop on release; a `f7 nn` notification clears
  exactly the right cushion; a dropped link disables every control and says
  "Looking for the sofa."; a failed request is shown verbatim and does **not**
  light a cushion.

**Not verified — needs a sofa:**

- That the sofa accepts these frames at all. This is the big one.
- Whether the `fe 5a` handshake is *required* (the app always sends it, but
  static analysis cannot show the peripheral enforces it).
- Whether `ae02` is notify or indicate (the app's module has an indicate branch
  it never reaches; this firmware writes `0x0001` and retries `0x0002` once).
- Whether the sofa keeps a per-address allow-list (the app has no pairing flow,
  so probably not).
- Whether commands need spacing. Writes are queued and drained one at a time,
  with a 20 ms backoff if the controller runs out of buffers.
- Whether the "all seats" button produces `f7 05` (one stop) or `f7 01..04`
  (four). The dashboard handles both.

The first thing to do with an actual sofa is the read-only half:

```bash
./tools/monitor.sh 20 --reset --send "sofa list" --send "sofa status"
```

Expect `scanning` → `connecting` → `discovering` → `ready`, a logged `tx fe 5a`,
and a `f5 .. f6 ..` notification. Only then start moving things, and keep a hand
near the power while you do.

## Troubleshooting

**The page never loads and the board has an IP.** Another device may already
answer to `esp32-sofa.local` — change `CONFIG_SOFA_MDNS_HOSTNAME`, or just use
the IP. If the IP itself is unreachable, check for AP client isolation on guest
networks, the most common cause.

**`http://esp32-sofa.local/` takes about five seconds, but the IP is instant.**
That is your Mac, not the board. Measured here: `curl` to the name spent
5.00 s in `time_namelookup` and 0.2 s on everything else, while the same
request by IP resolved in 1 ms — macOS tries unicast DNS before multicast and
waits out the timeout. Bookmark the IP if it bothers you; the board's HTTP task
is not slow.

**A warning about a device advertising `0xae30` that is "ignored".** That is
working as intended: something in range speaks the sofa's service UUID but does
not have a sofa name. Before this was a warning the board connected to it and
retried a connection that could never succeed, forever. Set
`CONFIG_SOFA_DEVICE_NAME_PREFIX` empty only if you have read the warning and
your sofa really does advertise a different name.

**The page loads but says "Cannot reach the board".** The board's HTTP task is
alive but `/api/status` is not answering; check the serial log.

**LED blinks forever.** Something is not up. `sofa status` says which: `network`
is the WiFi half, `link` the sofa half.

**`scanning` forever, `sofa list` empty.** `SOFA_DEVICE_NAME_PREFIX` does not
match, or the sofa is already connected to the phone app (these modules usually
take one central at a time). `sofa list` shows *everything* seen, so if even
unrelated devices are missing, it is the radio, not the filter.

**`sofa list` shows the sofa but nothing connects.** Turn
`SOFA_ANNOUNCE_NEW_DEVICES` on for a louder log, and try
`sofa connect <address>` to rule the scan match out.

**`service 0xae30 not found` right after connecting.** The sofa accepted the
connection but its GATT table differs from the app's. Raise the NimBLE log level
to Debug (*Component config → Bluetooth → NimBLE*), and try `sofa raw fe5a` or
`POST /api/raw` with body `fe5a` to see whether it is purely a discovery
problem.

**`IRAM0 segment data does not fit` at link time.** A `sdkconfig` from before
the IRAM options were added is still in force: those are `sdkconfig.defaults`
values, and an existing `sdkconfig` wins. Delete `sdkconfig` and rebuild — **but
note it is the only place your WiFi credentials live**, so re-enter them with
`idf.py menuconfig` afterwards. The same rule is why an old `sdkconfig` with an
empty SSID keeps the whole HTTP path compiled out even after you set one in
`menuconfig`.

**The binary is 538 KB and every route 404s.** `CONFIG_SOFA_WIFI_SSID` is empty,
so the whole WiFi + HTTP path was compiled out. That is the documented
behaviour, not a fault — but it also means a stale empty SSID in `sdkconfig`
looks exactly like a broken build. Check with
`grep CONFIG_SOFA_WIFI_SSID sdkconfig`.

## Layout

```
CMakeLists.txt          project root — idf.py must run from here
sdkconfig.defaults      intentional defaults (sdkconfig itself is generated)
main/
  CMakeLists.txt        idf_component_register — PRIV_REQUIRES + EMBED_FILES
  Kconfig.projbuild     every option in the table above
  main.c                app_main: NVS -> LED -> control -> WiFi -> HTTP -> NimBLE -> console
  sofa_proto.c/.h       frames and status parsing; no BLE, no ESP-IDF
  sofa_ble.c/.h         NimBLE central: scan, connect, discover, notify, TX queue
  sofa_control.c/.h     movement mode, held button, jog repeat, hold watchdog
  sofa_console.c/.h     the `sofa` REPL command
  http_server.c/.h      routes and the JSON API
  index.html            the dashboard, linked in via EMBED_FILES
  wifi_sta.c/.h         station bring-up, paced reconnects forever
  status_led.c/.h       GPIO2: blink = not ready, steady = ready
tools/
  monitor.sh            bounded, non-interactive serial capture
  serial_log.py         ...with a project-local --send for the REPL
README.md               this file
AGENTS.md               evolving notes for coding agents
```

`index.html` is served from flash, not a filesystem — the linker places it in
rodata and it goes out in one `httpd_resp_send`. Past ~100 KB, revisit.
