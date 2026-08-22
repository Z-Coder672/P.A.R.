# P.A.R. Shield — design review

**Current revision reviewed: Gerber V7 / Netlist V7 / BOM V6 (2026-08-22) — silk `PARShield V0.7`.**

> ## ✅ VERDICT: FABRICATION-READY
> Full DFM sweep passes with margin; copper matches the netlist exactly (113 islands,
> **0 shorts, 0 opens, 0 floating copper**); the D3 removal was verified forensically — no
> orphan pad, no gap, no stub in the `5VSWED` trunk. Servo loop **≈51 mΩ, 77 mV at 1.5 A stall**
> against a 0.2–0.3 V budget. Mega rail **4.93 V**, 430 mV above its 4.5 V floor.
>
> **One thing worth adding before you order: a 100 nF across J8.6/J8.7 (N3).** Everything else
> open is optional.
2-layer, 90.68 × 51.82 mm. Hosts an Arduino Nano ESP32 and a 5 V Arduino Nano, powers and
talks to an Arduino Mega running GRBL, and carries the servo, colour sensor, lidar and LED
bank connectors.

**Every finding keeps its original text, with a status line added directly beneath its
heading.** Nothing has been summarised away. Still-open items come first, then resolved, then
withdrawn. Original IDs (B*, C*, L*, m*) are unchanged so earlier discussion stays traceable.

| Status | Meaning |
|---|---|
| 🔴 **OPEN — BLOCKER** | Board does not work, or destroys itself, until fixed |
| 🟠 **OPEN** | Real defect, should be fixed this spin |
| 🟡 **OPEN — minor** | Worth fixing while the files are open |
| ✅ **RESOLVED** | Fixed — revision noted in the status line |
| ⚪ **WITHDRAWN** | Retracted — my error, or the designer's rebuttal was correct |

**MEASURED** = parsed from Gerbers/netlist directly. **INFERRED** = from renders or datasheets.

---

# 🔴 OPEN — BLOCKERS

**None. Both blockers were closed in V6** — the TVS is now a proper shunt to GND (O1) and
C11 is a 25 V part (O2). Both are recorded under RESOLVED with the measurement that closed them.

---

# 🟠 OPEN

## N1. D3 (SS34) drops the Mega below its 4.5 V minimum — MEASURED copper / datasheet

> ✅ **RESOLVED — V7.** D3 removed; `5VSWED` now reaches `J11.1`/`J11.2` directly and the
> `$1N7047` net is gone. The Mega is back to **~4.93 V, 430 mV above its 4.5 V floor**. The
> back-feed hazard stays covered by R6 = 1 kΩ, which caps the QOD sink at ~5 mA.

**Microchip ATmega2560 datasheet, p.1 speed grades: `0–16 MHz @ 4.5 V – 5.5 V`.** The Mega runs
at 16 MHz, so 4.5 V is a hard floor.

Measured budget to J11 at ~330 mA:

| Element | Drop | Rail |
|---|---|---|
| U4 R_ON (108 mΩ) | 35.6 mV | 4.964 V |
| U3 polyfuse (~50 mΩ) | 16.5 mV | 4.948 V |
| copper (43.7 mΩ) | 14.4 mV | 4.933 V |
| **D3 SS34 Vf typ @0.33 A** | **350 mV** | **4.583 V** |
| **D3 SS34 Vf max @0.33 A** | **450 mV** | **4.483 V — below spec** |

