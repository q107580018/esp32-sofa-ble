# AGENTS.md — esp32-sofa-ble

Project-specific instructions.

> **Read `../AGENTS.md` too, if you have it.** In the author's local workspace
> that file carries the board facts, serial port, toolchain activation,
> flash/monitor commands and safety rules that apply to every ESP32 project.
> It is **not part of this repository** — everything needed to build and flash
> this project is in `README.md`. Where the two disagree, this file and the
> README win.
>
> **This file holds only the evolving layer** — what this firmware does, and
> what to touch when that changes. Keep it short; keep it current.

## Current functionality

- NimBLE **central** that scans for a `Sofa_wwt_*` peripheral (or a pinned
  address), connects, discovers service `0xAE30` and characteristics
  `0xAE01`/`0xAE02`, subscribes to notifications, and sends the `fe 5a`
  handshake. Reconnects forever.
- **HTTP control surface** on `CONFIG_SOFA_HTTP_PORT`, with a self-contained
  dashboard (`main/index.html`) that draws the sofa and lights a cushion while
  that seat moves. WiFi station with unlimited paced reconnects, optional mDNS
  as `http://esp32-sofa.local/`.
- JSON API for moving, stopping, mode, direction, raw frames and rescan.
- `sofa` console command (esp_console REPL on UART0) with the same powers.
- Movement mode is tracked from the sofa's `f5` status frame; in jog mode a
  press repeats every `CONFIG_SOFA_JOG_REPEAT_MS` until stopped, with a hold
  watchdog the HTTP layer arms and the console deliberately does not.
- Status LED on `CONFIG_SOFA_STATUS_LED_GPIO`: 1 Hz blink until the sofa link is
  ready *and* (if configured) WiFi is up, then steady on.
- Log tags: `app`, `sofa-ble`, `sofa-ctl`, `sofa-cli`, `sofa-http`, `sofa-wifi`,
  `sofa-led`.

Nothing else yet. **Do not treat the current feature set as a scope limit.**

## Adding a feature

1. Update *Current functionality* above, and the README if a user-visible thing
   changed.
2. New tunables go in `main/Kconfig.projbuild`, not hardcoded in C. New hardware
   pins go there too.
3. Add the owning component to `PRIV_REQUIRES` in `main/CMakeLists.txt`. In
   v5.5 the drivers are split into `esp_driver_*` components and a missing entry
   is the #1 build error here; `bt` is the umbrella that exposes the NimBLE
   headers.
4. Read the threading rule below before touching anything that talks to the BLE
   stack — it is the whole design of `sofa_ble.c`.
5. Search the local ESP-IDF checkout before writing driver code. The closest
   working examples are `examples/bluetooth/nimble/blecent` and
   `examples/protocols/http_server/simple`.
6. Leave `../AGENTS.md` alone unless the board or toolchain actually changed.

## Layout

```
CMakeLists.txt          project root — idf.py must run from here
sdkconfig.defaults      intentional config defaults (sdkconfig itself is generated)
main/
  CMakeLists.txt        idf_component_register — PRIV_REQUIRES + EMBED_FILES
  Kconfig.projbuild     every tunable
  main.c                app_main and the LED fan-in
  sofa_proto.c/.h       frames + status parsing; no BLE, no ESP-IDF headers
  sofa_ble.c/.h         NimBLE central and the TX/control event marshalling
  sofa_control.c/.h     movement mode, held button, jog repeat, hold watchdog
  sofa_console.c/.h     the `sofa` command
  http_server.c/.h      routes and the JSON API
  index.html            the dashboard, linked in via EMBED_FILES
  wifi_sta.c/.h         station bring-up with paced reconnects
  status_led.c/.h       GPIO2, blink vs steady
tools/
  monitor.sh            bounded, non-interactive serial capture
  serial_log.py         ...with a project-local --send for the REPL
README.md               human-facing setup, protocol table, troubleshooting
```

## Project-specific notes

- **Match on the NAME, not the service UUID.** `adv_is_sofa()` requires the
  configured prefix whenever one is set, and the `0xAE30` check is only a hint
  for logging. This is what the app does, and getting it wrong is not
  theoretical: an unrelated device in this flat advertises `0xAE30`, and
  matching on it made the board connect to that and retry a doomed connection
  forever, never reaching a sofa. Any such mismatch is logged as a warning on
  first sight — that warning is the designed way to find out your sofa uses a
  different name.
- **The seen-device table is eviction-sensitive.** `seen_index()` evicts the
  least-seen entry and the caller wipes the slot completely. It used to
  overwrite slot 0 and keep the old `matched` flag and `seen_count`, which made
  `sofa list` name the wrong device as a sofa while evicting the address the
  board was actually talking to. If you touch that function, keep both halves:
  least-seen, and a full reset.
- **`http://esp32-sofa.local/` takes ~5 s on macOS and that is the Mac.** Timing
  it shows all five seconds in `time_namelookup`; by IP the same request takes
  ~0.2 s. Do not chase it in the firmware — measured, not assumed.
