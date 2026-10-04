# Cat Feeder

Automatic cat feeder, built with PlatformIO. Early stage: the hardware
bring-up is done, the feeder itself is not written yet.

This repository currently holds **one** firmware project — the load cell scale
that will eventually detect how much food is left in the bowl.

## Current state

| Component | Status |
|-----------|--------|
| Load cell scale (HX711 + NodeMCU) | **Working** — calibrated, calibration stored in EEPROM |
| Sustained weight change detection | **Working** — verified against simulated noise and vibration |
| ThingSpeak upload | **Working** — uploads confirmed weight changes |
| OLED display driver (SSD1306) | Hardware verified on a UNO, not yet merged here |
| Feeder mechanism (motor/servo) | **Not started** — no code exists |
| Scheduling / portioning logic | **Not started** |

The OLED work lives in a separate UNO project. It was verified on UNO hardware
but the SSD1306 is an I2C device, so the plan is to move it onto the NodeMCU
alongside the load cell and drive both from one board. See *Roadmap*.

## Hardware

### Load cell scale — in this repo

- NodeMCU 1.0 (ESP-12E)
- HX711 24-bit load cell amplifier
- Any 1–5 kg load cell

| HX711 | NodeMCU | GPIO |
|-------|---------|------|
| VCC   | 3V3     | —    |
| GND   | GND     | —    |
| DOUT  | D7      | 13   |
| SCK   | D6      | 12   |

A few notes on the wiring:

- **Power the HX711 from 3V3, not 5V.** The module's pins are 3.3V-tolerant but
  the board is designed around 3.3V. Feeding it 5V from the VIN pin works, but
  then the DOUT line idles high, which the ESP8266 does not enjoy.
- **SCK on GPIO12 is a boot-strapping pin.** It must be low or floating at
  power-on, or the chip boots in the wrong flash-voltage mode. The HX711 clock
  idles low, so this is safe in practice and the firmware does not touch it
  before `setup()` runs. Avoid GPIO0, GPIO2 and GPIO15 for anything the firmware
  has to hold high at boot.
- The load cell itself has four wires: `E+`, `E-`, `S+` and `S-`. `S-` must be
  tied to `E-` on the module, which most breakouts already do.

### OLED display — verified, not yet merged

- 128x64 SSD1306 over I2C at `0x3C`
- Was run from a UNO: SDA → A4, SCL → A5, VCC → 5V

The UNO sketch is a hardware smoke test rather than reusable driver code: it
cycles through fills, shapes and font sizes to prove every pixel is wired. That
has served its purpose. The next step is a driver plus real feeder screens
(last fed, next feed, portion size, bowl weight).

## Build, upload, monitor

```bash
pio run                 # compile
pio run -t upload       # flash to the NodeMCU
pio device monitor      # open the serial monitor at 115200
```

In VS Code with the PlatformIO IDE extension, use the buttons in the status bar
instead.

The firmware prints a reading a couple of times a second:

```
weight_g=248.31	raw=105234	filtered=248.29	baseline=0.00	dev=248.31	quiet=0
```

| Field | Meaning |
|-------|---------|
| `weight_g` | Calibrated mass from the scale |
| `raw` | Offset-corrected HX711 count |
| `filtered` | Median-filtered mass, what the detector actually uses |
| `baseline` | Last confirmed weight — the reference for change detection |
| `dev` | `filtered` minus `baseline` |
| `quiet` | 1 when the reading has held steady for the whole confirm window |
| `confirm` | Only shown while a change is being debounced, e.g. `2/3` |

Watch `quiet=1` before trusting a detection. If it never reaches 1, the sensor
is too noisy for the current settings — see *Detecting weight changes*.

## Calibrating the scale

Calibration is a two-step job, done once per load cell and mounting.

1. **Tare with nothing on the platform.** Leave the cell empty and do not touch
   it. The firmware tares automatically on first boot.
2. **Calibrate against a known weight.** Place your weight on the platform, then
   over the serial monitor type `c`, then the mass in grams, then Enter:

   ```
   c250
   ```

   Pressing Enter straight after `c` also works. The firmware prints the scale
   factor and saves it to EEPROM.

The stored calibration survives power cycles and reflashing, so this is a
one-time job per cell. If you move the cell to a new mount, recalibrate — the
scale factor is only valid for the geometry it was measured in.

Serial commands:

| Command | Action |
|---------|--------|
| `t` | Tare. Keeps the existing scale factor. |
| `c` | Calibrate: send `c`, then the known mass in grams, then Enter. |
| `g` | Set the change threshold: send `g`, then the grams, then Enter. |
| `x` | Erase the stored calibration. Next boot tares from scratch. |
| `h` | Print the command list. |