**Fix: remove D3** (or DNP it and bridge the pads). The hazard D3 was added for — the Mega's USB
back-feeding `5VSWED` into the TPS22919's QOD pull-down — **is already solved by R6 → 1 kΩ**,
which caps that sink at ~5 mA instead of 208 mA. D3 only added *isolation* (the ability to
actually cut the Mega's power), and it is not worth 0.45 V of the Mega's supply to have it.
Without D3 the Mega sits at **4.93 V, with 430 mV of margin**.

If the isolation is genuinely wanted, it needs an ideal-diode / P-FET ORing controller
(~30 mV) rather than a Schottky — but given R6 already removes the destructive failure, the
simplest correct answer is no diode.

## N2. No 3V3 bypass capacitor to GND — MEASURED

> ✅ **RESOLVED — V7.** C17 (100 nF) + C18 (10 µF) added from `3V3` to GND.


`3V3` = {C13.2, J2.2, Q2.2, R8.2, R9.2, R10.1, R11.1}. **Nothing goes to GND.** C13 is 100 nF
from 3V3 to Q2's *gate* — a slew-limiter, not a bypass.

Q2 switches into C14 + C15 + C16 ≈ 10.2 µF plus the sensor and LED load, and that inrush comes
straight out of the module's 3.3 V pin with no local reservoir. With C13/R12 giving a ~100 µs
gate ramp, the charge current is ≈ 10.2 µF × 3.3 V / 100 µs ≈ **0.34 A** off the module regulator.

**Fix:** 10 µF + 100 nF from `3V3` to GND, next to J2.2 / Q2.2.

## N3. J8.6 has no decoupling within 12 mm — MEASURED

> ✅ **RESOLVED — V8.** C19 (100 nF) added on `CLRNLDRDRN`, and it landed where it needed to:
> **4.52 mm from J8.6 / 4.46 mm from J8.7, loop 12.8 mΩ** (was C16 at 11.98 mm / 26.7 mΩ).
> Independently measured. The lidar end was already fine — C14 is 3.8 mΩ from J6.1.

>
> Two mechanisms, and they differ in size. **DC:** the ~160 mA LED bank steps the sensor supply
> by 160 mA × 21.9 mΩ ≈ **3.5 mV**, which against ±0.5 %/V is a 0.002 % frequency shift and is
> settled long before either 5-frame sample — negligible, as previously argued. **HF:** the
> TCS3200's own output driver switching into 20–25 nH of unbypassed supply is a different and
> genuinely un-modelled path. Small, but this is the sensor at the centre of the unexplained
> full-board false positives, and the fix is **one 100 nF 0603 across J8.6/J8.7** — adjacent
> pins, clear board area. **Do it.**


C16 sits at Q2's drain; the run on to J8.6 is **11.98 mm / 24.4 mΩ** of 10-mil trace. So the
TCS3200 — whose supply steadiness the ambient-subtracted read depends on — has no local
reservoir. **Fix:** 100 nF across J8 pins 6–7.

## N4. J9 silk says `SW+` / `SW-` but they are the same net — MEASURED

> ✅ **RESOLVED — V7.** Silk now reads **`LED-  SWI  SWO`** (read from the rendered top silk).
> `SW IN` / `SW OUT` makes the pass-through unambiguous — nobody will wire a switch across them.


`LIMSW` = {C1.2, J9.2, J9.3}: pins 2 and 3 are one node. That is correct for the intended
in/filter/out pass-through — **but the silkscreen still labels them `SW+` and `SW-`**, which
invites wiring a limit switch *across* them, where it would be permanently shorted.

**Fix:** relabel to something unambiguous — `SW IN` / `SW OUT`, or `SW` / `SW`.



## O6. `LED-` runs beside SCL and beside the servo-ack analog node — MEASURED geometry / INFERRED crosstalk

> ✅ **RESOLVED — V6.** The 45.7 mm run alongside SCL is gone; worst is now **8.16 mm at
> 152 µm** (`LEDGT` ∥ `SCL`, top), worth a few mV into Q1's ~700 pF gate — LOW.
>
> The item that actually mattered is clean: **`$1N7075`, the ack analog node, is now 1.69 mm
> from `LEDGT` and 6.27 mm from `LED-`** (was 5.7 mm at 184 µm). Its only coupled run is to
> `SDA`, worth ≈5 mV against a 1.2 V threshold.


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


## O10. C3 and C4 are on stubs at J7; only C5 is in the current path — MEASURED

> ✅ **RESOLVED — V6, and conceded as never functional.** C3/C4 taps went 10 mil → **23 mil**.
> They remain single-via stubs carrying no DC current, but the loop is ~6–9 nH, which for a
> 1.5 A step in ~10 µs is **~1.3 mV**. Electrically harmless — this was a principle issue, not a
> functional one. C5 remains correctly in-path (7.06 mΩ loop).


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


---

# 🟡 OPEN — minor

## Standing corrections — do NOT "fix" these

**J7 (KF128-2.54-3P) is adequate for the servo. A reviewer proposed swapping it to KF301-5.0
on the grounds that 2.54 mm pitch caps the wire gauge — the premise is wrong.** LCSC's page for
the exact BOM part gives **8 A, 18–26 AWG (1 mm²)**. At 18 AWG a 0.5 m run contributes **31 mV**
at 1.5 A, so board (79 mV) + harness (31 mV) = **110 mV** against a 200–300 mV budget. Use
18 AWG and the connector is a non-issue.


**R6 (1 kΩ, 2512, U4.5→U4.6) is CORRECT. Three separate reviewers have now flagged it as a CT
soft-start pin wired wrongly.** It is not. From the TI TPS22919 datasheet (SLVSEN5B) directly:
pin 4 is NC and **pin 5 is QOD**; §8.3.3 lists *"Placing an external resistor between VOUT and
QOD"* as one of three sanctioned configurations; `RPD,QOD` = 24 Ω; and TI's own worked example
uses **RQOD = 1 kΩ** (Figure 31). R6 is exactly right as fitted.

**R9 (100 kΩ, `5VSW`→3V3) is CORRECT as a pull-UP. Two reviewers have now asked for it to be
flipped to a pull-down.** Keep it. It ties U4's ON to the ESP32 module's *own* 3.3 V regulator,
so the peripheral rail comes up **with** the MCU's supply — and ESP32 GPIOs are high-Z at reset,
so nothing is driving peripheral pins during that window. A pull-down instead leaves a window
between ESP32 boot and the firmware's `digitalWrite(D11, HIGH)` in which the MCU is running
while peripherals are unpowered; any sketch that touches a peripheral pin first — or any older
sketch — then injects current into dead rails. That was the original hazard, and the pull-up
prevents it structurally rather than by convention. It also means an ESP32 reset no longer drops
the Mega and ServoNano. The observation that Q2 defaults OFF while U4 defaults ON is correct and
intended: Q2 gates a rail firmware deliberately power-cycles, U4 gates one that should just be on.


| # | Finding | Detail | Fix |
|---|---|---|---|
| **O14** | ~~Silk on an exposed pad (R1 pad 2)~~ | ✅ **RESOLVED — V6.** Re-measured against solder-mask openings: **0.00000 mm² of silk intrusion on both sides**. R1 pad 2 is clear. Nothing to see, as the designer said — I should have marked this when the V6 layout pass reported it. | none |
| **O15** | ~~J6/J8 silk reads `+5V`~~ | ✅ **RESOLVED — V6**, and the residual withdrawn. Silk now reads `3V3 GND SDA SCL` (J6) and `S0 S1 S2 S3 OUT 3V3 GND` (J8). Labelling the Q2-switched rail simply `3V3` is correct — it *is* 3.3 V — and there is no room for `SW3V3`. | none |
| **O16** | U4 mask dam 179 µm (expansion already cut to 1 mil) | Arithmetic: pitch 0.65 − pad 0.42 = **0.230 mm** copper gap; dam = gap − 2×expansion. At 1 mil (25.4 µm) → 0.179 mm. **0.5 mil → 0.205 mm ✓**; **zero → 0.230 mm ✓**. Alternatively narrow U4's pads to 0.38 mm → 0.219 mm at 1 mil. | Set U4's mask expansion to **0.5 mil or 0**, *or* narrow its pads to 0.38 mm. Accepting is also defensible — a missing dam on a 6-pin SC70 is a modest bridging risk with a decent stencil |
| **O17** | ~~Sub-0.15 mm silk segments~~ | ⚪ **WITHDRAWN.** The two arcs at (17.145, −13.081), r = 0.845 mm, are the **"~" fuse marker inside the U3 footprint** — decorative. Refdes and outline still print; nothing is lost if the squiggle comes out ragged. | none |
| **O18** | A 0.254 mm neck in the `5VSWED` trunk | At (25.512,−14.239)→(25.400,−14.351), in the *entire* switched-rail current path. Thermally OK (~0.88 A limit) but careless. | Widen the C9→trunk section to 0.762 mm |
| **N5** | ~~Silk-to-mask clearance as low as 0.038 mm~~ | ⚪ **WITHDRAWN.** Actual measured silk-in-opening area is **0.00000 mm²** — this was a registration-*tolerance* concern, not real overlap. Fabs clip silk off mask openings as standard, and even unclipped a 0.04 mm sliver on a 1206/SOT-23 pad edge costs a negligible fraction of the solderable area. The clearances are inherent to the library footprints, so fixing means editing footprints for no measurable gain. | none |
| **N6** | ~~Hole-to-hole 0.4134 mm~~ | ✅ **RESOLVED — V7.** Re-measured: minimum is now **0.4809 mm**, and that pair is **same-net** (both `3V3`), where fab minimums drop to 0.254–0.3 mm. Only one pair under 0.5 mm and it is not a violation. | none |
| **N7** | Copper balance top 21 % / bottom 89 % | Very asymmetric for 1.6 mm 2-layer; mild bow and plating-uniformity risk. | A top-side GND pour in the empty regions also helps the LEDGT/SCL coupling |
| **O19** | ~~Silk reads `PARBoard - V0.5`, no date~~ | ✅ **RESOLVED — V7.** Top silk now reads **`PARShield  V0.7`** and **`2026-08-22`** — name, revision and date. | none |
| **O20** | ~~No test points~~ | ⚪ **WITHDRAWN — designer correct.** Everything worth probing is on a screw terminal or exposed male header: `+5V`/GND/`+12V` at J1, servo rail at J7.1, switched 3V3 at J6.1/J8.6, `5VSWED` at J11's male pins. The one socket-side net, `$1N7075`, is probeable at R4/R5's 1206 pads. | none |


---

# ✅ RESOLVED

*Original claim text preserved verbatim; the status line records what closed it.*

## O1. The TVS is wired in series with the +5 V rail — MEASURED

> ✅ **RESOLVED — V6.** `D2.1` is on `+5V` and **`D2.2` is on GND** — a proper shunt — and
> `J1.3` now reaches `+5V` directly. The series-wiring blocker is gone.
>
> 🟡 **One residual, BOM-only:** D2 is still **SMBJ5.0A**, whose **5.0 V stand-off** sits below a
> 5.25 V rail, so it will leak. Change to **SMBJ6.0A or SMBJ6.5A**. No layout impact.


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

## O2. C11 is a 6.3 V part on an 11.3 V rail — MEASURED

> ✅ **RESOLVED — V6.** C11 is now in the `CL31B106KAHNNNE` group: **10 µF 25 V X7R 1206**.
> The 100 µF 6.3 V part is gone from the BOM entirely.


**Verified in V5:** `$1N1028` = {C10.1, **C11.2**, D1.1, J2.15} — the post-D1 VIN node, ~11.3 V.
BOM V4 still lists C11 = **CL31A107MQHNNNE (LCSC C15008) = 100 µF 6.3 V X5R 1206**.

**1.8× over rated voltage.** MLCCs fail **short**, so this takes the 12 V rail with it.

**Fix:** 100 µF at 25 V does not exist in 1206. Use **C14860 (10 µF 25 V X7R 1206)** — already
in your BOM for C4/C7/C9, so no new line item. 10 µF is a proper buck-input cap; at 11.3 V
bias it derates to roughly 5–6 µF, which is fine.

Every other capacitor's rating checks out: C2/C5 16 V on 5 V, C3/C6/C8/C10/C12 50 V,
C4/C7/C9 25 V on ≤5 V. C11 is the only one wrong.

## B3. The limit-switch half of J9 is electrically dead — MEASURED

> ✅ **RESOLVED — V6.** `LIMSW` is now a single net across `J9.2`, `J9.3` and `C1.2`, so the
> pass-through works: switch wire in on one terminal, filtered signal out on the other.
>
> 🟡 **Residual, optional:** it is still a bare **1 nF** shunt with no series resistor, and the
> 2026-06-12 Y-limit investigation concluded 1 nF (~50 µs) is too small, recommending
> **1 kΩ + 100 nF**. The software debounce (`HOMING_LIMIT_DEBOUNCE_N 8`) is deployed and holding,
> so this is a nicety — just don't expect the hardware filter to do much if the glitch returns.

`LIMSW-` is a **single-node net** (`J9.3` only — the only one on the board). `LIMSW+`
reaches `C1.2` and `J9.2` and nothing else. Neither touches an MCU pin, and C1 (1 nF) is
debouncing a net connected to nothing.

Note the main MCU has no limit-switch role in firmware — homing limits live on the
Mega/CNC Shield V3. So either route these to a free GPIO (D3, D12, D13) with a pull-up, or
delete the switch pins and C1 and mark them NC.

Related: **there is no `LED+` net anywhere on the board.** Only `LED-` (Q1 drain) exists, so
the LED bank's anode feed and any current limit stay off-board and undocumented. A tidy fix
for both: repurpose J9 as the LED connector — `J9.2` → LED+, `J9.1` → LED−, drop `J9.3`/C1.

**Update (V5) — the designer's intent, and why it is not implementable as drawn.** The stated
purpose is: *take the Y limit-switch wire in, filter it, and export the filtered version to the
CNC shield on the Mega.* Three things block that:

1. **J9.3 connects to nothing at all** — not GND, not J9.2, not the cap. Nothing can leave it.
2. **There is no series resistor**, so there is no RC filter — C1 is only a shunt cap.
3. **C1 is 1 nF**, and this project's own Y-limit investigation concluded 1 nF (~50 µs) is too
   small, recommending **1 kΩ + 100 nF**.

**Fix:** `J9.2 → R (1 kΩ) → J9.3`, C1 raised to **100 nF** from J9.3 to GND. Switch in on J9.2,
filtered signal out on J9.3, then the flying wire to the CNC shield.

## O3. The polyfuse will nuisance-trip on a jammed arm — MEASURED copper / INFERRED fuse data

> ✅ **RESOLVED — V6.** The polyfuse moved out of the servo path. The chain is now
> `U4.6 → $1N7109 → U3 → 5VSWED → {J5.12 ServoNano, D3 → J11 Mega}`, while the servo runs
> `J1.3 → +5V → J7.1` **unfused**. U3 now sees ~330 mA against its 2 A hold — roughly 6×
> margin instead of 89 % of it — and its 30–70 mΩ is out of the droop budget entirely.


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

## O4. Q2 switches the sensors' ground, which cannot turn them off — MEASURED topology / INFERRED behaviour

> ✅ **RESOLVED — V6.** Q2 is now an **AO3401A P-FET switching the HIGH side** of the 3.3 V feed
> (`3V3` → Q2 source, `CLRNLDRDRN` → J6.1/J8.6), and **`J6.2`/`J8.7` are bonded to GND**. Cutting
> VDD while the grounds stay bonded is a real off state, so the parasitic-powering path is gone.
> The gate network is right too: R8 100 kΩ to source ⇒ **default-OFF when D3 is high-Z on reset**,
> R12 1 kΩ limits the drive, C13 slows the edge.


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

## O5. `CLRNLDRSRC` is routed as a signal, not a return — MEASURED

> ✅ **RESOLVED — V6.** `CLRNLDRSRC` no longer exists. Sensor grounds are on the pour, so every
> sensor signal — `OUT` included — returns in the plane beneath itself. The 100–200 mm² return
> loop is gone.


38.3 mm, **entirely 0.254 mm (10 mil)**: Q2.3→J6.2 = 47.4 mΩ, Q2.3→J8.7 = 22.0 mΩ.

Resistively harmless (~1 mV at 25 mA). The cost is **return-path geometry**: every sensor
signal — SDA, SCL, S0–S3 and `OUT` — returns through this thin trace to Q2 before entering
the pour, instead of returning in the plane under its own signal. For `OUT` (the frequency
output timed with `pulseIn`, and the channel the unresolved full-board false positives live
on) the signal runs 24 mm on top while its return runs 9–23 mm on the bottom to a different
point — a loop of order **100–200 mm²**.

**Fix:** subsumed by O4 — deleting Q2 lets the pour be the return. If Q2 stays, widen to
≥0.6 mm and route it directly beneath the J6/J8 signal groups.

## O8. R6 (0 Ω) makes a plugged-in USB burn ~1 W in U4 — MEASURED + datasheet

> ✅ **RESOLVED — V6.** R6 is now **1 kΩ** (2512). The QOD sink is capped at ~5 mA instead of
> 208 mA, so a plugged-in USB can no longer cook U4.


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

## O9. The 3V3 sensor rail has no decoupling — MEASURED

> ✅ **RESOLVED — V6.** C14 (100 nF) + C15 (10 µF) + C16 (100 nF) now sit on `CLRNLDRDRN`, the
> switched sensor rail — inside the switched domain, so Q2 still isolates. C13 (100 nF)
> additionally sits across Q2's gate–source.


Every capacitor sits on `+5V`, `5VSWED`, `$1N1028`, `+12V` or `LIMSW+`. **`3V3` has none.**

Measured: J2.2 → J6.1 = **42.9 mΩ over ~40 mm**; J2.2 → J8.6 = 22.6 mΩ. The trace is 0.762 mm
(generous) — the gap is the missing local reservoir at the far end of a 40 mm feed to a
VL53L4CD whose VCSEL draws tens of mA in bursts.

**Fix:** 100 nF + 4.7 µF at J6, 100 nF at J8. Both connector areas have room.

## O13. Feeding the Mega's 5 V pin back-drives its USB port — MEASURED / INFERRED (F20)

> ✅ **RESOLVED — V6.** D3 = **SS34** (LCSC `C8678`) between `5VSWED` and J11, blocking the Mega
> from back-feeding the switched rail.
>
> The forward half is unchanged by design: the shield's 5 V still reaches the Mega's USB VBUS
> through its T1 FDN340P. Keep the "do not attach USB to the Mega while the shield is powered"
> note, or power the Mega through VIN rather than its 5 V pin.


`5VSWED` drives J11.1/J11.2 = the Mega's +5 V pins. Arduino's own documentation: *"Supplying
voltage via the 5V or 3.3V pins bypasses the regulator, and can damage your board. We don't
advise it."* There is no sanctioned voltage or current rating for injecting into that pin.

Via the FDN340P path described in O8, the shield's 5 V propagates onto the Mega's USB VBUS
through its 500 mA polyfuse and out to any attached host.

**Fix:** a series Schottky between U4's output and J11.1/2 — **LCSC `C8678`** (MDD **SS34**,
40 V / 3 A, SMA). Vf ≈ 0.3 V at the expected ~300 mA, so the Mega sees ~4.7 V, comfortably
above its needs; the 3 A rating is deliberate overkill to keep Vf and dissipation low.