- **The BLE scan duty cycle is an airtime knob, not a latency knob.**
  `SOFA_SCAN_INTERVAL_MS`/`SOFA_SCAN_WINDOW_MS` default to 100/30 ms. The
  controller default of a 10 ms window every 10 ms leaves the radio scanning
  permanently, which on this single-radio chip is airtime taken from WiFi. An
  earlier attempt to blame a five-second HTTP delay on that was **wrong** (it
  was mDNS); the justification is that 30% duty still found every advertiser in
  the room within ~3 s, including ones 90 dB down.
- **The protocol table in `README.md` is evidence, not design.** Every frame
  byte comes from the vendor APK (plaintext JS in `assets/widget/html`, plus the
  decompiled `com.apicloud.uzble` module). If a frame does not work, the first
  hypothesis is that *the sofa firmware differs*, not that the table is wrong —
  and the fix is new evidence, not a guess. `sofa_proto.c` compiles with plain
  `cc` (no ESP-IDF headers) precisely so its bytes can be asserted without a
  board.
- **Everything is Write Without Response on `0xAE01`.** The app's native module
  takes a `writeType` option defaulting to `"auto"`, which its own code maps to
  `WRITE_TYPE_NO_RESPONSE` (1); the app never overrides it. If a write ever
  needs a response type, that is a discovery, not a default to flip.
- **Only the NimBLE host task may start GAP procedures or touch the GATT
  client.** `ble_gattc_write_no_rsp_flat()` does *not* take the host lock, so
  calling it from the console or an HTTP handler is a real race, not a
  theoretical one. That is why `sofa_ble_send()` goes through a FreeRTOS queue
  plus a `ble_npl_event` posted to `nimble_port_get_dflt_eventq()`, and why
  `sofa_ble_scan_restart()` and `sofa_ble_connect_to()` post `s_ctl_ev` instead
  of calling GAP directly. HTTP handlers run on the httpd task and are no
  exception. Keep it that way.
- **In this NimBLE the GAP event tag is `event->type`, not
  `event->event_type`.** The bundled blecent example uses the latter name and
  does not compile against the locally installed v5.5.5 tree — expect that
  mismatch if you copy from the examples, and check the struct in
  `components/bt/host/nimble/nimble/nimble/host/include/host/ble_gap.h`.
- **`ble_uuid_cmp()` returns the *type* difference immediately** when the two
  UUIDs are of different widths, so it cannot compare a 16-bit on-air UUID with
  a 128-bit literal. `ble_uuid_from_str()` collapses SIG base UUIDs to 16 bits
  (ours are all SIG base UUIDs), but a peripheral that spelled one out long is
  still reported long — hence `uuid_matches()` in `sofa_ble.c`, which accepts
  either. Do not replace it with a bare `ble_uuid_cmp()`.
- **A scan response is a separate event and duplicate filtering must stay off.**
  The sofa puts the `0xAE30` service in the advertisement and its name in the
  scan response; `disc_params.filter_duplicates = 0` plus `passive = 0` is what
  turns the Android app's `startLeScan(serviceUuids, …)` into the same thing.
  Turning duplicate filtering on silently loses the name (and with it
  `sofa list`) while still matching on the UUID, so it fails confusingly.
- **One-shot mode makes "stop" direction-dependent.** Stopping means pressing
  the *same* button again, so stopping a target whose direction is unknown would
  have to guess — and guessing wrong starts the sofa moving. That is why
  `sofa_control_stop_target()` returns `ESP_ERR_NOT_SUPPORTED` rather than
  trying both codes, and why the dashboard sends **no** stop on button release
  in one-shot mode. Do not "fix" either by pressing both.
- **IRAM is the binding constraint and WiFi + BLE do not fit by default.** The
  link fails with `IRAM0 segment data does not fit` unless
  `ESP_WIFI_IRAM_OPT`, `ESP_WIFI_RX_IRAM_OPT` and `ESP_WIFI_SLP_IRAM_OPT` are
  off, which `sdkconfig.defaults` does. Result: ~82% of IRAM used, ~23 KB free.
  Anything new that wants `IRAM_ATTR` may not link — check `idf.py size` first.
  Flash is not the problem (1.15 MB of a 1.5 MB partition, 25% free).
