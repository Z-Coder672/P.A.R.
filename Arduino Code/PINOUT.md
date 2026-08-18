# P.A.R. — Pin & Wiring Reference

Every electrical connection on the rig, in one place. **Derived from source**, not from
memory — each row cites the file that defines it. If you change a pin, change it here in
the same edit (same rule as `PARMain/FLOW.md`).

Trust the `.ino` / `cpu_map.h` when they disagree with this file.

---

## 1. Boards on the rig

| Role | Board | Enumerates as | Notes |
|---|---|---|---|
| Main MCU | **Arduino Nano ESP32** (ESP32-S3 in a u-blox NORA-W106) | `usbmodem*` | FQBN `arduino:esp32:nano_nora`. Runs `PARMain.ino`. Native WiFi (no NINA co-processor). |
| Motion controller | **Arduino Mega 2560** + Protoneer **CNC Shield V3** | `usbmodem*` | Runs patched grbl-Mega (`~/Documents/Arduino/libraries/grbl-Mega`). |
| Servo driver | **Arduino Nano (AVR, 5 V)** | `usbserial-*` (CH340) | Runs `ServoNano/ServoNano.ino`. Drives the flip servo + witness servo. |

Port names **change on replug** — identify with `arduino-cli board list`, never hardcode.

---

## 2. Main MCU — Arduino Nano ESP32

The Arduino core defaults to **pin remapping** (`BOARD_HAS_PIN_REMAP`), so in sketch code
`D4` and the bare literal `4` are the *same* pin. The "GPIO" column is the underlying
ESP32-S3 hardware GPIO — you need it only for peripheral/matrix questions, never for
`pinMode`. (Source: `~/Library/Arduino15/packages/arduino/hardware/esp32/2.0.18-arduino.5/variants/arduino_nano_nora/pins_arduino.h`)

| Header | GPIO | Net | Direction | Defined in |
|---|---|---|---|---|
| **D0** | 44 | `Serial1` **RX** ← Mega **D18 / TX1** (GRBL) | in | `PARMain.ino:2113` |
| **D1** | 43 | `Serial1` **TX** → Mega **D19 / RX1** (GRBL) | out | `PARMain.ino:2113` |
| **D2** | 5 | **Servo ACK** ← ServoNano D3, via divider. **Active-HIGH** (idle LOW) | in (`INPUT_PULLDOWN`) | `PARMain.ino:500` |
| D3 | 6 | *unused* | — | — |
| **D4** | 7 | TCS3200 **S0** (frequency scaling, with S1) | out | `PARMain.ino:79` |
| **D5** | 8 | TCS3200 **S1** | out | `PARMain.ino:80` |
| **D6** | 9 | TCS3200 **S2** (filter select, see §4 caveat) | out | `PARMain.ino:81` |
| **D7** | 10 | TCS3200 **S3** (filter select, see §4 caveat) | out | `PARMain.ino:82` |
| **D8** | 17 | TCS3200 **OUT** (square wave, read by `pulseIn`) | in | `PARMain.ino:83` |
| **D9** | 18 | **Servo command TX** → ServoNano D2 (`Serial2`, 9600 8N1, TX-only). **Not the servo signal** — that is ServoNano D9, see §3 | out | `PARMain.ino:94,432` |
| **D10** | 21 | **LED illumination bank** via NPN base — HIGH = LEDs on | out | `PARMain.ino:84` |
| **D11** | 38 | **Load-switch enable** on the shield PCB (`5VSW` → TPS22919 `ON`). **No firmware drives it** — see §10 | out | shield only |
| D12–D13 | 47/48 | *unused* (SPI MISO/SCK; D13 = `LED_BUILTIN`) | — | — |
| A0–A3 | 1/2/3/4 | *unused* | — | — |
| **A4** | 11 | **I²C SDA** → VL53L4CD lidar | bidir | `Wire.begin()`, `PARMain.ino:1659` |
| **A5** | 12 | **I²C SCL** → VL53L4CD lidar | bidir | same |
| A6, A7 | 13/14 | *unused* | — | — |

### UART gotchas (do not re-litigate)
- **`Serial` is USB CDC.** `Serial0` is the D0/D1 UART. **`Serial1` has no default pins** —
  the GRBL link must be opened as
  `Serial0.end(); Serial1.begin(115200, SERIAL_8N1, D0, D1);`
  Every `Serial1.begin()`, *including recovery bounces*, must pass the pins.
