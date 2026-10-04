# Cat Feeder

Automatic cat feeder, built with PlatformIO. Early stage: the hardware
bring-up is done, the feeder itself is not written yet.

This repository currently holds **one** firmware project — the load cell scale
that will eventually detect how much food is left in the bowl.

## Current state

| Component | Status |
|-----------|--------|
| Load cell scale (HX711 + NodeMCU) | **Working** — calibrated, calibration stored in EEPROM |
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

The firmware prints a weight line roughly every 100 ms:

```
weight_g=248.31	raw=105234
```

`weight_g` is the calibrated mass, `raw` is the offset-corrected HX711 count.
Watch `raw` when diagnosing — it moves even when `weight_g` looks stuck.

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
| `x` | Erase the stored calibration. Next boot tares from scratch. |
| `h` | Print the command list. |

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
the SCK line, and add a capacitor across the HX711 supply.

## Files

```
platformio.ini   board, framework and library dependencies
src/main.cpp     scale firmware: sampling, serial commands, EEPROM calibration
README.md        this file
```

## Roadmap

- [ ] Move the SSD1306 driver onto the NodeMCU, share one I2C bus with the HX711
- [ ] Build a non-blocking display layer with real feeder screens
- [ ] Pick and drive the dispensing mechanism
- [ ] Portion control — dispense a repeatable amount, verified by the load cell
- [ ] Bowl-empty detection via a sustained drop in bowl weight
- [ ] Feed scheduling with RTC timekeeping over NTP
- [ ] Restructure into `firmware/<board>/` once a second project is added

## Notes

- `lib_deps` pins `bogde/HX711@^0.7.5` so the build is reproducible. EEPROM is
  bundled with the ESP8266 core and needs no declaration.
- Calibration lives in a 16-byte EEPROM record with a magic number, version,
  gain and checksum. Changing the gain invalidates the stored calibration
  rather than silently producing wrong readings.
- Memory usage: 26.0% flash (271 KB of 1.01 MB), 34.4% RAM (27.6 KB of 80 KB).
  Plenty of headroom for WiFi and the display.
- Two earlier prototypes were discarded rather than merged: a UNO HX711 sketch
  with a hardcoded calibration factor, and an ESP-01 blink test. The NodeMCU
  project supersedes both.
