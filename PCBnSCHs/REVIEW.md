# P.A.R. Shield — design review

**Current revision reviewed: Gerber V5 / Netlist V5 / BOM V4 (2026-08-19).**
2-layer, 90.68 × 51.82 mm. Hosts an Arduino Nano ESP32 and a 5 V Arduino Nano, powers and
talks to an Arduino Mega running GRBL, and carries the servo, colour sensor, lidar and LED
bank connectors.

Findings carry their status **inline at the claim**. Open items are first; resolved and
withdrawn items follow with the revision that closed them. Old IDs (B*, C*, L*, m*, N*, F*)
are kept in parentheses so earlier discussion stays traceable.

| Status | Meaning |
|---|---|
| 🔴 **OPEN — BLOCKER** | Board does not work, or destroys itself, until fixed |
| 🟠 **OPEN** | Real defect, should be fixed this spin |
| 🟡 **OPEN — minor** | Worth fixing while the files are open |
| ✅ **RESOLVED** | Fixed, with the revision noted |
| ⚪ **WITHDRAWN** | Retracted — my error, or the designer's rebuttal was correct |

**MEASURED** = parsed from Gerbers/netlist directly. **INFERRED** = from renders or datasheets.

---

# 🔴 OPEN — BLOCKERS

## O1. The TVS is wired in series with the +5 V rail 🔴 **OPEN in V5** — MEASURED

**Verified in V5:** `$1N7100` = {J1.3, U3.1}; `$1N7101` = {D2.2, U3.2}; `+5V` = {…, D2.1, …}.
**Neither D2 pin is on GND.**

```
J1.3 ─► U3 (polyfuse) ─► $1N7101 ─► D2.2 ─[SMBJ5.0A]─ D2.1 ─► +5V rail
```

A TVS is a shunt device. As drawn it is in the supply path and protects nothing:

- **Reverse-biased** (normal orientation): blocks below its ~6.4 V breakdown ⇒ **the +5 V rail
  is dead** and only the ESP32 (on 12 V) comes up.
- **Forward-biased**: behaves as a plain diode — ~1.0 V at 1.5 A, **~1.5 W in an SMB**
  (θJA ≈ 60 °C/W ⇒ ~90 °C rise), and the rail sags with servo current. That is precisely the
  current-dependent series drop this project has already spent a day misdiagnosing.

Confirmed independently by three passes. D2's silkscreen has no polarity bar, so which case
applies is undetermined — it does not change the fix.

**Fix:** `J1.3 → U3 → +5V` directly (bridge `$1N7101` into `+5V`; the pads are 5.18 mm apart
with clear space), and move **D2.2 to GND** with D2.1 on `+5V`.

**Also:** SMBJ5.0A has a **5.0 V stand-off**, which a 5.25 V rail exceeds ⇒ leakage.
Use **SMBJ6.0A or SMBJ6.5A**.

## O2. C11 is a 6.3 V part on an 11.3 V rail 🔴 **OPEN in V5** — MEASURED

**Verified in V5:** `$1N1028` = {C10.1, **C11.2**, D1.1, J2.15} — the post-D1 VIN node, ~11.3 V.
BOM V4 still lists C11 = **CL31A107MQHNNNE (LCSC C15008) = 100 µF 6.3 V X5R 1206**.

**1.8× over rated voltage.** MLCCs fail **short**, so this takes the 12 V rail with it.

**Fix:** 100 µF at 25 V does not exist in 1206. Use **C14860 (10 µF 25 V X7R 1206)** — already
in your BOM for C4/C7/C9, so no new line item. 10 µF is a proper buck-input cap; at 11.3 V
bias it derates to roughly 5–6 µF, which is fine.

Every other capacitor's rating checks out: C2/C5 16 V on 5 V, C3/C6/C8/C10/C12 50 V,
C4/C7/C9 25 V on ≤5 V. C11 is the only one wrong.

---

# 🟠 OPEN