- **D9 is a real hardware UART on the ESP32-S3** — `Serial2.begin(9600, SERIAL_8N1, -1, D9)`,
  TX-only (RX pin `-1`). The RP2040 predecessor had to bit-bang this with interrupts off;
  the bit-bang and all its timing calibration are **deleted**. Don't reintroduce them.
- MCU reset is `esp_restart()` (`<esp_system.h>`), not `NVIC_SystemReset()`.
  `esp_reset_reason()` names the cause (`BROWNOUT` / `POWERON` / `PANIC` / `SW` / `*_WDT`).

### Level shifting on the GRBL link — RESOLVED on the shield PCB
Mega **D18 (TX1)** is a **5 V** output and ESP32-S3 **D0** is **not 5 V tolerant** (abs max
VDD+0.3 = 3.6 V), with the Nano header exposing the GPIO directly. This was an open audit
item while the rig was hand-wired.

**The shield PCB fits a divider** (R2/R3, `ESP32RX` → D0), so the hazard is closed *on the
board*. Two caveats:
- **The as-drawn ratio is wrong.** 2 kΩ/2 kΩ gives 2.50 V against an ESP32-S3 V_IH of
  0.75 × VDD = **2.475 V** — 25 mV, and negative at any realistic driver V_OH. Use a
  **3.3 kΩ bottom leg** (2 k/3.3 k → 3.11 V, or 1.8 k/3.3 k → 3.24 V). See `PCBnSCHs/REVIEW.md`.
- **A hand-wired rig still has nothing.** If you are running without the shield, this link
  is still bare 5 V into D0 — inspect before trusting it.

The reverse direction (ESP32 3.3 V → Mega RX1) needs nothing: 3.3 V clears the AVR's
V_IH of 0.6 × Vcc = 3.0 V, a thin 0.3 V margin.

---

## 3. Servo — signal pin, and the link that feeds it

> ### ⚠ `D9` means two different things
> **The flip servo's signal wire is on ServoNano `D9`** — the 5 V AVR Nano, *not* the main
> MCU. Main-MCU `D9` is the **UART that talks to** ServoNano; nothing on the main board
> produces a servo pulse. The two are one hop apart and share a pin number, which is the
> single easiest thing to get wrong on this rig.
>
> `main D9 ──UART──▶ ServoNano D2 … ServoNano D9 ──PWM──▶ servo`

### 3a. The flip servo itself (MG90S)

| Servo wire | Goes to | Notes |
|---|---|---|
| **Signal** (orange/white) | **ServoNano `D9`** (`SERVO_PIN`) | Standard `Servo`-library PWM. `s.attach(SERVO_PIN, 544, 2400)` — 544–2400 µs is the library's 0–180° range, **not** the operating range. |
| **V+** (red) | 5 V servo feed | See §7 — **measure this under load before touching any constant**. MG90S stall ≈ 1.5 A, ~2× an SG90. |
| **GND** (brown/black) | Common ground | Must be shared with ServoNano. |

A **witness servo** is teed onto the same `D9` signal via a short fixed cable. If the main
servo misbehaves while the witness stays correct, the fault is on the main servo's own
branch — its long moving-carriage cable or connector — not the shared signal/5 V rail.
That is exactly how the intermittent open at full +X extension was localised.

**Commanded pulse widths** (`PARMain.ino`; these are only valid *as a set* with the lidar
compensation and reach trims — see CLAUDE.md before porting any of them):

| Constant | µs | Angle | Role |
|---|---|---|---|
| `SERVO_US_REST` | 565 | ≈ 2° | Parked. **Not 0°** — 544 µs is the library floor, not the rest angle. |
| `SERVO_US_SCAN` | = `SERVO_US_REST` (565) | ≈ 2° | Arm stays **up** for the whole scan (changed 2026-08-10; it used to drop to ~33.5°). |
| `SERVO_US_RELEASE` | 807 | ≈ 25.5° | Base angle for the catch stroke; lidar-compensated per cell, never commanded raw. |
| `SERVO_US_ENGAGE` | 1317 | 75° | Stage-1 90° squisk rotation. Deliberately *not* compensated. |

### 3b. The link — Nano ESP32 ↔ ServoNano

Two wires, opposite directions. Source: `ServoNano/ServoNano.ino`.

