# wled_pico

A WLED-*inspired* firmware for the Pimoroni Cosmic Unicorn (Pico W). Not a
literal port of [WLED](https://github.com/wled/WLED) — WLED's codebase
assumes an ESP8266/ESP32 + NeoPixelBus/FastLED stack that doesn't apply to
the Cosmic Unicorn's PIO-driven, row-multiplexed matrix. Instead this
reimplements WLED's *idea* (web UI, JSON API, effects engine) on top of a
purpose-built RP2040 display driver, reusing WLED's effect math and API
shape where that's architecture-agnostic.

**Note on hardware:** this project initially targeted the wrong Pimoroni
board (Galactic Unicorn, 53x11, one row-select address per row) when the
actual hardware is a Cosmic Unicorn (32x32, 1-in-2 row-multiplexed - 16
row-select addresses, each driving two physical rows at once). That
produced a real, visible bug: solid-color fills showed a localized block of
wrong colors on real hardware, traced to writing a 53x11-shaped bitstream
onto 32x32-shaped, differently-multiplexed silicon. The driver has since
been rebuilt against Cosmic Unicorn's actual geometry, pin/bitstream layout,
and PIO program - see Credits. Re-verified clean on hardware afterward.

## Status

**All 5 milestones done and verified on hardware**, including a live,
network-driven OTA update and automatic WiFi reconnection on reboot.

| # | Milestone | Status |
|---|-----------|--------|
| 1 | Display driver bring-up | done — verified on (correct) hardware |
| 2 | WiFi AP + captive portal + static page | done — verified on (correct) hardware |
| 3 | Port 3-4 WLED effects onto the pixel buffer | done — verified on hardware |
| 4 | Real WLED `/json/state` API + minimal custom UI | done — verified on hardware |
| 5 | Presets, WebSocket live preview, OTA | done — verified on hardware |

Milestone 5 notes:

- **Presets** (`api/preset_store.cpp`, LittleFS-backed, `psave`/`ps`/`pdel`
  fields on `POST /json/state`) and **live WebSocket sync** (`/ws`, pushes
  state to every connected client on any change) are implemented.
- **WiFi station join** (`net/captive_portal.cpp`'s "Join WiFi" form,
  `POST /wifi`) - drops the device's own AP and joins an existing network,
  real WLED-style onboarding, then **persists the credentials to LittleFS
  and auto-rejoins on every future boot** (confirmed: triggered a reboot
  via OTA and it rejoined with zero manual intervention). A "Forget saved
  WiFi" button (`POST /wifi/forget`) reverts to AP-only permanently. The
  Pico W's CYW43 WiFi chip doesn't always complete a join promptly, and
  rarely just hangs outright - a hardware watchdog
  (`rp2040.wdt_begin(8000)`/`wdt_reset()` in `main.cpp`) recovers a hung
  join (silent reset back to AP-only) within ~8s instead of needing a
  physical BOOTSEL recovery; confirmed firing correctly on a real hang.
- **OTA** (`api/ota.cpp`, `POST /update`, multipart firmware.bin upload via
  arduino-pico's `Update` class) is confirmed working end to end -
  including catching a real bug: arduino-pico's OTA writes the incoming
  firmware into the **LittleFS partition itself** (not a separate flash
  slot), so `board_build.filesystem_size` has to be bigger than the
  firmware image, not just big enough for presets - it's 1MB now (see
  `platformio.ini`), not the 256KB it started at.

## WLED tool compatibility

Beyond the DIY control page, this firmware is discoverable and controllable
by real WLED tooling — the WLED mobile app, Home Assistant's WLED
integration, and WLED-protocol pixel-pusher tools — without needing to run
actual WLED firmware. Three pieces make that work, all added after
milestone 5 and all verified against wled/WLED's own source
(`wled00/json.cpp`, `wled00/wled.cpp`, `wled00/udp.cpp`) and against Home
Assistant's client library (`frenck/python-wled`), not guessed at:

- **mDNS discovery** (`main.cpp`'s `begin_mdns()`) — advertises `_wled._tcp`
  and `_http._tcp` with a `mac` TXT record, matching real WLED's own
  `wled.cpp` mDNS setup exactly. Re-armed (`MDNS.end()` + re-`begin()`) on
  every AP↔STA transition, since it's bound to whichever interface was up
  when it started and this device's AP can drop out from under it (see
  `net/captive_portal.h`'s join behavior).
- **JSON API correctness fixes** (`api/json_api.cpp`) — real WLED clients
  hit `/json/si`, `/json/eff`, `/json/pal` specifically (now registered
  alongside the more readable `/json/effects`/`/json/palettes` this
  firmware already had). More importantly, `/json/info`'s `ver` field used
  to be `"wled_pico-0.5"`, an unparseable non-semver string — Home
  Assistant's WLED integration parses `ver` and outright refuses to set up
  any device below a minimum supported version, so a firmware with a
  version string that fails to parse never got that far at all. It's now a
  real WLED release string (`"0.15.0"`) this firmware's JSON schema is
  actually compatible with. `/json/state` and `/json/info` also gained
  several fields (`nl`, `udpn`, `lor`, `mac`, `fs`, …) that aren't
  implemented behaviorally but are required *keys* client libraries expect
  to at least find present — their absence was a hard parse failure, not a
  missing feature.
- **UDP realtime pixel protocol** (`net/realtime_udp.h/.cpp`, port 21324) —
  WARLS/DRGB/DRGBW/DNRGB/DNRGBW, the wire format real WLED's own "send
  realtime data" feature and third-party pixel-pusher tools (screen-capture
  sync utilities, Hyperion-style ambilight setups) use to push live frames
  to a WLED device. Ported line-for-line from `wled00/udp.cpp`'s
  `handleNotifications()`. While active it bypasses `effects::render()`
  entirely and pushes the received frame straight to the display, same as
  real WLED's `realtimeMode` override — pixel id is row-major
  (`id = y*32+x`), matching WLED's own default 2D mapping. Out of scope,
  deliberately: the separate "wled notifier" protocol for syncing multiple
  WLED *controllers'* state together (this is always the only unit),
  TPM2.NET, and Art-Net/E1.31/DDP (different, heavier protocols).

## Effects library

156 of real WLED's 219 numbered effects are ported (`src/effects/effects.cpp`
plus `src/effects/gen_batch0.cpp`-`gen_batch8.cpp`), each citing the exact
WLED source function/line it came from — every hardware-feasible effect
except one (2D scrolling text, below). Effect IDs match real WLED's
`FX_MODE_*` numbering exactly, not porting order — see `effects.h`'s top
comment. Not ported, by design:

- **Audio-reactive effects** (~40) — this board has no microphone.
- **Particle System effects** (~35) — real WLED's own particle-physics
  engine (gravity, collisions, per-particle state) is a substantial
  subsystem in its own right, out of scope here.
- **2D scrolling text** (id 122) — needs WLED's font/glyph-rendering
  subsystem, which nothing in this firmware provides.

A full 72-palette engine (`src/effects/palettes.h`/`.cpp`) backs every
effect that keys off `SEGMENT.palette` in the original — all 59 built-in
WLED gradient palettes plus the 7 FastLED and 6 segment-color-derived
ones, ported from WLED's actual palette data (not approximated).

All 219 effect IDs (0-219, the 4 genuinely-retired "RSVD" ones included)
have been cycled live against real hardware over the JSON API with no
crashes or watchdog resets. That confirms memory/bounds safety across the
whole set, not that each one's visual output has been eyeballed
individually — a handful of upstream WLED quirks were found and
deliberately reproduced rather than "fixed" (see the per-effect comments
for specifics), so if one looks off compared to real WLED, check there
first before assuming a porting bug.

## Hardware

[Cosmic Unicorn](https://shop.pimoroni.com/products/cosmic-unicorn) — Pico W
aboard, 32x32 RGB matrix, 1-in-2 row-multiplexed (16 physical row-select
addresses), driven through RP2040 PIO at ~300fps/14-bit.

## Building

Requires [PlatformIO](https://platformio.org/).

```
pio run                 # build
pio run -t upload       # flash (hold BOOTSEL, plug in, then run this)
pio device monitor       # serial console
```

The project uses [maxgerhardt's `platform-raspberrypi`
fork](https://github.com/maxgerhardt/platform-raspberrypi), which wraps
[earlephilhower/arduino-pico](https://github.com/earlephilhower/arduino-pico)
and adds Pico W support plus automatic `.pio` -> `.pio.h` compilation (see
`src/display/cosmic_unicorn.pio` — PlatformIO compiles it via a bundled
`pioasm` on every build, no manual step needed). Networking uses
`RPAsyncTCP`/`ESP32Async/ESPAsyncWebServer` as `lib_deps` — the RP2040 ports
of the same ESPAsyncTCP/ESPAsyncWebServer libraries WLED itself uses.

## Layout

```
platformio.ini
src/
  main.cpp                    wires display + effects + JsonApi + CaptivePortal together
  display/
    gu_pins.h                 board GPIO map
    cosmic_unicorn.pio        PIO program (bit-angle/BCD matrix scan-out)
    gu_display.h / .cpp       driver: begin() / set_pixel() / set_brightness()
  effects/
    effects.h / .cpp          effect engine core: Id enum, Params/State, Frame, self-registering dispatch
    wled_compat.h / .cpp      shared FastLED/WLED 8/16-bit math + PRNG helpers (random8, sin8, beatsin8, ...)
    palettes.h / .cpp         the 72-palette engine (color_from_palette)
    gen_batch0.cpp .. gen_batch8.cpp
                              152 more ported effects, one self-contained file per batch
  net/
    captive_portal.h / .cpp   WiFi AP + wildcard DNS + the served control page
    device_id.h / .cpp        shared colon-free/lowercase MAC helper (mDNS TXT, /json/info)
    realtime_udp.h / .cpp     WLED-protocol UDP realtime pixel receiver (port 21324)
  api/
    json_api.h / .cpp         WLED-shaped /json/state, /json/info, /json/effects, /json/palettes
    preset_store.h / .cpp     LittleFS-backed presets.json (WLED preset shape, trimmed)
    ota.h / .cpp              POST /update - web-based firmware upload via arduino-pico's Update class
```

## Credits

`gu_pins.h` and `cosmic_unicorn.pio` are vendored verbatim from
[pimoroni/pimoroni-pico](https://github.com/pimoroni/pimoroni-pico)
(MIT licensed) — the pin map and PIO program are board-specific and
hand-tuned; there was no reason to rederive them. `gu_display.cpp` is a
trimmed reimplementation of that repo's `cosmic_unicorn.cpp` (same
bitstream layout, row-doubling remap, and PIO/DMA setup, audio/synth
support removed, gamma table approximated rather than copied since we
didn't have Pimoroni's exact values — see the comment in `gu_display.cpp`
if displayed colors look off).

`effects/effects.cpp`'s original four effects are ported from specific
functions in [wled/WLED](https://github.com/wled/WLED)'s `wled00/FX.cpp`
(`mode_static`, `color_wipe`, `mode_rainbow_cycle`, `mode_breath`) and
`wled00/FX_fcn.cpp` (`Segment::color_wheel`, `color_blend`) — see the
per-function comments in that file for exact line references. The other
152 effects (`gen_batch0.cpp`-`gen_batch8.cpp`) and the full palette engine
(`palettes.cpp`) are ported the same way - each function/table cites its
exact WLED source location - see "Effects library" above for what's
deliberately not included and why.

`api/json_api.cpp`'s `/json/state` and `/json/info` field names and shapes
are matched against `wled00/json.cpp`'s `serializeState()`/
`deserializeSegment()`/`serializeInfo()`, trimmed to the single-segment
subset this firmware has state for (no presets, playlists, nightlight,
multi-segment, or palettes beyond a fixed "Default").

`net/realtime_udp.cpp`'s WARLS/DRGB/DRGBW/DNRGB/DNRGBW parsing is ported
directly from `wled00/udp.cpp`'s `handleNotifications()` (same header
bytes, same per-mode payload layout and loop bounds). The mDNS service/TXT
records `main.cpp`'s `begin_mdns()` registers match `wled00/wled.cpp`'s own
mDNS setup. See "WLED tool compatibility" above for what motivated both.
