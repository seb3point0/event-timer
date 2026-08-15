# Presentation timer firmware — ZECTRIX NOTE4

A standalone presentation timer. The three buttons configure and drive a
session, the 4.2" e-paper panel is the display, and the onboard speaker plays
the cues. No network, no phone, no server — the device is the whole product.

The web app in the repo root (`server.js`, `control.html`, `index.html`) is the
earlier phone-controlled version. It is untouched and still works; this
firmware does not talk to it.

## Controls

Three buttons: **OK** on the front, **UP** and **DOWN** on the side. DOWN is
also the power button.

### Setup screen

| Input | Action |
| --- | --- |
| UP / DOWN | Move the highlight |
| OK | Start editing the highlighted row |
| UP / DOWN while editing | Change the value |
| OK while editing | Confirm and go back to moving the highlight |
| OK on START SESSION | Begin |
| Hold DOWN (3s) | Power off |

Values, and the step each press applies:

| Row | Range | Step |
| --- | --- | --- |
| TALKS | 1–20 | 1 |
| DURATION | 1–180 min | 1 min |
| WARNING | 0–600 s (0 = off) | 15 s |
| BREAK | 0–1800 s (0 = none) | 15 s |

Warning and break move in 15-second steps rather than one second because every
press costs a panel refresh — see the timings below. Settings persist in NVS,
so the box comes back with the last event's configuration.

### Timer screen

| Input | Action |
| --- | --- |
| Tap OK | Pause / resume |
| Hold OK (2s) | End the session, back to setup |
| Tap UP | Skip forward to the next segment, past the buzzer |
| Tap DOWN | Restart the current segment |
| Double-tap DOWN | Jump to the start of the previous segment |
| Hold DOWN (3s) | Power off |

The back button follows CD-track semantics: one press restarts what you are in,
two presses go back one. A segment is one talk (its end buzzer included) or one
break.

## Why the display behaves the way it does

Measured on the fitted SSD2683 panel with `CONFIG_COUNTER_PANEL_BENCHMARK`:

| Operation | Time |
| --- | --- |
| Full refresh | 1219 ms |
| Partial, 20 rows | 776 ms |
| Partial, 131 rows (the clock band) | 831 ms |
| Partial, whole screen | 917 ms |

Cost barely varies with area, because it is dominated by the waveform
sequence. Breaking a 772 ms partial down further:

| Stage | Time |
| --- | --- |
| Hardware reset + OTP waveform reload | 40 ms |
| RAM write | 16 ms |
| Internal power on | 136 ms |
| Waveform drive | 445 ms |
| Internal power off | 134 ms |

Design consequences:

- **A single partial always beats a full refresh**, whatever it covers. So a
  whole-screen inversion is a partial, and `Display::Show` only falls back to a
  full frame when a frame needs two or more separate rectangles.
- **Everything the timer needs fits inside one second**, including the
  black/white flash during wrap-up. Only the full refresh does not, which is
  why one never happens mid-phase.
- **The progress bar sits directly under the digits.** Far apart, they are two
  dirty regions and cost two refreshes. Adjacent, they merge into one.
- **Full refreshes happen only on screen and phase changes.** Those are already
  visual breaks, and they double as the ghosting reset. A periodic
  ghost-clearing flash during a talk was tried and is horrible to watch.

Two things that were tried and rejected:

- **Holding the panel powered between frames.** 310 ms of every partial is a
  reset and a power-on/power-off bracketing 445 ms of actual drive. Skipping
  all three drops a refresh to 519 ms — and then the controller stops
  releasing BUSY after a frame or two, which on a live timer means a frozen
  display. Not worth it at any speed.
- **Refreshing a smaller rectangle.** The waveform drive is a fixed cost: 20
  rows and 159 rows both take 714 ms of drive time. Shrinking what you redraw
  buys almost nothing.

Grey is a dither, not true greyscale. The panel only does 16 levels in a 4bpp
full refresh, which then blocks partial refresh until a 1bpp frame
re-establishes the baseline — and a countdown needs partial refreshes.

## Layout

```
firmware/
  components/zectrix_epd/     vendored panel driver (MIT, Zectrix Lab)
  components/zectrix_board/   vendored board adapter (MIT, Zectrix Lab)
  main/
    app_main.cc               boot: NVS, board, panel, sound, UI
    session.cc                phase state machine, ported from server.js
    ui.cc                     screens, button handling, cue triggers
    display.cc                partial vs full refresh policy
    canvas.cc                 1bpp drawing primitives
    sound.cc                  ES8311 playback
    fonts/                    generated bitmap fonts
    assets/                   generated PCM cues
  tools/
    gen_fonts.py              TrueType -> 1bpp C tables
    prep_audio.sh             mp3 -> 16 kHz mono PCM
```

One local change to each vendored component, marked in the source: the OK
long-press threshold is 2s rather than 1.5s, and BUSY is polled at 1ms rather
than 10ms — there are half a dozen BUSY waits in a refresh, and each rounding
up to the next 10ms tick is time the countdown spends waiting.

## Building

Needs ESP-IDF 5.4+ (developed against 5.5.2), plus `cmake` and `ninja`, which
IDF does not bundle on macOS:

```sh
brew install cmake ninja
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 -b 460800 flash
```

The port number changes when the device re-enumerates; `ls /dev/cu.usbmodem*`
to find it.

### Regenerating assets

Both regenerate into the tree and are checked in, so a normal build does not
need Python or ffmpeg:

```sh
python3 -m venv /tmp/fontenv && /tmp/fontenv/bin/pip install pillow
/tmp/fontenv/bin/python tools/gen_fonts.py main/fonts
./tools/prep_audio.sh
```

`gen_fonts.py` renders Arial Narrow Bold at 180px for the clock — the narrow
cut buys ~20% more digit height than Arial Bold at the width "88:88" has to fit
into — and Arial Bold for the smaller text.

## Restoring the stock firmware

The vendor publishes no restore image. A full dump of the flash as it shipped
is at `~/zectrix-note4-backup/stock-note4-16mb.bin`, with its SHA-256 alongside:

```sh
esptool --port /dev/cu.usbmodem1101 write_flash 0x0 stock-note4-16mb.bin
```