| From | To | Function |
|---|---|---|
| Main **D9** | ServoNano **D2** (`RX_PIN`) | Command line. 9600 8N1, **one-way, no UART ack.** |
| ServoNano **D3** (`ACK_PIN`) | Main **D2** | Ack. **A level, not a UART** — **idle LOW, driven HIGH** for `ACK_HOLD_MS = 40` per accepted command (active-HIGH since 2026-08-17). |
| — | — | Common ground required between the two boards. |

### The ack divider is mandatory
ServoNano is a **5 V** AVR; the ESP32-S3 is **not 5 V tolerant**. The ack line is divided:

```
ServoNano D3 ──[ R_top ]──┬── Main MCU D2  (read as an ANALOG level)
                          │
                       [ R_bot ]
                          │
                         GND
```

| Where | R_top / R_bot | Asserted level |
|---|---|---|
| **Hand-wired rig (physically confirmed 2026-08-17)** | **2 kΩ / 2 kΩ** | **2.45–2.50 V** |
| Shield PCB (netlist V3 / BOM V2) | 2 kΩ / 3.3 kΩ | 3.03–3.11 V |
| Historical / as this file used to claim | 1.8 kΩ / 3.3 kΩ | 3.24 V |

⚠ **This file previously stated the rig was 1.8 kΩ/3.3 kΩ. It is not — it is 2 kΩ/2 kΩ,**
physically confirmed 2026-08-17. That matters, because 2 k/2 k puts the asserted level at
2.45–2.50 V, straddling the ESP32-S3's V_IH of 0.75 × VDD = **2.475 V**. It happened to work
under the old active-LOW protocol because the *idle* level with `INPUT_PULLUP` was 2.52 V —
45 mV of margin, which is why the 2026-07-27 validation run passed.

**The main MCU therefore reads this pin as an ANALOG level, not a digital one** — see
`servoAckHigh()`. Threshold is **1.20 V**, which sits more than 1.2 V from either state
(idle 0 V, asserted ≥ 2.45 V) and is independent of V_IH and of the divider ratio. It works
unchanged on 2 k/2 k, 2 k/3.3 k and 1.8 k/3.3 k. Do **not** revert it to `digitalRead` while
the rig is 2 k/2 k, and do **not** fit `INPUT_PULLDOWN` — a 45 kΩ internal pulldown drags
2 k/2 k down to 2.446 V and would hang `SERVO_ACK_MODE 2` forever on the first command.

**The bottom-leg resistor is what parks the line LOW if ServoNano goes away**, which is the
fail-safe the active-HIGH polarity depends on. `SERVO_ACK_PIN` = D2 = GPIO5 = **ADC1**_CH4;
ADC1 is mandatory since ADC2 is unusable while WiFi is up.

`ACK_HOLD_MS = 40` must outlast a repeat burst but stay under the 100 ms minimum settle,
so one command's ack can't be misread as the next one's.

### ⚠ The ack is ACTIVE-HIGH, and the old "fails loud" claim was wrong
This file used to say: *"Main MCU uses `INPUT_PULLUP`, so an unfitted or broken wire reads
HIGH = 'no ack' = fails loud."* **That was only true for a break at the D2 pin itself.**

The divider's bottom-leg resistor sits between that node and GND, so a break **anywhere
upstream** — the ack wire, an unplugged ServoNano, or simply an unpowered one — pinned the
node LOW through it (3.3 V × 2 k/(2 k + 45 k) ≈ 0.14 V on the shield). Under the old
active-LOW protocol the main MCU read that as **"acked"**, so *every* command reported
success and `SERVO_ACK_MODE 2`'s retry-until-confirmed guarantee became vacuous. No divider
ratio fixes it — the polarity had to change.

**Since 2026-08-17 the line idles LOW and pulses HIGH.** The bottom-leg resistor is now the
fail-safe: any upstream break parks the line LOW = "no ack" = the enforced retry actually
fires. `INPUT_PULLDOWN` covers the remaining case where the divider itself is absent.

**Both sides must be flashed together.** A ServoNano on the old active-LOW build idles HIGH,
which the new main-MCU code would read as a permanent ack — the exact failure this removes.
`servoAckProbeIdle()` runs once at boot in all five ack-equipped sketches (PARMain,
FlipAllTest, FlipAllMaskedTest, FlipLeftColumnTest, ServoCycle) and logs loudly if the line
idles HIGH. Note the reverse mismatch (new ServoNano + old main MCU) is **not** detected —
flash ServoNano *after* the main MCU, or check the log.