The threshold set with `g` is saved to EEPROM, so it survives power cycles and
reflashing. It is clamped to a floor of 0.8 g — see below for why.

## Detecting weight changes

A naive `if (weight - lastWeight > 1) upload()` is useless on a real load cell.
The reading jitters continuously, drifts with temperature, and swings wildly
while an object is being placed. So a change is confirmed only when **all** of
the following hold:

1. **The filtered reading has not moved more than 0.4 g across the last 10
   seconds.** This is the key test. It means the weight is *at rest*, not merely
   passing through some value on its way elsewhere.
2. **The filtered reading differs from the last confirmed weight by more than
   the threshold** (1 g by default).
3. **Both of the above have held for 3 consecutive samples.** This debounce
   stops a single lucky window from confirming a change by chance.

Only then is the weight uploaded and the baseline moved on.

### Why those numbers

The readings are smoothed with a **median over 13 samples**, not a mean. A
median throws away outliers completely, so a spike — a cat walking past, a
door slamming — cannot drag the filtered value at all, whereas a mean would
spread that spike across the next several readings.

The 0.4 g band and the 10 s window were chosen by measuring the filter's output
rather than guessing:

| Median window | Spread over 10 s, ±0.2 g noise | Settles after a step |
|---------------|-------------------------------|----------------------|
| 5  | 0.289 g | 1.0 s |
| 9  | 0.231 g | 2.0 s |
| 13 | 0.181 g | 3.0 s |

A window of 13 keeps the spread comfortably under the 0.4 g band while only
costing 3 s to settle, so a genuine change is confirmed about **13–14 s** after
the weight actually moves. Shorter windows settle faster but let ordinary noise
break the band, which means `quiet` never reaches 1 and nothing is ever
detected.

This also explains the threshold floor. The band has to stay well below the
threshold, otherwise sensor noise alone can exceed the threshold while the
reading looks settled, and every reading would trigger an upload.

### Tuning it

The threshold is the one setting you are likely to change — set it above the
portion size you care about. The other two are constants near the top of
`src/main.cpp`:

```cpp
constexpr float STABILITY_BAND_GRAMS = 0.4f;
constexpr uint32_t CONFIRM_WINDOW_MS = 10000;
constexpr uint8_t DEBOUNCE_TICKS = 3;
```

**If `quiet` never reaches 1**, the cell is noisier than the band allows. Raise
`STABILITY_BAND_GRAMS` first (0.6, then 0.8), and if that is not enough, raise
the threshold to match — otherwise the detector will confirm noise. On a cell
with noticeably worse noise, ±0.5 g, expect `quiet` to flicker rather than sit
at 1; that is still workable but detection becomes less reliable.

**If real changes are missed**, the weight probably is not settling within 10 s.
Check for a loose mount or something touching the platform, then consider
raising `CONFIRM_WINDOW_MS`.

## ThingSpeak setup

1. Create a channel at thingspeak.com and copy its **Write API Key**.
2. Copy `include/secrets.example.h` to `include/secrets.h` and fill in the WiFi
   credentials and the API key. `secrets.h` is gitignored, so your key stays out
   of the repository. `secrets.example.h` is committed and holds placeholders.
3. `pio run -t upload`.

The first settled reading after boot is uploaded, so the channel is never left
empty. After that only confirmed changes are sent.

Field 1 carries the weight in grams. Each upload also prints locally:

```
Confirmed sustained change to 500.06 g.
Uploaded 500.06 g to ThingSpeak (1)
```

### Things to know about the free tier

- **One update every 15 seconds.** The firmware enforces this minimum between
  attempts. Detected changes are queued rather than dropped, so if two changes
  land close together the second is sent as soon as the window allows. An HTTP
  429 is reported and retried.
- **HTTP 4xx means the key or channel is wrong.** The firmware says so, then
  discards the queued reading and carries on detecting — a misconfigured upload
  must not silently stop change detection.
- **TLS certificate validation is disabled** (`setInsecure()`), so the
  connection is encrypted but not authenticated. Pinning ThingSpeak's
  certificate on an ESP8266 is brittle because it rotates. For a hobby project
  on a trusted network this is an acceptable trade; do not reuse this code for
  anything that matters.
- A blocked or failed upload is retried every 15 s. Intermediate changes that
  occur during an outage are not recovered — on reconnect you get the current
  weight, which for a feeder is the value that actually matters.