- **A `sdkconfig` predating those options keeps them on**, because
  `sdkconfig.defaults` only seeds a *new* `sdkconfig`. This bites in three
  places at once, because that one file also holds the WiFi credentials and
  decides whether the HTTP path is compiled in at all:
  - `CONFIG_ESP_WIFI_IRAM_OPT` etc. left on → `IRAM0 segment data does not fit`.
  - `CONFIG_SOFA_WIFI_SSID` left empty → the whole WiFi + HTTP path is dropped
    and the image stays 538 KB, with no error to tell you why.
  - The credentials themselves are only in this file.

  So deleting `sdkconfig` is the fix for all of them, **but it loses the WiFi
  credentials** — re-enter them with `idf.py menuconfig`, or regenerate with a
  throwaway defaults fragment:
  ```bash
  printf 'CONFIG_SOFA_WIFI_SSID="MyAp"\nCONFIG_SOFA_WIFI_PASSWORD="secret"\n' > /tmp/wifi.defaults
  rm -f sdkconfig sdkconfig.old
  SDKCONFIG_DEFAULTS="sdkconfig.defaults;/tmp/wifi.defaults" eim run "idf.py build"
  rm /tmp/wifi.defaults
  ```
  Afterwards a plain `idf.py build` keeps them: an existing `sdkconfig` wins
  over `sdkconfig.defaults`, which is the same rule that caused the problem.
- **Do not drive `kconfiglib` blindly to set the credentials.** `set_value()`
  takes the *raw* value for a string symbol (`'my-ap'`), not a pre-quoted one
  (`'"my-ap"'`, which lands in `sdkconfig` as `\"my-ap\"` and never matches an
  AP), and a bare `write_config()` also drops the header, the deprecated-options
  block and unexpanded `$VAR` defaults. The defaults-fragment route above avoids
  both traps.
- **An empty `CONFIG_SOFA_WIFI_SSID` silently removes the WiFi + HTTP path.**
  `strlen("") == 0` folds at compile time, so GCC's dead-code elimination drops
  `wifi_sta.o`, `http_server.o` and the embedded `index.html` entirely: 538 KB
  instead of 1.15 MB, with no `http_server_start`/`wifi_sta_start`/`mdns_*`
  symbols in the ELF. A clean build therefore proves *nothing* about the web
  page until an SSID is set. To validate that half, build a throwaway tree:
  ```bash
  printf 'CONFIG_SOFA_WIFI_SSID="verify"\nCONFIG_SOFA_WIFI_PASSWORD="verify"\n' > /tmp/verify.defaults
  SDKCONFIG_DEFAULTS="sdkconfig.defaults;/tmp/verify.defaults" \
    eim run "idf.py -B /tmp/sofa-verify-build -D SDKCONFIG=/tmp/sofa-verify-sdkconfig build"
  ```
  Then confirm it is real: `xtensa-esp32-elf-nm` on the ELF should show
  `http_server_start`, and `_binary_index_html_end - _binary_index_html_start`
  should equal the byte size of `main/index.html`.
- **Credentials are never committed.** Not in `sdkconfig.defaults`, not in C.
  They live only in the git-ignored `sdkconfig`, set via `idf.py menuconfig`.
  `sdkconfig.defaults` must never gain a real SSID or password.
- **Boot order in `app_main` is load-bearing.** `wifi_sta_init()`
  (`esp_netif_init()`) must come before `http_server_start()`, because
  `httpd_start()` creates a socket and lwIP's socket layer reaches its TCP/IP
  thread through a mailbox that only `esp_netif_init()` creates. Getting it
  wrong aborts at *runtime* with `assert failed: tcpip_send_msg_wait_sem ...
  (Invalid mbox)` while compiling and linking perfectly happily.
- **Everything that moves furniture is a `POST`.** `GET /api/status` is the only
  read. A browser will issue a `GET` on its own — prefetch, link preview, reload
  — and this is a sofa. Do not add a `GET /api/move`.
- **The hold watchdog is armed by the caller, not by the movement code.** In jog
  mode the firmware keeps re-sending the frame, so anything that goes away
  mid-press would leave the seat running; the HTTP layer arms it on every move
  and refreshes it from `/api/hold`. The console deliberately does not, because
  a person at a terminal is their own watchdog. If you add another control
  surface (a physical button, MQTT), arm it there too — `sofa_control_move()`
  alone does not protect you.
- **The dashboard lights a cushion from what it commanded, and clears it from
  the `f7` notification**, because the sofa never announces a motor *starting*.
  `/api/status` exposes `motor_stop_count` (a counter, so the client can react
  once per event) and `last_motor` for exactly this. `5` means all seats.
- **The dashboard is one self-contained file.** No CDN, no web fonts, no
  external requests: the board may be on a LAN with no working internet path,
  and anything remote would block the page. It is linked in via `EMBED_FILES`,
  so it lives in rodata — past ~100 KB, revisit.
- **`main/index.html` is the only testable-without-hardware surface.** It was
  exercised against a mock of its own API in a real browser before shipping (see
  README's verification section): render, jog hold/release, one-shot no-stop,
  stop notification clearing a cushion, offline disabling, and error display.
  Worth repeating after any change to it — the mock is ~80 lines of Python and
  the failure modes (a stop on release in one-shot mode especially) are silent.
- Nothing in this project has been run against a real sofa. The README's
  *Verification status* lists exactly what is unproven, and what can be checked
  with a board but no sofa. Keep it honest when you change that.