## O3. The polyfuse will nuisance-trip on a jammed arm 🟠 **OPEN in V5** — MEASURED copper / INFERRED fuse data

`U3` (MF-NSMF200-2, **2.0 A hold**) is the sole element between J1.3 and everything on `+5V`
*and* `5VSWED` — servo included.

Load at stall ≈ 1.5 A servo + 0.2 A Mega + 0.03 A ServoNano + 0.05 A sensors ≈ **1.78 A**,
i.e. **89 % of hold at 23 °C**, derating to ~1.4–1.6 A hold at 50 °C in an enclosure.

A momentary stall will not trip it; a **jammed arm will** — and this rig has snapped its flip
arm once. A tripped PPTC goes to hundreds of ohms, collapsing the rail, which at firmware
level is indistinguishable from the failure modes already chased at length.

It also costs droop budget: **30–70 mΩ** of fuse resistance on top of **48.2 mΩ** measured
copper round-trip, roughly doubling the board's contribution to **117–177 mV at 1.5 A**.

**Fix:** fuse only the logic / `5VSWED` branch and leave the servo unfused, or move to a
≥3 A-hold, lower-resistance device. Re-verify Vmax ≥ 6 V either way.

## O4. Q2 switches the sensors' ground, which cannot turn them off 🟠 **OPEN in V5** — MEASURED topology / INFERRED behaviour

`CLRNLDRSRC` = {J6.2, J8.7, Q2.3}. `3V3` = {J2.2, J6.1, J8.6, R9.2, R10.1, R11.1} — always live.

Only the **ground** is switched; VDD stays at 3.3 V. With Q2 off, both sensors' local ground
floats, while the MCU still drives S0–S3 and the I²C pull-ups (**now fitted — R10/R11**) hold
SDA/SCL. Current flows through the sensors' ESD structures into the floating ground, pulling
it to ~0.7 V and **partially powering both parts through their I/O pins**. The "off" state is
an undefined bias condition, not off — so the power-cycling Q2 exists to provide is not
achieved, and the VL53L4CD has no guaranteed clean POR when Q2 re-closes.

Note the interaction: **adding the I²C pull-ups in V5 made this worse**, since they now feed
the floating ground.

**Fix (recommended):** delete Q2 + R8, bond J6.2/J8.7 to the pour, and make **J6 5-pin with
`XSHUT` on D3** — the VL53L4CD's purpose-built reset pin. The TCS3200 is stateless and never
needs power-cycling. Alternative: switch the **high side** (P-FET in the 3V3 feed).

## O5. `CLRNLDRSRC` is routed as a signal, not a return 🟠 **OPEN in V5** — MEASURED

38.3 mm, **entirely 0.254 mm (10 mil)**: Q2.3→J6.2 = 47.4 mΩ, Q2.3→J8.7 = 22.0 mΩ.

Resistively harmless (~1 mV at 25 mA). The cost is **return-path geometry**: every sensor
signal — SDA, SCL, S0–S3 and `OUT` — returns through this thin trace to Q2 before entering
the pour, instead of returning in the plane under its own signal. For `OUT` (the frequency
output timed with `pulseIn`, and the channel the unresolved full-board false positives live
on) the signal runs 24 mm on top while its return runs 9–23 mm on the bottom to a different
point — a loop of order **100–200 mm²**.

**Fix:** subsumed by O4 — deleting Q2 lets the pour be the return. If Q2 stays, widen to
≥0.6 mm and route it directly beneath the J6/J8 signal groups.

## O6. `LED-` runs beside SCL and beside the servo-ack analog node 🟠 **OPEN in V5** — MEASURED geometry / INFERRED crosstalk

| Victim | Coupled length | Min gap |
|---|---|---|
| `SCL` | **45.7 mm** (6.6 mm under 200 µm) | **152 µm** |
| `LEDGT` ‖ `SCL` | 8.2 mm | **152 µm** |
| **`$1N7075`** (servo-ack analog node) | 5.7 mm | **184 µm** |
| `S0` | 6.2 mm | 152 µm |