- The upload blocks for up to 6 s, so serial output pauses during it. This does
  not affect detection: the confirm window is time-based, not tick-based.

### Calibration accuracy

The scale factor is derived from a single point, so accuracy holds best near the
calibration weight and falls off toward zero and toward full scale. Calibrate
with a weight close to a typical portion — a few hundred grams rather than the
cell's full 5 kg capacity. For tighter accuracy across the range, extend this to
a two- or three-point fit (empty, mid, span) and solve for offset *and* slope.

## Troubleshooting

**`ERROR: HX711 never signalled data ready.`** The amplifier is not answering.
Check GND first, then VCC, then that DOUT and SCK are not swapped. Confirm the
module has power by measuring VCC against GND.

**Readings are roughly 2x or 4x too high, or too low.** The PGA gain does not
match the module. `HX711_CHANNEL_GAIN` at the top of `src/main.cpp` is set to
128, which suits most breakouts; some are wired for 64 or 32. If the error is a
clean factor of two or four, that is the cause.

**`No stored calibration; taring an empty platform.`** Normal on first boot, and
after a chip erase or a failed checksum. Put the platform in its final mounted
position first, since the tare depends on how the cell is loaded.

**Weight drifts over minutes or with a cold cell.** Load cells are
temperature-sensitive and creep under constant load. Mount the cell rigidly,
keep the amplifier away from the motor's heat and supply noise, and take an
average over several readings rather than trusting a single sample.

**Weight jumps when the feeder motor runs.** Motor current is coupling into the
amplifier. Starve the motor from a separate supply, keep its wiring away from
the SCK line, and add a capacitor across the HX711 supply. Persistent vibration
also stops `quiet` reaching 1, which is the signal that detection is being
suppressed rather than merely slow.

**`Upload failed: HTTP 404`.** The write API key or the channel is wrong. Check
`include/secrets.h` against the channel's API Keys page. Detection carries on
regardless; only the upload is abandoned.

**`Upload skipped: WiFi is down.`** The NodeMCU has no route to the network.
Readings are still detected and queued, and the upload is retried every 15 s
until the connection returns.

**ThingSpeak rejects updates with HTTP 429.** Free channels accept one update
every 15 seconds. The firmware already rate-limits itself; a 429 here usually
means something else is writing to the same channel.

## Files

```
platformio.ini               board, framework and library dependencies
src/main.cpp                 setup, sampling, serial commands, EEPROM records
src/change_detector.h/.cpp   sustained weight change detection
src/thingspeak.h/.cpp        WiFi and ThingSpeak upload
include/secrets.example.h    template for credentials, committed
include/secrets.h            your credentials, gitignored
README.md                    this file
```

## Roadmap

- [x] Load cell scale with EEPROM calibration
- [x] Sustained weight change detection
- [x] ThingSpeak upload of confirmed changes
- [ ] Move the SSD1306 driver onto the NodeMCU, share one I2C bus with the HX711
- [ ] Build a non-blocking display layer with real feeder screens
- [ ] Pick and drive the dispensing mechanism
- [ ] Portion control — dispense a repeatable amount, verified by the load cell
- [ ] Bowl-empty detection via a sustained drop in bowl weight
- [ ] Feed scheduling with RTC timekeeping (NTP is already connected)
- [ ] Restructure into `firmware/<board>/` once a second project is added

## Notes

- `lib_deps` pins `bogde/HX711@^0.7.5` so the build is reproducible. EEPROM,
  ESP8266WiFi and ESP8266HTTPClient are bundled with the ESP8266 core and need no
  declaration. Note the bundled header is `ESP8266HTTPClient.h`, not the older
  `HTTPClient.h`.
- Calibration and the change threshold live in separate 16- and 12-byte EEPROM
  records, each with a magic number, version and checksum. Adding settings will
  not disturb a stored calibration, and changing the gain invalidates the
  calibration rather than silently producing wrong readings.
- At PGA 128 the HX711 only produces about 10 readings per second, so each
  sample blocks for roughly half a second and the real sample rate is nearer
  1–2 Hz than the configured interval suggests. This is the main reason the
  confirm window is measured in seconds rather than samples.
- Memory usage: 37.9% flash (396 KB of 1.01 MB), 36.2% RAM (29.6 KB of 80 KB).
  BearSSL and the HTTP client account for most of the increase over the
  scale-only build.
- Two earlier prototypes were discarded rather than merged: a UNO HX711 sketch
  with a hardcoded calibration factor, and an ESP-01 blink test. The NodeMCU
  project supersedes both.