The stuck-low detector described under *Validated 2026-07-27* still applies with the sense
inverted: a line stuck at the **asserted** level passes `servoAckSeen()` vacuously while
adding ~100 ms of idle-wait per command. That inflated cadence is the symptom to watch for.

### ServoNano-side pins (AVR Nano, `ServoNano.ino`)

| Pin | Net | Notes |
|---|---|---|
| **D2** | `RX_PIN` — `SoftwareSerial` RX ← main D9 | |
| **D3** | `ACK_PIN` — ack out → main D2 (through divider) | Had to be taken off `SoftwareSerial`, which claimed it as an idle-high TX. |
| **D5** | `SS_DUMMY_TX_PIN` | `SoftwareSerial` insists on owning a TX pin even though the link is one-way. Nothing is wired here. |
| **D9** | `SERVO_PIN` → **flip servo signal** (+ teed witness servo) | The actual servo PWM output — see §3a. |
| `LED_BUILTIN` (D13) | ack blink | **Non-blocking** — the old inline `delay(50)` left the board deaf right when repeats arrive. |
| USB serial | debug echo @ 9600 | Echoes every byte + `[ok <us>]` (on value change only) / `[bad …]` / `[stale …]`. |

### ⚠ Servo feed resistance is a real and expensive failure mode
A high-resistance harness (~3.4 Ω loop) starved the servo and mimicked *everything else*:
two servos declared "dying", non-repeatable flips from identical commands, hot wires, rail
collapse. It invalidated a full day of trim tuning.

**If flips are inconsistent, measure volts at the servo connector under load before
touching any constant.** Target ≤ 0.2–0.3 V drop at stall. An **MG90S** (the current servo)
draws ~2× an SG90 — stall ~1.5 A — so a feed sized for the old servo is not adequate.

Diagnosed once this way: an intermittent open in the **main servo's moving-carriage cable
at full +X (right) extension** — the main dropped flips on the right while the teed witness
servo stayed fine. That's the point of the witness: main misbehaving + witness fine ⇒ fault
is on the main's own branch, not the shared signal/5 V rail.

---

## 4. Color sensor — TCS3200

Mounted on the head at offset **(−24.005, +8.0) mm** from the flip head (`SCAN_OFFSET_X` / `SCAN_OFFSET_Y`). Illumination bank
on **D10** through an NPN (HIGH = on) for ambient-subtracted reads.

| TCS3200 pin | Main MCU | Purpose |
|---|---|---|
| S0 | D4 | Output frequency scaling (with S1) — set to 20 % |
| S1 | D5 | " |
| S2 | D6 | Filter select MSB — **see caveat** |
| S3 | D7 | Filter select LSB — **see caveat** |
| OUT | D8 | Square wave, measured with `pulseIn` |
| VCC / GND | — | — |
| LED bank | D10 (via NPN base) | HIGH = LEDs on; `LED_SETTLE_MS = 20` after toggling |

### S2/S3 are crossed on this rig — the enum encodes it
Measured Aug 2026 over 888 samples. An unfiltered channel must ≈ the sum of the filtered
ones; `true R+G+B = 13276` matches the value-1 channel (13888, ratio 1.05), not the
value-2 channel (1753, ratio 0.07). A commanded (S2,S3) pair therefore selects the filter
the datasheet assigns to the **swapped** pair. Values 0 and 3 are symmetric and land on
their datasheet filter; 1 and 2 swap:

| Commanded S2,S3 | value | Datasheet says | **Actually selected** | `TcsFilter` label |
|---|---|---|---|---|
| L,L | 0 | RED | RED | `TCS_RED` |
| L,H | 1 | BLUE | **CLEAR** | `TCS_CLEAR` |
| H,L | 2 | CLEAR | **BLUE** | `TCS_BLUE` |
| H,H | 3 | GREEN | GREEN | `TCS_GREEN` |

**The enum labels name the physical filter, not the datasheet's** (relabelled Aug 2026 —
they previously carried the datasheet names and were wrong on values 1 and 2). So
`tcsReadRGBC`'s `b` slot holds true BLUE and `c` holds true CLEAR, and `classifyDisc(long b)`
thresholds **BLUE**, which is the right choice:

| Channel | black-back range | cyan-back range | separation |
|---|---|---|---|
| **blue** (value 2) | 828–1628 | 7677–14695 | **10.22×** |
| clear (value 1) | 2075–2903 | 4462–7046 | 2.22× |

Clear collects broadband return from both faces and dilutes the colour difference. Reading
CLEAR instead would cut usable margin from 2.17× each way to 1.24×.

**The invariant is that `classifyDisc` thresholds BLUE.** The enum's two value assignments
are the only place the wiring is encoded, so if S2/S3 are ever rewired straight, those
values must move with it — otherwise the classifier silently ends up on CLEAR.

One consequence for archived data: logs captured **before** the relabel were written when
the `b` slot carried the clear channel and `c` carried blue, so their last two columns mean
the opposite of what a current log's do. Check the capture date before comparing dumps.

Classification is `c < SCAN_ON_BLUE_MAX` (**3535**) → cell is displaying cyan. Not a
distance, not a ratio, not a model (the ML classifier is retired).

**Polarity: the sensor views the disc's BACK.** A displayed-cyan disc shows its black back
⇒ reads LOW; a displayed-black disc reads HIGH. `classifyDisc` returns the **front /
displayed** colour, matching `gridState` and the target bitmap.

---

## 5. Lidar — VL53L4CD (standoff scanning)

| Signal | Main MCU | Notes |
|---|---|---|
| SDA | **A4** (GPIO 11) | Default `Wire` bus; `Wire.begin()` takes no pin args. |
| SCL | **A5** (GPIO 12) | Bus clocked at **400 kHz** (`Wire.setClock(400000)`). |
| XSHUT | **not wired** | `LIDAR_XSHUT_PIN = -1` / `XSHUT_PIN = -1`. |
| GPIO1 (interrupt) | **not wired** | `GPIO1_PIN = -1`. Ranging is polled, free-running. |
| VCC / GND | — | — |

Head offset from the flip head: `LIDAR_OFFSET_X = +31.075`, `LIDAR_OFFSET_Y = +14.0` mm,
calibrated so column 36 targets exactly `X = 0`.

**Boot init is flaky** — it failed roughly half of observed power-ons on the bench, then
ran a full 71-minute pass flawlessly once up. `lidarEnsure()` retries with a full
`Wire.end()` / `Wire.begin()` bounce between attempts. In **PARMain a permanent failure
does not halt** — a dead ranger must never stop the rig printing; the failure is recorded
in the day's record and the run continues. (`ScanColorLidarTest` *does* halt — it's a
diagnostic.)

---

## 6. Motion — Arduino Mega 2560 + CNC Shield V3

Firmware: patched grbl-Mega, active map **`CPU_MAP_2560_RAMPS_BOARD`** (selected in
`config.h:43`; the name is historical — the RAMPS 1.4 map was **replaced** with a CNC
Shield V3 Uno-style pinout). Source: `~/Documents/Arduino/libraries/grbl-Mega/cpu_map.h:135–283`.

### Shield pins (Uno-style block, D2–D13)

| Function | Mega pin | AVR port/bit |
|---|---|---|
| X step | **D2** | PE4 |
| Y step | **D3** | PE5 |
| Z step | **D4** | PG5 |
| X dir | **D5** | PE3 |
| Y dir | **D6** | PH3 |
| Z dir | **D7** | PH4 |
| **~EN** (all three drivers, shared) | **D8** | PH5 |
| X limit | **D9** | PH6 |
| Y limit | **D10** | PB4 |
| Z limit | **D11** | PB5 |

The shield has one limit pin per axis, so MIN and MAX are aliased to the same pin.
**`DISABLE_HW_LIMITS` is set** — the pins are used for *homing only*, not as live hard
limits. Protection comes from soft limits (`$20=1`).

### Relocated to Mega-only pins (to avoid shield conflicts)

| Function | Mega pin | Port/bit |
|---|---|---|
| Spindle enable | D22 | PA0 |
| Spindle direction | D23 | PA1 |
| Coolant flood | D24 | PA2 |
| Coolant mist | D25 | PA3 |
| Spindle PWM | **D45** | PL4 (Timer5 OC5B) |
| Reset | A9 | PK1 |
| Feed hold | A10 | PK2 |
| Cycle start | A11 | PK3 |
| Safety door | A12 | PK4 |
| Probe | A15 | PK7 |

None of these are wired on this rig — there is no spindle, coolant, probe, or control
panel. They're listed so you don't accidentally reuse a pin GRBL is driving.