⚠ **It fixes only half of this.** The diode blocks current flowing *back* from the Mega into
`5VSWED` — the destructive half, since that is what drives the 24 Ω QOD sink in O8. It does
**not** stop the shield's 5 V reaching the Mega's USB VBUS through T1, because that is the
forward direction. So still document "do not attach USB to the Mega while the shield is
powered", or power the Mega through VIN instead of its 5 V pin.

## O11. `LED+` is tapped from the TCS3200's 3V3 line ✅ **RESOLVED — documented 2026-08-21**

> ✅ **RESOLVED.** The anode feed is deliberate and now documented: **the LEDs are powered from
> the TCS3200's 3V3 line, soldered at the sensor.** The original claim — that this would put
> LED current through the sensor's own supply pin and produce a droop *in phase* with the
> (on − off) subtraction — is **quantitatively negligible**, so the mechanism I warned about
> does not bite:
>
> LED load ≈ **160 mA** (the TCS3200 module's own 4 LEDs plus 4 more, all at 3V3) through the
> measured **22.6 mΩ** of 3V3 copper from J2.2 to J8.6 ⇒ **3.6 mV** of sensor-VDD droop when
> the bank is on. Against the TCS3200's supply-voltage sensitivity of **±0.5 %/V**, that is a
> **0.002 % shift in output frequency** — five orders of magnitude below the 2.17× threshold
> margin. Not measurable, let alone significant.
>
> One residual worth a single check, not a respin: the 3V3 rail now carries ~160 mA of LED
> load on top of the ESP32-S3's own draw, all from the Nano's onboard MP2322GQH buck. That
> part is a 2 A class device so headroom is almost certainly fine — confirm once and forget it.

## O12. `LED-` carries the bank current over 64 mm of 10-mil trace ✅ **RESOLVED — current now known**

> ✅ **RESOLVED.** This was blocked on not knowing the bank current. It is now known: **8 LEDs
> at 3V3, ≈160 mA**. The measured **125.5 mΩ** of `LED-` therefore drops **20 mV** and
> dissipates **3.2 mW** — and 160 mA is **18 %** of the IPC-2221 10 °C-rise limit (0.89 A) for
> 0.254 mm on 1 oz copper. **The 10-mil trace is adequate; no widening needed.**
>
> Note this does not clear **O6** — the coupling between `LED-` and SCL / the ack analog node
> is about the switching edge, not the current, so it stands on its own.

## B1. The switched rail is never enabled — MEASURED

> ✅ **RESOLVED — V5 + firmware.** `PARMain` drives D11 HIGH as the first statement in
> `setup()`, and **V5 adds R9 = 100 kΩ from `5VSW` to 3V3**. The pull-up also closes the
> "ON floats once the Smart Pull-Down disconnects" hole. Referencing it to **3V3 rather than
> 5 V** is the better choice — it gates on the ESP32's own regulator, so the rail still
> sequences behind the MCU without depending on firmware to do it.

`U4.3` (TPS22919 `ON`) ← net `5VSW` ← `U1.20` = **D11**. No sketch in the repo drives D11;
`PINOUT.md §2` lists D11–D13 as unused. The part has an internal **530 kΩ smart pull-down**,
so `ON` is held low and `5VSWED` stays **off** — and `5VSWED` powers the servo (J7), the
ServoNano (U2.27), the lidar (J6) and the colour sensor (J8). Everything downstream is dark.

**Fix (hardware, preferred): 100 kΩ from `U4.3` to 3V3.** Against the 530 kΩ pull-down that
gives 3.3 × 530/630 = **2.78 V**, far above the 1.0 V `ON` V_IH → rail on by default, and all
~22 existing sketches work unmodified while D11 remains an optional software kill.
**Pull it to 5 V and you put 5 V on D11 — don't.** Requires B4 (route 3V3).

Firmware alternative — `pinMode(D11, OUTPUT); digitalWrite(D11, HIGH);` first thing in
`setup()` — works but must be replicated in every sketch that uses the shield, and leaves a
window during reset/boot where the ESP32 tri-states D11 and the rail drops.

> **Why the firmware-only route is not enough.** The ESP32 tri-states its GPIOs on reset,
> brownout and boot, and this rig has *confirmed* brownout resets. Gating the servo rail on
> an MCU pin means a brownout now cuts servo power mid-flip: the arm goes limp wherever it
> is, `PARMain` reboots and runs `$H`, and homing drags the carriage with the arm possibly
> still at ENGAGE. That is precisely the sequence that snapped the flip arm, and precisely
> what `SERVO_ACK_MODE 2` was designed to prevent by never resetting on an unconfirmed arm
> position. **The servo should not be behind an MCU-gated switch at all** (see C1).

## B2. Both level-shift dividers are out of spec — MEASURED

> ✅ **RESOLVED — V3.** R3/R5 → 3.3 kΩ, giving 3.11 V against V_IH 2.475 V.

R2/R3 (Mega TX1 → `U1.32` = D0) and R4/R5 (ServoNano D3 ack → `U1.29` = D2) are both
**2 k / 2 k**. ESP32-S3 V_IH = 0.75 × VDD = **2.475 V** (datasheet Table 5-4).

| Source condition | Divider out | Margin |
|---|---|---|
| Ideal 5.000 V | 2.500 V | +25 mV |
| Realistic V_OH ≈ 4.90 V | 2.450 V | **−25 mV — fails** |
| Mega on USB, rail 4.75 V | 2.375 V | **−100 mV — fails** |
| VDD +2 % → V_IH 2.525 V | 2.500 V | **−25 mV — fails** |

This works only when the source is exactly 5.00 V *and* VDD is exactly 3.30 V. Symptoms:
framing garbage on the 115200 GRBL link (surfacing as `error:N` retries → 60 s watchdog
`esp_restart()`), and missed acks → infinite retry stall.

**Fix: R3 and R5 → 3.3 kΩ.** Keeping 2 k on top gives 3.11 V; going to the documented
1.8 k/3.3 k gives 3.24 V. Either is fine and both stay under the 3.6 V abs max. Four
resistors, no layout change.

RC is a non-issue at both baud rates (τ ≈ 10 ns against an 8.68 µs bit at 115200) — **do not
attribute any GRBL link problem to divider bandwidth. It is entirely the DC level.**

## B4. 3V3 is not routed anywhere — MEASURED

> ✅ **RESOLVED — V4.** `3V3` is now a routed net. Conceded: this was an enabler for other
> fixes, not a standalone defect, and should not have been listed as blocking.

Net `$1N5429` = `J2.2` ↔ `U1.3` only: the 3.3 V rail exists on the socket and goes nowhere.
It is the enabler for B1's `ON` pull-up, the I²C pull-ups (C5), and moving the lidar off 5 V
(C6). **Routing 3V3 as a board net is the single highest-leverage change available.**

---

## C1. The servo droop budget is spent on the board, before any wire — MEASURED

> ✅ **RESOLVED — V3.** `J7.1` moved to unswitched `+5V`, rail widened to 30 mil. Measured
> **43.86 / 44.3 / 44.4 mΩ** by three independent methods ⇒ ~70 mV at stall.
> ⚠ **O1 and O3 put this back** — the series TVS and the polyfuse re-enter the same path.

`PINOUT.md §7` sets the target at **≤ 0.2–0.3 V total drop at stall, measured at the servo
connector**. Measured path from J1 to the servo and back:

| Segment | R | Drop @ 1.5 A |
|---|---|---|
| J1.3 → U4.1 (`+5V` in, 80 mil) | 14.1 mΩ | 21 mV |
| **U4 TPS22919 R_on** (88 mΩ typ / ~120 mΩ hot) | 88–120 mΩ | **132–180 mV** |
| **U4.6 → J7.1 (`5VSWED`): 82.5 mm of 17-mil** | **93.6 mΩ** | **140 mV** |
| GND plane J7.2 → J1.2 (FD solve) | 2.3 mΩ | 3.5 mV |
| **Board total** | **198–230 mΩ** | **300–345 mV** |

The rail travels **82.5 mm** for a ~35 mm straight-line distance — west, down the left edge,
32 mm east along the bottom, then north-east to J7. Thermally the 17-mil is fine (ΔT ≈ 14 °C
at 1.5 A); **resistively it is the whole problem.** Every millivolt the harness rework bought
is handed straight back.

**Fix, in order of value:**
1. **Take the servo off the load switch.** Feed `J7.1` from `+5V` directly. Removes
   88–120 mΩ and the 1.5 A ceiling at a stroke, and fixes B1's brownout interaction.
2. Move U4 and C2 beside J7; route VOUT→J7 as a short 80-mil trace or a top-layer pour —
   30 mm at 80 mil is **7 mΩ**, a 13× improvement.
3. At minimum widen `5VSWED` to ≥ 60 mil (→ 26.5 mΩ) and pour it on the empty top layer.

## C2. The switched rail is a daisy chain with the servo *last* — MEASURED

> ✅ **RESOLVED — V3/V4.** Servo on `+5V`, sensors on `3V3`; only the ServoNano and the Mega
> remain on `5VSWED`, at tens to hundreds of mA.

Chain order from U4: ServoNano 5 V (`J5.12`, 36.5 mΩ) → lidar (`J6.1`, 44.5 mΩ) → TCS3200
(`J8.6`, 90.4 mΩ) → **servo (`J7.1`, 93.6 mΩ)**. Because the largest load is at the far end,
the full 1.5 A flows through every sensor's supply trace. Node sag at stall, relative to
U4 VOUT: ServoNano **−55 mV**, lidar −67 mV, TCS3200 **−136 mV**.

So a flip stall pulls down the supply of the microcontroller generating the servo pulse, and
the colour sensor's supply, simultaneously and by construction. Your brownout note
prescribes *"separate the logic supply from the servo/stepper supply with a common ground"* —
this does the opposite.

**Fix:** star the rail from a single node, highest current first; better, give the servo its
own feed from J1 and leave only sensors behind U4.

## C3. Zero decoupling on the entire board — MEASURED

> ✅ **RESOLVED — V4.** Ten capacitors added; placement correct at U4 and for C5.
> ⚠ Partially: **C3/C4 sit on stubs (O10)** and **3V3 still has none (O9)**.

The complete capacitor census is **C2 (1500 µF, on the *input* side of the switch, 27.7 mm
away) and C1 (1 nF, on the dead LIMSW+ net)**. There is not one 100 nF anywhere, and
**`5VSWED` — feeding servo, ServoNano, lidar and colour sensor — has no capacitor at all.**
TI's datasheet *requires* a local input cap on U4; none exists.

**Fix:** 10 µF X7R + 100 nF within 2 mm of `U4.1`; bulk + 10 µF + 100 nF **at J7**; 100 nF at
J6 and J8; 100 nF at each module's 5 V/GND pins.

⚠ **Inrush ceiling if you keep the switch:** the TPS22919's slew is fixed at 3.2 mV/µs, so
turn-on inrush = C_out × 3.2 mA/µF. 220 µF → 0.70 A; **470 µF → 1.50 A, at the device limit.**
Cap C_out at ~220 µF behind U4. If the servo moves to unswitched `+5V` (C1), this constraint
disappears and you can fit 470–1000 µF at J7.

## C4. R5 destroys the ack line's fail-loud property — MEASURED topology

> ✅ **RESOLVED — firmware.** Polarity inverted to active-HIGH, so the divider's bottom leg
> *is* the fail-safe. `servoAckWaitIdle()` now returns a bool and a line stuck asserted is
> treated as a fault that blocks, instead of timing out silently and letting `servoAckSeen()`
> pass vacuously.

With R5 (2 kΩ) permanently soldered from the ack node to GND, a broken ack wire, an unplugged
ServoNano, or an **unpowered** ServoNano leaves D2 held at

> 3.3 × 2k/(2k + 45k) = **0.14 V** — a hard LOW, far below V_IL = 0.825 V

The ack is **active-LOW**, so a dead ack source reads as **permanently acknowledged**.
`servoAckSeen()` returns true immediately and `SERVO_ACK_MODE 2`'s retry-until-confirmed
enforcement — the thing credited with preventing the arm break — passes vacuously.

**This is a topology problem, not a ratio problem.** The proven 1.8 k/3.3 k gives 0.23 V and
fails identically. No resistor value fixes it while the bottom leg is populated.

**Fix — cheapest is firmware, today:** `servoAckWaitIdle()` already detects this (the line
never returns HIGH, so every command carries an extra ~100 ms). Promote that timeout from a
silent diagnostic to a **hard fault**. Three lines, no hardware change.
Proper respin: open-drain + pull-up, or a buffer whose absence reads HIGH.

> **`PINOUT.md §3b` is wrong and should be corrected.** It claims *"an unfitted or broken wire
> reads HIGH = 'no ack' = fails loud."* That holds only when the connection to D2 itself is
> absent. A break **upstream** of the divider reads LOW and fails **silent**. The doc is also
> internally inconsistent — its own validation section describes the stuck-low vacuous pass
> and its +100 ms signature. On the breadboard the divider lived on the removable harness, so
> "unfitted" was the likely failure; soldering it down makes the silent case the likely one.

## C5. No I²C pull-ups — MEASURED

> ✅ **RESOLVED — V5.** R10/R11 = 3.3 kΩ to 3V3. The bus was empirically fine beforehand
> (~6–7 h of clean lidar scans), so this is margin rather than a repair — but it **interacts
> with O4**: pull-ups feed the floating sensor ground when Q2 is off.

`SDA` = {`J2.8`, `J6.3`, `U1.9`} and `SCL` = {`J2.9`, `J6.4`, `U1.10`} — three pins each, no
resistor, no footprint. Firmware runs `Wire.setClock(400000)`, which needs t_r ≤ 300 ns:

| Pull-up | Max bus C at 400 kHz |
|---|---|
| 45 kΩ (ESP32 internal) | **7.9 pF — impossible** |
| 10 kΩ (typical breakout) | 35 pF — marginal |
| 2.2 kΩ | 161 pF ✓ |

`PINOUT.md §5` records the lidar's boot init failing *"roughly half of observed power-ons"*,
then running a 71-minute pass flawlessly once up — a signature that fits init-sequence timing
sensitivity. **Slow rise time is a credible root cause.**

**Fix:** 2.2 kΩ (or 4.7 kΩ) from SDA and SCL to **3V3**. Free diagnostic before any respin:
drop `Wire.setClock()` to 100000 and see whether the boot-failure rate changes.

## C6. 5 V into non-5 V-tolerant pins, in two places

> ✅ **RESOLVED — V4, and cleanly.** Both sensors moved to `3V3`, so `OUT` swings 0–3.3 V
> natively and any breakout pull-ups reference 3.3 V. **Recalibration is small**: the TCS3200's
> supply-voltage sensitivity is ±0.5 %/V, so 5 V→3.3 V shifts output frequency under 1 %, well
> inside the 2.17× threshold margin. (Characterised at 5 V ±10 %, so verify empirically.)

- **TCS3200 `OUT` → D8, bare copper.** `J8.6` = `5VSWED` (5 V), so the sensor runs at 5 V and
  its push-pull `OUT` (V_OH ≈ 4.6–5.0 V) goes straight to `U1.23` = D8. ESP32-S3 abs max is
  VDD+0.3 = **3.6 V**. This is the line `pulseIn` hammers for ~70 min per full-board scan.
  *Caveat: `PINOUT.md` never recorded the sensor's supply voltage on the existing rig, so this
  may be inherited rather than introduced — check with a meter before judging it a regression.*
  **Fix:** move `J8.6` to 3V3 (TCS3200 runs 2.7–5.5 V; its V_IH is 2.0 V so 3.3 V drive on
  S0–S3 is legal) — but note output frequency scales with VDD, so `SCAN_ON_BLUE_MAX = 3535`
  must be re-derived. Calibration-preserving alternative: 1 k series + 2 k to GND on `OUT`.
- **Lidar I²C.** `J6.1` feeds the VL53L4CD breakout from 5 V while SDA/SCL run straight to
  A4/A5. Safe *only* if that specific carrier level-shifts and doesn't reference its pull-ups
  to VIN. **Measure SDA idle voltage at `J6.3` before powering the board.** Feeding `J6.1`
  from 3V3 removes the question entirely.

## C7. Q1 (2N7002) — under-driven, undersized, no gate pull-down — MEASURED + datasheet

> ✅ **RESOLVED — V4.** AO3400A + R7 100 kΩ gate pull-down.
> ⚠ **See O6**: the faster edge makes the `LED-`/SCL and `LED-`/ack-node coupling matter more,
> and no gate series resistor is fitted.

`GATE` = {`J3.13`, `Q1.1`, `U1.21`} — no pull-down, no series resistor. The 2N7002's
R_DS(on) is characterised at V_GS = 10 V and 4.5 V, **never at the 3.3 V the ESP32 drives**,
and V_GS(th) runs to 2.5 V (3.0 V on some vendors' parts) — so a worst-case device has under
1 V of overdrive. Continuous I_D is only ~115 mA (Vishay) / 300 mA (Diodes).

Two consequences worth taking seriously:
- **No gate pull-down** ⇒ the LED bank's state is undefined through reset, boot and brownout.
  `readAmbientSubtracted()` is only valid if the bank is definitively **off** during the OFF
  flash — this is a correctness risk, not cosmetic.
- Operating near threshold, channel resistance drifts with die temperature, so **LED
  brightness drifts over a long scan**. That is a *candidate* mechanism for the unresolved
  "12 false positives on a 70-min full-board pass, 0/888 on the 13-min bottom-rows test"
  asymmetry — both predict long-run-only failure. **Hypothesis, not a diagnosis.**

**Fix:** 100 kΩ gate→GND (mandatory, cheap), ~100 Ω gate series, and swap to a logic-level
part specified at V_GS = 2.5 V — **AO3400A** or DMG3414U.

## C8. R1 (2 kΩ) on the servo command line raises noise gain ~50×

> ✅ **RESOLVED — V4.** R1 → 330 Ω. Kept at 330 Ω rather than raised to 2.2 kΩ for injection
> limiting; instead **`Serial2.begin()` was moved after the rail-settle delay** in firmware, so
> D9 stays high-Z until the ServoNano is powered. Injection goes to zero without giving up
> noise immunity.

DC-harmless (AVR input leakage ≤ 1 µA → 2 mV drop), but it lifts the ServoNano RX node from
~40 Ω to 2 kΩ — on the exact net `CLAUDE.md` nominates as the untested stepper-EMI coupling
path, and the one the bench rig (steppers unpowered, 91 kB, zero dropouts) could not
reproduce. **Fix: 100–330 Ω.**

Optional, if servo-link reliability stays open: R1 = 1 kΩ plus a 10 kΩ pull-up from the
ServoNano-side node to 5VSWED gives V_high = 3.455 V, a solid +455 mV over the AVR's 3.00 V
V_IH. Only adopt once rail sequencing (B1) is deliberate, or 0.45 mA flows into the ESP32
clamp when 5VSWED is live and the ESP32 isn't.

## C10. No decoupling on the ESP32 VIN node — MEASURED

> ✅ **RESOLVED — V4.** C10 (100 nF) + C11 on the D1 cathode, C12 on +12 V.
> ⚠ **C11's voltage rating is a blocker — see O2.**

`+12V` → 15-mil trace → D1 → `U1.16`. **No capacitor on either the +12 V net or the D1
cathode.** If 12 V is shared with the stepper supply, every commutation transient lands on
the Nano's regulator input unbuffered — on a board with a confirmed brownout history.
**Fix:** 100 µF/25 V + 100 nF at the D1 cathode, 100 nF at the anode. Widen +12 V to 25 mil.

## C11. Two 5 V sources shorted together — MEASURED

> ✅ **RESOLVED — V3.** J11 moved to `5VSWED` as an output; `J1.3` is now the sole source.

`+5V` is driven from **both** `J1.3` (screw terminal) and `J11.1`/`J11.2`, with no ORing, no
fuse, and no reverse protection (D1 guards only +12 V). If J11 mates to the Mega's 5 V, the
servo's 1.5 A stall shares the Mega's 5 V node — the coupling `PINOUT.md §7` already flags.
**Fix:** pick one source; mark the other NC.


---

# ⚪ WITHDRAWN / CORRECTED

*Original claim text preserved; the status line records why it was retracted.*

## C9. J12 has no ground pin — MEASURED

> ⚪ **WITHDRAWN — the designer was right.** J12 mates to the Mega's COMMUNICATION header
> (D21–D14), which contains **no GND pin**. Tying a spare J12 pin to GND would short a Mega
> GPIO. The return via J11's GND is the only option and is adequate.

`J12` uses only pins 3 (`ESP32TX`) and 4 (`ESP32RX`). The only GND pins on any header are
`J11.35/36`. A 115200 UART with its return several inches away through a different connector
is exactly how you get the coupling the EMI hypothesis describes. **Fix: assign J12.1 and/or
J12.2 to GND** (both free) and route them flanking TX/RX.

## C12. ServoNano USB back-feeds the switched rail — INFERRED

> ⚪ **WITHDRAWN as a production issue** — no USB is attached in production. The bench hazard
> it creates (flashing GRBL with the shield unpowered) is tracked under **O8**.

`U2.27` (ServoNano 5 V) sits on `5VSWED`, and ServoNano's USB is in active use as a
signal-integrity probe. On CH340 Nano clones USB V_BUS reaches the 5 V rail through a
diode/fuse, so with USB plugged the rail is powered even when U4 is off — silently powering
the servo, lidar and sensor from the Mac's USB port, and putting a 1.5 A stall load on a
500 mA budget. The TPS22919's QOD FET would also be trying to sink that rail through 24 Ω.
**Measure `5VSWED` with U4 off and ServoNano USB attached before leaving the board powered.**

---


## Claims withdrawn without a standalone section

| Was | Why withdrawn |
|---|---|
| **m2** — "silk over pads, 20 instances including R6" | **Designer correct, and my method was wrong.** That figure came from testing silk against **copper**, which false-positives on every trace. Re-tested against **solder-mask openings**: exactly **one** genuine case board-wide (R1 pad 2 — now O14). R6 is clean, as the designer said. |
| **m3** — "J7.2 has only 2 thermal spokes" | **Immaterial.** Two spokes ≈ 0.34 mΩ ⇒ 0.5 mV at 1.5 A. The designer's explanation (adjacent non-GND pads) accounts for it. |
| **m9** — "no header for the witness servo" | **Designer correct.** A KF128 screw terminal accepts two wires. |
| **L5** — "108 mm of bottom trace slots the ground plane" | **Second-order and already minimised** — the designer routes on the bottom only where no top path exists. Ground measures excellent regardless. |
| **L6** — "+5V to J11 at 15 mil may be too thin" | **Adequate.** 15 mil on 1 oz external = 1.19 A at 10 °C rise; Mega + CNC-shield *logic* is ~300 mA. ~4× margin. |
| "TCS3200 S0–S3 at 3.3 V may not meet V_IH" | **Wrong.** ams DS000107 v1-01 p.6: `VIH min 2 V, max VDD` — an absolute 2.0 V, not a fraction of VDD. |
| "J1 silkscreen arrows point into gaps" | **Wrong.** The glyphs are **closed 1.0 × 1.5 mm rectangles** — J1's body outline, not arrows. The text labels are correctly aligned over their pads. |
| "ESP32 TX may not pull the Mega's RX low (16U2 contention)" | **Does not apply.** That mechanism lives on D0/D1; this design uses Serial1 (D18/D19). |
| "R6 / U4 pin 5 may be a CT pin wired wrongly" | **Wrong.** Pin 5 is **QOD**; tying it to VOUT is sanctioned. Only the 0 Ω *value* is at issue — see O8. |


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
defects. **`LIMSW-` is the only genuine one (B3).**

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