Estimated C_m(LED−, SCL) ≈ **1.6 pF** against ~30 pF of bus ⇒ a ~5 % step: 250 mV for a 5 V
swing. With the pull-ups now fitted (R10/R11 at 3.3 kΩ) recovery is ~100 ns rather than
1.35 µs, so this is **less severe than before V5** — but `LED-` is toggled ~3× per cell ×
666 cells per pass, so exposure is high.

`$1N7075` matters independently: that is the ack divider node, now read as a **1.20 V analog
threshold** from a 1.25 kΩ source. A switching LED return 184 µm away for 5.7 mm should move
regardless of the threshold's margin.

⚠ **This is the interaction to respect: Q1 is now an AO3400A with fast edges.** Upgrading the
FET without addressing the routing trades an LED-repeatability problem for an I²C/analog
corruption problem.

**Fix:** reroute `LED-`/`LEDGT` away from the SDA/SCL/S0 bundle or interpose a grounded
guard; **and** add a **100–470 Ω series gate resistor** at Q1 to slow the edge deliberately
(the bank switches at a few kHz and needs no speed). Minimum viable: open the two sub-200 µm
runs at x ≈ 63.7–64.2 to ≥0.5 mm.

## O7. The limit-switch pins do not implement the intended function 🟠 **OPEN in V5** — MEASURED

**Verified in V5:** `LIMSW-` = **{J9.3}** — still the only genuine single-pin net on the board.
`LIMSW+` = {J9.2, C1.2} — reaches a 1 nF cap and nothing else.

*Designer's intent (stated): take the Y limit-switch wire in, filter it, and export the
filtered version to the CNC shield on the Mega.* As drawn that is not implementable, for
three separate reasons:

1. **J9.3 connects to nothing at all** — not GND, not J9.2, not the cap. Nothing can leave it.
2. **There is no series resistor**, so there is no RC filter — C1 is only a shunt cap.
3. **C1 is 1 nF**, and this project's own Y-limit investigation concluded 1 nF (~50 µs) is too
   small and recommended **1 kΩ + 100 nF**.

**Fix:** `J9.2 → R (1 kΩ) → J9.3`, with C1 raised to **100 nF** from J9.3 to GND. Switch in on
J9.2, filtered signal out on J9.3, then the flying wire to the CNC shield. Three changes, and
it matches both the stated intent and the documented fix.

## O8. R6 (0 Ω) makes a plugged-in USB burn ~1 W in U4 🟠 **OPEN in V5** — MEASURED + datasheet