Spindle PWM was moved to Timer5 because Timer4 (used by the stock RAMPS map) only outputs
to PORTH bits that collide with the shield's Y/Z dir and ~EN.

### Host link
GRBL's serial is **USART1** (`SERIAL_RX = USART1_RX_vect`, `serial.c` drives `UCSR1*`),
i.e. Mega **D19 (RX1)** and **D18 (TX1)** @ 115200 — *not* the USB port. That's the link
to the main MCU's D0/D1. The Mega's USB `Serial` is free, which is what
`Arduino Code/SerialBridge/` exploits (flash it to the **main MCU** for a USB↔Serial1
passthrough to send raw G-code from a PC serial monitor).

### Coordinate system
Homing drives to full negatives, so the work area lives in negative coordinates.

- `X_TRAVEL = 777.695`, `Y_TRAVEL = 412.0`
- `initGrid()` origins to `(-X_TRAVEL + 25.0, -Y_TRAVEL + 0.0)`
- Cell pitch **20.045 mm in X**, **23.40 mm in Y**
- Bitmap `y=0` is the top row but physical Y increases upward ⇒ Y is mirrored as `(GRID_H-1) - y`

**`Y_TRAVEL` MUST equal GRBL `$131` (currently `412`).** Homing pins the corner at `−$131`;
the switch is the physical anchor, so the bottom row stays physically put only when the two
match. Changing one without the other shifts the whole pattern by the difference — and
`Y_TRAVEL = 412.0f` appears in **~15 sketches**. Bump them all together with `$131`.

---

## 7. Power & grounds

Not fully captured in source — record measured values here as they're taken.

| Rail | Feeds | Notes |
|---|---|---|
| 5 V servo feed | MG90S flip servo + witness servo | **Measure at the connector under load.** Target ≤ 0.2–0.3 V drop at stall (~1.5 A). See §3. |
| 5 V logic | ServoNano | |
| Stepper supply | CNC Shield V3 / A4988-class drivers | |
| USB | Main MCU, Mega, ServoNano (via hub) | Servo current on the 5 V/ground shared through the USB hub correlates with the historical link dropouts. |

**Common ground is required** between main MCU, ServoNano, and Mega for the two logic links
to work at all.

---

## 8. Known discrepancies between sketches

Most sketches agree on the pin map. These do not — check before flashing.

| Sketch | Deviation |
|---|---|
| `CollectColorAmbient/` | **Shifted TCS map**: S0=D5, S1=D6, S2=D7, S3=D8, OUT=**D10**, LED=**D9**. This collides with the servo TX (D9) and the LED bank (D10) in every other sketch. Either it targets a different physical wiring or it's stale — **verify before flashing**. |
| `Color Sensor ML/ColorSensorStream/`, `ColorClassifier/` | Use bare integers (`4,5,6,7,8,10`) instead of `D4`…`D10`. Harmless under the default pin remap (`D4 == 4`), but they'd break if `BOARD_USES_HW_GPIO_NUMBERS` were ever defined. Both are part of the **retired** ML pipeline. |
| `VL53L4CDTest/` | Carries `DISTANCE_OFFSET_MM = -13` and `XSHUT_PIN`/`GPIO1_PIN` as `-1` placeholders. Nothing is wired to those. |

Everything else — `PARMain`, `FlipAll*`, `FlipLeftColumnTest`, `Scan*`, `Servo*`,
`ColorSensorTest`, `FullBoardScanTest` — uses the §2 map verbatim.

---

## 9. Bench / debug hookups

| Tool | Wiring |
|---|---|
| `arduino-cli board list` | Identify boards. Ports change on replug. |
| Free a port before flashing | `pkill -f serial-monitor`, plus any `cat`/logger holding it. |
| Multimeter on servo feed | Use `ServoLoadTest/` — 5 s motion / 5 s idle square wave, no GRBL, carriage never moves. Read under load, then the idle baseline. |
| Servo link integrity | `ServoTxStress/` (main MCU, link only) + `ServoLinkProbe/` (AVR Nano, servo pinned at REST, parses with ServoNano's exact logic and flags merges). |
| Color sensor sanity | `ColorSensorTest/` prints ambient-subtracted RGBC + the front cyan/black result. |
| Scan diagnostics | `ScanBottomRowsTest/` logs raw LEDs-OFF and LEDs-ON **separately**, per-frame clear min/max, and `pulseIn` timeout counts — the production helper returns only the subtracted value and can't distinguish "ambient drifted up" from "lit came in low". |
| Safe park during reassembly | `ServoCenter/` — holds at REST, never touches GRBL. |
| Raw G-code to the Mega | Flash `SerialBridge/` to the **main MCU** (USB↔Serial1 passthrough). |
| Rig camera capture | Select the Brio **by name**: `ffmpeg -f avfoundation -i "Brio 100"`. The numeric index is unstable (Continuity camera shifts it). Add `-movflags +frag_keyframe+empty_moov` to anything you might kill. |

### Reading the flash log over USB (this wasted hours twice)
- **`PlogDump` prints its dump ONCE**, then only re-dumps on receiving a newline. A
  read-only capture that attaches after that window sees **nothing**, which is
  indistinguishable from a wedged board. Send a newline periodically.
- **`Serial` is native USB CDC, so `while (!Serial)` blocks until the host raises DTR** —
  and `cat /dev/cu.*` never raises it (that's what separates `cu.*` from `tty.*`). Set DTR
  explicitly, or use a bounded wait in the sketch.
- **`arduino-cli monitor` block-buffers into a redirect** and loses the buffer when killed.
- **The IDE's `serial-monitor` grabs the port back** as soon as the board re-enumerates
  after a flash, and silently eats the output. `pkill -f serial-monitor` immediately before
  *capturing*, not just before flashing.
- A DFU upload failing with `LIBUSB_ERROR_PIPE` usually clears on retry; loop a few attempts.
- **plog lives on the `ffat` partition, mounted by LittleFS.** Build with **no**
  `PartitionScheme` flag. Do NOT chase `spiffs` — DFU uploads write only the app image
  (`dfu-util -D {sketch}.bin`), never `partitions.bin`, so the chip keeps its factory table
  and a `spiffs`-labelled partition never exists.
  `LittleFS.begin(true, "/littlefs", 10, "ffat")` → 9,830,400 bytes. Getting this wrong
  makes plog silently no-op **with no error**.

---

## 10. Shield PCB (Board3 / PCB3, 2026-08-16) — NOT YET FABBED

A 2-layer shield carrying both Arduinos plus a TPS22919 load switch, a 2N7002 LED driver and
screw terminals for servo/sensor/I²C/limit. Source files and a full design review are in
`PCBnSCHs/` — **read `PCBnSCHs/REVIEW.md` before ordering.**

Two things from it that change how the pin map above should be read:

- **D11 becomes the load-switch enable** (`5VSW` → TPS22919 `ON`), gating a switched 5 V rail
  (`5VSWED`) that feeds the servo, ServoNano, lidar and colour sensor. **No sketch drives
  D11**, and the part has an internal 530 kΩ pull-down, so as drawn the rail is off and none
  of those peripherals power up. The recommended fix is a 100 kΩ pull-up from `ON` to 3V3
  (→ 2.78 V, rail on by default) rather than firmware, so every existing sketch keeps working
  — and so a brownout can't cut servo power mid-flip.
- **The level-shift dividers are 2 kΩ/2 kΩ** on both the GRBL RX and ack lines, which is out
  of spec (see §2 and §3b). Both need a 3.3 kΩ bottom leg.

Until the board exists, the rig is hand-wired and the pin map in §2–§6 is the whole story.

---

## Sources

- `Arduino Code/PARMain/PARMain.ino` — lines 79–84 (TCS), 94/432 (servo TX), 500 (ack), 1659/1673 (I²C), 2088–2113 (`Serial1`/`Serial2`)
- `Arduino Code/ServoNano/ServoNano.ino` — lines 4–10, 33, 49, 92–101
- `~/Documents/Arduino/libraries/grbl-Mega/cpu_map.h` — lines 135–283 (`CPU_MAP_2560_RAMPS_BOARD`)
- `~/Documents/Arduino/libraries/grbl-Mega/config.h:43` — active map selection
- `~/Documents/Arduino/libraries/grbl-Mega/serial.c` — USART1 confirmation
- `~/Library/Arduino15/packages/arduino/hardware/esp32/2.0.18-arduino.5/variants/arduino_nano_nora/pins_arduino.h` — header→GPIO map
- `CLAUDE.md`, `Arduino Code/PARMain/FLOW.md` — narrative context