R6 ties QOD to VOUT, which is **correct per TI SLVSEN5B** (one of three sanctioned options —
see [Adjudicated facts](#adjudicated-datasheet-facts)). The problem is the interaction.

Table 2: with ON low and QOD tied to VOUT, VOUT is pulled to GND through **RPD,QOD = 24 Ω**.
`5VSWED` reaches the Mega's 5 V pins (J11.1/2) and the ServoNano's (J5.12), and the Mega's
`T1` FDN340P **hard-connects its +5 V to USBVCC when no VIN is present** — so the Mega's 5 V
pin is a genuine *source*, not a passive load.

⇒ switch off + either USB plugged: **~196–208 mA, ~0.92–1.04 W in an SC-70-6**, and with
**RθJA = 210.7 °C/W** that is a **194–219 °C rise** — past the 180 °C thermal shutdown.

Live during any bring-up session (flashing GRBL over the Mega's USB with the shield
unpowered). *Accepted as low-risk in production, where no USB is attached.*

**Fix:** **R6 → 1 kΩ** caps the sink at ~5 mA. TI's own worked example uses 1 kΩ. Free.

## O9. The 3V3 sensor rail has no decoupling 🟠 **OPEN in V5** — MEASURED

Every capacitor sits on `+5V`, `5VSWED`, `$1N1028`, `+12V` or `LIMSW+`. **`3V3` has none.**

Measured: J2.2 → J6.1 = **42.9 mΩ over ~40 mm**; J2.2 → J8.6 = 22.6 mΩ. The trace is 0.762 mm
(generous) — the gap is the missing local reservoir at the far end of a 40 mm feed to a
VL53L4CD whose VCSEL draws tens of mA in bursts.

**Fix:** 100 nF + 4.7 µF at J6, 100 nF at J8. Both connector areas have room.

## O10. C3 and C4 are on stubs at J7; only C5 is in the current path 🟠 **OPEN in V5** — MEASURED

| Cap | Value | Distance to J7.1 | R | In path? |
|---|---|---|---|---|
| **C5** | 470 µF | 9.72 mm | 4.85 mΩ | **YES** — both trunk legs terminate in its pad. Correct. |
| C4 | 10 µF | 8.26 mm | 9.63 mΩ | **STUB** — 1.73 mm of 10 mil + one via |
| C3 | 100 nF | 4.60 mm | 5.20 mΩ | **STUB** — 1.33 mm of 10 mil + one via |

The bulk cap is placed well; the two **high-frequency** parts are the ones hung off 10-mil
stubs. C3's HF loop is ≈22 mm of conductor ⇒ ~12 nH ⇒ **SRF ≈ 4.6 MHz** — worth perhaps a
third of a properly in-path 100 nF.

**Fix:** widen the C3/C4 taps to 0.6 mm, double the vias, and place C3 so the trunk passes
*through* its pads — exactly as was done correctly for C6–C9 at U4 (see R11).

## O11. There is still no `LED+` on the board 🟠 **OPEN in V5** — MEASURED

`LED-` = {J9.1, Q1.3} is the only LED connection. The anode feed and any current-limiting
element remain off-board and undocumented, so the bank's current is unknown — which is what
prevents sizing `LED-` (O12) and confirming Q1.

If the anode is tapped from a sensor supply, the LED current flows through that sensor's own
supply pin, producing a droop that appears **only when the LEDs are on** — i.e. exactly in
phase with the (on − off) subtraction, and therefore not cancelled by it.

**Fix:** make J9 4-pin with a dedicated `LED+` from unswitched `+5V`, with its own local bulk,
star-separated from the sensor feed.

## O12. `LED-` carries the bank current over 64 mm of 10-mil trace 🟠 **OPEN in V5** — MEASURED / current UNDETERMINED

J9.1 → Q1.3 = **125.5 mΩ** (≈64 mm of 0.254 mm, vs 36 mm straight). At 0.2 A that is 25 mV and
5 mW — fine. At 1 A it is 125 mV and above the IPC 10 °C-rise limit of ~0.88 A for that width.

Blocked on O11 (bank current unknown). **Fix:** if >0.5 A, widen to ≥0.6 mm — which also
creates the room to move it away from SCL (O6).

## O13. Feeding the Mega's 5 V pin back-drives its USB port 🟠 **OPEN in V5** — MEASURED / INFERRED (F20)

`5VSWED` drives J11.1/J11.2 = the Mega's +5 V pins. Arduino's own documentation: *"Supplying
voltage via the 5V or 3.3V pins bypasses the regulator, and can damage your board. We don't
advise it."* There is no sanctioned voltage or current rating for injecting into that pin.

Via the FDN340P path described in O8, the shield's 5 V propagates onto the Mega's USB VBUS
through its 500 mA polyfuse and out to any attached host.

**Fix:** a series Schottky or ideal-diode between U4's output and J11.1/2 — **or** accept it
and document "do not attach USB to the Mega while the shield is powered." The O8 fix handles
the destructive half regardless.

---

# 🟡 OPEN — minor

| # | Finding | Detail | Fix |
|---|---|---|---|
| **O14** | **Silk on an exposed pad — one genuine case** | **R1 pad 2**, mask opening at (9.570, −44.069): **0.1995 mm² = 8.46 % of the pad**, a 0.152 mm band from U2's body outline at y = −43.752. Bottom side: **0.000 mm²**. | Move U2's courtyard line to y ≈ −43.2, or R1 down 0.6 mm |
| **O15** | J6/J8 silkscreen still reads `+5V` | The net is now `3V3`. GND labels are also wrong while Q2 is in place. | Relabel; revisit after O4 |
| **O16** | U4 mask dams are 128 µm | Below most fabs' 0.15–0.20 mm minimum, so pins 1-2-3 and 5-6 will share open windows. Not a defect; raises bridging risk on the one fine-pitch part. | Reduce mask expansion to 0.025 mm on U4, or stencil carefully |
| **O17** | Two silk segments use a 0.0762 mm (3 mil) aperture | 1.8 mm total; below the ~0.15 mm printable minimum, so they print raggedly or not at all. | Widen to ≥0.15 mm |
| **O18** | A 0.254 mm neck in the `5VSWED` trunk | At (25.512,−14.239)→(25.400,−14.351), in the *entire* switched-rail current path. Thermally OK (~0.88 A limit) but careless. | Widen the C9→trunk section to 0.762 mm |
| **O19** | No board name, revision or date on the silk | Divider values and part choices have changed between revisions; an unmarked board is a real hazard. | Add `P.A.R. Shield — Rev E — 2026-08-19` |
| **O20** | No test points | The documented bring-up procedure is built on probing the servo feed and the divider nodes. | Pads on `+5V`, `5VSWED`, `3V3`, GND, J7.1 and `$1N7075` |

---

# ✅ RESOLVED

| # | Was | Status |
|---|---|---|
| **R1** (B1) | Switched rail never enabled — `U4.3` ← D11, undriven; internal 530 kΩ SPD holds it off | ✅ **V5 + firmware.** `PARMain` drives D11 HIGH as the first statement in `setup()`, **and V5 adds R9 = 100 kΩ from `5VSW` to 3V3**. The pull-up also closes the "ON floats after the SPD disconnects" hole (TI: the Smart Pull-Down is disconnected once ON has been driven high, and the pin table says *do not leave floating*). Referencing it to **3V3 rather than 5 V** is the right call — it gates on the ESP32's own regulator, so the rail still sequences behind the MCU without needing firmware to do it. |
| **R2** (B2) | Both level-shift dividers 2 k/2 k = 2.50 V against V_IH 2.475 V | ✅ **V3.** R3/R5 → 3.3 kΩ ⇒ 3.11 V. |
| **R3** (B4) | 3V3 not routed anywhere | ✅ **V4.** `3V3` = {J2.2, U1.3, J6.1, J8.6, …}. Conceded: this was an enabler, not a standalone defect, and should not have been listed as blocking. |
| **R4** (C1) | Servo droop: 198–230 mΩ / 300–345 mV at stall | ✅ **V3.** `J7.1` moved to unswitched `+5V` and the rail widened to 30 mil. Measured **44.3 / 44.4 / 43.86 mΩ** by three independent methods ⇒ ~70 mV. Inside the ≤0.2–0.3 V budget — *subject to O1 and O3, which put it back*. |
| **R5** (C2) | Switched rail a daisy chain with the servo last | ✅ **V3/V4.** Servo on `+5V`, sensors on `3V3`; only ServoNano and Mega remain on `5VSWED`. |
| **R6** (C3) | Zero decoupling on the entire board | ✅ **V4** — ten capacitors added. Placement is good at U4 (R11) and for C5; **C3/C4 stubs remain open (O10)** and **3V3 has none (O9)**. |
| **R7** (C4) | R5 destroys the ack's fail-loud property | ✅ **Firmware.** Polarity inverted to active-HIGH so the divider's bottom leg *is* the fail-safe, and `servoAckWaitIdle()` now returns a bool — a line stuck asserted is treated as a fault and blocks, instead of timing out silently and letting `servoAckSeen()` pass vacuously. |
| **R8** (C5) | No I²C pull-ups | ✅ **V5.** R10/R11 = 3.3 kΩ to 3V3. Bus was empirically fine beforehand (~6–7 h of clean lidar scans), so this is margin rather than a repair — but it does interact with O4. |
| **R9** (C6) | 5 V into non-5 V-tolerant pins (TCS3200 `OUT`→D8; lidar I²C) | ✅ **V4**, and cleanly: both sensors moved to `3V3`, so `OUT` swings 0–3.3 V natively and any breakout pull-ups now reference 3.3 V. **Recalibration is small** — the TCS3200's supply-voltage sensitivity is ±0.5 %/V, so 5 V→3.3 V shifts output frequency <1 %, well inside the 2.17× threshold margin. (That spec is characterised at 5 V ±10 %, so verify empirically.) |
| **R10** (C7) | Q1 = 2N7002, undriven at 3.3 V, no gate pull-down | ✅ **V4.** AO3400A + R7 100 kΩ pull-down. **But see O6** — the faster edge makes the `LED-`/SCL coupling matter more, and no gate series resistor is fitted. |
| **R11** (C8) | R1 = 2 kΩ raising noise gain on the servo command line | ✅ **V4.** R1 → 330 Ω. Kept at 330 Ω rather than raised to 2.2 kΩ for injection limiting: instead, **`Serial2.begin()` was moved after the rail-settle delay** in firmware, so D9 stays high-Z until the ServoNano is powered. Injection goes to zero without giving up noise immunity. |
| **R12** (C10) | No decoupling on the ESP32 VIN node | ✅ **V4.** C10 (100 nF) + C11 on the D1 cathode, C12 on +12 V. **C11's voltage rating is a blocker — see O2.** |
| **R13** (C11) | Two 5 V sources shorted (J1.3 and J11) | ✅ **V3.** J11 moved to `5VSWED` as an output; `J1.3` is now the sole source. |
| **R14** (L1) | ~11 cm² high-current loop | ✅ **V4.** C5 placed at J7 collapses it. |
| **R15** (N8) | J12's mapping to a Mega UART unproven | ✅ **Verified.** Reconstructed against Arduino's official Mega 2560 Rev3 reference design: one self-consistent affine fit lands every pin on the 2.54 mm grid. **J11 = XIO power/ground** (5V/5V and GND/GND pairs, so the column swap is harmless), **J12.3/J12.4 = D19/D18 = RX1/TX1 with correct polarity**, **J10 = A8–A15** (intentionally unused). And the patched grbl-Mega **is** built for USART1 (`SERIAL_RX = USART1_RX_vect`; `serial.c` drives `UCSR1*`/`UBRR1*`), so the link works end to end. |

---

# ⚪ WITHDRAWN / CORRECTED

| # | Claim | Why withdrawn |
|---|---|---|
| **W1** (C9) | "J12 has no ground pin — assign one of the spares" | **Designer correct.** J12 mates to the Mega's COMMUNICATION header (D21–D14), which contains **no GND pin**; tying a spare to GND would short a Mega GPIO. The return via J11's GND is the only option and is adequate. |
| **W2** (m2) | "Silk over pads — 20 instances including R6" | **Designer correct, and my method was wrong.** That figure came from testing silk against **copper**, which false-positives on every trace. Re-tested against **solder-mask openings**: exactly **one** genuine case board-wide (R1 pad 2 — now O14). R6 is clean, as the designer said. |
| **W3** (m3) | "J7.2 has only 2 thermal spokes vs 4 elsewhere" | **Immaterial.** Two spokes ≈ 0.34 mΩ ⇒ 0.5 mV at 1.5 A. The designer's explanation (adjacent non-GND pads) also accounts for it. |
| **W4** (m9) | "No header for the witness servo" | **Designer correct.** A KF128 screw terminal accepts two wires; a second header is unnecessary. |
| **W5** (L5) | "108 mm of bottom trace slots the ground plane" | **Second-order, and already minimised** — the designer routes on the bottom only where no top path exists. Ground measured excellent regardless (R16). |
| **W6** (C12) | "ServoNano USB back-feeds the switched rail" | **Accepted as a production non-issue** — no USB attached in production. The bench hazard it creates is tracked under O8 instead. |
| **W7** | "TCS3200 S0–S3 at 3.3 V may not meet V_IH" | **Wrong.** ams DS000107 v1-01 p.6: `VIH min 2, max VDD` at VDD 2.7–5.5 V — an **absolute 2.0 V**, not a fraction. 3.3 V is in spec at VDD = 5 V. |
| **W8** | "The J1 silkscreen arrows point into gaps and invite a mis-wire" | **Wrong.** The two glyphs are **closed 1.0 × 1.5 mm rectangles** (four strokes each) — J1's body outline, not arrows. The three text labels are correctly aligned over their pads. |
| **W9** | "ESP32 D1/TX may be unable to pull the Mega's RX low (16U2 contention)" | **Does not apply.** That mechanism lives on D0/D1; this design uses Serial1 (D18/D19). |
| **W10** | "R6 / U4 pin 5 may be a CT soft-start pin wired wrongly" | **Wrong.** Pin 5 is **QOD**, and tying it to VOUT is one of three sanctioned configurations. Only the 0 Ω *value* is at issue — see O8. |
| **W11** | "L6: +5V to J11 at 15 mil may be too thin for the Mega" | **Adequate.** 15 mil on 1 oz external = 1.19 A at 10 °C rise; a Mega plus CNC-shield *logic* is ~300 mA (motor current comes from its own supply). ~4× margin. |

---

# Reference

## Decoupling — what is actually needed

The Arduino modules each carry their own decoupling and bulk at their own pins and seat
directly in the headers, so **caps at J11, J5.12 or J2/J3 are pure duplication — do not fit
them.** There is also an inrush constraint: the TPS22919's slew is fixed at 3.2 mV/µs, so
inrush = C × 3.2 mA/µF against a **1.5 A** limit, and that budget is already partly spent
charging the modules' own bulk (0.6–1.0 A at an estimated 200–300 µF). `5VSWED` can take a
10 µF ceramic and no more. The servo's bulk is exempt because it sits on the **unswitched**
rail.

| Position | Part | LCSC | Verified |
|---|---|---|---|
| 100 nF 50 V X7R 0603 | Samsung CL10B104KB8NNNC | **C1591** | ✅ |
| 10 µF 25 V X7R 1206 | Samsung CL31B106KAHNNNE | **C14860** | ✅ (also the C11 replacement — O2) |
| 470 µF 16 V bulk at J7 | Lelon RXK471M1CBK-0811, 8 × 11.5 mm, 3.5 mm pitch | **C231191** | ⚠ LCSC publishes no ESR or ripple figure for it — nor for the existing C2. Cannot verify the ≤60 mΩ target from LCSC data. |

**X7R** = ±15 % over −55…+125 °C. **DC bias derating is the trap**: a 10 µF 6.3 V 0805 at 5 V
delivers 2–4 µF; the 25 V 1206 delivers ~7–8 µF. Spec 25 V/1206 for 10 µF on a 5 V rail.
**Layout:** cap in the current path (not on a stub — see O10), smallest value nearest the load,
ground via immediately adjacent to the pad (0.3–0.5 mm, not in-pad), two vias where there is
room. Bulk covers only the first tens of µs of a current step; **sustained stall droop is pure
I×R and is fixed by resistance, not capacitance.**

## Ground — measured, and good

Single connected bottom pour, **3406 mm²**, 115 interior voids (all antipads), survives the
rework unslotted. Returns to J1.2: J7.2 **3.08 mΩ**, J11.35/36 2.64 mΩ, J2.14 2.81 mΩ,
J5.14 2.02 mΩ, U4.2 1.41 mΩ, Q2.2 3.23 mΩ. **Ground is not a problem on this board — every
issue is on the high side.** Useful diagnostically: droop measured at the servo will be almost
entirely on the +5 V wire.

Narrowest corridor: **1.80 mm** at y ≈ −33. The dominant discontinuity is J11's 36 through-hole
pads leaving **0.227 mm** of web between antipads — a comb across mid-board, with only 4 of 36
pins used.

## DFM — everything else passes

Min track 0.254 mm ✅ · min clearance 0.152 mm ✅ · min annular ring 0.152 mm (vias) ✅ ·
hole-to-hole 413 µm ✅ · hole-to-edge 566 µm ✅ · copper-to-edge 256 µm, none outside the
outline ✅ · all 49 vias tented ✅ · zero pads without mask openings ✅ ·
**copper matches the netlist exactly: 114 nets, 114 islands, 0 shorts, 0 opens, 0 floating
copper** ✅.

Hole counts differ by revision — V3: 158 PTH (24 vias) + 8 NPTH; **V5: 185 PTH (49 vias) +
7 NPTH**. The 49 via coordinates appear in **both** drill files; deduplicate by coordinate or
you will double-count.

Note: netlist V5 is exported from the **PCB** document, so the U1/U2 module symbols are absent
and ~36 header pins appear as single-pin nets. Those are unpopulated header positions, not
defects. **`LIMSW-` is the only genuine one (O7).**

## Adjudicated datasheet facts

Settled against primary sources, not by preferring a reviewer.

- **TCS3200** (ams DS000107 v1-01 p.6): `VIH min 2 V, max VDD` at VDD 2.7–5.5 V — **absolute**.
  `VIL 0–0.8 V`. `VOH min 4.0 / typ 4.5 V @ IOH = −2 mA` (so the pre-V4 overstress on D8 was
  0.4–0.9 V over abs max, not 1.3 V). Supply-voltage sensitivity **±0.5 %/V**.
- **TPS22919** (TI SLVSEN5B): ON pin *"Active high switch control input. Do not leave
  floating."* §8.3: *"When power is first applied to VIN, a Smart Pull Down is used to keep the
  ON pin from floating until the system sequencing is complete. **Once the ON pin is
  deliberately driven high (≥VIH), the Smart Pull Down is disconnected.**"* RPD,ON = 530 kΩ;
  **RPD,QOD = 24 Ω**; VIH(ON) = 1–5.5 V; **RθJA = 210.7 °C/W**; I_MAX 1.5 A; slew 3.2 mV/µs.
  Table 2: ON low + QOD tied to VOUT ⇒ VOUT pulled to GND through RPD,QOD.
- **ESP32-S3**: V_IH = 0.75 × VDD = **2.475 V**; V_OH min = 0.8 × VDD = 2.64 V.
- **ATmega328P/2560**: V_IH = 0.6 × VCC = **3.00 V** at 5 V; input leakage ≤1 µA.

## Still needs a bench measurement

1. **LED bank current and forward voltage**, and where `LED+` is tapped — gates O11, O12 and Q1 sizing.
2. **Copper weight ordered.** All resistances assume 1 oz; they halve at 2 oz.
3. **Actual MG90S stall current** — vendor figures span 0.65–2.5 A, and O3's fuse margin depends on it.
4. **Total Mega + CNC Shield V3 draw** — no official Arduino figure exists; needed to size O3.
5. **Which VL53L4CD carrier** is fitted (pull-up rail, regulator) — affects O4.
6. **Whether the TCS3200 breakout ties `/OE` low** — J8 does not bring it out; if it floats, `OUT` is high-Z.
7. **D2's cathode orientation** — decides whether O1 gives a ~1 V sag or a dead rail. Does not change the fix.
8. After any fix: **volts at J7.1 under a real stall** with `ServoLoadTest/`, before touching any firmware constant.

## Open hypothesis for the unresolved scan false positives

Not a finding — a candidate worth testing. The TCS3200 datasheet recommends buffering `OUT`
beyond 12 inches, and this harness is longer. A **74LVC1G17 Schmitt receiver** at the MCU end
is the most structural of the available explanations for false positives that appear only on
70-minute full-board passes and never on the 13-minute bottom-rows test. O5's 100–200 mm²
return loop on that same net is a second contributor pointing the same way.
